#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

// Precompile a bounded set of previously seen sources without skipping draws.
// This complements binary pipeline archives; it does not serialize MTLLibrary.
class ShaderLibraryCache {
  public:
    explicit ShaderLibraryCache(MTL::Device* device,
                                std::filesystem::path directory = {});
    ~ShaderLibraryCache();
    MTL::Library* get(std::string_view source, NS::Error** error);
    void flush();
    usize getResidentCount();
    u64 getHits();
    static constexpr usize MAX_LIBRARIES = 64;
    static constexpr usize MAX_SOURCE_BYTES = 1024 * 1024;

  private:
    struct Entry {
        MTL::Library* library;
        u64 used;
    };
    struct Save {
        std::string key;
        std::string source;
    };
    MTL::Device* device;
    std::filesystem::path directory;
    bool enabled{false};
    std::mutex mutex;
    std::condition_variable condition;
    std::map<std::string, Entry> libraries;
    std::deque<Save> saves;
    usize queued_bytes{0};
    u64 clock{0};
    u64 hits{0};
    bool stopping{false};
    bool busy{true};
    std::thread worker;
    MTL::Library* compile(std::string_view source, NS::Error** error);
    void insert(const std::string& key, MTL::Library* library);
    void run();
    void prewarm();
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
