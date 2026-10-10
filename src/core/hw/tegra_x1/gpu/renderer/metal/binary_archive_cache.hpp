#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

// Each pipeline has its own archive so a Metal serializer failure cannot
// poison the other entries. The worker saves misses without skipping draws.
class BinaryArchiveCache {
  public:
    explicit BinaryArchiveCache(MTL::Device* device,
                                std::filesystem::path path = {});
    ~BinaryArchiveCache();
    BinaryArchiveCache(const BinaryArchiveCache&) = delete;
    BinaryArchiveCache& operator=(const BinaryArchiveCache&) = delete;

    MTL::RenderPipelineState*
    createPipeline(MTL::RenderPipelineDescriptor* descriptor, NS::Error** error,
                   std::string_view shader_key);
    void flush();
    u64 getHits() const { return hits; }
    u64 getMisses() const { return misses; }
    static std::filesystem::path defaultPath(MTL::Device* device);

  private:
    void run();
    MTL::Device* device;
    std::filesystem::path path;
    bool enabled{false};
    struct Pending {
        MTL::RenderPipelineDescriptor* descriptor;
        std::filesystem::path path;
    };
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<Pending> pending;
    std::thread worker;
    bool stopping{false};
    bool busy{false};
    u64 hits{0};
    u64 misses{0};
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
