#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_events.hpp"
#include "core/hot/table.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <set>

#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {


TextureCache::~TextureCache() {
    for (auto& [mem_key, mem] : entries) {
        for (auto& [group_key, group] : mem.cache) {
            for (auto& [storage_key, storage] : group.cache) {
                for (auto& [view_key, view] : storage.view_cache)
                    delete view;
                delete storage.base;
            }
        }
    }
}

ITextureView* TextureCache::find(ICommandBuffer* command_buffer,
                                 const TextureDescriptor& descriptor,
                                 TextureUsage usage) {
    return find(
        command_buffer, descriptor,
        TextureViewDescriptor(descriptor.type, descriptor.format,
                              ztd::Range<u32>(0, descriptor.level_count),
                              ztd::Range<u32>(0, descriptor.layer_count),
                              SwizzleChannels()),
        usage);
}

ITextureView* TextureCache::find(ICommandBuffer* command_buffer,
                                 const TextureDescriptor& descriptor,
                                 const TextureViewDescriptor& view_descriptor,
                                 TextureUsage usage) {
    const auto range = descriptor.getRange();
    if (usage != TextureUsage::Read)
        epoch++;

    // Check for containing interval
    auto it = entries.upper_bound(range.getBegin());
    if (it != entries.begin()) {
        auto prev = std::prev(it);
        auto& prev_mem = prev->second;
        if (prev_mem.range.getEnd() >= range.getEnd()) {
            // Fully contained
            return addToMemory(command_buffer, prev_mem, descriptor,
                               view_descriptor, usage);
        }
    }

    // Insert and merge
    epoch++;
    TextureMem mem{.range = range};

    it = entries.lower_bound(range.getBegin());

    // Merge with previous if overlapping
    if (it != entries.begin()) {
        auto prev = std::prev(it);
        auto& prev_mem = prev->second;
        if (prev_mem.range.getEnd() > mem.range.getBegin()) {
            mergeMemories(mem, prev_mem);
            it = entries.erase(prev);
        }
    }

    // Merge with following entries
    while (it != entries.end() && it->first < mem.range.getEnd()) {
        auto& crnt_mem = it->second;
        mergeMemories(mem, crnt_mem);
        it = entries.erase(it);
    }

    // Insert merged interval
    auto inserted = entries.emplace(mem.range.getBegin(), std::move(mem));
    return addToMemory(command_buffer, inserted.first->second, descriptor,
                       view_descriptor, usage);
}

std::optional<TextureFormat>
TextureCache::findDepthStencilAlias(const TextureDescriptor& descriptor) {
    const auto& format_info = getTextureFormatInfo(descriptor.format);
    if (format_info.is_depth_stencil)
        return std::nullopt;

    for (auto& [base, mem] : entries) {
        if (!mem.range.contains(descriptor.ptr))
            continue;
        for (auto& [group_key, group] : mem.cache) {
            for (auto& [storage_key, storage] : group.cache) {
                const auto& other = storage.base->getDescriptor();
                if (other.ptr != descriptor.ptr ||
                    other.width != descriptor.width ||
                    other.height != descriptor.height)
                    continue;
                const auto& other_info = getTextureFormatInfo(other.format);
                if (other_info.is_depth_stencil &&
                    other_info.bytes_per_block == format_info.bytes_per_block)
                    return other.format;
            }
        }
    }

    return std::nullopt;
}

bool TextureCache::overlapsCompressedStorage(ztd::Range<uptr> range) {
    for (auto& [base, mem] : entries) {
        if (!mem.range.intersects(range))
            continue;
        for (auto& [group_key, group] : mem.cache) {
            for (auto& [storage_key, storage] : group.cache) {
                const auto& descriptor = storage.base->getDescriptor();
                if (isTextureFormatCompressed(descriptor.format) &&
                    descriptor.getRange().intersects(range))
                    return true;
            }
        }
    }
    return false;
}

bool TextureCache::hasUnflushedGpuWrites(ztd::Range<uptr> range) {
    for (auto& [base, mem] : entries) {
        if (!mem.range.intersects(range))
            continue;
        for (auto& [group_key, group] : mem.cache) {
            for (auto& [storage_key, storage] : group.cache) {
                if (storage.gpu_written_timestamp > storage.flushed_timestamp &&
                    storage.base->getDescriptor().getRange().intersects(range))
                    return true;
            }
        }
    }
    return false;
}

void TextureCache::invalidateMemory(ztd::Range<uptr> range) {
    epoch++;
    ::hydra::hot::Scope hot_scope;
    HYDRA_HOT_CALL(*this, texture_cache_invalidate_memory)(range);
}

void TextureCache::synchronizeStorageWrites(
    ICommandBuffer* command_buffer,
    const std::vector<ztd::Range<uptr>>& ranges) {
    if (ranges.empty())
        return;
    // Texture uploads decode guest bytes on the CPU. Finish the shader's
    // writes before a later lookup can snapshot those bytes.
    renderer.debugSyncCommandBuffer(command_buffer);
    for (const auto& range : ranges)
        invalidateMemory(range);
}

void TextureCache::markGpuWritesInGuest(ztd::Range<uptr> range) {
    epoch++;
    ::hydra::hot::Scope hot_scope;
    HYDRA_HOT_CALL(*this, texture_cache_mark_gpu_writes_in_guest)(range);
}

void TextureCache::mergeMemories(TextureMem& mem, TextureMem& other) {
    mem.range = mem.range.merged(other.range);
    mem.info = {
        .modified_timestamp = std::max(mem.info.modified_timestamp,
                                       other.info.modified_timestamp),
        .read_timestamp =
            std::max(mem.info.read_timestamp, other.info.read_timestamp),
        .written_timestamp =
            std::max(mem.info.written_timestamp, other.info.written_timestamp),
    };

    for (const auto& [begin, write] : other.gpu_writes)
        mem.recordGpuWrite(ztd::Range<uptr>(begin, write.end), write.timestamp);

    for (auto& [group_key, other_group] : other.cache) {
        auto group_opt = mem.cache.find(group_key);
        auto& group =
            (group_opt.has_value() ? **group_opt : mem.cache.insert(group_key));
        for (auto& [storage_key, storage] : other_group.cache) {
            group.cache.insert(getFreeStorageKey(group, storage_key),
                               std::move(storage));
        }
    }
}

namespace {

// The storage holds the requested texture, possibly with more levels or
// layers (storages are keyed by the request that created them)
bool servesRequest(const TextureDescriptor& storage,
                   const TextureDescriptor& request) {
    if (storage.ptr != request.ptr || storage.is_linear != request.is_linear ||
        storage.width != request.width || storage.height != request.height ||
        storage.depth != request.depth ||
        storage.layer_size != request.layer_size ||
        storage.level_count < request.level_count ||
        storage.layer_count < request.layer_count)
        return false;
    if (storage.is_linear)
        return storage.linear_stride == request.linear_stride;
    return storage.block_width_gobs_log2 == request.block_width_gobs_log2 &&
           storage.block_height_gobs_log2 == request.block_height_gobs_log2 &&
           storage.block_depth_gobs_log2 == request.block_depth_gobs_log2;
}

} // namespace

// Storage keys are hashes: storages of other descriptors can have the same
// key, the following keys are probed
std::optional<u32> TextureCache::findStorageKey(
    TextureGroup& group, const TextureDescriptor& descriptor) {
    for (u32 key = descriptor.getStorageHash();; key++) {
        const auto storage = group.cache.find(key);
        if (!storage)
            return std::nullopt;
        if ((**storage).base != nullptr &&
            servesRequest((**storage).base->getDescriptor(), descriptor))
            return key;
    }
}

u32 TextureCache::getFreeStorageKey(TextureGroup& group, u32 key) {
    while (group.cache.find(key))
        key++;
    return key;
}

namespace {

bool calculateLevelAndLayer(const TextureDescriptor& base_descriptor, uptr ptr,
                            u32& out_level, u32& out_layer) {
    const auto offset = static_cast<u32>(ptr - base_descriptor.ptr);

    // Layer
    out_layer = 0;
    u32 layer_offset = 0;
    if (base_descriptor.layer_count > 1) {
        out_layer = offset / base_descriptor.layer_size;
        layer_offset = out_layer * base_descriptor.layer_size;
    }

    // Level
    out_level = 0;
    u32 crnt_level_offset = 0;
    const u32 level_offset = offset - layer_offset;
    for (; crnt_level_offset < level_offset;
         crnt_level_offset += base_descriptor.getLevelSize(out_level++)) {
    }

    // Check if level is aligned
    return crnt_level_offset == level_offset;
}

bool haveCompatibleLayout(const TextureDescriptor& descriptor1, u32 level1,
                          const TextureDescriptor& descriptor2, u32 level2);

bool calculateLevelAndLayer(const TextureDescriptor& base_descriptor,
                            const TextureDescriptor& view_descriptor,
                            u32& out_level, u32& out_layer) {
    if (!calculateLevelAndLayer(base_descriptor, view_descriptor.ptr, out_level,
                                out_layer))
        return false;

    if (base_descriptor.getLevelDimensions(out_level) !=
        MTL::Size(view_descriptor.width, view_descriptor.height,
                  view_descriptor.depth))
        return false;

    // Matching dimensions do not imply matching guest byte layout. In
    // particular, mode/allocator transitions can change pitch or tiling at
    // the same address. Such requests need their own decoded storage.
    return haveCompatibleLayout(base_descriptor, out_level, view_descriptor, 0);
}

// Layered textures (e.g. cubemaps) whose levels are rendered one at a time:
// the base texture may describe fewer levels than its layer stride holds, so
// a later level lies past its range even though it belongs to it
bool isLevelOfLayeredTexture(const TextureDescriptor& base_descriptor,
                             const TextureDescriptor& view_descriptor) {
    // Single layer views (e.g. one cubemap face) don't have a layer stride
    const bool single_layer =
        view_descriptor.layer_count == 1 && view_descriptor.layer_size == 0;
    if (base_descriptor.is_linear || view_descriptor.is_linear ||
        base_descriptor.layer_count <= 1 || base_descriptor.layer_size == 0 ||
        (!single_layer &&
         view_descriptor.layer_size != base_descriptor.layer_size) ||
        view_descriptor.ptr < base_descriptor.ptr)
        return false;

    const u64 offset = view_descriptor.ptr - base_descriptor.ptr;
    const u64 first_layer = offset / base_descriptor.layer_size;
    return first_layer + view_descriptor.layer_count <=
           base_descriptor.layer_count;
}

// Whether two block linear levels share the same memory layout, even if their
// dimensions differ slightly (e.g. a render target with a padded width)
bool haveCompatibleLayout(const TextureDescriptor& descriptor1, u32 level1,
                          const TextureDescriptor& descriptor2, u32 level2) {
    if (descriptor1.is_linear || descriptor2.is_linear)
        return descriptor1.is_linear && descriptor2.is_linear &&
               descriptor1.linear_stride == descriptor2.linear_stride;

    const auto dims1 = descriptor1.getLevelDimensions(level1);
    const auto dims2 = descriptor2.getLevelDimensions(level2);
    if (static_cast<u32>(dims1.depth) != static_cast<u32>(dims2.depth))
        return false;

    const auto block_size_log2 = descriptor1.getLevelBlockSizeLog2(level1);
    if (block_size_log2 != descriptor2.getLevelBlockSizeLog2(level2))
        return false;

    const u32 block_width = GOB_WIDTH << static_cast<u32>(block_size_log2.width);
    return align(getTextureFormatStride(descriptor1.format, static_cast<u32>(dims1.width)),
                 block_width) ==
           align(getTextureFormatStride(descriptor2.format, static_cast<u32>(dims2.width)),
                 block_width);
}

bool calculateLevelAndLayer(const TextureDescriptor& base1_descriptor,
                            const TextureDescriptor& base2_descriptor, uptr ptr,
                            u32& out_level1, u32& out_layer1, u32& out_level2,
                            u32& out_layer2) {
    if (!calculateLevelAndLayer(base1_descriptor, ptr, out_level1, out_layer1))
        return false;

    if (!calculateLevelAndLayer(base2_descriptor, ptr, out_level2, out_layer2))
        return false;

    if (base1_descriptor.getLevelDimensions(out_level1) !=
        base2_descriptor.getLevelDimensions(out_level2))
        return false;

    return haveCompatibleLayout(base1_descriptor, out_level1,
                                base2_descriptor, out_level2);
}

bool calculateLevelAndSlice(const TextureDescriptor& base_descriptor, uptr ptr,
                            u32& out_level, u32& out_slice) {
    const auto offset = static_cast<u32>(ptr - base_descriptor.ptr);

    // Level
    out_level = 0;
    u32 level_offset = 0;
    while (level_offset < offset) {
        const u32 level_size = base_descriptor.getLevelSize(out_level);
        if (level_offset + level_size >= offset)
            break;

        level_offset += level_size;
        out_level++;
    }

    // Slice
    const u32 slice_offset = offset - level_offset;
    const u32 slice_size = base_descriptor.size / base_descriptor.depth;
    out_slice = slice_offset / slice_size;

    // Check if slice is aligned
    return out_slice * slice_size == slice_offset;
}

bool calculateLevelAndSlice(const TextureDescriptor& base1_descriptor,
                            const TextureDescriptor& base2_descriptor, uptr ptr,
                            u32& out_level1, u32& out_slice1, u32& out_level2,
                            u32& out_slice2) {
    if (!calculateLevelAndSlice(base1_descriptor, ptr, out_level1, out_slice1))
        return false;

    if (!calculateLevelAndSlice(base2_descriptor, ptr, out_level2, out_slice2))
        return false;

    const auto dims1 = base1_descriptor.getLevelDimensions(out_level1);
    const auto dims2 = base2_descriptor.getLevelDimensions(out_level2);
    // Z can differ
    return static_cast<u32>(dims1.width) == static_cast<u32>(dims2.width) && static_cast<u32>(dims1.height) == static_cast<u32>(dims2.height);
}

} // namespace

ITextureView*
TextureCache::addToMemory(ICommandBuffer* command_buffer, TextureMem& mem,
                          const TextureDescriptor& descriptor,
                          const TextureViewDescriptor& view_descriptor,
                          TextureUsage usage) {
    const auto range = descriptor.getRange();
    const auto group_hash = descriptor.getGroupHash();
    const auto storage_hash = descriptor.getStorageHash();

    // Check if it is a new entry
    auto group_opt = mem.cache.find(group_hash);
    if (!group_opt.has_value()) {
        epoch++;
        auto& group = mem.cache.insert(group_hash);
        auto& storage = group.cache.insert(storage_hash);
        return getTexture(command_buffer, storage, mem, descriptor,
                          view_descriptor, usage);
    }

    auto& group = **group_opt;

    // Check if the storage already exists
    if (const auto storage_key = findStorageKey(group, descriptor)) {
        auto& storage = **group.cache.find(*storage_key);
        return getTextureView(command_buffer, storage, mem, view_descriptor,
                              usage);
    }
    epoch++;

    // Buffer subranges are not mip/layer views. Keep exact-sized storages;
    // overlapping GPU contents are synchronized by byte range in update().
    if (descriptor.type == MTL::TextureTypeTextureBuffer) {
        auto& storage =
            group.cache.insert(getFreeStorageKey(group, storage_hash));
        return getTexture(command_buffer, storage, mem, descriptor,
                          view_descriptor, usage);
    }

    // ---------------- View ----------------
    for (auto& [key, storage] : group.cache) {
        const auto& other_descriptor = storage.base->getDescriptor();
        const auto other_range = other_descriptor.getRange();
        if (other_range.contains(range) ||
            isLevelOfLayeredTexture(other_descriptor, descriptor)) {
            u32 level;
            u32 layer;
            if (!calculateLevelAndLayer(other_descriptor, descriptor, level,
                                        layer)) {
                // Render targets for small levels of layered textures can have
                // a padded width
                const bool padded_level =
                    isLevelOfLayeredTexture(other_descriptor, descriptor) &&
                    descriptor.level_count == 1 &&
                    calculateLevelAndLayer(other_descriptor, descriptor.ptr,
                                           level, layer) &&
                    static_cast<u32>(other_descriptor.getLevelDimensions(level).height) ==
                        descriptor.height &&
                    static_cast<u32>(other_descriptor.getLevelDimensions(level).width) <=
                        descriptor.width &&
                    haveCompatibleLayout(other_descriptor, level, descriptor,
                                         0);
                if (!padded_level) {
                    LOG_DEBUG(Gpu,
                              "Misaligned textures (existing: ({}), new: ({}))",
                              other_descriptor, descriptor);
                    continue;
                }
            }

            auto actual_storage = &storage;

            // Check if the base texture has enough levels
            const u32 min_levels = level + descriptor.level_count;
            if (other_descriptor.level_count < min_levels) {
                // Keep what is needed from the old storage: removing it frees
                // its slot, which the new storage may reuse
                const auto old_base = storage.base;
                const auto old_descriptor = other_descriptor;
                const auto old_gpu_written = storage.gpu_written_timestamp;
                const auto old_cpu_modified = storage.cpu_modified_timestamp;
                const auto old_flushed_ranges = storage.flushed_ranges;
                const auto old_stale_ranges = storage.stale_ranges;
                group.cache.remove(key);

                // Create a new storage
                auto new_descriptor = old_descriptor;
                new_descriptor.level_count = min_levels;
                auto& new_storage = group.cache.insert(getFreeStorageKey(
                    group, new_descriptor.getStorageHash()));
                updateStorage(command_buffer, new_storage, mem, new_descriptor,
                              usage);

                // Copy the old storage to the new one, unless the guest
                // overwrote all of what the GPU held in it
                bool old_fresh = false;
                TextureStorage::forEachOutside(
                    old_stale_ranges, old_descriptor.getRange(),
                    [&](ztd::Range<uptr>) { old_fresh = true; });
                if (old_fresh)
                    new_storage.base->copyFrom(command_buffer, old_base, 0, 0, 0,
                                               0, old_descriptor.level_count,
                                               old_descriptor.layer_count);
                new_storage.gpu_written_timestamp = std::max(
                    new_storage.gpu_written_timestamp, old_gpu_written);
                new_storage.cpu_modified_timestamp = std::max(
                    new_storage.cpu_modified_timestamp, old_cpu_modified);
                new_storage.flushed_ranges = old_flushed_ranges;
                new_storage.stale_ranges = old_stale_ranges;

                // TODO: destroy the old storage

                actual_storage = &new_storage;
            }

            return getTextureView(
                command_buffer, *actual_storage, mem,
                TextureViewDescriptor(
                    view_descriptor.type, view_descriptor.format,
                    ztd::Range<u32>::fromSize(
                        level + view_descriptor.levels.getBegin(),
                        view_descriptor.levels.getSize()),
                    ztd::Range<u32>::fromSize(
                        layer + view_descriptor.layers.getBegin(),
                        view_descriptor.layers.getSize()),
                    view_descriptor.swizzle_channels),
                usage);
        }
    }

    // ---------------- New storage ----------------

    // Find overlapping storages
    struct OverlappingStorage {
        TextureStorage storage;
        u32 level;
        u32 layer;
    };

    std::vector<OverlappingStorage> overlapping_storages;
    u32 level_count = descriptor.level_count;
    u32 layer_count = descriptor.layer_count;
    for (auto it = group.cache.begin(); it != group.cache.end();) {
        auto& storage = it->second;
        const auto& other_descriptor = storage.base->getDescriptor();
        const auto other_range = other_descriptor.getRange();
        if (range.intersects(other_range)) {
            u32 layer = 0;
            u32 level = 0;
            if (other_range.getBegin() >= range.getBegin()) {
                if (!calculateLevelAndLayer(descriptor, other_descriptor, level,
                                            layer)) {
                    LOG_DEBUG(Gpu,
                              "Misaligned textures (existing: ({}), new: ({}))",
                              other_descriptor, descriptor);
                    ++it;
                    continue;
                }

                // Make sure the new storage can fit the levels and layers of
                // all the old storages
                level_count =
                    std::max(level_count, level + other_descriptor.level_count);
                layer_count =
                    std::max(layer_count, layer + other_descriptor.layer_count);
            } else {
                // The existing texture starts before the new one: it cannot
                // be merged into it. Both stay separate storages; their
                // shared bytes are synchronized by address range (update()).
                // This happens thousands of times per second in UE4 games.
                ++it;
                continue;
            }

            overlapping_storages.emplace_back(std::move(storage), level, layer);
            it = group.cache.remove(it);
        } else {
            ++it;
        }
    }

    // Create new descriptor
    auto new_descriptor = descriptor;
    new_descriptor.level_count = level_count;
    new_descriptor.layer_count = layer_count;

    // Create a new storage
    auto& storage = group.cache.insert(getFreeStorageKey(group, storage_hash));
    updateStorage(command_buffer, storage, mem, new_descriptor, usage);

    // Copy overlapping storages
    for (auto& overlapping_storage : overlapping_storages) {
        const auto other_base = overlapping_storage.storage.base;
        const auto& other_descriptor = other_base->getDescriptor();
        // What the guest overwrote since the GPU wrote this storage is stale:
        // the new storage was decoded from the guest's data already
        bool fresh = false;
        overlapping_storage.storage.forEachFresh(
            other_descriptor.getRange(),
            [&](ztd::Range<uptr>) { fresh = true; });
        if (!fresh) {
            logTextureEvent("copy_skipped_stale_storage", descriptor,
                            fmt::format("old={:#x} {} {}", other_descriptor.ptr,
                                        other_descriptor.type,
                                        other_descriptor.format));
            continue;
        }
        storage.base->copyFrom(
            command_buffer, other_base, 0, 0, overlapping_storage.level,
            overlapping_storage.layer, other_descriptor.level_count,
            other_descriptor.layer_count);
    }

    // TODO: destroy overlapping storages

    // Return view
    return getTextureView(storage, view_descriptor);
}

void TextureCache::updateStorage(ICommandBuffer* command_buffer,
                                 TextureStorage& storage, TextureMem& mem,
                                 const TextureDescriptor& descriptor,
                                 TextureUsage usage) {
    if (storage.base == nullptr) {
        storage.base = renderer.createTexture(descriptor);
        // DEBUG: HYDRA_RT_ZERO_INIT=1 starts render targets in memory the GPU
        // never wrote with zeros. Decoding guest memory into them shows
        // whatever other data (e.g. streamed textures) shares that memory.
        static const bool zero_init =
            std::getenv("HYDRA_RT_ZERO_INIT") != nullptr;
        zero_fill_decode = zero_init && usage == TextureUsage::Write &&
                           mem.info.written_timestamp == TextureCacheTimePoint{};
        logTextureEvent(
            "create", descriptor,
            fmt::format("usage={} zero_fill={}", static_cast<u32>(usage),
                        zero_fill_decode));
        decodeTexture(command_buffer, storage);
        zero_fill_decode = false;
    }
    update(command_buffer, storage, mem, usage);
}

ITextureView*
TextureCache::getTextureView(TextureStorage& storage,
                             const TextureViewDescriptor& view_descriptor) {
    ztd::hash::XxHash32 view_hash;
    view_hash.add(view_descriptor.getHash());
    view_hash.add(::hydra::hot::generation());
    const auto key = view_hash.toHashCode();
    // Views of other descriptors can have the same key
    u32 free_key = key;
    for (;; free_key++) {
        auto view_opt = storage.view_cache.find(free_key);
        if (!view_opt.has_value())
            break;
        if ((**view_opt)->getDescriptor() == view_descriptor)
            return **view_opt;
    }

    auto view = storage.base->createView(view_descriptor);
    storage.view_cache.insert(free_key, view);
    return view;
}

ITextureView* TextureCache::getTextureView(
    ICommandBuffer* command_buffer, TextureStorage& storage, TextureMem& mem,
    const TextureViewDescriptor& view_descriptor, TextureUsage usage) {
    update(command_buffer, storage, mem, usage);
    return getTextureView(storage, view_descriptor);
}

ITextureView* TextureCache::getTexture(
    ICommandBuffer* command_buffer, TextureStorage& storage, TextureMem& mem,
    const TextureDescriptor& descriptor,
    const TextureViewDescriptor& view_descriptor, TextureUsage usage) {
    updateStorage(command_buffer, storage, mem, descriptor, usage);
    return getTextureView(storage, view_descriptor);
}

void TextureCache::update(ICommandBuffer* command_buffer,
                          TextureStorage& storage, TextureMem& mem,
                          TextureUsage usage) {
    ::hydra::hot::Scope hot_scope;
    HYDRA_HOT_CALL(*this, texture_cache_update)(command_buffer, storage, mem, usage);
}

void TextureCache::synchronize2DWith2DHot0(ICommandBuffer* command_buffer,
                                       TextureStorage& storage,
                                       TextureStorage& other_storage) {
    const auto& descriptor = storage.base->getDescriptor();
    const auto& other_descriptor = other_storage.base->getDescriptor();
    const auto copy_range =
        descriptor.getRange().clampedTo(other_descriptor.getRange());

    u32 level;
    u32 layer;
    u32 other_level;
    u32 other_layer;
    if (!calculateLevelAndLayer(descriptor, other_descriptor,
                                copy_range.getBegin(), level, layer,
                                other_level, other_layer)) {
        // Same memory layout, but different dimensions (e.g. a padded render
        // target): copy the overlapping region of a single level
        if (descriptor.ptr != other_descriptor.ptr ||
            !calculateLevelAndLayer(descriptor, copy_range.getBegin(), level,
                                    layer) ||
            !calculateLevelAndLayer(other_descriptor, copy_range.getBegin(),
                                    other_level, other_layer) ||
            !haveCompatibleLayout(descriptor, level, other_descriptor,
                                  other_level)) {
            // DEBUG: report textures that alias but cannot be synchronized
            static u32 debug_count = 0;
            if (debug_count++ < 20)
                LOG_WARN(Gpu, "DEBUG cannot synchronize 2D textures ({}) and ({})",
                         descriptor, other_descriptor);
            return;
        }

        const auto dims = descriptor.getLevelDimensions(level);
        const auto other_dims =
            other_descriptor.getLevelDimensions(other_level);
        const MTL::Size size(std::min(static_cast<u32>(dims.width), static_cast<u32>(other_dims.width)),
                             std::min(static_cast<u32>(dims.height), static_cast<u32>(other_dims.height)),
                             std::min(static_cast<u32>(dims.depth), static_cast<u32>(other_dims.depth)));
        storage.base->copyFrom(
            command_buffer, other_storage.base, MTL::Origin(0, 0, 0), other_level,
            other_layer, MTL::Origin(0, 0, 0), level, layer, size,
            std::min(descriptor.layer_count - layer,
                     other_descriptor.layer_count - other_layer));
        return;
    }

    storage.base->copyFrom(
        command_buffer, other_storage.base, other_level, other_layer, level,
        layer,
        std::min(descriptor.level_count - level,
                 other_descriptor.level_count - other_level),
        std::min(descriptor.layer_count - layer,
                 other_descriptor.layer_count - other_layer));
}

void TextureCache::synchronize3DWith3DHot1(ICommandBuffer* command_buffer,
                                       TextureStorage& storage,
                                       TextureStorage& other_storage) {
    const auto& descriptor = storage.base->getDescriptor();
    const auto& other_descriptor = other_storage.base->getDescriptor();
    const auto copy_range =
        descriptor.getRange().clampedTo(other_descriptor.getRange());

    u32 level;
    u32 slice;
    u32 other_level;
    u32 other_slice;
    if (!calculateLevelAndSlice(descriptor, other_descriptor,
                                copy_range.getBegin(), level, slice,
                                other_level, other_slice)) {
        LOG_DEBUG(Gpu, "Cannot synchronize 3D textures ({}) and ({})",
                  descriptor, other_descriptor);
        return;
    }

    const auto dims = descriptor.getLevelDimensions(level);
    const auto other_dims = other_descriptor.getLevelDimensions(other_level);
    const u32 level_count =
        std::min(descriptor.level_count - level,
                 other_descriptor.level_count - other_level);

    if (slice == 0 && other_slice == 0 && static_cast<u32>(dims.depth) == static_cast<u32>(other_dims.depth)) {
        // Do a simplified copy in case we are copying whole levels
        storage.base->copyFrom(command_buffer, other_storage.base, other_level,
                               0, level, 0, level_count, 1);
    } else {
        ASSERT_DEBUG(
            level_count == 1, Gpu,
            "Cannot copy multiple 3D levels with non-matching dimensions");
        const NS::UInteger slice_count =
            std::min(static_cast<u32>(dims.depth) - slice, static_cast<u32>(other_dims.depth) - other_slice);
        storage.base->copyFrom(command_buffer, other_storage.base,
                               MTL::Origin(0, 0, other_slice), other_level, 0,
                               MTL::Origin(0, 0, slice), level, 0,
                               MTL::Size(static_cast<u32>(dims.width), static_cast<u32>(dims.height), slice_count),
                               1);
    }
}

u32 TextureCache::getDataHash(const ITexture* texture) {
    constexpr u32 SAMPLE_COUNT = 37;

    const auto& descriptor = texture->getDescriptor();
    u64 mem_range = descriptor.size;
    u64 mem_step = std::max(mem_range / SAMPLE_COUNT, 1ull);

    ztd::hash::XxHash32 hash;
    for (u64 offset = 0; offset < mem_range; offset += mem_step)
        hash.add(*reinterpret_cast<u64*>(descriptor.ptr + offset));

    return hash.toHashCode();
}

void TextureCache::flushMemory(ICommandBuffer* command_buffer,
                               ztd::Range<uptr> range) {
    epoch++;
    ::hydra::hot::Scope hot_scope;
    HYDRA_HOT_CALL(*this, texture_cache_flush_memory)(command_buffer, range);
}

void TextureCache::discardGpuWrites(ICommandBuffer* command_buffer,
                                    ztd::Range<uptr> range) {
    epoch++;
    bool partial = false;
    auto it = entries.upper_bound(range.getBegin());
    if (it != entries.begin())
        it--;
    for (; it != entries.end() && it->first < range.getEnd(); it++) {
        auto& mem = it->second;
        if (mem.range.getEnd() <= range.getBegin())
            continue;
        for (auto& [group_key, group] : mem.cache) {
            for (auto& [storage_key, storage] : group.cache) {
                if (storage.base == nullptr ||
                    storage.gpu_written_timestamp <= storage.flushed_timestamp)
                    continue;
                const auto texture_range =
                    storage.base->getDescriptor().getRange();
                if (!texture_range.intersects(range))
                    continue;
                if (texture_range.getBegin() >= range.getBegin() &&
                    texture_range.getEnd() <= range.getEnd())
                    storage.flushed_timestamp = storage.gpu_written_timestamp;
                else
                    partial = true;
            }
        }
    }
    if (partial)
        flushMemory(command_buffer, range);
}

void TextureCache::encodeTexture(ICommandBuffer* command_buffer,
                                 TextureStorage& storage,
                                 ztd::Range<uptr> range) {
    ::hydra::hot::Scope hot_scope;
    HYDRA_HOT_CALL(*this, texture_cache_encode_texture)(command_buffer, storage, range);
}

void TextureCache::decodeTextureHot2(ICommandBuffer* command_buffer,
                                 TextureStorage& storage) {
    const auto& descriptor = storage.base->getDescriptor();

    // Calculate size
    u32 size = 0;
    for (u32 level = 0; level < descriptor.level_count; level++) {
        const auto dims = descriptor.getLevelDimensions(level);
        const u32 stride = getTextureFormatStride(descriptor.format, static_cast<u32>(dims.width));
        const u32 rows = getTextureFormatRows(descriptor.format, static_cast<u32>(dims.height));
        const u32 slice_stride = rows * stride;
        size += static_cast<u32>(dims.depth) * slice_stride;
    }
    size *= descriptor.layer_count;

    // Allocate temporary buffer
    const auto upload_start = PerfStats::isEnabled()
                                  ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    auto tmp_buffer = renderer.allocateTextureUploadBuffer(size);

    const u8* in_data = reinterpret_cast<const u8*>(descriptor.ptr);
    u8* out_data = reinterpret_cast<u8*>(tmp_buffer->getPtr());
    if (zero_fill_decode) {
        std::memset(out_data, 0, size);
    } else if (descriptor.is_linear) {
        const u32 stride =
            getTextureFormatStride(descriptor.format, descriptor.width);
        const u32 rows =
            getTextureFormatRows(descriptor.format, descriptor.height);
        for (u32 row = 0; row < rows; row++) {
            std::memcpy(out_data + static_cast<usize>(row) * stride,
                        in_data +
                            static_cast<usize>(row) * descriptor.linear_stride,
                        stride);
        }
    } else {
        // Levels and layers are aligned to blocks in guest memory, but packed
        // in the output
        const u32 layer_stride =
            descriptor.layer_size != 0
                ? descriptor.layer_size
                : descriptor.getLevelOffset(descriptor.level_count);
        u32 offset = 0;
        for (u32 layer = 0; layer < descriptor.layer_count; layer++) {
            for (u32 level = 0; level < descriptor.level_count; level++) {
                const usize in_offset =
                    static_cast<usize>(layer) * layer_stride +
                    descriptor.getLevelOffset(level);

                // Calculate sizes
                const auto dims = descriptor.getLevelDimensions(level);
                const auto block_size_log2 =
                    descriptor.getLevelBlockSizeLog2(level);
                const u32 stride =
                    getTextureFormatStride(descriptor.format, static_cast<u32>(dims.width));
                const u32 rows =
                    getTextureFormatRows(descriptor.format, static_cast<u32>(dims.height));
                const u32 slice_stride = rows * stride;

                // Convert
                // The base level's rows are padded to whole blocks in X
                // (TIC tile width spacing), e.g. 4000 byte rows take 64 GOBs,
                // not 63. Mip levels aren't (like yuzu).
                convertBlockLinearToLinear(
                    align(stride, GOB_WIDTH << static_cast<u32>(block_size_log2.width)),
                    rows,
                    static_cast<u32>(dims.depth), static_cast<u32>(block_size_log2.height), static_cast<u32>(block_size_log2.depth),
                    in_data + in_offset,
                    [=](const u8* in_gob, u32 gob_x, u32 gob_y, u32 gob_z) {
                        const u32 x = gob_x * GOB_WIDTH;
                        if (x >= stride)
                            return;
                        for (u32 local_y = 0; local_y < GOB_HEIGHT; local_y++) {
                            const u32 y = gob_y * GOB_HEIGHT + local_y;
                            if (y >= rows)
                                break;

                            const usize crnt_offset =
                                offset +
                                static_cast<usize>(gob_z) * slice_stride +
                                static_cast<usize>(y) * stride + x;
                            std::memcpy(out_data + crnt_offset,
                                        in_gob + static_cast<usize>(local_y) *
                                                     GOB_WIDTH,
                                        std::min(GOB_WIDTH, stride - x));
                        }
                    });

                // Add offset
                offset += static_cast<u32>(dims.depth) * slice_stride;
            }
        }
    }

    storage.base->copyFrom(command_buffer, tmp_buffer);
    renderer.retireTextureUploadBuffer(command_buffer, tmp_buffer);
    if (PerfStats::isEnabled()) {
        auto& stats = PerfStats::instance();
        stats.texture_upload_bytes += size;
        stats.texture_upload_ns += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - upload_start)
                .count());
    }
}

} // namespace hydra::hw::tegra_x1::gpu::renderer
