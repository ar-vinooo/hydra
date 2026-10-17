#pragma once

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::cpu {
class IMmu;
}

namespace hydra::hw::tegra_x1::gpu::renderer {

class ICommandBuffer;
class ITexture;
class ITextureView;
class IRenderer;

using TextureCacheClock = std::chrono::steady_clock;
using TextureCacheTimePoint = TextureCacheClock::time_point;

struct TextureStorage {
    ITexture* base{nullptr};
    SmallCache<u32, ITextureView*> view_cache;
    TextureCacheTimePoint update_timestamp;
    // Last time this texture was rendered to
    TextureCacheTimePoint gpu_written_timestamp;
    // Last time the guest modified this texture, in memory that also contains
    // render targets
    TextureCacheTimePoint cpu_modified_timestamp;
    // Last GPU write that was copied back to guest memory
    TextureCacheTimePoint flushed_timestamp;

    void markUpdated() { update_timestamp = TextureCacheClock::now(); }
};

struct TextureGroup {
    SmallCache<u32, TextureStorage> cache;

    // Debug
    usize getStorageCount() const { return cache.getCount(); }

    const TextureStorage& getStorage(u32 index) const {
        // HACK: const cast
        auto it = const_cast<SmallCache<u32, TextureStorage>&>(cache).begin();
        std::advance(it, index);
        return it->second;
    }
};

struct TextureMemInfo {
    TextureCacheTimePoint modified_timestamp;
    TextureCacheTimePoint read_timestamp;
    TextureCacheTimePoint written_timestamp;

    void markModified() { modified_timestamp = TextureCacheClock::now(); }
    void markRead() { read_timestamp = TextureCacheClock::now(); }
    void markWritten() { written_timestamp = TextureCacheClock::now(); }
};

struct TextureMem {
    ztd::Range<uptr> range;
    TextureMemInfo info;
    SmallCache<u32, TextureGroup> cache;

    // Conservative history of GPU-written address ranges. Entries contain no
    // storage pointers, so replacing storages or merging memories is safe.
    struct WrittenRange {
        uptr end;
        TextureCacheTimePoint timestamp;
    };
    std::map<uptr, WrittenRange> gpu_writes;

    void recordGpuWrite(ztd::Range<uptr> written_range,
                        TextureCacheTimePoint timestamp) {
        auto [it, inserted] = gpu_writes.try_emplace(
            written_range.getBegin(),
            WrittenRange{written_range.getEnd(), timestamp});
        if (!inserted) {
            // Retain the union when a render target at this address shrinks.
            it->second.end = std::max(it->second.end, written_range.getEnd());
            it->second.timestamp = std::max(it->second.timestamp, timestamp);
        }
    }

    bool hasGpuWriteSince(ztd::Range<uptr> query_range,
                          TextureCacheTimePoint timestamp) const {
        for (auto it = gpu_writes.begin();
             it != gpu_writes.end() && it->first < query_range.getEnd(); ++it) {
            if (it->second.end > query_range.getBegin() &&
                it->second.timestamp > timestamp)
                return true;
        }
        return false;
    }

    // Debug
    usize getTextureGroupCount() const { return cache.getCount(); }

    const TextureGroup& getTextureGroup(u32 index) const {
        // HACK: const cast
        auto it = const_cast<SmallCache<u32, TextureGroup>&>(cache).begin();
        std::advance(it, index);
        return it->second;
    }
};

// TODO: destroy textures
class TextureCache {
  public:
    explicit TextureCache(IRenderer& renderer_) : renderer{renderer_} {}
    ~TextureCache();

    ITextureView* find(ICommandBuffer* command_buffer,
                       const TextureDescriptor& descriptor, TextureUsage usage);
    ITextureView* find(ICommandBuffer* command_buffer,
                       const TextureDescriptor& descriptor,
                       const TextureViewDescriptor& view_descriptor,
                       TextureUsage usage);

    void invalidateMemory(ztd::Range<uptr> range);
    // The GPU writes to the range are also in guest memory (written there
    // directly), so they must not be written back over it
    void markGpuWritesInGuest(ztd::Range<uptr> range);

    // Copies textures rendered by the GPU in the range back to guest memory,
    // waiting for the GPU. Engines that read guest memory directly (DMA
    // copies used for readbacks) would otherwise see stale data.
    void flushMemory(ICommandBuffer* command_buffer, ztd::Range<uptr> range);
    // The range is about to be overwritten entirely by guest memory writes:
    // GPU writes to textures inside it are dropped instead of waiting for a
    // readback, textures only partially inside it are still flushed
    void discardGpuWrites(ICommandBuffer* command_buffer,
                          ztd::Range<uptr> range);

    // Returns the format of a depth stencil texture at the same address with
    // the same dimensions and pixel size as the descriptor, if there is one.
    // Games copy depth buffers with the 2D engine using color formats.
    std::optional<TextureFormat>
    findDepthStencilAlias(const TextureDescriptor& descriptor);
    // Whether a compressed storage overlaps the range (copies through color
    // formats of the same block size alias them)
    bool overlapsCompressedStorage(ztd::Range<uptr> range);
    // Whether a storage overlapping the range has GPU writes that guest memory
    // does not have yet
    bool hasUnflushedGpuWrites(ztd::Range<uptr> range);

    // Changes whenever a lookup could return another view or need a
    // synchronization: GPU writes, invalidations, flushes and new storages.
    // Engines can reuse a previous lookup while it stays the same.
    u64 getEpoch() const { return epoch; }

    // Debug
    usize getMemoryCount() const { return entries.size(); }

    const TextureMem& getMemory(u32 index) const {
        auto it = entries.begin();
        std::advance(it, index);
        return it->second;
    }

    // Hot reloadable implementations (core/hot/texture_cache_hot.cpp), call
    // the functions above instead
    void invalidateMemoryImpl(ztd::Range<uptr> range);
    void markGpuWritesInGuestImpl(ztd::Range<uptr> range);
    void flushMemoryImpl(ICommandBuffer* command_buffer,
                         ztd::Range<uptr> range);
    void updateImpl(ICommandBuffer* command_buffer, TextureStorage& storage,
                    TextureMem& mem, TextureUsage usage);
    void encodeTextureImpl(ICommandBuffer* command_buffer,
                           TextureStorage& storage, ztd::Range<uptr> range);

  private:
    IRenderer& renderer;

    std::mutex mutex;
    std::map<uptr, TextureMem> entries;
    u64 epoch{0};

    static void mergeMemories(TextureMem& mem, TextureMem& other);
    // Storage keys are hashes of the descriptor that created the storage
    static std::optional<u32>
    findStorageKey(TextureGroup& group, const TextureDescriptor& descriptor);
    static u32 getFreeStorageKey(TextureGroup& group, u32 key);
    ITextureView* addToMemory(ICommandBuffer* command_buffer, TextureMem& mem,
                              const TextureDescriptor& descriptor,
                              const TextureViewDescriptor& view_descriptor,
                              TextureUsage usage);
    void updateStorage(ICommandBuffer* command_buffer, TextureStorage& storage,
                       TextureMem& mem, const TextureDescriptor& descriptor,
                       TextureUsage usage);
    static ITextureView*
    getTextureView(TextureStorage& storage,
                   const TextureViewDescriptor& view_descriptor);
    ITextureView* getTextureView(ICommandBuffer* command_buffer,
                                 TextureStorage& storage, TextureMem& mem,
                                 const TextureViewDescriptor& view_descriptor,
                                 TextureUsage usage);
    ITextureView* getTexture(ICommandBuffer* command_buffer,
                             TextureStorage& storage, TextureMem& mem,
                             const TextureDescriptor& descriptor,
                             const TextureViewDescriptor& view_descriptor,
                             TextureUsage usage);
    void update(ICommandBuffer* command_buffer, TextureStorage& storage,
                TextureMem& mem, TextureUsage usage);

    // Data synchronization
    static void synchronize2DWith2D(ICommandBuffer* command_buffer,
                                    TextureStorage& storage,
                                    TextureStorage& other_storage);
    static void synchronize3DWith3D(ICommandBuffer* command_buffer,
                                    TextureStorage& storage,
                                    TextureStorage& other_storage);

    // Helpers
    static u32 getDataHash(const ITexture* texture);
    void decodeTexture(ICommandBuffer* command_buffer, TextureStorage& storage);
    void encodeTexture(ICommandBuffer* command_buffer, TextureStorage& storage,
                       ztd::Range<uptr> range);

  public:
    REF_GETTER(mutex, getMutex);

  public:
    // ABI-stable implementations selected by the live module.
    static void synchronize2DWith2DHot0(ICommandBuffer* command_buffer,
                                        TextureStorage& storage,
                                        TextureStorage& other_storage);
    static void synchronize3DWith3DHot1(ICommandBuffer* command_buffer,
                                        TextureStorage& storage,
                                        TextureStorage& other_storage);
    void decodeTextureHot2(ICommandBuffer* command_buffer,
                           TextureStorage& storage);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
