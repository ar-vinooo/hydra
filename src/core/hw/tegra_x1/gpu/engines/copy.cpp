#include "core/hw/tegra_x1/gpu/engines/copy.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/capture/guest_events.hpp"

#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"

namespace hydra::hw::tegra_x1::gpu::engines {

DEFINE_METHOD_TABLE(Copy, 0xc0, 1, launchDma, LaunchDMAData)

namespace {

// LINE_COUNT may cover more than one Z slice. Each slice starts at the
// programmed Y origin; LAYER selects the initial slice.
u32 availableLines(const TextureCopyInfo& info) {
    if (info.origin.y >= info.height || info.layer >= info.depth)
        return 0;
    return static_cast<u32>(std::min<u64>(
        u64(info.height - info.origin.y) * (info.depth - info.layer),
        std::numeric_limits<u32>::max()));
}

std::pair<u32, u32> sliceRow(const TextureCopyInfo& info, u32 line) {
    const u32 rows = info.height - info.origin.y;
    return {info.origin.y + line % rows, info.layer + line / rows};
}

u64 pitchRow(const TextureCopyInfo& info, u32 line) {
    const u32 rows = info.height - info.origin.y;
    return u64(line / rows) * info.height + line % rows;
}

// Copies a sub rectangle between pitch and block linear memory, honouring the
// origin and initial layer of the block linear surface
void copySubrect(u8* pitch, u32 pitch_stride, u8* block_linear,
                 const TextureCopyInfo& info, u32 width, u32 bytes_per_pixel,
                 u32 line_length, u32 line_count, bool to_block_linear) {
    const u32 block_height_log2 = static_cast<u32>(
        std::max(getBlockSizeLog2(info.block_size.height), 0));
    const u32 block_depth_log2 = static_cast<u32>(
        std::max(getBlockSizeLog2(info.block_size.depth), 0));
    const u32 origin_x = info.origin.x * bytes_per_pixel;
    for (u32 line = 0; line < line_count; line++) {
        const auto [y, z] = sliceRow(info, line);
        u8* pitch_line = pitch + pitchRow(info, line) * pitch_stride;
        for (u32 i = 0; i < line_length;) {
            const u32 x = origin_x + i;
            // Contiguous bytes until the next 16 byte boundary
            const u32 bytes = std::min(16 - x % 16, line_length - i);
            u8* bl = block_linear +
                     getBlockLinearOffset(x, y, z, width, info.height,
                                          block_height_log2, block_depth_log2);
            if (to_block_linear)
                std::memcpy(bl, pitch_line + i, bytes);
            else
                std::memcpy(pitch_line + i, bl, bytes);
            i += bytes;
        }
    }
}

// Copies a sub rectangle between two block linear surfaces
void copyBlockLinear(const u8* src, const TextureCopyInfo& src_info,
                     u32 src_width, u8* dst, const TextureCopyInfo& dst_info,
                     u32 dst_width, u32 bytes_per_pixel, u32 line_length,
                     u32 line_count) {
    const auto log2 = [](const BlockSize& size, bool depth) {
        return static_cast<u32>(std::max(
            getBlockSizeLog2(depth ? size.depth : size.height), 0));
    };
    const u32 src_height_log2 = log2(src_info.block_size, false);
    const u32 src_depth_log2 = log2(src_info.block_size, true);
    const u32 dst_height_log2 = log2(dst_info.block_size, false);
    const u32 dst_depth_log2 = log2(dst_info.block_size, true);
    const u32 src_origin_x = src_info.origin.x * bytes_per_pixel;
    const u32 dst_origin_x = dst_info.origin.x * bytes_per_pixel;
    for (u32 line = 0; line < line_count; line++) {
        const auto [src_y, src_z] = sliceRow(src_info, line);
        const auto [dst_y, dst_z] = sliceRow(dst_info, line);
        for (u32 i = 0; i < line_length;) {
            const u32 src_x = src_origin_x + i;
            const u32 dst_x = dst_origin_x + i;
            // Contiguous bytes of both surfaces until their next 16 byte
            // boundary
            const u32 bytes = std::min(
                {16 - src_x % 16, 16 - dst_x % 16, line_length - i});
            std::memcpy(dst + getBlockLinearOffset(
                                  dst_x, dst_y, dst_z, dst_width,
                                  dst_info.height, dst_height_log2,
                                  dst_depth_log2),
                        src + getBlockLinearOffset(
                                  src_x, src_y, src_z, src_width,
                                  src_info.height, src_height_log2,
                                  src_depth_log2),
                        bytes);
            i += bytes;
        }
    }
}

} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

void Copy::remapPitchCopy(const LaunchDMAData data) {
    // Each of the line_length_in elements has dst_component_count components
    // of component_size bytes, taken from a source component, a constant or
    // left unwritten. Games clear buffers with constants (src unused).
    enum : u32 { ConstA = 4, ConstB = 5, NoWrite = 6 };
    const auto& c = regs.remap_components;
    const u32 component_size = c.component_size_minus_one + 1u;
    const u32 src_components = c.src_component_count_minus_one + 1u;
    const u32 dst_components = c.dst_component_count_minus_one + 1u;
    const std::array<u32, 4> selects{c.dst_x, c.dst_y, c.dst_z, c.dst_w};
    bool reads_src = false;
    bool writes_all = true;
    for (u32 i = 0; i < dst_components; i++) {
        if (selects[i] < ConstA)
            reads_src = true;
        else if (selects[i] != ConstA && selects[i] != ConstB)
            writes_all = false;
    }

    const u32 lines = data.multi_line_enable ? regs.line_count : 1;
    if (lines == 0 || regs.line_length_in == 0)
        return;
    const u64 src_line = u64(regs.line_length_in) * src_components * component_size;
    const u64 dst_line = u64(regs.line_length_in) * dst_components * component_size;
    const auto host = [](uptr gpu_addr, u64 size) -> u8* {
        uptr base;
        const auto& as = tls_crnt_gmmu->findAddrImpl(gpu_addr, base);
        if (gpu_addr - base + size > as.size)
            return nullptr;
        return reinterpret_cast<u8*>(tls_crnt_gmmu->unmapAddr(gpu_addr));
    };
    const u64 src_span = u64(regs.stride_in) * (lines - 1) + src_line;
    const u64 dst_span = u64(regs.stride_out) * (lines - 1) + dst_line;
    u8* src = reads_src ? host(regs.offset_in, src_span) : nullptr;
    u8* dst = host(regs.offset_out, dst_span);
    if (dst == nullptr || (reads_src && src == nullptr)) {
        LOG_WARN(Engines, "Remapped copy spans several mappings, skipped");
        return;
    }
    if (capture::GuestEvents::instance().enabled()) {
        resource::ContentManifest accesses;
        if (reads_src)
            accesses.addRead(gpu.getRenderer().getContentVersions(),
                             ztd::Range<uptr>::fromSize(
                                 reinterpret_cast<uptr>(src), src_span));
        accesses.addWrite(
            ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(dst), dst_span));
        capture::recordGuestCommand(
            "dma_access", capture::currentGuestEventOrigin(), accesses);
    }

    {
        auto& texture_cache = gpu.getRenderer().getTextureCache();
        std::scoped_lock lock(texture_cache.getMutex());
        if (reads_src)
            texture_cache.flushMemory(
                tls_crnt_command_buffer,
                ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(src), src_span));
        const auto dst_range =
            ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(dst), dst_span);
        // A complete overwrite needs no readback of GPU written textures
        if (writes_all && (lines == 1 || regs.stride_out == dst_line))
            texture_cache.discardGpuWrites(tls_crnt_command_buffer, dst_range);
        else
            texture_cache.flushMemory(tls_crnt_command_buffer, dst_range);
    }

    const u32 src_element = src_components * component_size;
    const u32 dst_element = dst_components * component_size;
    std::vector<u8> line_copy;
    for (u32 line = 0; line < lines; line++) {
        const u8* in = nullptr;
        if (reads_src) {
            // The source can overlap the destination
            line_copy.assign(src + u64(regs.stride_in) * line,
                             src + u64(regs.stride_in) * line + src_line);
            in = line_copy.data();
        }
        u8* out = dst + u64(regs.stride_out) * line;
        for (u32 e = 0; e < regs.line_length_in; e++) {
            for (u32 i = 0; i < dst_components; i++) {
                u8* component = out + u64(e) * dst_element + i * component_size;
                switch (selects[i]) {
                case ConstA:
                    std::memcpy(component, &regs.remap_const1, component_size);
                    break;
                case ConstB:
                    std::memcpy(component, &regs.remap_const2, component_size);
                    break;
                default:
                    if (selects[i] < ConstA && selects[i] < src_components)
                        std::memcpy(component,
                                    in + u64(e) * src_element +
                                        selects[i] * component_size,
                                    component_size);
                    break;
                }
            }
        }
    }

    gpu.getRenderer().invalidateMemory(
        ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(dst), dst_span),
        renderer::MemoryInvalidationScope::BufferCache |
            renderer::MemoryInvalidationScope::TextureCache);
}

void Copy::launchDma(const u32 index, const LaunchDMAData data) {
    ScopedGpuWork work(GpuWork::Copy);
    (void)index;
    capture::GuestEvents::instance().event(
        "dma_copy", capture::currentGuestEventOrigin(), 0, 0, 0,
        static_cast<u32>(data.type));
    // A launch without a transfer only releases its semaphore
    if (data.type != TransferType::None)
        executeCopy(data);
    releaseSemaphore(data);
}

void Copy::releaseSemaphore(const LaunchDMAData data) {
    if (data.semaphore_type == SemaphoreType::None)
        return;

    // SET_SEMAPHORE_A/B (address) and SET_SEMAPHORE_PAYLOAD
    constexpr u32 REG_SEMAPHORE_A = 0x90;
    constexpr u32 REG_SEMAPHORE_PAYLOAD = 0x92;
    const u64 address = (static_cast<u64>(regs_raw[REG_SEMAPHORE_A] & 0xff)
                         << 32) |
                        regs_raw[REG_SEMAPHORE_A + 1];
    const u32 payload = regs_raw[REG_SEMAPHORE_PAYLOAD];
    uptr base;
    const auto as = tls_crnt_gmmu->findAddr(address, base);
    const bool four_words = data.semaphore_type == SemaphoreType::ReleaseFourWords;
    const u64 size = four_words ? 16 : 4;
    if (!as || as->ptr == 0 || address - base + size > as->size ||
        (as->ptr + (address - base)) %
                std::atomic_ref<u32>::required_alignment != 0) {
        ONCE(LOG_WARN(Engines, "Semaphore release to unmapped memory {:#x}",
                      address));
        return;
    }
    auto* ptr = reinterpret_cast<u8*>(as->ptr + (address - base));
    if (four_words) {
        const u64 timestamp = getGpuTimestamp();
        std::memcpy(ptr + 8, &timestamp, sizeof(timestamp));
    }
    // The payload is published last: a waiter that sees it also sees the copy
    std::atomic_ref<u32> word(*reinterpret_cast<u32*>(ptr));
    if (data.semaphore_reduction_enable) {
        const auto operation = static_cast<u32>(data.semaphore_reduction);
        const bool is_signed = !data.semaphore_reduction_unsigned;
        u32 old = word.load(std::memory_order_relaxed);
        while (!word.compare_exchange_weak(
            old, reduceDmaSemaphoreWord(operation, is_signed, old, payload),
            std::memory_order_acq_rel)) {
        }
    } else {
        word.store(payload, std::memory_order_release);
    }
    // A CPU reading the semaphore sees fresh bytes; GPU caches mirroring the
    // range are stale
    gpu.getRenderer().invalidateMemory(
        ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(ptr), size));
    capture::GuestEvents::instance().event("semaphore_release",
                                           capture::currentGuestEventOrigin(),
                                           address, size, 0, payload);
}

void Copy::executeCopy(const LaunchDMAData data) {
    if (data.remap_enable && data.src_memory_layout == MemoryLayout::Pitch &&
        data.dst_memory_layout == MemoryLayout::Pitch) {
        remapPitchCopy(data);
        return;
    }

    // Without REMAP_ENABLE an element is a byte. With it, an element has
    // NUM_SRC_COMPONENTS (source) or NUM_DST_COMPONENTS (destination)
    // components of COMPONENT_SIZE bytes, and the surface widths count elements.
    u32 src_stride = regs.src.stride;
    u32 dst_stride = regs.dst.stride;
    u32 bytes_per_pixel = 1;     // destination element
    u32 src_bytes_per_pixel = 1; // source element
    bool generic_remap = false;  // a remap that is not a plain copy of bytes
    if (data.remap_enable) {
        const auto& c = regs.remap_components;
        const u32 component_size = c.component_size_minus_one + 1;
        const u32 src_components = c.src_component_count_minus_one + 1u;
        const u32 dst_components = c.dst_component_count_minus_one + 1u;
        src_bytes_per_pixel = src_components * component_size;
        bytes_per_pixel = dst_components * component_size;
        src_stride *= src_bytes_per_pixel;
        dst_stride *= bytes_per_pixel;
        const std::array<u32, 4> selects{c.dst_x, c.dst_y, c.dst_z, c.dst_w};
        generic_remap = src_components != dst_components;
        for (u32 i = 0; i < dst_components; i++)
            generic_remap |= selects[i] != i;
    }

    // DEBUG: log copies
    {
        static u32 debug_count = 0;
        static u32 subrect_count = 0;
        const auto is_subrect = [&](const TextureCopyInfo& info) {
            return info.origin.x != 0 || info.origin.y != 0 ||
                   info.layer != 0 || regs.line_count != info.height;
        };
        const bool subrect =
            (data.dst_memory_layout == MemoryLayout::BlockLinear &&
             is_subrect(regs.dst)) ||
            (data.src_memory_layout == MemoryLayout::BlockLinear &&
             is_subrect(regs.src));
        if (debug_count++ < 100 || (subrect && subrect_count++ < 300))
            LOG_INFO(Engines,
                     "DEBUG DMA: in={:#x} out={:#x} src_layout={} "
                     "dst_layout={} stride_in={} stride_out={} line_length={} "
                     "line_count={} remap={} | dst: stride={} height={} "
                     "depth={} layer={} origin=({}, {}) block_h={} | src: "
                     "stride={} height={} origin=({}, {})",
                     static_cast<u64>(regs.offset_in),
                     static_cast<u64>(regs.offset_out),
                     static_cast<u32>(data.src_memory_layout),
                     static_cast<u32>(data.dst_memory_layout), regs.stride_in,
                     regs.stride_out, regs.line_length_in, regs.line_count,
                     static_cast<bool>(data.remap_enable), regs.dst.stride,
                     regs.dst.height, regs.dst.depth, regs.dst.layer,
                     static_cast<u32>(regs.dst.origin.x),
                     static_cast<u32>(regs.dst.origin.y),
                     static_cast<u32>(regs.dst.block_size.height),
                     regs.src.stride, regs.src.height,
                     static_cast<u32>(regs.src.origin.x),
                     static_cast<u32>(regs.src.origin.y));
    }

    // Bound the rectangle to the valid Z slices before calculating host
    // pointers. A line that would cross the X edge is invalid, not a wrap.
    u32 copy_lines = data.multi_line_enable ? regs.line_count : 1;
    const auto valid_surface = [&](const TextureCopyInfo& info, u32 width,
                                   u32 element_size) {
        if (getBlockSizeLog2(info.block_size.height) < 0 ||
            getBlockSizeLog2(info.block_size.depth) < 0 ||
            static_cast<u32>(info.block_size.height) > 5 ||
            static_cast<u32>(info.block_size.depth) > 5 ||
            info.height > std::numeric_limits<u32>::max() - 255 ||
            width > std::numeric_limits<u32>::max() - 63 ||
            info.depth > std::numeric_limits<u32>::max() - 31)
            return false;
        const u64 aligned_width = align(width, GOB_WIDTH);
        const u64 aligned_rows = align(
            info.height, GOB_HEIGHT << getBlockSizeLog2(info.block_size.height));
        const u64 aligned_depth = align(
            info.depth, 1u << getBlockSizeLog2(info.block_size.depth));
        if (aligned_depth != 0 && aligned_width * aligned_rows >
                                      std::numeric_limits<u64>::max() / aligned_depth)
            return false;
        copy_lines = std::min(copy_lines, availableLines(info));
        return u64(info.origin.x) * element_size +
                   u64(regs.line_length_in) * element_size <= width;
    };
    if ((data.src_memory_layout == MemoryLayout::BlockLinear &&
         !valid_surface(regs.src, src_stride, src_bytes_per_pixel)) ||
        (data.dst_memory_layout == MemoryLayout::BlockLinear &&
         !valid_surface(regs.dst, dst_stride, bytes_per_pixel))) {
        ONCE(LOG_WARN(Engines, "DMA rectangle outside block-linear surface"));
        return;
    }
    if (copy_lines == 0 || regs.line_length_in == 0)
        return;

    const auto mapped_base = [](uptr address) -> std::optional<uptr> {
        uptr base;
        const auto as = tls_crnt_gmmu->findAddr(address, base);
        if (!as || as->ptr == 0)
            return std::nullopt;
        return as->ptr + (address - base);
    };
    const auto src_host = mapped_base(regs.offset_in);
    const auto dst_host = mapped_base(regs.offset_out);
    if (!src_host || !dst_host) {
        ONCE(LOG_WARN(Engines, "DMA copy has an unmapped base address"));
        return;
    }
    const auto src_ptr = *src_host;
    const auto dst_ptr = *dst_host;
    // Bytes a pitch destination spans: LINE_COUNT and PITCH_OUT only apply to
    // multi-line copies
    const u64 dst_line_bytes =
        data.src_memory_layout == MemoryLayout::Pitch
            ? regs.line_length_in
            : static_cast<u64>(regs.line_length_in) * bytes_per_pixel;
    const u64 pitch_dst_rows =
        data.src_memory_layout == MemoryLayout::BlockLinear
            ? pitchRow(regs.src, copy_lines - 1)
            : copy_lines - 1;
    const u64 pitch_dst_size = u64(regs.stride_out) * pitch_dst_rows + dst_line_bytes;

    // The source can be a texture rendered by the GPU (readbacks)
    {
        u64 src_size;
        if (data.src_memory_layout == MemoryLayout::Pitch) {
            const u64 last_row =
                data.dst_memory_layout == MemoryLayout::BlockLinear
                    ? pitchRow(regs.dst, copy_lines - 1)
                    : copy_lines - 1;
            src_size = u64(regs.stride_in) * last_row +
                       u64(regs.line_length_in) * src_bytes_per_pixel;
        } else {
            const u32 stride = align(src_stride, GOB_WIDTH);
            const u32 rows = align(
                regs.src.height,
                GOB_HEIGHT << static_cast<u32>(
                    getBlockSizeLog2(regs.src.block_size.height)));
            const u32 slices = align(
                regs.src.depth, 1u << static_cast<u32>(getBlockSizeLog2(
                                    regs.src.block_size.depth)));
            src_size = static_cast<u64>(slices) * rows * stride;
        }
        // GPU writes to the destination that the copy only partially
        // overwrites must reach guest memory first, the copy then replaces
        // them there
        u64 dst_size;
        if (data.dst_memory_layout == MemoryLayout::Pitch) {
            dst_size = pitch_dst_size;
        } else {
            const u32 stride = align(dst_stride, GOB_WIDTH);
            const u32 rows = align(
                regs.dst.height,
                GOB_HEIGHT << static_cast<u32>(
                    getBlockSizeLog2(regs.dst.block_size.height)));
            const u32 slices = align(
                regs.dst.depth, 1u << static_cast<u32>(getBlockSizeLog2(
                                    regs.dst.block_size.depth)));
            dst_size = static_cast<u64>(slices) * rows * stride;
        }
        // The swizzle routines use a contiguous host span. Reject split or
        // missing mappings rather than treating adjacent guest mappings as
        // adjacent host allocations. Pitch-to-pitch already resolves chunks.
        if (data.src_memory_layout == MemoryLayout::BlockLinear ||
            data.dst_memory_layout == MemoryLayout::BlockLinear) {
            const auto mapped = [](uptr address, u64 size) {
                uptr base;
                const auto as = tls_crnt_gmmu->findAddr(address, base);
                return as && as->ptr != 0 && address - base <= as->size &&
                       size <= as->size - (address - base);
            };
            if (!mapped(regs.offset_in, src_size) ||
                !mapped(regs.offset_out, dst_size)) {
                ONCE(LOG_WARN(Engines,
                              "DMA tiled copy requires mapped host spans"));
                return;
            }
        }
        if (capture::GuestEvents::instance().enabled()) {
            resource::ContentManifest accesses;
            accesses.addRead(gpu.getRenderer().getContentVersions(),
                             ztd::Range<uptr>::fromSize(src_ptr, src_size));
            accesses.addWrite(ztd::Range<uptr>::fromSize(dst_ptr, dst_size));
            capture::recordGuestCommand(
                "dma_access", capture::currentGuestEventOrigin(), accesses);
        }
        // Opt-in summary to correlate DMA copies with texture readbacks.
        // Formatting keys and growing a map on every copy is diagnostic work.
        static const bool trace_dma =
            std::getenv("HYDRA_TRACE_DMA") != nullptr;
        if (trace_dma) {
            static std::map<std::string, u32> seen;
            static auto last = std::chrono::steady_clock::now();
            seen[fmt::format("src={:#x}+{:#x} L{} -> dst={:#x}+{:#x} L{} line={} x{} multi={} remap={}",
                             src_ptr, src_size, static_cast<u32>(data.src_memory_layout),
                             dst_ptr, dst_size, static_cast<u32>(data.dst_memory_layout),
                             regs.line_length_in, regs.line_count,
                             static_cast<bool>(data.multi_line_enable),
                             static_cast<bool>(data.remap_enable))]++;
            if (std::chrono::steady_clock::now() - last > std::chrono::seconds(2)) {
                last = std::chrono::steady_clock::now();
                for (const auto& [key, count] : seen)
                    LOG_INFO(Engines, "DEBUG DMA summary: {} n={}", key, count);
                seen.clear();
            }
        }
        auto& texture_cache = gpu.getRenderer().getTextureCache();
        std::scoped_lock lock(texture_cache.getMutex());
        texture_cache.flushMemory(tls_crnt_command_buffer,
                                  ztd::Range<uptr>::fromSize(src_ptr, src_size));
        texture_cache.flushMemory(tls_crnt_command_buffer,
                                  ztd::Range<uptr>::fromSize(dst_ptr, dst_size));
    }
    if (generic_remap) {
        // Element by element: select source components, constants or leave the
        // destination component alone, for any mix of pitch and block linear
        enum : u32 { ConstA = 4, ConstB = 5 };
        const auto& c = regs.remap_components;
        const u32 component_size = c.component_size_minus_one + 1u;
        const u32 src_components = c.src_component_count_minus_one + 1u;
        const u32 dst_components = c.dst_component_count_minus_one + 1u;
        const std::array<u32, 4> selects{c.dst_x, c.dst_y, c.dst_z, c.dst_w};
        const auto log2 = [](BlockDim size) {
            return static_cast<u32>(std::max(getBlockSizeLog2(size), 0));
        };
        const auto* src_bytes = reinterpret_cast<const u8*>(src_ptr);
        auto* dst_bytes = reinterpret_cast<u8*>(dst_ptr);
        // Pitch rows of a tiled copy follow the slices of the tiled side
        const bool src_tiled = data.src_memory_layout == MemoryLayout::BlockLinear;
        const bool dst_tiled = data.dst_memory_layout == MemoryLayout::BlockLinear;
        const auto tiled_offset = [&](const TextureCopyInfo& info, u32 width,
                                      u32 element_size, u32 line, u32 e,
                                      u32 i) {
            const auto [y, z] = sliceRow(info, line);
            return getBlockLinearOffset(
                (info.origin.x + e) * element_size + i, y, z, width,
                info.height, log2(info.block_size.height),
                log2(info.block_size.depth));
        };
        for (u32 line = 0; line < copy_lines; line++) {
            for (u32 e = 0; e < regs.line_length_in; e++) {
                std::array<u8, 16> in{};
                for (u32 i = 0; i < src_bytes_per_pixel; i++) {
                    in[i] = src_tiled
                                ? src_bytes[tiled_offset(regs.src, src_stride,
                                                         src_bytes_per_pixel,
                                                         line, e, i)]
                                : src_bytes[u64(regs.stride_in) *
                                                (dst_tiled ? pitchRow(regs.dst, line)
                                                           : line) +
                                            u64(e) * src_bytes_per_pixel + i];
                }
                for (u32 k = 0; k < dst_components; k++) {
                    for (u32 i = 0; i < component_size; i++) {
                        const u32 byte = k * component_size + i;
                        u8* out = dst_tiled
                                      ? &dst_bytes[tiled_offset(
                                            regs.dst, dst_stride,
                                            bytes_per_pixel, line, e, byte)]
                                      : &dst_bytes[u64(regs.stride_out) *
                                                       (src_tiled ? pitchRow(regs.src, line)
                                                                  : line) +
                                                   u64(e) * bytes_per_pixel + byte];
                        if (selects[k] == ConstA)
                            *out = reinterpret_cast<const u8*>(&regs.remap_const1)[i];
                        else if (selects[k] == ConstB)
                            *out = reinterpret_cast<const u8*>(&regs.remap_const2)[i];
                        else if (selects[k] < 4 && selects[k] < src_components)
                            *out = in[selects[k] * component_size + i];
                    }
                }
            }
        }
    } else if (data.src_memory_layout == MemoryLayout::Pitch) {
        if (data.dst_memory_layout == MemoryLayout::Pitch) {
            const u32 lines = data.multi_line_enable ? regs.line_count : 1;
            for (u32 i = 0; i < lines; i++) {
                uptr src_addr = static_cast<u64>(regs.offset_in) +
                                static_cast<u64>(regs.stride_in) * i;
                uptr dst_addr = static_cast<u64>(regs.offset_out) +
                                static_cast<u64>(regs.stride_out) * i;
                u64 remaining = regs.line_length_in;
                while (remaining != 0) {
                    uptr src_base, dst_base;
                    const auto& src_as =
                        tls_crnt_gmmu->findAddrImpl(src_addr, src_base);
                    const auto& dst_as =
                        tls_crnt_gmmu->findAddrImpl(dst_addr, dst_base);
                    const u64 bytes = std::min(
                        {remaining, src_as.size - (src_addr - src_base),
                         dst_as.size - (dst_addr - dst_base)});
                    const auto src = tls_crnt_gmmu->unmapAddr(src_addr);
                    const auto dst = tls_crnt_gmmu->unmapAddr(dst_addr);
                    memcpy(reinterpret_cast<void*>(dst),
                           reinterpret_cast<const void*>(src), bytes);
                    gpu.getRenderer().invalidateMemory(
                        ztd::Range<uptr>::fromSize(dst, bytes),
                        renderer::MemoryInvalidationScope::BufferCache |
                            renderer::MemoryInvalidationScope::TextureCache);
                    src_addr += bytes;
                    dst_addr += bytes;
                    remaining -= bytes;
                }
            }
            return;
        } else {
            copySubrect(reinterpret_cast<u8*>(src_ptr), regs.stride_in,
                        reinterpret_cast<u8*>(dst_ptr), regs.dst, dst_stride,
                        bytes_per_pixel, regs.line_length_in * bytes_per_pixel,
                        copy_lines, true);
        }
    } else {
        if (data.dst_memory_layout == MemoryLayout::Pitch) {
            copySubrect(reinterpret_cast<u8*>(dst_ptr), regs.stride_out,
                        reinterpret_cast<u8*>(src_ptr), regs.src, src_stride,
                        bytes_per_pixel, regs.line_length_in * bytes_per_pixel,
                        copy_lines, false);
        } else {
            copyBlockLinear(reinterpret_cast<const u8*>(src_ptr), regs.src,
                            src_stride, reinterpret_cast<u8*>(dst_ptr),
                            regs.dst, dst_stride, bytes_per_pixel,
                            regs.line_length_in * bytes_per_pixel,
                            copy_lines);
        }
    }

    // Invalidate memory
    if (data.dst_memory_layout == MemoryLayout::Pitch) {
        gpu.getRenderer().invalidateMemory(
            ztd::Range<uptr>::fromSize(dst_ptr, pitch_dst_size),
            renderer::MemoryInvalidationScope::BufferCache |
                renderer::MemoryInvalidationScope::TextureCache);
    } else {
        const u32 stride = align(dst_stride, GOB_WIDTH);
        const u32 rows = align(
            regs.dst.height, GOB_HEIGHT << static_cast<u32>(
                                 getBlockSizeLog2(regs.dst.block_size.height)));
        const u32 slices =
            align(regs.dst.depth, 1u << static_cast<u32>(getBlockSizeLog2(
                                      regs.dst.block_size.depth)));
        gpu.getRenderer().invalidateMemory(
            ztd::Range<uptr>::fromSize(dst_ptr, static_cast<uptr>(slices) *
                                                    rows * stride),
            renderer::MemoryInvalidationScope::TextureCache);
    }
}

#pragma GCC diagnostic pop

} // namespace hydra::hw::tegra_x1::gpu::engines
