#pragma once

#include "core/hw/tegra_x1/gpu/const.hpp"
#include "core/hw/tegra_x1/gpu/engines/2d.hpp"
#include "core/hw/tegra_x1/gpu/engines/3d.hpp"
#include "core/hw/tegra_x1/gpu/engines/compute.hpp"
#include "core/hw/tegra_x1/gpu/engines/copy.hpp"
#include "core/hw/tegra_x1/gpu/engines/inline.hpp"
#include "core/hw/tegra_x1/gpu/pfifo.hpp"
#include "core/hw/tegra_x1/gpu/renderer/renderer.hpp"

// TODO: remove dependency
#include "core/horizon/handle_pool.hpp"

namespace hydra::hw::tegra_x1::cpu {
class IMmu;
}

namespace hydra::hw::tegra_x1::gpu {

namespace renderer {
class ICommandBuffer;
}

struct MemoryMap {
    uptr addr = 0;
    u64 size;
    bool write;
    // TODO: alignment
    // TODO: kind
};

constexpr usize SUBCHANNEL_COUNT = 8;

inline thread_local GMmu* tls_crnt_gmmu = nullptr;
inline thread_local renderer::ICommandBuffer* tls_crnt_command_buffer = nullptr;

class Gpu {
  public:
    Gpu() noexcept;
    ~Gpu() noexcept = default;

    ZTD_MAKE_NON_COPYABLE(Gpu);
    ZTD_MAKE_NON_MOVABLE(Gpu);

    // Memory map
    std::expected<Handle, ztd::mem::IAllocator::Error> createMap(u64 size) {
        try {
            const auto raw = free_map_handles.empty() ? next_map_handle
                                                      : free_map_handles.back();
            if (raw == 0)
                return std::unexpected(
                    ztd::mem::IAllocator::Error::OutOfMemory);
            memory_maps.emplace(
                raw, MemoryMap{.addr = 0, .size = size, .write = false});
            if (free_map_handles.empty())
                ++next_map_handle;
            else
                free_map_handles.pop_back();
            return Handle(raw);
        } catch (const std::bad_alloc&) {
            return std::unexpected(ztd::mem::IAllocator::Error::OutOfMemory);
        }
    }

    bool allocateMap(Handle handle, uptr addr, bool write) {
        auto memory_map = getMap(handle);
        if (!memory_map)
            return false;
        memory_map.value()->addr = addr;
        memory_map.value()->write = write;
        return true;
    }

    bool freeMap(Handle handle) {
        if (!memory_maps.contains(handle.getRaw()))
            return false;
        free_map_handles.push_back(handle.getRaw());
        memory_maps.erase(handle.getRaw());
        return true;
    }

    std::optional<MemoryMap*> getMap(Handle handle) {
        auto it = memory_maps.find(handle.getRaw());
        if (it == memory_maps.end())
            return std::nullopt;
        return &it->second;
    }

    // Engines
    std::optional<engines::EngineBase*> getEngineAtSubchannel(u32 subchannel) {
        if (subchannel >= SUBCHANNEL_COUNT || !subchannels[subchannel] ||
            *subchannels[subchannel] == nullptr)
            return std::nullopt;
        return subchannels[subchannel];
    }

    void subchannelMethod(u32 subchannel, u32 method, u32 arg);

    void subchannelFlushMacro(u32 subchannel) {
        const auto engine = getEngineAtSubchannel(subchannel);
        if (!engine)
            return;

        (*engine)->flushMacro();
    }

    // Texture
    renderer::ITextureView* getTexture(renderer::ICommandBuffer* command_buffer,
                                       cpu::IMmu* mmu,
                                       const NvGraphicsBuffer& buff);

    // Getters
    Pfifo& getPfifo() { return pfifo; }
    renderer::IRenderer& getRenderer() const { return *renderer; }

  private:
    // Pfifo
    Pfifo pfifo;

    // Engines
    engines::ThreeD three_d_engine;
    engines::Compute compute_engine;
    engines::Inline inline_engine;
    engines::TwoD two_d_engine;
    engines::Copy copy_engine;
    std::optional<engines::EngineBase*> subchannels[SUBCHANNEL_COUNT] = {};

    // Renderer
    std::unique_ptr<renderer::IRenderer> renderer;

    // Memory
    // TODO: move to nvmap
    // Stable addresses while the table grows; no fixed 2048-map limit.
    std::map<u32, MemoryMap> memory_maps;
    std::vector<u32> free_map_handles;
    u32 next_map_handle = 1;
};

} // namespace hydra::hw::tegra_x1::gpu
