#pragma once

#include "core/hw/tegra_x1/gpu/const.hpp"
#include "core/video/decoder.hpp"
#include "core/video/syncpoints.hpp"
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
    u32 host1x_address = 0;
    // Creating process; null means unowned (tests). Guest addresses only
    // resolve in that process, so other processes cannot map the handle.
    const void* owner = nullptr;
    // TODO: alignment
    // TODO: kind
};

constexpr usize SUBCHANNEL_COUNT = 8;

inline thread_local GMmu* tls_crnt_gmmu = nullptr;
inline thread_local renderer::ICommandBuffer* tls_crnt_command_buffer = nullptr;

struct GpuChannelContext {
    explicit GpuChannelContext(Gpu& gpu);
    engines::ThreeD three_d;
    engines::Compute compute;
    engines::Inline inline_engine;
    engines::TwoD two_d;
    engines::Copy copy;
    std::optional<engines::EngineBase*> subchannels[SUBCHANNEL_COUNT] = {};
    u32 puller_regs[0x40] = {};
};

class Gpu {
  public:
    Gpu() noexcept;
    ~Gpu() noexcept { pfifo.shutdown(); }

    ZTD_MAKE_NON_COPYABLE(Gpu);
    ZTD_MAKE_NON_MOVABLE(Gpu);

    // Memory map
    std::expected<Handle, ztd::mem::IAllocator::Error> createMap(u64 size, const void* owner = nullptr) {
        std::scoped_lock lock(map_mutex);
        try {
            const auto raw = free_map_handles.empty() ? next_map_handle
                                                      : free_map_handles.back();
            if (raw == 0)
                return std::unexpected(
                    ztd::mem::IAllocator::Error::OutOfMemory);
            memory_maps.emplace(
                raw, MemoryMap{.addr = 0, .size = size, .write = false, .owner = owner});
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
        std::scoped_lock lock(map_mutex);
        auto memory_map = findMap(handle);
        if (!memory_map)
            return false;
        memory_map.value()->addr = addr;
        memory_map.value()->write = write;
        return true;
    }

    bool freeMap(Handle handle) {
        std::scoped_lock maps_lock(map_mutex);
        if (!memory_maps.contains(handle.getRaw()))
            return false;
        free_map_handles.push_back(handle.getRaw());
        const auto& map = memory_maps.at(handle.getRaw());
        {
            std::scoped_lock lock(video_frames_mutex);
            std::erase_if(video_frames, [&](const auto& item) {
                if (!map.host1x_address || item.first < map.host1x_address ||
                    item.first - map.host1x_address >= map.size)
                    return false;
                video_frame_bytes -= item.second->pixels.size();
                return true;
            });
        }
        memory_maps.erase(handle.getRaw());
        return true;
    }

    // The pointer stays valid until the handle is freed; table changes
    // themselves are serialized.
    std::optional<MemoryMap*> getMap(Handle handle) {
        std::scoped_lock lock(map_mutex);
        return findMap(handle);
    }

    // Host1x is a separate, 32-bit address space shared by NVDEC and VIC.
    // Keep an assignment stable until the nvmap is freed, including across
    // temporary unmaps while queued surfaces are still referenced.
    std::optional<u32> mapHost1x(Handle handle) {
        std::scoped_lock lock(map_mutex);
        auto map = findMap(handle);
        if (!map || !(*map)->addr || !(*map)->size)
            return std::nullopt;
        if (!(*map)->host1x_address) {
            const auto size = (*map)->size;
            if (size > 0xffff0000ull ||
                next_host1x_address + size > 0x100000000ull)
                return std::nullopt;
            (*map)->host1x_address = static_cast<u32>(next_host1x_address);
            next_host1x_address += (size + 0xffff) & ~0xffffull;
        }
        return (*map)->host1x_address;
    }

    std::optional<u64> resolveHost1x(u64 address, u64 size) const {
        std::scoped_lock lock(map_mutex);
        for (const auto& [handle, map] : memory_maps) {
            if (!map.host1x_address || address < map.host1x_address)
                continue;
            const auto offset = address - map.host1x_address;
            if (offset <= map.size && size <= map.size - offset &&
                map.addr + offset >= map.addr)
                return map.addr + offset;
        }
        return std::nullopt;
    }

    // VIC consumes the decoded picture selected by its luma surface address.
    // Retain owned pictures across temporary unmaps; bound storage to 64 MiB.
    void publishVideoFrame(u64 address, const video::Frame& frame) {
        std::scoped_lock lock(video_frames_mutex);
        if (const auto previous = video_frames.find(address); previous != video_frames.end()) {
            video_frame_bytes -= previous->second->pixels.size();
            video_frames.erase(previous);
        }
        while (!video_frames.empty() && video_frame_bytes + frame.pixels.size() > 64 * 1024 * 1024) {
            video_frame_bytes -= video_frames.begin()->second->pixels.size();
            video_frames.erase(video_frames.begin());
        }
        if (frame.pixels.size() > 64 * 1024 * 1024) return;
        video_frames.emplace(address, std::make_shared<const video::Frame>(frame));
        video_frame_bytes += frame.pixels.size();
    }
    std::shared_ptr<const video::Frame> getVideoFrame(u64 address) {
        std::scoped_lock lock(video_frames_mutex);
        const auto found = video_frames.find(address);
        return found == video_frames.end() ? nullptr : found->second;
    }

    video::Syncpoints& getVideoSyncpoints() { return *video_syncpoints; }
    std::shared_ptr<video::Syncpoints> shareVideoSyncpoints() { return video_syncpoints; }

    // GPU fences. A submission that asks for a fence gets a syncpoint value
    // the host reaches when the Metal command buffer of the submission
    // completed. On by default; HYDRA_GPU_FENCES=0 turns them off (every fence
    // then reads as reached at once, while PFIFO still runs the work later).
    bool reserveFence(Fence& out_fence, PfifoDecodeState* channel = nullptr,
                      u32 increments = 1);
    // Reaches the fence of a submission
    std::function<void()> makeFenceSignal(const Fence& fence) const {
        return [syncpoints = video_syncpoints, id = fence.id] {
            syncpoints->complete(id, 1);
        };
    }
    bool isFenceSyncpoint(u32 id) {
        std::scoped_lock lock(fence_registry->mutex);
        return fence_registry->ids.contains(id);
    }

    // Engines
    std::optional<engines::EngineBase*> getEngineAtSubchannel(u32 subchannel) {
        auto& subchannels = channelContext().subchannels;
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
    engines::ThreeD& get3DEngine() { return channelContext().three_d; }
    GpuChannelContext& channelContext() {
        return tls_crnt_decode_state && tls_crnt_decode_state->context
            ? *tls_crnt_decode_state->context : default_channel;
    }
    renderer::IRenderer& getRenderer() const { return *renderer; }

  private:
    // Pfifo
    Pfifo pfifo;

    // Engines
    GpuChannelContext default_channel;

    // Renderer
    std::unique_ptr<renderer::IRenderer> renderer;

    // Memory
    // TODO: move to nvmap
    // Stable addresses while the table grows; no fixed 2048-map limit.
    mutable std::mutex map_mutex;
    std::map<u32, MemoryMap> memory_maps;
    std::optional<MemoryMap*> findMap(Handle handle) {
        auto it = memory_maps.find(handle.getRaw());
        if (it == memory_maps.end())
            return std::nullopt;
        return &it->second;
    }
    std::vector<u32> free_map_handles;
    u32 next_map_handle = 1;
    u64 next_host1x_address = 0x10000;
    std::mutex video_frames_mutex;
    std::map<u64, std::shared_ptr<const video::Frame>> video_frames;
    usize video_frame_bytes = 0;
    std::shared_ptr<video::Syncpoints> video_syncpoints =
        std::make_shared<video::Syncpoints>();
    std::mutex fence_mutex;
    u32 fence_syncpoint{0};
    u32 fence_target{0};
    // Shared with channel states, which release their counter when destroyed
    struct FenceRegistry {
        std::mutex mutex;
        std::set<u32> ids;
    };
    std::shared_ptr<FenceRegistry> fence_registry = std::make_shared<FenceRegistry>();

    // Puller (host) methods of the channel: semaphores and syncpoints
    void pullerMethod(u32 method, u32 arg);
};

} // namespace hydra::hw::tegra_x1::gpu
