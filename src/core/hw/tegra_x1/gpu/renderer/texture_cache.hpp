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
    std::shared_ptr<void> base_owner;
    std::vector<std::shared_ptr<void>> view_owners;
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
    // Address ranges whose GPU data must not be written back to guest memory
    // any more: written back already, or overwritten by the guest since (what
    // the GPU holds there is stale). A partial write back leaves the rest of
    // the texture unflushed, and a later write back of a range that the guest
    // reused would put stale data over its newer contents. Reset by the next
    // GPU write.
    std::vector<ztd::Range<uptr>> flushed_ranges;

    // Address ranges the guest overwrote since the GPU last wrote them: the
    // GPU data there is stale, so it must neither be written back nor copied
    // over other textures that alias the memory. Reset by the next GPU write.
    std::vector<ztd::Range<uptr>> stale_ranges;

    static void addRange(std::vector<ztd::Range<uptr>>& ranges,
                         ztd::Range<uptr> range) {
        if (range.getSize() == 0 || range.getBegin() > range.getEnd())
            return;
        ranges.push_back(range);
        // Keep the list short: fold overlapping and adjacent entries
        if (ranges.size() > 32) {
            std::sort(ranges.begin(), ranges.end(),
                      [](const auto& a, const auto& b) {
                          return a.getBegin() < b.getBegin();
                      });
            std::vector<ztd::Range<uptr>> folded;
            for (const auto& entry : ranges) {
                if (!folded.empty() &&
                    entry.getBegin() <= folded.back().getEnd())
                    folded.back() = folded.back().merged(entry);
                else
                    folded.push_back(entry);
            }
            ranges = std::move(folded);
        }
    }

    void markFlushed(ztd::Range<uptr> range) { addRange(flushed_ranges, range); }
    // The guest wrote over the range: nothing the GPU holds there is valid
    void markStale(ztd::Range<uptr> range) {
        addRange(stale_ranges, range);
        addRange(flushed_ranges, range);
    }

    // Calls fn with the parts of the range that still hold fresh GPU data
    template <typename Fn>
    void forEachFresh(ztd::Range<uptr> range, Fn&& fn) const {
        forEachOutside(stale_ranges, range, std::forward<Fn>(fn));
    }

    // Calls fn with the parts of the range that may still be written back
    template <typename Fn>
    void forEachUnflushed(ztd::Range<uptr> range, Fn&& fn) const {
        forEachOutside(flushed_ranges, range, std::forward<Fn>(fn));
    }

    template <typename Fn>
    static void forEachOutside(const std::vector<ztd::Range<uptr>>& excluded,
                               ztd::Range<uptr> range, Fn&& fn) {
        std::vector<ztd::Range<uptr>> pieces{range};
        for (const auto& flushed : excluded) {
            std::vector<ztd::Range<uptr>> next;
            for (const auto& piece : pieces) {
                if (!piece.intersects(flushed)) {
                    next.push_back(piece);
                    continue;
                }
                if (piece.getBegin() < flushed.getBegin())
                    next.emplace_back(piece.getBegin(), flushed.getBegin());
                if (flushed.getEnd() < piece.getEnd())
                    next.emplace_back(flushed.getEnd(), piece.getEnd());
            }
            pieces = std::move(next);
        }
        for (const auto& piece : pieces)
            fn(piece);
    }

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
    void synchronizeStorageWrites(
        ICommandBuffer* command_buffer,
        const std::vector<ztd::Range<uptr>>& ranges);
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
    // Disjoint intervals, by their begin
    std::map<uptr, TextureMem> entries;

    // The entries that intersect `range`, without visiting the others
    template <typename F>
    void forEachIntersecting(ztd::Range<uptr> range, F&& f) {
        auto it = entries.upper_bound(range.getBegin());
        if (it != entries.begin())
            it--;
        for (; it != entries.end() && it->first < range.getEnd(); it++)
            if (it->second.range.intersects(range))
                f(it->second);
    }
    u64 epoch{0};
    // DEBUG: set around the first decode of a new render target, which then
    // uploads zeros instead of the guest memory contents (HYDRA_RT_ZERO_INIT)
    bool zero_fill_decode{false};

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
    // Returns false when the layouts cannot be copied directly
    static bool synchronize2DWith2D(ICommandBuffer* command_buffer,
                                    TextureStorage& storage,
                                    TextureStorage& other_storage);
    static bool synchronize3DWith3D(ICommandBuffer* command_buffer,
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
    static bool synchronize2DWith2DHot0(ICommandBuffer* command_buffer,
                                        TextureStorage& storage,
                                        TextureStorage& other_storage);
    static bool synchronize3DWith3DHot1(ICommandBuffer* command_buffer,
                                        TextureStorage& storage,
                                        TextureStorage& other_storage);
    void decodeTextureHot2(ICommandBuffer* command_buffer,
                           TextureStorage& storage);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
