#pragma once

#include "core/hw/tegra_x1/gpu/renderer/buffer_view.hpp"

namespace hydra::hw::tegra_x1::cpu {
class IMmu;
}

namespace hydra::hw::tegra_x1::gpu::renderer {

class IRenderer;

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

    void invalidateMemory(ztd::Range<uptr> range);

  private:
    IRenderer& renderer;

    std::mutex mutex;
    std::map<uptr, BufferEntry> entries;

    // Helpers
    void updateRange(ICommandBuffer* command_buffer, BufferEntry& entry,
                     ztd::Range<uptr> range);
    BufferEntry& find(ztd::Range<uptr> range);

  public:
    REF_GETTER(mutex, getMutex);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
