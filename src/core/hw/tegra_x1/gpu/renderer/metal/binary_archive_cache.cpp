#include "core/hw/tegra_x1/gpu/renderer/metal/binary_archive_cache.hpp"

#include "common/config.hpp"
#include "core/debugger/debugger_manager.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/shader_library_cache.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <fstream>
#include <optional>
#include <tuple>
#include <pthread.h>
#include <unistd.h>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {
namespace {
template <typename... T>
void logWorker(fmt::format_string<T...> format, T&&... args) {
    // SwiftUI can terminate without destroying the renderer first. Avoid the
    // debugger callback here: its static state may already be torn down while
    // this worker finishes saving a binary.
    const auto message = fmt::format(format, std::forward<T>(args)...);
    std::fprintf(stderr, "[Metal shader cache] %s\n", message.c_str());
}

NS::URL* archiveUrl(const std::filesystem::path& path) {
    return NS::URL::fileURLWithPath(
        NS::String::string(path.c_str(), NS::UTF8StringEncoding));
}

MTL::BinaryArchive* openArchive(MTL::Device* device,
                                const std::filesystem::path& path = {}) {
    NS_STACK_SCOPED auto descriptor =
        MTL::BinaryArchiveDescriptor::alloc()->init();
    if (!path.empty())
        descriptor->setUrl(archiveUrl(path));
    NS::Error* error = nullptr;
    return device->newBinaryArchive(descriptor, &error);
}

std::string hexDigest(const void* data, usize size) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, static_cast<CC_LONG>(size), digest);
    std::string key;
    for (const auto byte : digest)
        key += fmt::format("{:02x}", byte);
    return key;
}

constexpr u32 VERTEX_BUFFER_LAYOUT_COUNT = 31;
constexpr usize FIELD_COUNT =
    VERTEX_BUFFER_LAYOUT_COUNT * 6 + COLOR_TARGET_COUNT * 9 + 7;

// Every pipeline state Hydra sets, in a fixed order. Encode fields
// explicitly; hashing descriptor pointers or C++ struct padding would not give
// a stable cache key between runs.
std::vector<u64> descriptorFields(MTL::RenderPipelineDescriptor* descriptor) {
    std::vector<u64> fields;
    fields.reserve(FIELD_COUNT);
    auto add = [&fields](auto value) {
        fields.push_back(static_cast<u64>(value));
    };
    for (u32 i = 0; i < VERTEX_BUFFER_LAYOUT_COUNT; i++) {
        const auto attribute =
            descriptor->vertexDescriptor()->attributes()->object(i);
        add(attribute->format());
        add(attribute->offset());
        add(attribute->bufferIndex());
        const auto layout =
            descriptor->vertexDescriptor()->layouts()->object(i);
        add(layout->stride());
        add(layout->stepFunction());
        add(layout->stepRate());
    }
    for (u32 i = 0; i < COLOR_TARGET_COUNT; i++) {
        const auto color = descriptor->colorAttachments()->object(i);
        add(color->pixelFormat());
        add(color->isBlendingEnabled());
        add(color->rgbBlendOperation());
        add(color->alphaBlendOperation());
        add(color->sourceRGBBlendFactor());
        add(color->destinationRGBBlendFactor());
        add(color->sourceAlphaBlendFactor());
        add(color->destinationAlphaBlendFactor());
        add(color->writeMask());
    }
    add(descriptor->depthAttachmentPixelFormat());
    add(descriptor->stencilAttachmentPixelFormat());
    add(descriptor->sampleCount());
    add(descriptor->isRasterizationEnabled());
    add(descriptor->isAlphaToCoverageEnabled());
    add(descriptor->isAlphaToOneEnabled());
    add(descriptor->inputPrimitiveTopology());
    return fields;
}

// Inverse of descriptorFields
void applyDescriptorFields(MTL::RenderPipelineDescriptor* descriptor,
                           const std::vector<u64>& fields) {
    usize index = 0;
    auto next = [&] { return fields[index++]; };
    for (u32 i = 0; i < VERTEX_BUFFER_LAYOUT_COUNT; i++) {
        const auto attribute =
            descriptor->vertexDescriptor()->attributes()->object(i);
        attribute->setFormat(static_cast<MTL::VertexFormat>(next()));
        attribute->setOffset(next());
        attribute->setBufferIndex(next());
        const auto layout =
            descriptor->vertexDescriptor()->layouts()->object(i);
        layout->setStride(next());
        layout->setStepFunction(static_cast<MTL::VertexStepFunction>(next()));
        layout->setStepRate(next());
    }
    for (u32 i = 0; i < COLOR_TARGET_COUNT; i++) {
        const auto color = descriptor->colorAttachments()->object(i);
        color->setPixelFormat(static_cast<MTL::PixelFormat>(next()));
        color->setBlendingEnabled(next() != 0);
        color->setRgbBlendOperation(static_cast<MTL::BlendOperation>(next()));
        color->setAlphaBlendOperation(static_cast<MTL::BlendOperation>(next()));
        color->setSourceRGBBlendFactor(static_cast<MTL::BlendFactor>(next()));
        color->setDestinationRGBBlendFactor(
            static_cast<MTL::BlendFactor>(next()));
        color->setSourceAlphaBlendFactor(static_cast<MTL::BlendFactor>(next()));
        color->setDestinationAlphaBlendFactor(
            static_cast<MTL::BlendFactor>(next()));
        color->setWriteMask(static_cast<MTL::ColorWriteMask>(next()));
    }
    descriptor->setDepthAttachmentPixelFormat(
        static_cast<MTL::PixelFormat>(next()));
    descriptor->setStencilAttachmentPixelFormat(
        static_cast<MTL::PixelFormat>(next()));
    descriptor->setSampleCount(next());
    descriptor->setRasterizationEnabled(next() != 0);
    descriptor->setAlphaToCoverageEnabled(next() != 0);
    descriptor->setAlphaToOneEnabled(next() != 0);
    descriptor->setInputPrimitiveTopology(
        static_cast<MTL::PrimitiveTopologyClass>(next()));
}

std::string pipelineKey(const std::vector<u64>& fields,
                        std::string_view shader_key) {
    std::string bytes(shader_key);
    bytes.push_back('\0');
    bytes.append(reinterpret_cast<const char*>(fields.data()),
                 fields.size() * sizeof(u64));
    return hexDigest(bytes.data(), bytes.size());
}

// Recipe file: magic, version, shader key, vertex and fragment library keys,
// descriptor fields
constexpr u32 RECIPE_MAGIC = 0x50434852; // "RHCP"
constexpr u32 RECIPE_VERSION = 1;
constexpr u32 MAX_RECIPE_STRING = 4096;

struct Recipe {
    std::string shader_key;
    PipelineSources sources;
    std::vector<u64> fields;
};

std::string encodeRecipe(std::string_view shader_key,
                         const PipelineSources& sources,
                         const std::vector<u64>& fields) {
    std::string bytes;
    auto add_word = [&bytes](u32 value) {
        bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    auto add_string = [&](std::string_view string) {
        add_word(static_cast<u32>(string.size()));
        bytes.append(string);
    };
    add_word(RECIPE_MAGIC);
    add_word(RECIPE_VERSION);
    add_string(shader_key);
    add_string(sources.vertex);
    add_string(sources.fragment);
    add_word(static_cast<u32>(fields.size()));
    bytes.append(reinterpret_cast<const char*>(fields.data()),
                 fields.size() * sizeof(u64));
    return bytes;
}

std::optional<Recipe> readRecipe(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    auto read_word = [&input]() -> std::optional<u32> {
        u32 value;
        if (!input.read(reinterpret_cast<char*>(&value), sizeof(value)))
            return std::nullopt;
        return value;
    };
    auto read_string = [&]() -> std::optional<std::string> {
        const auto size = read_word();
        if (!size || *size > MAX_RECIPE_STRING)
            return std::nullopt;
        std::string string(*size, '\0');
        if (!input.read(string.data(), *size))
            return std::nullopt;
        return string;
    };
    if (read_word() != RECIPE_MAGIC || read_word() != RECIPE_VERSION)
        return std::nullopt;
    Recipe recipe;
    auto shader_key = read_string();
    auto vertex = read_string();
    auto fragment = read_string();
    if (!shader_key || !vertex || !fragment || vertex->empty() ||
        read_word() != FIELD_COUNT)
        return std::nullopt;
    recipe.shader_key = std::move(*shader_key);
    recipe.sources = {std::move(*vertex), std::move(*fragment)};
    recipe.fields.resize(FIELD_COUNT);
    if (!input.read(reinterpret_cast<char*>(recipe.fields.data()),
                    FIELD_COUNT * sizeof(u64)))
        return std::nullopt;
    return recipe;
}

bool writeFile(const std::filesystem::path& path, std::string_view bytes) {
    const auto temporary = fmt::format("{}.{}.tmp", path.string(), getpid());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    std::error_code error;
    if (output)
        std::filesystem::rename(temporary, path, error);
    if (!output || error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}
} // namespace

std::filesystem::path BinaryArchiveCache::defaultPath(MTL::Device* device) {
    // Source/options and pipeline state identify entries; GPU and OS/driver
    // identify the directory. Metal validates the actual binary on every hit.
    // The GPU is identified by its name: the registry ID changes between
    // sessions, which threw the whole cache away and recompiled every shader
    // of the game on each launch.
    const auto* version = NS::ProcessInfo::processInfo()
                              ->operatingSystemVersionString()
                              ->utf8String();
    const auto* gpu_name = device->name()->utf8String();
    return std::filesystem::path(CONFIG_INSTANCE.getAppDataPath()) / "cache" /
           "metal" /
           fmt::format("v3-{}-{}",
                       hexDigest(gpu_name, std::strlen(gpu_name)).substr(0, 16),
                       hexDigest(version, std::strlen(version)));
}

BinaryArchiveCache::BinaryArchiveCache(MTL::Device* device_,
                                       std::filesystem::path path_)
    : device(device_->retain()),
      path(path_.empty() ? defaultPath(device_) : std::move(path_)) {
    // Draws whose pipeline is compiling are skipped, which can briefly drop
    // geometry, so this is opt-in
    if (const char* value = std::getenv("HYDRA_ASYNC_PIPELINES");
        value && std::string_view(value) == "1") {
        async = true;
        // The compiler service runs compilations of different threads in
        // parallel. Leave most cores to the guest.
        const auto count =
            std::clamp(std::thread::hardware_concurrency() / 4, 1u, 2u);
        for (u32 i = 0; i < count; i++)
            async_workers.emplace_back([this] { runAsync(); });
        LOG_INFO(MetalRenderer, "Async pipeline compilation: {} workers",
                 count);
    }
    if (const char* disabled = std::getenv("HYDRA_DISABLE_SHADER_CACHE");
        disabled && std::string_view(disabled) == "1")
        return;

    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        LOG_WARN(MetalRenderer, "Cannot write shader cache: {}",
                 error.message());
        return;
    }
    enabled = true;
    LOG_INFO(MetalRenderer, "Shader binary cache directory: {}", path.string());
    worker = std::thread([this] { run(); });
}

BinaryArchiveCache::~BinaryArchiveCache() {
    stopPrewarm();
    // Workers finish their current compilation, which may queue a save
    {
        std::lock_guard lock(mutex);
        async_stopping = true;
    }
    async_condition.notify_all();
    for (auto& async_worker : async_workers)
        async_worker.join();
    for (auto& job : async_jobs)
        job.descriptor->release();
    async_jobs.clear();
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    condition.notify_all();
    if (worker.joinable())
        worker.join();
    for (auto& [key, pipeline] : prewarmed_pipelines)
        pipeline->release();
    logWorker("Shader binary cache: {} hits, {} misses, {} prewarmed "
              "pipelines used of {}",
              hits.load(), misses.load(), prewarm_hits.load(),
              prewarmed.load());
    device->release();
}

MTL::RenderPipelineState*
BinaryArchiveCache::build(MTL::RenderPipelineDescriptor* descriptor,
                          NS::Error** error,
                          const std::filesystem::path& entry,
                          bool& archive_hit) {
    archive_hit = false;
    std::error_code file_error;
    if (!entry.empty() && std::filesystem::is_regular_file(entry, file_error)) {
        NS_STACK_SCOPED auto archive = openArchive(device, entry);
        if (archive) {
            NS::Object* archives[] = {archive};
            descriptor->setBinaryArchives(NS::Array::array(archives, 1));
            auto pipeline = device->newRenderPipelineState(
                descriptor, MTL::PipelineOptionFailOnBinaryArchiveMiss,
                nullptr, error);
            descriptor->setBinaryArchives(nullptr);
            if (pipeline) {
                archive_hit = true;
                {
                    std::lock_guard lock(mutex);
                    pending.push_back({nullptr, entry, {}, {}});
                }
                condition.notify_all();
                return pipeline;
            }
        }
        // Invalid, incompatible, or missing binary: render normally and
        // replace just this entry. Other entries remain usable.
        *error = nullptr;
    }
    auto pipeline = device->newRenderPipelineState(descriptor, error);
    if (pipeline && !entry.empty()) {
        auto copy = descriptor->copy();
        {
            std::lock_guard lock(mutex);
            pending.push_back({copy, entry, {}, {}});
        }
        condition.notify_all();
    }
    return pipeline;
}

void BinaryArchiveCache::queueRecipe(const std::string& key,
                                     std::string_view shader_key,
                                     const PipelineSources* sources,
                                     const std::vector<u64>& fields) {
    // Called with the mutex held
    if (recipe_directory.empty() || used_recipes.contains(key))
        return;
    used_recipes.insert(key);
    auto recipe_path = recipe_directory / (key + ".recipe");
    if (recipe_keys.contains(key)) {
        pending.push_back({nullptr, {}, {}, std::move(recipe_path)});
    } else {
        // Shaders without a stored source (e.g. AIR) cannot be rebuilt
        if (!sources || sources->vertex.empty())
            return;
        recipe_keys.insert(key);
        pending.push_back({nullptr,
                           {},
                           encodeRecipe(shader_key, *sources, fields),
                           std::move(recipe_path)});
    }
    condition.notify_all();
}

MTL::RenderPipelineState*
BinaryArchiveCache::createPipeline(MTL::RenderPipelineDescriptor* descriptor,
                                   NS::Error** error,
                                   std::string_view shader_key,
                                   const PipelineSources* sources) {
    *error = nullptr;
    if (!enabled) {
        misses++;
        return device->newRenderPipelineState(descriptor, error);
    }

    const auto fields = descriptorFields(descriptor);
    const auto key = pipelineKey(fields, shader_key);
    {
        std::lock_guard lock(mutex);
        queueRecipe(key, shader_key, sources, fields);
    }
    return obtain(descriptor, error, key);
}

MTL::RenderPipelineState*
BinaryArchiveCache::obtain(MTL::RenderPipelineDescriptor* descriptor,
                           NS::Error** error, const std::string& key) {
    std::shared_ptr<Building> building_entry;
    {
        std::unique_lock lock(mutex);
        for (;;) {
            if (auto it = prewarmed_pipelines.find(key);
                it != prewarmed_pipelines.end()) {
                auto* pipeline = it->second;
                prewarmed_pipelines.erase(it);
                prewarm_hits++;
                if (PerfStats::isEnabled())
                    PerfStats::instance().pipeline_prewarm_hits++;
                return pipeline;
            }
            // Wait for a pipeline the prewarm or another draw is building
            // instead of compiling it twice
            const auto it = building.find(key);
            if (it == building.end())
                break;
            const auto other = it->second;
            built.wait(lock, [&] { return other->done; });
            if (other->pipeline)
                return other->pipeline->retain();
            // A failed build is retried below to report its error
        }
        created.insert(key);
        building_entry = std::make_shared<Building>();
        building.emplace(key, building_entry);
    }

    bool archive_hit;
    auto* pipeline = build(descriptor, error,
                           path / (key + ".metallib"), archive_hit);
    {
        std::lock_guard lock(mutex);
        building.erase(key);
        building_entry->done = true;
        if (pipeline)
            building_entry->pipeline = pipeline->retain();
    }
    built.notify_all();
    if (archive_hit) {
        const auto count = ++hits;
        if (count == 1 || count % 128 == 0)
            LOG_INFO(MetalRenderer, "Shader binary cache: {} hits, {} misses",
                     count, misses.load());
    } else {
        misses++;
    }
    return pipeline;
}

MTL::RenderPipelineState* BinaryArchiveCache::createPipelineAsync(
    MTL::RenderPipelineDescriptor* descriptor, NS::Error** error,
    std::string_view shader_key, const PipelineSources* sources,
    PipelineCallback done) {
    if (!async)
        return createPipeline(descriptor, error, shader_key, sources);
    if (enabled) {
        // Taking a prewarmed pipeline or loading an archived one is cheap
        const auto key = pipelineKey(descriptorFields(descriptor), shader_key);
        bool prewarmed_entry;
        {
            std::lock_guard lock(mutex);
            prewarmed_entry = prewarmed_pipelines.contains(key);
        }
        std::error_code file_error;
        if (prewarmed_entry ||
            std::filesystem::is_regular_file(path / (key + ".metallib"),
                                             file_error))
            return createPipeline(descriptor, error, shader_key, sources);
    }

    *error = nullptr;
    {
        std::lock_guard lock(mutex);
        async_jobs.push_back(
            {descriptor->retain(), std::string(shader_key),
             sources ? std::optional<PipelineSources>(*sources) : std::nullopt,
             std::move(done)});
    }
    async_condition.notify_one();
    return nullptr;
}

void BinaryArchiveCache::runAsync() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess().registerThisThread(
        "Pipeline compile");
    for (;;) {
        AsyncJob job;
        {
            std::unique_lock lock(mutex);
            async_condition.wait(
                lock, [this] { return async_stopping || !async_jobs.empty(); });
            if (async_stopping)
                break;
            job = std::move(async_jobs.front());
            async_jobs.pop_front();
        }
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        NS::Error* error = nullptr;
        auto* pipeline =
            createPipeline(job.descriptor, &error, job.shader_key,
                           job.sources ? &*job.sources : nullptr);
        job.descriptor->release();
        job.done(pipeline, error);
    }
    DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess()
        .unregisterThisThread();
}

void BinaryArchiveCache::startPrewarm(std::filesystem::path directory,
                                      ShaderLibraryCache& libraries) {
    std::vector<std::pair<std::filesystem::file_time_type,
                          std::filesystem::path>>
        files;
    {
        std::lock_guard lock(mutex);
        if (!enabled || !recipe_directory.empty())
            return;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            LOG_WARN(MetalRenderer, "Cannot write pipeline recipes: {}",
                     error.message());
            return;
        }
        recipe_directory = std::move(directory);

        const auto now = std::filesystem::file_time_type::clock::now();
        for (std::filesystem::directory_iterator it(recipe_directory, error),
             end;
             !error && it != end; it.increment(error)) {
            if (it->path().extension() != ".recipe" ||
                !it->is_regular_file(error))
                continue;
            const auto time = it->last_write_time(error);
            if (error)
                continue;
            if (now - time > MAX_UNUSED_AGE) {
                std::filesystem::remove(it->path(), error);
                continue;
            }
            recipe_keys.insert(it->path().stem().string());
            files.emplace_back(time, it->path());
        }
    }
    LOG_INFO(MetalRenderer, "Pipeline recipes: {} in {}", files.size(),
             recipe_directory.string());
    if (files.empty())
        return;

    // Most recently used first
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::filesystem::path> recipes;
    for (auto& [time, file] : files)
        recipes.push_back(std::move(file));
    prewarm(std::move(recipes), libraries);
}

MTL::RenderPipelineState*
BinaryArchiveCache::prewarmRecipe(const std::filesystem::path& recipe_path,
                                  ShaderLibraryCache& libraries) {
    const auto recipe = readRecipe(recipe_path);
    if (!recipe)
        return nullptr;
    const auto vertex_library =
        NS::TransferPtr(libraries.getStored(recipe->sources.vertex));
    if (!vertex_library)
        return nullptr;
    NS::SharedPtr<MTL::Library> fragment_library;
    if (!recipe->sources.fragment.empty()) {
        fragment_library =
            NS::TransferPtr(libraries.getStored(recipe->sources.fragment));
        if (!fragment_library)
            return nullptr;
    }

    const auto descriptor =
        NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
    const auto vertex =
        NS::TransferPtr(vertex_library->newFunction(toNsString("main_")));
    if (!vertex)
        return nullptr;
    descriptor->setVertexFunction(vertex.get());
    NS::SharedPtr<MTL::Function> fragment;
    if (fragment_library) {
        fragment = NS::TransferPtr(
            fragment_library->newFunction(toNsString("main_")));
        if (!fragment)
            return nullptr;
        descriptor->setFragmentFunction(fragment.get());
    }
    applyDescriptorFields(descriptor.get(), recipe->fields);
    // The rebuilt descriptor must be the recorded one
    const auto key =
        pipelineKey(descriptorFields(descriptor.get()), recipe->shader_key);
    if (key != recipe_path.stem().string())
        return nullptr;

    NS::Error* error = nullptr;
    bool archive_hit;
    return build(descriptor.get(), &error, path / (key + ".metallib"),
                 archive_hit);
}

void BinaryArchiveCache::prewarm(std::vector<std::filesystem::path> recipes,
                                 ShaderLibraryCache& libraries) {
    const auto count = std::clamp(std::thread::hardware_concurrency() / 4, 2u,
                                  4u);
    auto shared = std::make_shared<std::vector<std::filesystem::path>>(
        std::move(recipes));
    auto next = std::make_shared<std::atomic<usize>>(0);
    const auto start = std::chrono::steady_clock::now();
    std::lock_guard guard(mutex);
    prewarm_pending = count;
    for (u32 i = 0; i < count; i++) {
        prewarmers.emplace_back([this, shared, next, start, &libraries] {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
            DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess()
                .registerThisThread("Pipeline prewarm");
            for (;;) {
                const auto index = next->fetch_add(1);
                if (index >= shared->size())
                    break;
                const auto& recipe_path = (*shared)[index];
                const auto key = recipe_path.stem().string();
                auto building_entry = std::make_shared<Building>();
                {
                    std::lock_guard lock(mutex);
                    if (prewarm_stopping)
                        break;
                    if (created.contains(key) ||
                        prewarmed_pipelines.contains(key) ||
                        building.contains(key))
                        continue;
                    building.emplace(key, building_entry);
                }
                NS_STACK_SCOPED auto pool =
                    NS::AutoreleasePool::alloc()->init();
                auto* pipeline = prewarmRecipe(recipe_path, libraries);
                {
                    std::lock_guard lock(mutex);
                    building.erase(key);
                    building_entry->done = true;
                    if (pipeline) {
                        prewarmed_pipelines.emplace(key, pipeline);
                        prewarmed++;
                    }
                }
                built.notify_all();
            }
            std::unique_lock lock(mutex);
            if (--prewarm_pending == 0) {
                if (!prewarm_stopping)
                    LOG_INFO(MetalRenderer,
                             "Pipeline prewarm: {} of {} pipelines built in "
                             "{} ms",
                             prewarmed.load(), shared->size(),
                             std::chrono::duration_cast<
                                 std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count());
                condition.notify_all();
            }
            lock.unlock();
            DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess()
                .unregisterThisThread();
        });
    }
}

void BinaryArchiveCache::stopPrewarm() {
    {
        std::lock_guard lock(mutex);
        prewarm_stopping = true;
    }
    for (auto& prewarmer : prewarmers)
        prewarmer.join();
    prewarmers.clear();
}

void BinaryArchiveCache::flush() {
    std::unique_lock lock(mutex);
    condition.wait(lock, [this] {
        return pending.empty() && !busy && prewarm_pending == 0;
    });
}

void BinaryArchiveCache::trimArchives() {
    // Archives are never replaced in place: changed shaders or state get new
    // entries, and the old ones would accumulate forever
    std::vector<std::tuple<std::filesystem::file_time_type, u64,
                           std::filesystem::path>>
        archives;
    u64 total = 0;
    usize removed = 0;
    const auto now = std::filesystem::file_time_type::clock::now();
    std::error_code error;
    for (std::filesystem::directory_iterator it(path, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (it->path().extension() != ".metallib" ||
            !it->is_regular_file(entry_error))
            continue;
        const auto time = it->last_write_time(entry_error);
        const auto size = it->file_size(entry_error);
        if (entry_error)
            continue;
        if (now - time > MAX_UNUSED_AGE) {
            removed += std::filesystem::remove(it->path(), entry_error);
            continue;
        }
        archives.emplace_back(time, size, it->path());
        total += size;
    }
    std::sort(archives.begin(), archives.end());
    for (const auto& [time, size, archive] : archives) {
        if (total <= MAX_ARCHIVE_BYTES)
            break;
        std::error_code entry_error;
        if (std::filesystem::remove(archive, entry_error)) {
            total -= size;
            removed++;
        }
    }
    if (removed != 0)
        logWorker("Removed {} unused pipeline binaries, {} MiB remain",
                  removed, total / (1024 * 1024));
}

void BinaryArchiveCache::run() {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    trimArchives();

    u64 saved = 0;
    u64 failed = 0;

    u64 next_report = 1;
    for (;;) {
        std::vector<Pending> batch;
        {
            std::unique_lock lock(mutex);
            condition.wait(lock,
                           [this] { return stopping || !pending.empty(); });
            if (pending.empty() && stopping)
                break;
            batch.swap(pending);
            busy = true;
        }
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        for (auto& entry : batch) {
            std::error_code file_error;
            if (!entry.recipe_path.empty()) {
                if (entry.recipe.empty())
                    std::filesystem::last_write_time(
                        entry.recipe_path,
                        std::filesystem::file_time_type::clock::now(),
                        file_error);
                else
                    writeFile(entry.recipe_path, entry.recipe);
            }
            if (!entry.descriptor) {
                if (!entry.path.empty())
                    std::filesystem::last_write_time(
                        entry.path,
                        std::filesystem::file_time_type::clock::now(),
                        file_error);
                continue;
            }
            NS_STACK_SCOPED auto descriptor = entry.descriptor;
            NS_STACK_SCOPED auto archive = openArchive(device);
            NS::Error* error = nullptr;
            const auto temporary =
                fmt::format("{}.{}.{}.tmp", entry.path.string(), getpid(),
                            reinterpret_cast<uintptr_t>(this));
            bool success =
                archive &&
                archive->addRenderPipelineFunctions(descriptor, &error) &&
                archive->serializeToURL(archiveUrl(temporary), &error);
            if (success) {
                std::filesystem::rename(temporary, entry.path, file_error);
                success = !file_error;
            }
            if (success)
                saved++;
            else {
                failed++;
                if (failed <= 3)
                    logWorker("Cannot save shader cache entry: {}",
                              error
                                  ? error->localizedDescription()->utf8String()
                                  : file_error.message());
            }
            std::filesystem::remove(temporary, file_error);
        }
        if (saved >= next_report) {
            logWorker("Shader binary cache saved: {} entries, {} failures",
                      saved, failed);
            next_report = (saved / 128 + 1) * 128;
        }
        {
            std::lock_guard lock(mutex);
            busy = false;
        }
        condition.notify_all();
    }
    logWorker("Shader binary cache saved: {} entries, {} failures", saved,
              failed);
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
