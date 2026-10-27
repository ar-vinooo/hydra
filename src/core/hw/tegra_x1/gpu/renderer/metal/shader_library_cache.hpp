#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

struct ShaderLibraryLimits {
    usize resident_libraries = 4096;
    // Sources average ~50 KiB
    usize disk_files = 8192;
    usize disk_bytes = 256ull * 1024 * 1024;
};

// Keeps the MSL sources of a title's shaders on disk and compiles them again
// in the background when the title starts, so draws find their libraries
// ready instead of compiling them. No draw is skipped: a draw whose library is
// still compiling waits for that compilation rather than starting another.
// This complements binary pipeline archives; it does not serialize MTLLibrary.
class ShaderLibraryCache {
  public:
    using Limits = ShaderLibraryLimits;

    // Without a directory the cache only lives in memory until setDirectory
    explicit ShaderLibraryCache(MTL::Device* device,
                                std::filesystem::path directory = {},
                                Limits limits = {});
    ~ShaderLibraryCache();
    // Persist sources to directory and prewarm the ones already stored there.
    // Only the first call has an effect.
    void setDirectory(std::filesystem::path directory);
    MTL::Library* get(std::string_view source, NS::Error** error);
    // Library of a source stored on disk under key, compiled if not resident
    MTL::Library* getStored(const std::string& key);
    // Key of a source given its SHA-256 hex digest
    static std::string keyFromDigest(std::string_view hex_digest);
    // Wait until queued saves and the prewarm finished
    void flush();
    usize getResidentCount();
    u64 getHits();
    u64 getPrewarmed();
    const Limits& getLimits() const { return limits; }
    static constexpr usize MAX_SOURCE_BYTES = 4 * 1024 * 1024;
    // Stored sources unused for this long are deleted
    static constexpr auto MAX_UNUSED_AGE = std::chrono::hours(24 * 30);

  private:
    struct Entry {
        MTL::Library* library;
        u64 used;
    };
    struct Save {
        std::string key;
        std::string source; // Empty to mark a stored source as used
    };
    struct Compilation {
        bool done{false};
        bool succeeded{false};
    };
    struct StoredFile {
        usize size;
        std::filesystem::file_time_type time;
    };
    // Who needs a library: a draw, the prewarm (which neither waits for other
    // compilations nor evicts), or a prewarmed pipeline
    enum class Use { Draw, Prewarm, Dependency };
    static constexpr usize MAX_QUEUED_SAVES = 4096;
    static constexpr usize MAX_QUEUED_BYTES = 256 * 1024 * 1024;
    MTL::Device* device;
    std::filesystem::path directory;
    const Limits limits;
    bool disabled{false};
    bool persistent{false};
    std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable compiled;
    std::map<std::string, Entry> libraries;
    std::map<std::string, std::shared_ptr<Compilation>> compiling;
    // Sources on disk, and the ones queued to be saved or marked as used
    // during this session
    std::set<std::string> stored_keys;
    std::set<std::string> used_keys;
    std::deque<Save> saves;
    usize queued_bytes{0};
    u64 clock{0};
    u64 hits{0};
    u64 prewarmed{0};
    bool stopping{false};
    bool busy{false};
    usize prewarm_pending{0};
    std::thread worker;
    std::vector<std::thread> prewarmers;
    MTL::Library* compile(std::string_view source, NS::Error** error);
    MTL::Library* obtain(const std::string& key, std::string_view source,
                         NS::Error** error, Use use);
    void insert(const std::string& key, MTL::Library* library);
    void queueSave(const std::string& key, std::string_view source);
    void run(std::map<std::filesystem::path, StoredFile> stored);
    void prewarm(std::vector<std::filesystem::path> files);

  public:
    // ABI-stable implementations selected by the live module.
    MTL::Library* compileHot0(std::string_view source, NS::Error** error);
    MTL::Library* getHot1(std::string_view source, NS::Error** error);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
