#include "core/hw/tegra_x1/gpu/renderer/metal/shader_library_cache.hpp"
#include "core/debugger/debugger_manager.hpp"
#include "core/hot/table.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <optional>
#include <pthread.h>
#include <unistd.h>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {
namespace {
std::string sourceKey(std::string_view source) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(source.data(), static_cast<CC_LONG>(source.size()), digest);
    std::string hex;
    for (auto byte : digest)
        hex += fmt::format("{:02x}", byte);
    return ShaderLibraryCache::keyFromDigest(hex);
}

// Live revisions are local to this process: their sources are not stored
bool isLiveRevisionKey(std::string_view key) {
    return key.find("-hot-") != std::string_view::npos;
}

std::optional<std::string> readSource(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > ShaderLibraryCache::MAX_SOURCE_BYTES)
        return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    std::string source((std::istreambuf_iterator<char>(input)), {});
    if (!input.good() && !input.eof())
        return std::nullopt;
    return source;
}
} // namespace

std::string ShaderLibraryCache::keyFromDigest(std::string_view hex_digest) {
    std::string key(hex_digest);
    // Compiler-option changes must not hit libraries from an older image.
    if (const auto revision = ::hydra::hot::generation(); revision != 0)
        key += fmt::format("-hot-{}-{}", getpid(), revision);
    return key;
}

ShaderLibraryCache::ShaderLibraryCache(MTL::Device* device_,
                                       std::filesystem::path path,
                                       Limits limits_)
    : device(device_->retain()), limits{limits_} {
    const char* disable = std::getenv("HYDRA_DISABLE_SHADER_CACHE");
    disabled = disable && std::string_view(disable) == "1";
    if (!path.empty())
        setDirectory(std::move(path));
}

ShaderLibraryCache::~ShaderLibraryCache() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    condition.notify_all();
    for (auto& prewarmer : prewarmers)
        prewarmer.join();
    if (worker.joinable())
        worker.join();
    for (auto& [key, entry] : libraries)
        entry.library->release();
    device->release();
}

void ShaderLibraryCache::setDirectory(std::filesystem::path path) {
    std::lock_guard lock(mutex);
    if (disabled || persistent)
        return;
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        LOG_WARN(MetalRenderer, "Cannot write shader source cache: {}",
                 error.message());
        return;
    }
    directory = std::move(path);
    persistent = true;

    // Forget sources that were not used for a long time (e.g. from an older
    // decompiler) and prewarm the most recently used ones first
    const auto now = std::filesystem::file_time_type::clock::now();
    std::vector<std::pair<std::filesystem::file_time_type,
                          std::filesystem::path>>
        files;
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
        if (it->path().extension() != ".metal" || !it->is_regular_file(error))
            continue;
        const auto time = it->last_write_time(error);
        if (error)
            continue;
        if (now - time > MAX_UNUSED_AGE) {
            std::filesystem::remove(it->path(), error);
            continue;
        }
        files.emplace_back(time, it->path());
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::map<std::filesystem::path, StoredFile> stored;
    std::vector<std::filesystem::path> order;
    for (const auto& [time, file] : files) {
        stored_keys.insert(file.stem().string());
        stored.emplace(file,
                       StoredFile{std::filesystem::file_size(file, error), time});
        order.push_back(file);
    }
    LOG_INFO(MetalRenderer, "Shader source cache: {} sources in {}",
             order.size(), directory.string());

    busy = true;
    worker = std::thread(
        [this, stored = std::move(stored)]() mutable { run(std::move(stored)); });
    if (!order.empty())
        prewarm(std::move(order));
}

MTL::Library* ShaderLibraryCache::compileHot0(std::string_view source,
                                              NS::Error** error) {
    NS_STACK_SCOPED auto options = MTL::CompileOptions::alloc()->init();
    options->setPreserveInvariance(true);
    options->setFastMathEnabled(false);
    return device->newLibrary(toNsString(std::string(source)), options, error);
}

void ShaderLibraryCache::insert(const std::string& key, MTL::Library* library) {
    if (libraries.contains(key))
        return;
    if (libraries.size() >= limits.resident_libraries) {
        auto oldest = std::min_element(
            libraries.begin(), libraries.end(),
            [](auto& a, auto& b) { return a.second.used < b.second.used; });
        oldest->second.library->release();
        libraries.erase(oldest);
    }
    libraries.emplace(key, Entry{library->retain(), ++clock});
}

MTL::Library* ShaderLibraryCache::obtain(const std::string& key,
                                         std::string_view source,
                                         NS::Error** error, Use use) {
    const bool prewarm = use == Use::Prewarm;
    if (disabled)
        return compile(source, error);

    std::shared_ptr<Compilation> compilation;
    {
        std::unique_lock lock(mutex);
        for (;;) {
            if (auto it = libraries.find(key); it != libraries.end()) {
                it->second.used = ++clock;
                if (use == Use::Draw) {
                    ++hits;
                    if (PerfStats::isEnabled())
                        PerfStats::instance().shader_library_hits++;
                }
                return it->second.library->retain();
            }
            // Another thread (usually the prewarm) compiles this source: wait
            // for it instead of compiling it twice
            const auto it = compiling.find(key);
            if (it == compiling.end())
                break;
            if (prewarm)
                return nullptr;
            const auto other = it->second;
            compiled.wait(lock, [&] { return other->done; });
            // A failed compilation is retried below to report its error
            if (!other->succeeded)
                break;
        }
        // Prewarming stops once the resident set is full, so it never evicts
        // libraries that are in use
        if (prewarm && libraries.size() >= limits.resident_libraries)
            return nullptr;
        compilation = std::make_shared<Compilation>();
        compiling.emplace(key, compilation);
    }

    std::optional<ScopedPerfTimer> timer;
    if (use == Use::Draw && PerfStats::isEnabled()) {
        timer.emplace(PerfStats::instance().shader_compile_ns);
        PerfStats::instance().shader_compiles++;
    }
    auto* library = compile(source, error);
    timer.reset();
    {
        std::lock_guard lock(mutex);
        if (library) {
            insert(key, library);
            if (use != Use::Draw)
                ++prewarmed;
        }
        compilation->done = true;
        compilation->succeeded = library != nullptr;
        compiling.erase(key);
    }
    compiled.notify_all();
    return library;
}

void ShaderLibraryCache::queueSave(const std::string& key,
                                   std::string_view source) {
    if (!persistent || isLiveRevisionKey(key) || used_keys.contains(key))
        return;
    // A stored source only needs to be marked as used
    const bool stored = stored_keys.contains(key);
    if (!stored && source.size() > MAX_SOURCE_BYTES)
        return;
    if (saves.size() >= MAX_QUEUED_SAVES ||
        (!stored && queued_bytes + source.size() > MAX_QUEUED_BYTES))
        return;
    used_keys.insert(key);
    if (stored) {
        saves.push_back({key, {}});
    } else {
        saves.push_back({key, std::string(source)});
        queued_bytes += source.size();
    }
    condition.notify_all();
}

MTL::Library* ShaderLibraryCache::getHot1(std::string_view source,
                                          NS::Error** error) {
    const auto key = sourceKey(source);
    auto* library = obtain(key, source, error, Use::Draw);
    if (library && !disabled) {
        std::lock_guard lock(mutex);
        queueSave(key, source);
    }
    return library;
}

MTL::Library* ShaderLibraryCache::getStored(const std::string& key) {
    {
        std::lock_guard lock(mutex);
        if (disabled || !persistent)
            return nullptr;
        if (auto it = libraries.find(key); it != libraries.end()) {
            it->second.used = ++clock;
            return it->second.library->retain();
        }
    }
    const auto source = readSource(directory / (key + ".metal"));
    if (!source || sourceKey(*source) != key)
        return nullptr;
    NS::Error* error = nullptr;
    return obtain(key, *source, &error, Use::Dependency);
}

void ShaderLibraryCache::prewarm(std::vector<std::filesystem::path> files) {
    // The compiler service runs compilations of different threads in
    // parallel. Leave most cores to the guest, which is booting meanwhile.
    const auto count = std::clamp(std::thread::hardware_concurrency() / 4, 2u,
                                  4u);
    auto shared = std::make_shared<std::vector<std::filesystem::path>>(
        std::move(files));
    auto next = std::make_shared<std::atomic<usize>>(0);
    auto failed = std::make_shared<std::atomic<u64>>(0);
    const auto start = std::chrono::steady_clock::now();
    prewarm_pending = count;
    for (u32 i = 0; i < count; i++) {
        prewarmers.emplace_back([this, shared, next, failed, start] {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
            DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess()
                .registerThisThread("Shader prewarm");
            for (;;) {
                ::hydra::hot::Scope hot_scope;
                {
                    std::lock_guard lock(mutex);
                    if (stopping ||
                        libraries.size() >= limits.resident_libraries)
                        break;
                }
                const auto index = next->fetch_add(1);
                if (index >= shared->size())
                    break;
                const auto& file = (*shared)[index];
                const auto source = readSource(file);
                const auto key = file.stem().string();
                if (!source || sourceKey(*source) != key)
                    continue;
                NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
                NS::Error* error = nullptr;
                if (auto* library = obtain(key, *source, &error, Use::Prewarm))
                    library->release();
                else if (error)
                    (*failed)++;
            }
            std::unique_lock lock(mutex);
            if (--prewarm_pending == 0) {
                if (!stopping)
                    LOG_INFO(MetalRenderer,
                             "Shader prewarm: {} of {} libraries compiled in "
                             "{} ms ({} failed)",
                             prewarmed, shared->size(),
                             std::chrono::duration_cast<
                                 std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count(),
                             failed->load());
                condition.notify_all();
            }
            lock.unlock();
            DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess()
                .unregisterThisThread();
        });
    }
}

void ShaderLibraryCache::run(std::map<std::filesystem::path, StoredFile> stored) {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    usize disk_bytes = 0;
    for (const auto& [path, file] : stored)
        disk_bytes += file.size;
    std::unique_lock lock(mutex);
    while (true) {
        busy = false;
        condition.notify_all();
        condition.wait(lock, [this] { return stopping || !saves.empty(); });
        if (saves.empty() && stopping)
            break;
        auto save = std::move(saves.front());
        saves.pop_front();
        queued_bytes -= save.source.size();
        busy = true;
        lock.unlock();
        std::error_code error;
        const auto path = directory / (save.key + ".metal");
        const auto now = std::filesystem::file_time_type::clock::now();
        bool written = false;
        if (save.source.empty()) {
            std::filesystem::last_write_time(path, now, error);
            if (auto it = stored.find(path); it != stored.end())
                it->second.time = now;
        } else {
            // Evict the least recently used sources
            while (!stored.empty() &&
                   (stored.size() >= limits.disk_files ||
                    disk_bytes + save.source.size() > limits.disk_bytes)) {
                auto oldest = std::min_element(
                    stored.begin(), stored.end(), [](const auto& a, const auto& b) {
                        return a.second.time < b.second.time;
                    });
                if (!std::filesystem::remove(oldest->first, error) || error)
                    break;
                disk_bytes -= oldest->second.size;
                stored.erase(oldest);
            }
            if (stored.size() < limits.disk_files &&
                disk_bytes + save.source.size() <= limits.disk_bytes) {
                std::ofstream output(path.string() + ".tmp",
                                     std::ios::binary | std::ios::trunc);
                output.write(save.source.data(), save.source.size());
                output.close();
                if (output) {
                    std::filesystem::rename(path.string() + ".tmp", path, error);
                    if (!error) {
                        disk_bytes += save.source.size();
                        stored[path] = StoredFile{save.source.size(), now};
                        written = true;
                    }
                }
            }
        }
        lock.lock();
        if (written)
            stored_keys.insert(save.key);
    }
}

void ShaderLibraryCache::flush() {
    std::unique_lock lock(mutex);
    condition.wait(lock, [this] {
        return !busy && saves.empty() && prewarm_pending == 0;
    });
}

usize ShaderLibraryCache::getResidentCount() {
    std::lock_guard lock(mutex);
    return libraries.size();
}

u64 ShaderLibraryCache::getHits() {
    std::lock_guard lock(mutex);
    return hits;
}

u64 ShaderLibraryCache::getPrewarmed() {
    std::lock_guard lock(mutex);
    return prewarmed;
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
