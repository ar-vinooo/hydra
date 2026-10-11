#include "core/hw/tegra_x1/gpu/renderer/metal/shader_library_cache.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/binary_archive_cache.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <fstream>
#include <pthread.h>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {
namespace {
std::string sourceKey(std::string_view source) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(source.data(), static_cast<CC_LONG>(source.size()), digest);
    std::string key;
    for (auto byte : digest)
        key += fmt::format("{:02x}", byte);
    return key;
}
} // namespace
ShaderLibraryCache::ShaderLibraryCache(MTL::Device* device_,
                                       std::filesystem::path path)
    : device(device_->retain()),
      directory(path.empty() ? BinaryArchiveCache::defaultPath(device_) /
                                   "msl-invariant-v1"
                             : std::move(path)) {
    const char* disabled = std::getenv("HYDRA_DISABLE_SHADER_CACHE");
    enabled = !(disabled && std::string_view(disabled) == "1");
    std::error_code error;
    if (enabled) {
        std::filesystem::create_directories(directory, error);
        enabled = !error;
    }
    if (enabled)
        worker = std::thread([this] { run(); });
    else
        busy = false;
}
ShaderLibraryCache::~ShaderLibraryCache() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    condition.notify_all();
    if (worker.joinable())
        worker.join();
    for (auto& [key, entry] : libraries)
        entry.library->release();
    device->release();
}
MTL::Library* ShaderLibraryCache::compile(std::string_view source,
                                          NS::Error** error) {
    NS_STACK_SCOPED auto options = MTL::CompileOptions::alloc()->init();
    options->setPreserveInvariance(true);
    // Keep the existing default fastMath setting and shader semantics.
    return device->newLibrary(toNsString(std::string(source)), options, error);
}
void ShaderLibraryCache::insert(const std::string& key, MTL::Library* library) {
    if (libraries.contains(key))
        return;
    if (libraries.size() >= MAX_LIBRARIES) {
        auto oldest = std::min_element(
            libraries.begin(), libraries.end(),
            [](auto& a, auto& b) { return a.second.used < b.second.used; });
        oldest->second.library->release();
        libraries.erase(oldest);
    }
    libraries.emplace(key, Entry{library->retain(), ++clock});
}
MTL::Library* ShaderLibraryCache::get(std::string_view source,
                                      NS::Error** error) {
    const auto key = sourceKey(source);
    if (enabled) {
        std::lock_guard lock(mutex);
        if (auto it = libraries.find(key); it != libraries.end()) {
            it->second.used = ++clock;
            ++hits;
            if (PerfStats::isEnabled())
                PerfStats::instance().shader_library_hits++;
            if (saves.size() < 128) {
                saves.push_back(
                    {key, {}}); // Refresh disk LRU off the draw thread.
                condition.notify_all();
            }
            return it->second.library->retain();
        }
    }
    std::optional<ScopedPerfTimer> timer;
    if (PerfStats::isEnabled()) {
        timer.emplace(PerfStats::instance().shader_compile_ns);
        PerfStats::instance().shader_compiles++;
    }
    auto* library = compile(source, error);
    if (library && enabled) {
        std::lock_guard lock(mutex);
        insert(key, library);
        if (source.size() <= MAX_SOURCE_BYTES && saves.size() < 128 &&
            queued_bytes + source.size() <= 16 * MAX_SOURCE_BYTES) {
            saves.push_back({key, std::string(source)});
            queued_bytes += source.size();
            condition.notify_all();
        }
    }
    return library;
}
void ShaderLibraryCache::prewarm() {
    std::error_code error;
    std::vector<std::filesystem::directory_entry> files;
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
        if (it->path().extension() == ".metal" && it->is_regular_file(error))
            files.push_back(*it);
    }
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) {
        std::error_code ea, eb;
        return a.last_write_time(ea) > b.last_write_time(eb);
    });
    usize compiled = 0;
    for (auto& file : files) {
        {
            std::lock_guard lock(mutex);
            if (stopping || compiled >= MAX_LIBRARIES)
                break;
        }
        const auto size = file.file_size(error);
        if (error || size > MAX_SOURCE_BYTES)
            continue;
        std::ifstream input(file.path(), std::ios::binary);
        std::string source((std::istreambuf_iterator<char>(input)), {});
        const auto key = sourceKey(source);
        if (key != file.path().stem().string())
            continue;
        {
            std::lock_guard lock(mutex);
            if (libraries.contains(key))
                continue;
        }
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        NS::Error* compile_error = nullptr;
        auto* library = compile(source, &compile_error);
        if (!library)
            continue;
        {
            std::lock_guard lock(mutex);
            insert(key, library);
        }
        library->release();
        ++compiled;
    }
}
void ShaderLibraryCache::run() {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    prewarm();
    std::unique_lock lock(mutex);
    // Limit source files to 128 entries / 64 MiB per GPU/OS cache directory.
    usize file_count = 0, disk_bytes = 0;
    std::map<std::filesystem::path, usize> stored;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
        if (it->path().extension() == ".metal" && it->is_regular_file(error)) {
            ++file_count;
            const auto size = it->file_size(error);
            stored.emplace(it->path(), size);
            disk_bytes += size;
        }
    }
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
        const auto path = directory / (save.key + ".metal");
        if (std::filesystem::exists(path, error)) {
            std::filesystem::last_write_time(
                path, std::filesystem::file_time_type::clock::now(), error);
        } else if (!save.source.empty()) {
            while (!stored.empty() &&
                   (file_count >= 128 ||
                    disk_bytes + save.source.size() > 64 * MAX_SOURCE_BYTES)) {
                auto oldest = std::min_element(
                    stored.begin(), stored.end(),
                    [](const auto& a, const auto& b) {
                        std::error_code ea, eb;
                        return std::filesystem::last_write_time(a.first, ea) <
                               std::filesystem::last_write_time(b.first, eb);
                    });
                if (!std::filesystem::remove(oldest->first, error) || error)
                    break;
                --file_count;
                disk_bytes -= oldest->second;
                stored.erase(oldest);
            }
            if (file_count >= 128 ||
                disk_bytes + save.source.size() > 64 * MAX_SOURCE_BYTES) {
                lock.lock();
                continue;
            }
            std::ofstream output(path.string() + ".tmp",
                                 std::ios::binary | std::ios::trunc);
            output.write(save.source.data(), save.source.size());
            output.close();
            if (output) {
                std::filesystem::rename(path.string() + ".tmp", path, error);
                if (!error) {
                    ++file_count;
                    disk_bytes += save.source.size();
                    stored.emplace(path, save.source.size());
                }
            }
        }
        lock.lock();
    }
}
void ShaderLibraryCache::flush() {
    std::unique_lock lock(mutex);
    condition.wait(lock, [this] { return !busy && saves.empty(); });
}
usize ShaderLibraryCache::getResidentCount() {
    std::lock_guard lock(mutex);
    return libraries.size();
}
u64 ShaderLibraryCache::getHits() {
    std::lock_guard lock(mutex);
    return hits;
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
