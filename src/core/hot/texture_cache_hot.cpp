// Hot reloadable texture cache policies (HYDRA_HOT_RELOAD). Compiled into
// hydra-core and into the hydra-hot module.
#include "core/hot/table.hpp"
#include "core/hw/tegra_x1/gpu/capture/guest_events.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_events.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

namespace {
void reportUnsupportedAlias(const TextureDescriptor& destination,
                            const TextureDescriptor& source) {
    struct Window {
        TextureCacheTimePoint started{TextureCacheClock::now()};
        u64 attempts{0};
        u32 details{0};
    };
    static thread_local Window window;
    const auto now = TextureCacheClock::now();
    if (now - window.started >= std::chrono::seconds(2)) {
        if (window.attempts != 0)
            LOG_WARN(Gpu, "Unsupported texture alias synchronization: {} attempts "
                     "in the last interval ({} detailed warnings; capture a selected draw for bindings)",
                     window.attempts, window.details);
        window = {now, 0, 0};
    }
    ++window.attempts;
    if (window.details < 8) {
        ++window.details;
        LOG_WARN(Gpu, "Cannot synchronize textures ({}) and ({})", destination, source);
    }
    if (auto& visual = VisualDiagnostics::instance(); visual.selected())
        visual.event("unsupported_texture_alias", {{"destination", fmt::format("{}", destination)},
            {"source", fmt::format("{}", source)}, {"attempts_in_interval", std::to_string(window.attempts)}});
}
} // namespace

// DEBUG: live toggles, a file named <flag> in HYDRA_TRACE_DIR enables it
static bool debugFlag(const char* name) {
    static const char* dir = std::getenv("HYDRA_TRACE_DIR");
    if (dir == nullptr)
        return false;
    struct Entry {
        std::chrono::steady_clock::time_point checked;
        u32 calls{0};
        bool value{false};
    };
    // Keyed by the literal's address and only re-checked every 1024 calls:
    // this runs for every texture lookup
    static thread_local std::map<const char*, Entry> cache;
    auto& entry = cache[name];
    if ((entry.calls++ & 1023) != 0)
        return entry.value;
    const auto now = std::chrono::steady_clock::now();
    if (now - entry.checked > std::chrono::seconds(1)) {
        entry.checked = now;
        std::error_code ec;
        const bool value =
            std::filesystem::exists(std::filesystem::path(dir) / name, ec);
        if (value != entry.value)
            LOG_INFO(Gpu, "DEBUG flag {} = {}", name, value);
        entry.value = value;
    }
    return entry.value;
}

void TextureCache::invalidateMemoryImpl(ztd::Range<uptr> range) {
    auto it = entries.upper_bound(range.getBegin());
    if (it != entries.begin())
        it--;

    for (; it != entries.end() && it->first < range.getEnd(); it++) {
        auto& mem = it->second;

        if (mem.range.getEnd() <= range.getBegin())
            continue;

        if (mem.info.written_timestamp == TextureCacheTimePoint{}) {
            // Never rendered to, the whole memory needs to be updated
            mem.info.markModified();
            continue;
        }

        // The memory contains render targets. Their GPU contents are only in
        // guest memory once written back (flushMemory), so textures with
        // newer GPU writes are kept, everything else is updated.
        const auto now = TextureCacheClock::now();
        for (auto& [group_key, group] : mem.cache) {
            for (auto& [storage_key, storage] : group.cache) {
                if (storage.base == nullptr)
                    continue;
                if (storage.gpu_written_timestamp > storage.flushed_timestamp) {
                    // The guest wrote over a part of this render target that
                    // was never written back: what the GPU holds there is
                    // stale and must not be written over the guest's data by
                    // a later write back
                    if (storage.base->getDescriptor().getRange().intersects(
                            range))
                        LOG_TEXTURE_EVENT(
                            "guest_overwrote_unflushed_gpu_data",
                            storage.base->getDescriptor(),
                            fmt::format("range={:#x}+{:#x}", range.getBegin(),
                                        range.getSize()));
                    storage.markStale(
                        storage.base->getDescriptor().getRange().clampedTo(
                            range));
                    // Once a later write back marks this target flushed, the
                    // stale bytes are only replaced by loading guest memory
                    if (storage.base->getDescriptor().getRange().intersects(
                            range))
                        storage.cpu_modified_timestamp = now;
                    continue;
                }
                if (debugFlag("s2_oldinval") &&
                    storage.gpu_written_timestamp != TextureCacheTimePoint{})
                    continue;
                if (storage.base->getDescriptor().getRange().intersects(range)) {
                    // A render target that was written back before the guest
                    // overwrote this part: what the GPU holds there is stale
                    // as well. Without this mark an alias scan finds it
                    // "newer" than a texture created later and copies it, or
                    // writes it back over the guest's data (streamed textures
                    // reuse the memory of dead render targets)
                    if (storage.gpu_written_timestamp != TextureCacheTimePoint{})
                        storage.markStale(
                            storage.base->getDescriptor().getRange().clampedTo(
                                range));
                    storage.cpu_modified_timestamp = now;
                    LOG_TEXTURE_EVENT("guest_modified",
                                    storage.base->getDescriptor(),
                                    fmt::format("range={:#x}+{:#x}",
                                                range.getBegin(),
                                                range.getSize()));
                }
            }
        }
    }
}

void TextureCache::markGpuWritesInGuestImpl(ztd::Range<uptr> range) {
    auto it = entries.upper_bound(range.getBegin());
    if (it != entries.begin())
        it--;

    for (; it != entries.end() && it->first < range.getEnd(); it++) {
        auto& mem = it->second;
        if (mem.range.getEnd() <= range.getBegin())
            continue;

        for (auto& [group_key, group] : mem.cache) {
            for (auto& [storage_key, storage] : group.cache) {
                if (storage.base != nullptr &&
                    storage.base->getDescriptor().getRange().intersects(range))
                    storage.flushed_timestamp = storage.gpu_written_timestamp;
            }
        }
    }
}

namespace {

// Writes the packed levels and layers read back from a texture into guest
// memory, except for the excluded ranges (already written back, or rewritten
// by the guest since)
void writeBackToGuest(const TextureDescriptor& descriptor,
                      ztd::Range<uptr> range, const u8* in_data,
                      const std::vector<ztd::Range<uptr>>& excluded) {
    u8* out_data = reinterpret_cast<u8*>(descriptor.ptr);

    if (descriptor.is_linear) {
        // Linear textures often alias buffers the CPU also writes, so only
        // the requested bytes are written back
        const u32 stride =
            getTextureFormatStride(descriptor.format, descriptor.width);
        const u32 rows =
            getTextureFormatRows(descriptor.format, descriptor.height);
        for (u32 row = 0; row < rows; row++) {
            const uptr row_begin =
                descriptor.ptr +
                static_cast<uptr>(row) * descriptor.linear_stride;
            const uptr begin = std::max(row_begin, range.getBegin());
            const uptr end = std::min(row_begin + stride, range.getEnd());
            if (begin >= end)
                continue;
            TextureStorage::forEachOutside(
                excluded, ztd::Range<uptr>(begin, end),
                [&](ztd::Range<uptr> piece) {
                    std::memcpy(reinterpret_cast<u8*>(piece.getBegin()),
                                in_data + static_cast<usize>(row) * stride +
                                    (piece.getBegin() - row_begin),
                                piece.getSize());
                    capture::GuestEvents::instance().memory(
                        "memory_writeback", piece.getBegin(), piece.getSize(),
                        0, capture::currentGuestEventOrigin());
                });
        }
    } else {
        const u32 layer_stride =
            descriptor.layer_size != 0
                ? descriptor.layer_size
                : descriptor.getLevelOffset(descriptor.level_count);
        u32 offset = 0;
        for (u32 layer = 0; layer < descriptor.layer_count; layer++) {
            for (u32 level = 0; level < descriptor.level_count; level++) {
                const usize out_offset =
                    static_cast<usize>(layer) * layer_stride +
                    descriptor.getLevelOffset(level);
                const auto dims = descriptor.getLevelDimensions(level);
                const auto block_size_log2 =
                    descriptor.getLevelBlockSizeLog2(level);
                const u32 stride =
                    getTextureFormatStride(descriptor.format, static_cast<u32>(dims.width));
                const u32 rows =
                    getTextureFormatRows(descriptor.format, static_cast<u32>(dims.height));
                const u8* level_data = in_data + offset;
                const uptr level_begin = descriptor.ptr + out_offset;
                const u32 level_size = descriptor.getLevelSize(level);
                const uptr begin = std::max(level_begin, range.getBegin());
                const uptr end =
                    std::min(level_begin + level_size, range.getEnd());
                if (begin >= end) {
                    offset += static_cast<u32>(dims.depth) * rows * stride;
                    continue;
                }
                // CPU page faults and DMA copies can request only a few bytes
                // of a tiled surface. Retiling directly into guest memory
                // overwrites unrelated CPU uploads and other texture mips.
                // Preserve padding as well as bytes outside the request.
                std::vector<u8> tiled(level_size);
                std::memcpy(tiled.data(), out_data + out_offset, level_size);
                auto* tiled_data = tiled.data();
                const u32 tiled_stride =
                    align(stride, GOB_WIDTH << static_cast<u32>(block_size_log2.width));
                convertLinearToBlockLinear(
                    tiled_stride, rows, static_cast<u32>(dims.depth), static_cast<u32>(block_size_log2.height),
                    static_cast<u32>(block_size_log2.depth),
                    [=](u32 gob_x, u32 gob_y, u32 gob_z, u8* out_gob) {
                        const u64 gob_offset = getBlockLinearOffset(
                            gob_x * GOB_WIDTH, gob_y * GOB_HEIGHT, gob_z,
                            tiled_stride, rows, static_cast<u32>(block_size_log2.height),
                            static_cast<u32>(block_size_log2.depth));
                        convertBlockLinearToLinear(
                            GOB_WIDTH, GOB_WIDTH, GOB_SIZE, GOB_HEIGHT, 1, 0, 0,
                            tiled_data + gob_offset, out_gob);
                        const u32 x = gob_x * GOB_WIDTH;
                        if (x >= stride)
                            return;
                        for (u32 local_y = 0; local_y < GOB_HEIGHT; local_y++) {
                            const u32 y = gob_y * GOB_HEIGHT + local_y;
                            if (y >= rows)
                                break;
                            std::memcpy(out_gob + local_y * GOB_WIDTH,
                                        level_data +
                                            static_cast<usize>(gob_z) * rows *
                                                stride +
                                            static_cast<usize>(y) * stride + x,
                                        std::min(GOB_WIDTH, stride - x));
                        }
                    },
                    tiled.data());
                TextureStorage::forEachOutside(
                    excluded, ztd::Range<uptr>(begin, end),
                    [&](ztd::Range<uptr> piece) {
                        std::memcpy(reinterpret_cast<void*>(piece.getBegin()),
                                    tiled.data() + (piece.getBegin() - level_begin),
                                    piece.getSize());
                        capture::GuestEvents::instance().memory(
                            "memory_writeback", piece.getBegin(),
                            piece.getSize(), 0,
                            capture::currentGuestEventOrigin());
                    });
                offset += static_cast<u32>(dims.depth) * rows * stride;
            }
        }
    }
}

// Readbacks encoded while a batch is open wait for the GPU once, when the
// batch finishes, instead of once per texture. They are written to guest
// memory in the order they were encoded.
struct PendingWriteback {
    TextureStorage* storage;
    ztd::Range<uptr> range;
    BufferBase* buffer;
    std::vector<ztd::Range<uptr>> excluded;
};
thread_local std::vector<PendingWriteback>* tls_writeback_batch = nullptr;

class WritebackBatch {
  public:
    WritebackBatch(IRenderer& renderer_, ICommandBuffer* command_buffer_)
        : renderer{renderer_}, command_buffer{command_buffer_},
          previous{tls_writeback_batch} {
        tls_writeback_batch = &pending;
    }
    ~WritebackBatch() {
        finish();
        tls_writeback_batch = previous;
    }
    WritebackBatch(const WritebackBatch&) = delete;
    WritebackBatch& operator=(const WritebackBatch&) = delete;

    // Guest memory holds every readback encoded so far afterwards
    void finish() {
        if (pending.empty())
            return;
        {
            ScopedCheckpointReason reason(command_buffer,
                                          CheckpointReason::TextureReadback);
            renderer.debugSyncCommandBuffer(command_buffer);
        }
        for (auto& writeback : pending) {
            const auto& descriptor = writeback.storage->base->getDescriptor();
            writeBackToGuest(
                descriptor, writeback.range,
                reinterpret_cast<const u8*>(writeback.buffer->getPtr()),
                writeback.excluded);
            writeback.storage->markFlushed(
                writeback.range.clampedTo(descriptor.getRange()));
            renderer.retireTextureUploadBuffer(command_buffer,
                                               writeback.buffer);
        }
        pending.clear();
    }

  private:
    IRenderer& renderer;
    ICommandBuffer* command_buffer;
    std::vector<PendingWriteback>* previous;
    std::vector<PendingWriteback> pending;
};

} // namespace

void TextureCache::updateImpl(ICommandBuffer* command_buffer,
                          TextureStorage& storage, TextureMem& mem,
                          TextureUsage usage) {
    // The guest overwrote parts of a render target since the GPU wrote it, and
    // the texture is read now (for example through a view of another format
    // of the same memory). What the GPU holds in those parts is stale: write
    // back the parts that are still fresh, then load everything from guest
    // memory. Render targets also need this reconciliation: a draw can keep
    // pixels outside its viewport or scissor.
    if (!storage.stale_ranges.empty() &&
        storage.gpu_written_timestamp > storage.flushed_timestamp) {
        const auto storage_range = storage.base->getDescriptor().getRange();
        bool fresh = false;
        storage.forEachFresh(storage_range,
                             [&](ztd::Range<uptr>) { fresh = true; });
        LOG_TEXTURE_EVENT("reconcile_stale_render_target",
                        storage.base->getDescriptor(),
                        fmt::format("fresh_parts_written_back={}", fresh));
        if (fresh)
            encodeTexture(command_buffer, storage, storage_range);
        storage.stale_ranges.clear();
        storage.flushed_ranges.clear();
        storage.flushed_timestamp = storage.gpu_written_timestamp;
        // Everything is in guest memory now: load it
        storage.cpu_modified_timestamp = TextureCacheClock::now();
    }
    bool sync = false;
    bool uploaded_cpu = false;
    bool alias_sync_complete = true;
    // Guest memory doesn't hold unflushed GPU writes, never reload those from
    // it. CPU accesses write them back first (memory tracking faults).
    // Without that write back (stage 1 tracking), guest memory is the newer
    // copy once the CPU rewrites it.
    const bool gpu_dirty =
        storage.gpu_written_timestamp > storage.flushed_timestamp &&
        tls_crnt_gmmu != nullptr && tls_crnt_gmmu->getMmu() != nullptr &&
        tls_crnt_gmmu->getMmu()->protectsGpuWrites();
    // DEBUG: "reload_buffers" reloads buffer textures on every use
    const auto& reload_descriptor = storage.base->getDescriptor();
    const bool reload_buffers = reload_descriptor.type == MTL::TextureTypeTextureBuffer &&
                                usage == TextureUsage::Read && debugFlag("reload_buffers");
    const bool reload_sampled = debugFlag("reload_sampled");
    // DEBUG: "reload_always" reloads every use of a texture the GPU never
    // wrote, to tell stale CPU data from other corruption
    const bool reload_always = debugFlag("reload_always");
    const bool cpu_only_range =
        (!reload_buffers && !reload_sampled && !reload_always) ||
        !mem.hasGpuWriteSince(reload_descriptor.getRange(), TextureCacheTimePoint{});
    bool force_reload =
        cpu_only_range &&
        (reload_buffers ||
         (reload_always && usage == TextureUsage::Read &&
          storage.gpu_written_timestamp == TextureCacheTimePoint{}));
    // DEBUG: "reload_sampled" reloads once every texture the GPU never wrote
    // (their contents only come from guest memory)
    {
        static std::set<const void*> reloaded;
        if (!reload_sampled)
            reloaded.clear();
        else if (cpu_only_range && usage == TextureUsage::Read &&
                 storage.gpu_written_timestamp == TextureCacheTimePoint{} &&
                 reloaded.insert(storage.base).second)
            force_reload = true;
    }
    if (!gpu_dirty &&
        (force_reload ||
         storage.update_timestamp < mem.info.modified_timestamp ||
         storage.update_timestamp < storage.cpu_modified_timestamp)) {
        // If modified by the guest
        // DEBUG: log each storage reloaded after its first upload once
        if (storage.update_timestamp != TextureCacheTimePoint{} &&
            debugFlag("log_reupload")) {
            static std::set<std::pair<uptr, u32>> logged;
            const auto& d = storage.base->getDescriptor();
            if (logged.insert({d.ptr, static_cast<u32>(d.format)}).second)
                LOG_INFO(Gpu,
                         "DEBUG reupload {} {} {}x{}x{} levels {} ptr {:#x} "
                         "gpu_written {}",
                         d.type, d.format, d.width, d.height, d.depth,
                         d.level_count, d.ptr,
                         storage.gpu_written_timestamp !=
                             TextureCacheTimePoint{});
        }
        LOG_TEXTURE_EVENT(
            "reload_from_guest", storage.base->getDescriptor(),
            fmt::format("forced={} mem_modified={} cpu_modified={} first={}",
                        force_reload,
                        storage.update_timestamp < mem.info.modified_timestamp,
                        storage.update_timestamp < storage.cpu_modified_timestamp,
                        storage.update_timestamp == TextureCacheTimePoint{}));
        decodeTexture(command_buffer, storage);
        uploaded_cpu = true;
        // CPU uploads establish a baseline, but newer GPU writes into aliases
        // must still be applied before this texture is sampled.
        storage.update_timestamp = std::max(mem.info.modified_timestamp,
                                            storage.cpu_modified_timestamp);
        sync = true;
    }
    struct FailedAliasScan {
        TextureCacheTimePoint updated, cpu_modified, self_written, observed;
    };
    static thread_local std::map<const ITexture*, FailedAliasScan> failed_scans;
    auto scan_timestamp = std::max(storage.update_timestamp, storage.gpu_written_timestamp);
    if (const auto it = failed_scans.find(storage.base); it != failed_scans.end()) {
        const auto& previous = it->second;
        if (previous.updated == storage.update_timestamp &&
            previous.cpu_modified == storage.cpu_modified_timestamp &&
            previous.self_written == storage.gpu_written_timestamp) {
            scan_timestamp = std::max(scan_timestamp, previous.observed);
            alias_sync_complete = false;
        } else failed_scans.erase(it);
    }
    // Right after the upload from guest memory, GPU data that was never
    // written back is missing from this texture whatever the timestamps say:
    // the memory timestamps cover the whole (possibly merged) memory, so a
    // guest write somewhere else in it can look newer than a GPU write here
    if (storage.update_timestamp < mem.info.written_timestamp || uploaded_cpu) {
        // Scan observations are separate from content freshness. An unsupported
        // alias needs another attempt only after overlapping GPU data changes.
        // Other textures in this memory changed, let's copy them
        const auto base = storage.base;
        const auto& descriptor = base->getDescriptor();
        const auto range = descriptor.getRange();
        // Replay incompatible aliases through guest bytes in write order.
        // Preserve this destination's GPU data before decoding it again.
        bool decode_pending = false;
        bool aliases_replayed = false;
        const auto sync_via_guest = [&](TextureStorage& source) {
            if (debugFlag("no_alias_via_guest"))
                return false;
            std::vector<TextureStorage*> replay;
            for (auto& [group_key, group] : mem.cache) {
                for (auto& [storage_key, candidate] : group.cache) {
                    if (candidate.base != nullptr &&
                        candidate.gpu_written_timestamp != TextureCacheTimePoint{} &&
                        range.intersects(candidate.base->getDescriptor().getRange()))
                        replay.push_back(&candidate);
                }
            }
            std::sort(replay.begin(), replay.end(), [](const auto* a, const auto* b) {
                return a->gpu_written_timestamp < b->gpu_written_timestamp;
            });
            WritebackBatch batch(renderer, command_buffer);
            for (auto* candidate : replay) {
                // A newer alias already flushed to guest must be replayed too:
                // an older unflushed destination could otherwise overwrite it.
                // CPU-written bytes remain excluded, including tiled padding.
                auto flushed = std::move(candidate->flushed_ranges);
                candidate->flushed_ranges = candidate->stale_ranges;
                encodeTexture(command_buffer, *candidate,
                    range.clampedTo(candidate->base->getDescriptor().getRange()));
                for (const auto& previous : flushed)
                    candidate->markFlushed(previous);
            }
            // The decode below reads what was written back
            batch.finish();
            LOG_TEXTURE_EVENT("alias_via_guest", descriptor,
                            fmt::format("source={:#x} {} {}",
                                        source.base->getDescriptor().ptr,
                                        source.base->getDescriptor().type,
                                        source.base->getDescriptor().format));
            aliases_replayed = true;
            decode_pending = true;
            return true;
        };
        const auto flush_decode = [&] {
            if (!decode_pending)
                return;
            decode_pending = false;
            decodeTexture(command_buffer, storage);
        };
        // A shared memory interval can contain thousands of sampled textures
        // but only a few render targets. Avoid walking all storages when none
        // of those targets wrote to this texture's address range.
        static const bool disable_range_filter =
            std::getenv("HYDRA_DISABLE_TEXTURE_WRITE_FILTER") != nullptr;
        if (disable_range_filter ||
            mem.hasGpuWriteSince(range, scan_timestamp) ||
            (uploaded_cpu &&
             mem.hasGpuWriteSince(range, TextureCacheTimePoint{}))) {
            alias_sync_complete = true;
            if (PerfStats::isEnabled())
                PerfStats::instance().texture_sync_scans++;
            std::vector<TextureStorage*> sources;
            for (auto& [group_key, group] : mem.cache) {
                for (auto& [storage_key, candidate] : group.cache) {
                    // GPU data that never reached guest memory (outside what
                    // was written back, and not overwritten by the guest)
                    bool unflushed = false;
                    if (uploaded_cpu && &candidate != &storage &&
                        candidate.base != nullptr &&
                        candidate.gpu_written_timestamp >
                            candidate.flushed_timestamp)
                        candidate.forEachUnflushed(
                            range.clampedTo(
                                candidate.base->getDescriptor().getRange()),
                            [&](ztd::Range<uptr>) { unflushed = true; });
                    if (&candidate != &storage &&
                        (candidate.gpu_written_timestamp >
                             std::max(storage.update_timestamp,
                                      storage.gpu_written_timestamp) ||
                         unflushed) &&
                        range.intersects(
                            candidate.base->getDescriptor().getRange())) {
                        scan_timestamp = std::max(scan_timestamp, candidate.gpu_written_timestamp);
                        // A render target the guest overwrote since it was
                        // rendered to holds stale data in the overlap (its
                        // memory was reused, e.g. by streamed textures): it
                        // must not be copied over this texture
                        bool fresh = false;
                        candidate.forEachFresh(
                            range.clampedTo(
                                candidate.base->getDescriptor().getRange()),
                            [&](ztd::Range<uptr>) { fresh = true; });
                        if (!fresh) {
                            LOG_TEXTURE_EVENT(
                                "alias_sync_skipped_stale_source",
                                storage.base->getDescriptor(),
                                fmt::format(
                                    "source={:#x} {} {}",
                                    candidate.base->getDescriptor().ptr,
                                    candidate.base->getDescriptor().type,
                                    candidate.base->getDescriptor().format));
                            continue;
                        }
                        sources.push_back(&candidate);
                    }
                }
            }
            // Apply overlapping writes in time order so an older alias cannot
            // overwrite the newer contents of another alias.
            std::sort(sources.begin(), sources.end(),
                      [](const auto* a, const auto* b) {
                          return a->gpu_written_timestamp <
                                 b->gpu_written_timestamp;
                      });
            for (auto* source : sources) {
                if (aliases_replayed)
                    break;
                auto& other_storage = *source;
                const auto& other_descriptor =
                    other_storage.base->getDescriptor();
                const auto other_range = other_descriptor.getRange();

                if (range.intersects(other_range)) {
                    LOG_TEXTURE_EVENT(
                        "alias_sync", descriptor,
                        fmt::format("source={:#x} {} {} written_ms_after_update={}",
                                    other_descriptor.ptr, other_descriptor.type,
                                    other_descriptor.format,
                                    std::chrono::duration_cast<
                                        std::chrono::milliseconds>(
                                        other_storage.gpu_written_timestamp -
                                        storage.update_timestamp)
                                        .count()));
                    // The guest overwrote part of this render target since
                    // the GPU wrote it. A direct GPU copy would carry the
                    // stale part over the guest's data in the overlap, so
                    // replay through guest memory: only the fresh parts are
                    // written back, then everything is loaded from there
                    {
                        const auto overlap = range.clampedTo(other_range);
                        bool partly_stale = false;
                        for (const auto& stale : other_storage.stale_ranges)
                            partly_stale |= stale.intersects(overlap);
                        if (partly_stale) {
                            if (!sync_via_guest(other_storage)) {
                                alias_sync_complete = false;
                                reportUnsupportedAlias(descriptor,
                                                       other_descriptor);
                            }
                            continue;
                        }
                    }
                    // Expanded texels have different host byte offsets. Raw
                    // alias copies cannot reinterpret them as guest bytes.
                    const bool expanded =
                        getHostTextureFormat(descriptor.format) != descriptor.format ||
                        getHostTextureFormat(other_descriptor.format) != other_descriptor.format;
                    if (expanded &&
                        (descriptor.format != other_descriptor.format ||
                         descriptor.type == MTL::TextureTypeTextureBuffer ||
                         other_descriptor.type == MTL::TextureTypeTextureBuffer)) {
                        if (!sync_via_guest(other_storage)) {
                            alias_sync_complete = false;
                            reportUnsupportedAlias(descriptor,
                                                   other_descriptor);
                        }
                        continue;
                    }
                    const auto type_class =
                        getTextureTypeClass(descriptor.type);
                    const auto other_type_class =
                        getTextureTypeClass(other_descriptor.type);
                    if (type_class == TextureTypeClass::_2D &&
                        other_type_class == TextureTypeClass::_2D) {
                        flush_decode();
                        // DEBUG: "no_block_alias" skips the direct GPU copy
                        // between block data and the texels standing in for
                        // it (BC mips streamed through RG32/RGBA32 targets)
                        const bool skip_direct =
                            isTextureFormatCompressed(descriptor.format) !=
                                isTextureFormatCompressed(
                                    other_descriptor.format) &&
                            debugFlag("no_block_alias");
                        if ((skip_direct ||
                             !synchronize2DWith2D(command_buffer, storage,
                                                  other_storage)) &&
                            !sync_via_guest(other_storage)) {
                            alias_sync_complete = false;
                            // DEBUG: report textures that alias but cannot be synchronized
                            static u32 debug_count = 0;
                            if (debug_count++ < 20)
                                LOG_WARN(Gpu, "DEBUG cannot synchronize 2D textures ({}) and ({})",
                                         descriptor, other_descriptor);
                        }
                    } else if (type_class == TextureTypeClass::_1DBuffer &&
                               other_type_class ==
                                   TextureTypeClass::_1DBuffer) {
                        flush_decode();
                        const auto begin =
                            std::max(range.getBegin(), other_range.getBegin());
                        const auto end =
                            std::min(range.getEnd(), other_range.getEnd());
                        storage.base->copyBufferBytes(
                            command_buffer, other_storage.base,
                            begin - other_range.getBegin(),
                            begin - range.getBegin(), end - begin);
                    } else if ((descriptor.type == MTL::TextureTypeTextureBuffer &&
                                other_descriptor.type == MTL::TextureType2D) ||
                               (descriptor.type == MTL::TextureType2D &&
                                other_descriptor.type == MTL::TextureTypeTextureBuffer)) {
                        const auto& surface = descriptor.type == MTL::TextureType2D
                                                  ? descriptor : other_descriptor;
                        if (surface.level_count == 1 && surface.layer_count == 1 &&
                            surface.depth == 1 && !isTextureFormatCompressed(surface.format) &&
                            !isTextureFormatDepthOrStencil(surface.format)) {
                            flush_decode();
                            const auto begin = std::max(range.getBegin(), other_range.getBegin());
                            const auto end = std::min(range.getEnd(), other_range.getEnd());
                            storage.base->copyBufferBytes(command_buffer, other_storage.base,
                                begin - other_range.getBegin(), begin - range.getBegin(), end - begin);
                        } else if (!sync_via_guest(other_storage)) {
                            alias_sync_complete = false;
                            reportUnsupportedAlias(descriptor, other_descriptor);
                        }
                    } else if (type_class == TextureTypeClass::_3D &&
                               other_type_class == TextureTypeClass::_3D) {
                        flush_decode();
                        if (!synchronize3DWith3D(command_buffer, storage,
                                                 other_storage) &&
                            !sync_via_guest(other_storage)) {
                            alias_sync_complete = false;
                            reportUnsupportedAlias(descriptor, other_descriptor);
                        }
                    } else if (!sync_via_guest(other_storage)) {
                        alias_sync_complete = false;
                        reportUnsupportedAlias(descriptor, other_descriptor);
                    }
                }
            }
        } else if (PerfStats::isEnabled()) {
            PerfStats::instance().texture_sync_skips++;
        }
        flush_decode();

        if (alias_sync_complete) {
            failed_scans.erase(storage.base);
            storage.markUpdated();
        } else {
            if (!failed_scans.contains(storage.base) && failed_scans.size() >= 4096)
                failed_scans.erase(failed_scans.begin());
            failed_scans[storage.base] = {storage.update_timestamp,
                storage.cpu_modified_timestamp, storage.gpu_written_timestamp,
                std::max(scan_timestamp, mem.info.written_timestamp)};
        }
    } else if (mem.info.written_timestamp == TextureCacheTimePoint{}) {
        // Never written to
        if (usage == TextureUsage::Present) {
            // Presented, but never written to
            sync = true;
        } else if (usage == TextureUsage::Read) {
            // Read, but never written to
        }
    }

    if (sync && !uploaded_cpu && usage == TextureUsage::Present &&
        mem.info.written_timestamp == TextureCacheTimePoint{})
        decodeTexture(command_buffer, storage);

    if (usage == TextureUsage::Read)
        mem.info.markRead();
    else if (usage == TextureUsage::Write) {
        mem.info.markWritten();
        // Guest memory no longer holds the contents: make CPU accesses fault
        // so they are written back first
        if (storage.gpu_written_timestamp <= storage.flushed_timestamp &&
            tls_crnt_gmmu != nullptr && tls_crnt_gmmu->getMmu() != nullptr &&
            !debugFlag("s2_noprotect"))
            tls_crnt_gmmu->getMmu()->protectGpuWritten(
                storage.base->getDescriptor().getRange());
        storage.gpu_written_timestamp = TextureCacheClock::now();
        // The GPU data is fresh everywhere again
        storage.flushed_ranges.clear();
        storage.stale_ranges.clear();
        mem.recordGpuWrite(storage.base->getDescriptor().getRange(),
                           storage.gpu_written_timestamp);
    }

    if (alias_sync_complete && (usage == TextureUsage::Write || sync))
        storage.markUpdated();
}

void TextureCache::flushMemoryImpl(ICommandBuffer* command_buffer,
                               ztd::Range<uptr> range) {
    std::vector<TextureStorage*> storages;
    auto it = entries.upper_bound(range.getBegin());
    if (it != entries.begin())
        it--;
    for (; it != entries.end() && it->first < range.getEnd(); it++) {
        auto& mem = it->second;
        if (mem.range.getEnd() <= range.getBegin() ||
            mem.info.written_timestamp == TextureCacheTimePoint{})
            continue;

        for (auto& [group_key, group] : mem.cache) {
            for (auto& [storage_key, storage] : group.cache) {
                if (storage.base == nullptr ||
                    storage.gpu_written_timestamp <= storage.flushed_timestamp)
                    continue;
                const auto& descriptor = storage.base->getDescriptor();
                if (!ztd::Range<uptr>::fromSize(descriptor.ptr, descriptor.size)
                         .intersects(range))
                    continue;
                storages.push_back(&storage);
            }
        }
    }

    // Aliases may contain snapshots from different GPU writes. Replay them
    // oldest first so an older storage cannot overwrite newer guest bytes.
    std::sort(storages.begin(), storages.end(), [](const auto* a, const auto* b) {
        return a->gpu_written_timestamp < b->gpu_written_timestamp;
    });
    WritebackBatch batch(renderer, command_buffer);
    for (auto* storage : storages)
        encodeTexture(command_buffer, *storage, range);
}

void TextureCache::encodeTextureImpl(ICommandBuffer* command_buffer,
                                 TextureStorage& storage,
                                 ztd::Range<uptr> range) {
    ScopedGpuWork work(GpuWork::Texture);
    const auto& descriptor = storage.base->getDescriptor();
    LOG_TEXTURE_EVENT("writeback_to_guest", descriptor,
                    fmt::format("range={:#x}+{:#x}", range.getBegin(),
                                range.getSize()));
    // A partial readback does not make the rest of the GPU storage coherent.
    if (range.getBegin() <= descriptor.ptr &&
        range.getEnd() >= descriptor.ptr + descriptor.size)
        storage.flushed_timestamp = storage.gpu_written_timestamp;

    // Copy every level and layer into a packed buffer
    u32 size = 0;
    for (u32 level = 0; level < descriptor.level_count; level++) {
        const auto dims = descriptor.getLevelDimensions(level);
        size += static_cast<u32>(dims.depth) *
                getTextureFormatStride(descriptor.format, static_cast<u32>(dims.width)) *
                getTextureFormatRows(descriptor.format, static_cast<u32>(dims.height));
    }
    size *= descriptor.layer_count;
    auto tmp_buffer = renderer.allocateTextureUploadBuffer(size);

    auto view = getTextureView(
        storage, TextureViewDescriptor(
                     descriptor.type, descriptor.format,
                     ztd::Range<u32>(0, descriptor.level_count),
                     ztd::Range<u32>(0, descriptor.layer_count)));
    u32 offset = 0;
    for (u32 layer = 0; layer < descriptor.layer_count; layer++) {
        for (u32 level = 0; level < descriptor.level_count; level++) {
            const auto dims = descriptor.getLevelDimensions(level);
            tmp_buffer->copyFrom(command_buffer, view, MTL::Origin(0, 0, 0),
                                 dims,
                                 ztd::Range<u32>::fromSize(level, 1),
                                 ztd::Range<u32>::fromSize(layer, 1), offset);
            offset += static_cast<u32>(dims.depth) *
                      getTextureFormatStride(descriptor.format, static_cast<u32>(dims.width)) *
                      getTextureFormatRows(descriptor.format, static_cast<u32>(dims.height));
        }
    }
    // Descriptor formatting and aggregation are diagnostics, not part of
    // readback/coherence. Keep them off the normal CPU render path.
    static const bool trace_readbacks =
        std::getenv("HYDRA_TRACE_TEXTURE_READBACK") != nullptr;
    if (trace_readbacks) {
        static std::map<std::string, u32> seen;
        static auto last = std::chrono::steady_clock::now();
        const auto origin = tls_texture_flush_origin;
        seen[fmt::format("from {}:{} ptr={:#x} {} {}x{} lin={} lv={} ly={} range={:#x}+{:#x}",
                         origin.function ? origin.function : "?", origin.line,
                         descriptor.ptr, descriptor.format,
                         descriptor.width, descriptor.height, descriptor.is_linear,
                         descriptor.level_count, descriptor.layer_count,
                         range.getBegin() - descriptor.ptr, range.getSize())]++;
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(2)) {
            last = std::chrono::steady_clock::now();
            for (const auto& [key, count] : seen)
                LOG_INFO(Gpu, "DEBUG readback: {} x{}", key, count);
            seen.clear();
        }
    }
    // The ranges to skip are those at encoding time: callers may change them
    // before a batched write back happens
    auto excluded = storage.flushed_ranges;
    if (tls_writeback_batch != nullptr) {
        tls_writeback_batch->push_back(
            {&storage, range, tmp_buffer, std::move(excluded)});
        return;
    }
    {
        ScopedCheckpointReason reason(command_buffer,
                                      CheckpointReason::TextureReadback);
        renderer.debugSyncCommandBuffer(command_buffer);
    }

    writeBackToGuest(descriptor, range,
                     reinterpret_cast<const u8*>(tmp_buffer->getPtr()),
                     excluded);
    // What the GPU held in the range is in guest memory now
    storage.markFlushed(range.clampedTo(descriptor.getRange()));
    renderer.retireTextureUploadBuffer(command_buffer, tmp_buffer);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer
