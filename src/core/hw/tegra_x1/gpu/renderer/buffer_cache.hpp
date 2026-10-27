#pragma once

#include "core/hw/tegra_x1/gpu/renderer/buffer_view.hpp"

namespace hydra::hw::tegra_x1::cpu {
class IMmu;
}

namespace hydra::hw::tegra_x1::gpu::renderer {

class IRenderer;

// Buffers up to this size are CPU snapshots of guest memory: an update
// replaces the snapshot instead of copying on the GPU, which would end the
// open render pass (storing and reloading its attachments). Unified memory
// makes the copy cheap.
constexpr u64 BUFFER_SNAPSHOT_MAX_SIZE = 1024 * 1024;

struct BufferEntry {
    std::shared_ptr<BufferBase> buffer;
    ztd::Range<uptr> range;
    std::optional<ztd::Range<uptr>> invalidation_range;
    bool inline_copy{false}; // TODO: implement
};

// TODO: optional data hashing
class BufferCache {
  public:
    explicit BufferCache(IRenderer& renderer_) : renderer{renderer_} {}
    ~BufferCache();

    u64 getCachedBytes() const {
        u64 bytes = 0;
        for (const auto& [key, entry] : entries)
            if (entry.buffer)
                bytes += entry.buffer->getSize();
        return bytes;
    }

    BufferView get(ICommandBuffer* command_buffer, ztd::Range<uptr> range);

    // Owned CPU data has no guest write tracking or stable cache address.
    BufferView snapshot(ztd::Range<uptr> range);

    void invalidateMemory(ztd::Range<uptr> range);

    // Ranges a shader writes through storage buffers. Their contents live in
    // guest memory and change on the GPU timeline, so later users of them get
    // a buffer aliasing that memory instead of a CPU snapshot
    void markGpuModified(ztd::Range<uptr> range);

  private:
    // Disjoint intervals, begin -> end
    std::map<uptr, uptr> gpu_modified;
    bool intersectsGpuModified(ztd::Range<uptr> range) const;

    IRenderer& renderer;

    std::mutex mutex;
    std::map<uptr, BufferEntry> entries;

    // Helpers
    void updateRange(ICommandBuffer* command_buffer, BufferEntry& entry,
                     ztd::Range<uptr> range);
    BufferEntry& find(ztd::Range<uptr> range);

  public:
    REF_GETTER(mutex, getMutex);

  public:
    // ABI-stable implementations selected by the live module.
    BufferView getHot0(ICommandBuffer* command_buffer, ztd::Range<uptr> range);
    void invalidateMemoryHot1(ztd::Range<uptr> range);
    void updateRangeHot2(ICommandBuffer* command_buffer, BufferEntry& entry,
                         ztd::Range<uptr> range);
    BufferEntry& findHot3(ztd::Range<uptr> range);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
