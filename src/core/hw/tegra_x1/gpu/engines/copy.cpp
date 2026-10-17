#include "core/hw/tegra_x1/gpu/engines/copy.hpp"

#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"

namespace hydra::hw::tegra_x1::gpu::engines {

DEFINE_METHOD_TABLE(Copy, 0xc0, 1, launchDma, LaunchDMAData)

namespace {

// Copies a sub rectangle between pitch and block linear memory, honouring the
// origin and layer of the block linear surface
void copySubrect(u8* pitch, u32 pitch_stride, u8* block_linear,
                 const TextureCopyInfo& info, u32 width, u32 bytes_per_pixel,
                 u32 line_length, u32 line_count, bool to_block_linear) {
    const u32 block_height_log2 = static_cast<u32>(
        std::max(getBlockSizeLog2(info.block_size.height), 0));
    const u32 block_depth_log2 = static_cast<u32>(
        std::max(getBlockSizeLog2(info.block_size.depth), 0));
    const u32 origin_x = info.origin.x * bytes_per_pixel;
    for (u32 line = 0; line < line_count; line++) {
        const u32 y = info.origin.y + line;
        u8* pitch_line = pitch + static_cast<usize>(line) * pitch_stride;
        for (u32 i = 0; i < line_length;) {
            const u32 x = origin_x + i;
            // Contiguous bytes until the next 16 byte boundary
            const u32 bytes = std::min(16 - x % 16, line_length - i);
            u8* bl = block_linear +
                     getBlockLinearOffset(x, y, info.layer, width, info.height,
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
        const u32 src_y = src_info.origin.y + line;
        const u32 dst_y = dst_info.origin.y + line;
        for (u32 i = 0; i < line_length;) {
            const u32 src_x = src_origin_x + i;
            const u32 dst_x = dst_origin_x + i;
            // Contiguous bytes of both surfaces until their next 16 byte
            // boundary
            const u32 bytes = std::min(
                {16 - src_x % 16, 16 - dst_x % 16, line_length - i});
            std::memcpy(dst + getBlockLinearOffset(
                                  dst_x, dst_y, dst_info.layer, dst_width,
                                  dst_info.height, dst_height_log2,
                                  dst_depth_log2),
                        src + getBlockLinearOffset(
                                  src_x, src_y, src_info.layer, src_width,
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
    (void)index;
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
    if (!as || as->ptr == 0 || address - base + size > as->size) {
        ONCE(LOG_WARN(Engines, "Semaphore release to unmapped memory {:#x}",
                      address));
        return;
    }
    auto* ptr = reinterpret_cast<u8*>(as->ptr + (address - base));
    std::memcpy(ptr, &payload, sizeof(payload));
    if (four_words) {
        const u64 timestamp = getGpuTimestamp();
        std::memcpy(ptr + 8, &timestamp, sizeof(timestamp));
    }
    // A CPU reading the semaphore sees fresh bytes; GPU caches mirroring the
    // range are stale
    gpu.getRenderer().invalidateMemory(
        ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(ptr), size));
}

void Copy::executeCopy(const LaunchDMAData data) {
    if (data.remap_enable && data.src_memory_layout == MemoryLayout::Pitch &&
        data.dst_memory_layout == MemoryLayout::Pitch) {
        remapPitchCopy(data);
        return;
    }

    // TODO: implement component remapping
    // HACK
    u32 src_stride = regs.src.stride;
    u32 dst_stride = regs.dst.stride;
    u32 bytes_per_pixel = 1;
    if (data.remap_enable) {
        const auto& c = regs.remap_components;
        ASSERT_DEBUG(
            c.dst_x == 0 && c.dst_y == 1 && c.dst_z == 2 && c.dst_w == 3,
            Engines, "Component remapping not implemented ({}, {}, {}, {})",
            c.dst_x, c.dst_y, c.dst_z, c.dst_w);

        const u32 component_size = c.component_size_minus_one + 1;
        bytes_per_pixel =
            (c.dst_component_count_minus_one + 1u) * component_size;
        {
            const u32 component_count = c.src_component_count_minus_one + 1;
            u32 bytes_per_block = component_count * component_size;
            src_stride *= bytes_per_block;
        }
        {
            const u32 component_count = c.dst_component_count_minus_one + 1;
            u32 bytes_per_block = component_count * component_size;
            dst_stride *= bytes_per_block;
        }
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

    const auto src_ptr = tls_crnt_gmmu->unmapAddr(regs.offset_in);
    const auto dst_ptr = tls_crnt_gmmu->unmapAddr(regs.offset_out);
    // Bytes a pitch destination spans: LINE_COUNT and PITCH_OUT only apply to
    // multi-line copies
    const u32 copy_lines = data.multi_line_enable ? regs.line_count : 1;
    const u64 dst_line_bytes =
        data.src_memory_layout == MemoryLayout::Pitch
            ? regs.line_length_in
            : static_cast<u64>(regs.line_length_in) * bytes_per_pixel;
    const u64 pitch_dst_size =
        copy_lines == 0 ? 0
                        : static_cast<u64>(regs.stride_out) * (copy_lines - 1) +
                              dst_line_bytes;

    // The source can be a texture rendered by the GPU (readbacks)
    {
        u64 src_size;
        if (data.src_memory_layout == MemoryLayout::Pitch) {
            src_size = copy_lines == 0
                           ? 0
                           : static_cast<u64>(regs.stride_in) *
                                     (copy_lines - 1) +
                                 regs.line_length_in;
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
        // DEBUG: summary of the copies per 2 s (to correlate with readbacks)
        {
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
    if (data.src_memory_layout == MemoryLayout::Pitch) {
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
            // TODO: can this copy to multiple slices at once?
            copySubrect(reinterpret_cast<u8*>(src_ptr), regs.stride_in,
                        reinterpret_cast<u8*>(dst_ptr), regs.dst, dst_stride,
                        bytes_per_pixel, regs.line_length_in * bytes_per_pixel,
                        data.multi_line_enable ? regs.line_count : 1, true);
        }
    } else {
        if (data.dst_memory_layout == MemoryLayout::Pitch) {
            // TODO: can this copy from multiple slices at once?
            copySubrect(reinterpret_cast<u8*>(dst_ptr), regs.stride_out,
                        reinterpret_cast<u8*>(src_ptr), regs.src, src_stride,
                        bytes_per_pixel, regs.line_length_in * bytes_per_pixel,
                        data.multi_line_enable ? regs.line_count : 1, false);
        } else {
            copyBlockLinear(reinterpret_cast<const u8*>(src_ptr), regs.src,
                            src_stride, reinterpret_cast<u8*>(dst_ptr),
                            regs.dst, dst_stride, bytes_per_pixel,
                            regs.line_length_in * bytes_per_pixel,
                            data.multi_line_enable ? regs.line_count : 1);
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
