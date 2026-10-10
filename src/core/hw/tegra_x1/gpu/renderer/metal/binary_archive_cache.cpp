#include "core/hw/tegra_x1/gpu/renderer/metal/binary_archive_cache.hpp"

#include "common/config.hpp"
#include <CommonCrypto/CommonDigest.h>
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

// Encode fields explicitly; hashing descriptor pointers or C++ struct padding
// would not give a stable cache key between runs.
std::string pipelineKey(MTL::RenderPipelineDescriptor* descriptor,
                        std::string_view shader_key) {
    std::string bytes(shader_key);
    bytes.push_back('\0');
    auto add = [&bytes](auto value) {
        const u64 canonical = static_cast<u64>(value);
        bytes.append(reinterpret_cast<const char*>(&canonical),
                     sizeof(canonical));
    };
    for (u32 i = 0; i < 31; i++) {
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
    return hexDigest(bytes.data(), bytes.size());
}
} // namespace

std::filesystem::path BinaryArchiveCache::defaultPath(MTL::Device* device) {
    // Source/options and pipeline state identify entries; GPU and OS/driver
    // identify the directory. Metal validates the actual binary on every hit.
    const auto* version = NS::ProcessInfo::processInfo()
                              ->operatingSystemVersionString()
                              ->utf8String();
    return std::filesystem::path(CONFIG_INSTANCE.getAppDataPath()) / "cache" /
           "metal" /
           fmt::format("v2-{:016x}-{}", device->registryID(),
                       hexDigest(version, std::strlen(version)));
}

BinaryArchiveCache::BinaryArchiveCache(MTL::Device* device_,
                                       std::filesystem::path path_)
    : device(device_->retain()),
      path(path_.empty() ? defaultPath(device_) : std::move(path_)) {
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
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    condition.notify_all();
    if (worker.joinable())
        worker.join();
    logWorker("Shader binary cache: {} hits, {} misses", hits, misses);
    device->release();
}

MTL::RenderPipelineState*
BinaryArchiveCache::createPipeline(MTL::RenderPipelineDescriptor* descriptor,
                                   NS::Error** error,
                                   std::string_view shader_key) {
    *error = nullptr;
    std::filesystem::path entry;
    if (enabled) {
        entry = path / (pipelineKey(descriptor, shader_key) + ".metallib");
        std::error_code file_error;
        if (std::filesystem::is_regular_file(entry, file_error)) {
            NS_STACK_SCOPED auto archive = openArchive(device, entry);
            if (archive) {
                NS::Object* archives[] = {archive};
                descriptor->setBinaryArchives(NS::Array::array(archives, 1));
                auto pipeline = device->newRenderPipelineState(
                    descriptor, MTL::PipelineOptionFailOnBinaryArchiveMiss,
                    nullptr, error);
                descriptor->setBinaryArchives(nullptr);
                if (pipeline) {
                    hits++;
                    if (hits == 1 || hits % 128 == 0)
                        LOG_INFO(MetalRenderer,
                                 "Shader binary cache: {} hits, {} misses",
                                 hits, misses);
                    return pipeline;
                }
            }
            // Invalid, incompatible, or missing binary: render normally and
            // replace just this entry. Other entries remain usable.
            *error = nullptr;
        }
    }
    misses++;
    auto pipeline = device->newRenderPipelineState(descriptor, error);
    if (pipeline && enabled) {
        auto copy = descriptor->copy();
        {
            std::lock_guard lock(mutex);
            pending.push_back({copy, std::move(entry)});
        }
        condition.notify_all();
    }
    return pipeline;
}

void BinaryArchiveCache::flush() {
    std::unique_lock lock(mutex);
    condition.wait(lock, [this] { return pending.empty() && !busy; });
}

void BinaryArchiveCache::run() {
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
            std::error_code file_error;
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
