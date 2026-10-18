#include "core/hw/tegra_x1/gpu/engines/2d.hpp"

#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::engines {

std::optional<BlitRegion2D> getBlitRegion(const Regs2D& regs) {
    const auto& p = regs.pixels_from_memory;
    u64 x0 = p.dst_x0;
    u64 y0 = p.dst_y0;
    u64 x1 = std::min(x0 + p.dst_width, static_cast<u64>(regs.dst.width));
    u64 y1 = std::min(y0 + p.dst_height, static_cast<u64>(regs.dst.height));
    if ((regs.clip_enabled & 1) != 0) {
        x0 = std::max(x0, static_cast<u64>(regs.clip_x));
        y0 = std::max(y0, static_cast<u64>(regs.clip_y));
        x1 = std::min(x1, static_cast<u64>(regs.clip_x) + regs.clip_width);
        y1 = std::min(y1, static_cast<u64>(regs.clip_y) + regs.clip_height);
    }
    if (x0 >= x1 || y0 >= y1)
        return std::nullopt;
    const auto dudx = static_cast<f64>(p.dudx);
    const auto dvdy = static_cast<f64>(p.dvdy);
    // The reference rounds the corner correction to whole source texels;
    // applying a fractional half derivative would blur 1:1 copies.
    const auto origin_x = regs.sample_mode.origin ? std::floor(dudx / 2.0) : 0.0;
    const auto origin_y = regs.sample_mode.origin ? std::floor(dvdy / 2.0) : 0.0;
    return BlitRegion2D{
        static_cast<u32>(x0), static_cast<u32>(y0),
        static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0),
        static_cast<f64>(p.src_x0) - origin_x + static_cast<f64>(x0 - p.dst_x0) * dudx,
        static_cast<f64>(p.src_y0) - origin_y + static_cast<f64>(y0 - p.dst_y0) * dvdy,
        static_cast<f64>(x1 - x0) * dudx,
        static_cast<f64>(y1 - y0) * dvdy};
}

namespace {

std::optional<u32> rawSurfaceSize(const Texture2DInfo& info, u32 bpp) {
    const u64 depth = std::max(info.depth, 1u);
    u64 stride;
    u64 rows;
    u64 slices;
    if (info.layout == MemoryLayout::Pitch) {
        if (static_cast<u64>(info.width) * bpp > info.stride)
            return std::nullopt;
        stride = info.stride;
        rows = info.height;
        slices = depth;
    } else {
        stride = (static_cast<u64>(info.width) * bpp + 63) & ~63ull;
        const u64 bh = 8ull << info.block_height_gobs_log2;
        const u64 bd = 1ull << info.block_depth_gobs_log2;
        rows = (static_cast<u64>(info.height) + bh - 1) & ~(bh - 1);
        slices = (depth + bd - 1) & ~(bd - 1);
    }
    // Bound each factor before multiplication, including malformed registers.
    if (stride > UINT32_MAX || rows > UINT32_MAX || slices > UINT32_MAX ||
        (rows != 0 && stride > UINT32_MAX / rows) ||
        (slices != 0 && stride * rows > UINT32_MAX / slices))
        return std::nullopt;
    return static_cast<u32>(stride * rows * slices);
}

u64 rawPixelOffset(const Texture2DInfo& info, u32 bpp, u32 x, u32 y,
                   u32 slice) {
    if (info.layout == MemoryLayout::Pitch)
        return (static_cast<u64>(slice) * info.height + y) * info.stride +
               static_cast<u64>(x) * bpp;
    return getBlockLinearOffset(x * bpp, y, slice, info.width * bpp,
                                info.height, info.block_height_gobs_log2,
                                info.block_depth_gobs_log2);
}

u8 binaryRop(u8 rop, u8 src, u8 dst) {
    // Bit index is (pattern << 2) | (source << 1) | destination.
    return ((rop & 1) ? static_cast<u8>(~src & ~dst) : 0) |
           ((rop & 2) ? static_cast<u8>(~src & dst) : 0) |
           ((rop & 4) ? static_cast<u8>(src & ~dst) : 0) |
           ((rop & 8) ? static_cast<u8>(src & dst) : 0);
}

} // namespace

bool supportsRawBlit2D(const Regs2D& regs) {
    const u32 operation = regs.operation & 7;
    if (operation != 3 &&
        (operation != 4 || (regs.rop & 15) != ((regs.rop >> 4) & 15)))
        return false;
    if ((regs.color_key_enabled & 1) || regs.sample_mode.filter != 0 ||
        regs.src.format != regs.dst.format || regs.src.width == 0 ||
        regs.src.height == 0 || regs.dst.layer >= std::max(regs.dst.depth, 1u))
        return false;
    const auto format = renderer::toTextureFormat(regs.src.format);
    if (format == renderer::TextureFormat::Invalid)
        return false;
    const auto& info = renderer::getTextureFormatInfo(format);
    return info.block_width == 1 && info.block_height == 1 &&
           regs.src.width <= UINT32_MAX / info.bytes_per_block &&
           regs.dst.width <= UINT32_MAX / info.bytes_per_block &&
           rawSurfaceSize(regs.src, info.bytes_per_block).has_value() &&
           rawSurfaceSize(regs.dst, info.bytes_per_block).has_value();
}

bool rawBlit2D(const Regs2D& regs, const BlitRegion2D& region,
               const u8* src, usize src_size, u8* dst, usize dst_size) {
    if (!supportsRawBlit2D(regs) || !src || !dst || region.width == 0 ||
        region.height == 0 ||
        static_cast<u64>(region.dst_x) + region.width > regs.dst.width ||
        static_cast<u64>(region.dst_y) + region.height > regs.dst.height)
        return false;
    const auto bpp = renderer::getTextureFormatInfo(
                         renderer::toTextureFormat(regs.src.format))
                         .bytes_per_block;
    const f64 dx = region.src_width / region.width;
    const f64 dy = region.src_height / region.height;
    if (!std::isfinite(region.src_x) || !std::isfinite(region.src_y) ||
        !std::isfinite(dx) || !std::isfinite(dy))
        return false;
    // Validate every access before changing memory. Store the operation result
    // first so overlapping/in-place blits never feed writes back into reads.
    const u64 result_size = static_cast<u64>(region.width) * region.height * bpp;
    if (result_size > dst_size || result_size > SIZE_MAX)
        return false;
    std::vector<u8> result(static_cast<usize>(result_size));
    for (u32 y = 0; y < region.height; ++y) {
        const auto sy = static_cast<u32>(std::clamp(
            std::floor(region.src_y + (static_cast<f64>(y) + 0.5) * dy),
            0.0, static_cast<f64>(regs.src.height - 1)));
        for (u32 x = 0; x < region.width; ++x) {
            const auto sx = static_cast<u32>(std::clamp(
                std::floor(region.src_x + (static_cast<f64>(x) + 0.5) * dx),
                0.0, static_cast<f64>(regs.src.width - 1)));
            const auto so = rawPixelOffset(regs.src, bpp, sx, sy, 0);
            const auto to = rawPixelOffset(regs.dst, bpp, region.dst_x + x,
                                            region.dst_y + y, regs.dst.layer);
            if (so > src_size || bpp > src_size - so || to > dst_size ||
                bpp > dst_size - to)
                return false;
            auto* out = result.data() +
                        (static_cast<usize>(y) * region.width + x) * bpp;
            for (u32 byte = 0; byte < bpp; ++byte)
                out[byte] = (regs.operation & 7) == 3
                                ? src[so + byte]
                                : binaryRop(static_cast<u8>(regs.rop), src[so + byte],
                                            dst[to + byte]);
        }
    }
    for (u32 y = 0; y < region.height; ++y)
        for (u32 x = 0; x < region.width; ++x) {
            const auto to = rawPixelOffset(regs.dst, bpp, region.dst_x + x,
                                            region.dst_y + y, regs.dst.layer);
            std::memcpy(dst + to,
                        result.data() +
                            (static_cast<usize>(y) * region.width + x) * bpp,
                        bpp);
        }
    return true;
}

DEFINE_METHOD_TABLE(TwoD, 0x237, 1, copy, u32)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

void TwoD::copy(const u32 index, const u32 pixels_from_memory_src_y0_int) {
    auto& pixels = regs.pixels_from_memory;
    pixels.src_y0.integer = pixels_from_memory_src_y0_int;
    const auto region = getBlitRegion(regs);
    if (!region)
        return;

    std::scoped_lock texture_cache_lock(
        gpu.getRenderer().getTextureCache().getMutex());

    if ((regs.operation & 7) != 3 || regs.src.depth > 1 ||
        regs.dst.depth > 1 || regs.dst.layer != 0) {
        copyRaw(*region);
        return;
    }

    // Depth buffers are copied with color formats of the same size. Copy the
    // depth stencil texture rendered by the GPU instead of stale guest memory
    const auto depth_stencil_format =
        gpu.getRenderer().getTextureCache().findDepthStencilAlias(
            getTextureDescriptor(regs.src));

    if (depth_stencil_format)
        ONCE(LOG_INFO(Engines, "2D copy of a depth stencil texture as {}",
                      *depth_stencil_format));

    if (!depth_stencil_format &&
        (renderer::toTextureFormat(regs.src.format) ==
             renderer::TextureFormat::Invalid ||
         renderer::toTextureFormat(regs.dst.format) ==
             renderer::TextureFormat::Invalid)) {
        ONCE(LOG_NOT_IMPLEMENTED(Engines, "2D copy from {:#x} to {:#x}",
                                 static_cast<u32>(regs.src.format),
                                 static_cast<u32>(regs.dst.format)));
        return;
    }

    // TODO: can these also not be textures?
    auto src =
        getTexture(regs.src, renderer::TextureUsage::Read, depth_stencil_format);
    auto dst = getTexture(regs.dst, renderer::TextureUsage::Write,
                          depth_stencil_format);

    const auto dudx = static_cast<f64>(pixels.dudx);
    const auto dvdy = static_cast<f64>(pixels.dvdy);
    const auto src_x0 = region->src_x;
    const auto src_y0 = region->src_y;

    // DEBUG: log 2D copies
    {
        static u32 debug_count = 0;
        if (debug_count++ < 40)
            LOG_INFO(Engines,
                     "DEBUG 2D copy: src {:#x} fmt {} {}x{} -> dst {:#x} fmt {} "
                     "{}x{} (src {}x{} @ {},{} dst {}x{} @ {},{})",
                     static_cast<u64>(regs.src.addr),
                     renderer::toTextureFormat(regs.src.format), regs.src.width,
                     regs.src.height, static_cast<u64>(regs.dst.addr),
                     renderer::toTextureFormat(regs.dst.format), regs.dst.width,
                     regs.dst.height, pixels.dst_width * dudx,
                     pixels.dst_height * dvdy, src_x0, src_y0,
                     pixels.dst_width, pixels.dst_height, pixels.dst_x0,
                     pixels.dst_y0);
    }

    // The source extent is not truncated: a du/dx of (1280 << 32) / 720 would
    // drop the last source column. Word 0x90 is not a source layer, the class
    // has none
    const auto exact_width = region->src_width;
    const auto exact_height = region->src_height;
    const auto round_extent = [](f64 extent) {
        return static_cast<u32>(
            std::clamp(std::round(std::abs(extent)), 0.0, 65536.0));
    };
    const auto src_width = round_extent(exact_width);
    const auto src_height = round_extent(exact_height);

    renderer::BlitOptions options{
        .nearest = regs.sample_mode.filter == 0,
        .src_width = static_cast<f32>(exact_width),
        .src_height = static_cast<f32>(exact_height),
    };
    gpu.getRenderer().blitTexture(
        tls_crnt_command_buffer, src,
        {static_cast<f32>(src_x0), static_cast<f32>(src_y0), 0.0f},
        {src_width, src_height, 1}, 0, 0, dst,
        {static_cast<f32>(region->dst_x), static_cast<f32>(region->dst_y),
         0.0f},
        {region->width, region->height, 1}, 0, regs.dst.layer, 1, 1,
        options);

    // Small and pitch linear destinations are typically staging copies the CPU
    // reads back directly (e.g. UE4's sky light average brightness), without a
    // DMA copy that would flush them. Write the GPU result back right away.
    // Mip chain copies of texture streaming must not be written back: their
    // host copies can be stale.
    const auto dst_desc = getTextureDescriptor(regs.dst);
    const auto dst_format = renderer::toTextureFormat(regs.dst.format);
    const bool float32_target =
        dst_format == renderer::TextureFormat::RGBA32Float ||
        dst_format == renderer::TextureFormat::RG32Float ||
        dst_format == renderer::TextureFormat::R32Float;
    const bool readback =
        regs.dst.layout == MemoryLayout::Pitch ||
        (float32_target && pixels.dst_width * pixels.dst_height <= 4);
    // NVN copies compressed textures (texture streaming copies mip chains)
    // through same sized color formats, e.g. BC1 blocks as RG32 pixels. The
    // compressed textures are separate storages, so also copy the bytes in
    // guest memory and let them reload from it.
    const auto format = renderer::toTextureFormat(regs.src.format);
    const u32 bpp = renderer::getTextureFormatInfo(format).bytes_per_block;
    // Only the 32-bit component formats NVN aliases compressed blocks with
    const bool block_alias = format == renderer::TextureFormat::RG32Float ||
                             format == renderer::TextureFormat::RG32Uint ||
                             format == renderer::TextureFormat::RGBA32Float ||
                             format == renderer::TextureFormat::RGBA32Uint;
    // Only when the copy aliases compressed storages, and guest memory has the
    // source: otherwise it would replace the GPU blit with stale bytes
    if (!depth_stencil_format && regs.src.format == regs.dst.format &&
        block_alias && regs.dst.layer == 0 &&
        dudx == 1.0 && dvdy == 1.0 && src_x0 >= 0 && src_y0 >= 0 &&
        std::floor(src_x0) == src_x0 && std::floor(src_y0) == src_y0 &&
        (bpp == 8 || bpp == 16)) {
        auto& texture_cache = gpu.getRenderer().getTextureCache();
        const auto src_desc = getTextureDescriptor(regs.src);
        const auto src_range =
            ztd::Range<uptr>::fromSize(src_desc.ptr, src_desc.size);
        const auto dst_range =
            ztd::Range<uptr>::fromSize(dst_desc.ptr, dst_desc.size);
        if ((texture_cache.overlapsCompressedStorage(src_range) ||
             texture_cache.overlapsCompressedStorage(dst_range)) &&
            !texture_cache.hasUnflushedGpuWrites(src_range))
            copyGuestMemory(bpp, static_cast<u32>(src_x0),
                            static_cast<u32>(src_y0), region->dst_x, region->dst_y,
                            region->width, region->height);
    }

    if (!depth_stencil_format && readback)
        gpu.getRenderer().getTextureCache().flushMemory(
            tls_crnt_command_buffer,
            ztd::Range<uptr>::fromSize(dst_desc.ptr, dst_desc.size));
}

void TwoD::copyRaw(const BlitRegion2D& region) {
    if (!supportsRawBlit2D(regs)) {
        ONCE(LOG_NOT_IMPLEMENTED(Engines,
                                 "2D operation {}, ROP {:#x}, filter {} or format conversion",
                                 regs.operation & 7, regs.rop & 255,
                                 static_cast<u32>(regs.sample_mode.filter)));
        return;
    }
    for (const auto* info : {&regs.src, &regs.dst}) {
        uptr base;
        const auto mapping = tls_crnt_gmmu->findAddr(
            static_cast<u64>(info->addr) & 0xffffffffffull, base);
        if (!mapping || mapping->ptr == 0)
            return;
    }
    auto src_desc = getTextureDescriptor(regs.src);
    auto dst_desc = getTextureDescriptor(regs.dst);
    // Preserve programmed block dimensions instead of texture mip clamping.
    const auto bpp = renderer::getTextureFormatInfo(
                         renderer::toTextureFormat(regs.src.format))
                         .bytes_per_block;
    src_desc.size = *rawSurfaceSize(regs.src, bpp);
    dst_desc.size = *rawSurfaceSize(regs.dst, bpp);
    // Texture descriptors describe contiguous host backing. Refuse fragmented
    // mappings rather than reading beyond the first mapping.
    const auto is_mapped = [](const Texture2DInfo& info, u64 size) {
        uptr base;
        const u64 address = static_cast<u64>(info.addr) & 0xffffffffffull;
        const auto mapping = tls_crnt_gmmu->findAddr(address, base);
        return mapping && address - base <= mapping->size &&
               size <= mapping->size - (address - base);
    };
    if (!is_mapped(regs.src, src_desc.size) ||
        !is_mapped(regs.dst, dst_desc.size)) {
        ONCE(LOG_WARN(Engines, "2D raw blit needs complete mapped surfaces"));
        return;
    }
    auto& cache = gpu.getRenderer().getTextureCache();
    cache.flushMemory(tls_crnt_command_buffer, src_desc.getRange());
    cache.flushMemory(tls_crnt_command_buffer, dst_desc.getRange());
    // A format which cannot be encoded back to guest memory must not consume
    // stale bytes (notably depth aliases and unsupported volume writeback).
    if (cache.hasUnflushedGpuWrites(src_desc.getRange()) ||
        cache.hasUnflushedGpuWrites(dst_desc.getRange())) {
        ONCE(LOG_WARN(Engines, "2D raw blit cannot read a GPU-dirty surface"));
        return;
    }
    if (!rawBlit2D(regs, region, reinterpret_cast<const u8*>(src_desc.ptr),
                   src_desc.size, reinterpret_cast<u8*>(dst_desc.ptr),
                   dst_desc.size)) {
        ONCE(LOG_WARN(Engines, "2D raw blit has an invalid surface span"));
        return;
    }
    cache.markGpuWritesInGuest(dst_desc.getRange());
    cache.invalidateMemory(dst_desc.getRange());
    gpu.getRenderer().invalidateMemory(
        dst_desc.getRange(), renderer::MemoryInvalidationScope::BufferCache |
                                 renderer::MemoryInvalidationScope::ShaderCache);
}

void TwoD::copyGuestMemory(u32 bpp, u32 src_x, u32 src_y, u32 dst_x,
                           u32 dst_y, u32 width, u32 height) {
    const auto src_desc = getTextureDescriptor(regs.src);
    const auto dst_desc = getTextureDescriptor(regs.dst);
    auto& texture_cache = gpu.getRenderer().getTextureCache();

    // Guest memory is the source of truth here: aliases of the source on the
    // host can be stale (they are not updated by CPU uploads after a GPU
    // write), so flushing them would overwrite valid mips with old data.

    const auto offset = [bpp](const Texture2DInfo& info, u32 x_bytes,
                              u32 y) {
        if (info.layout == MemoryLayout::Pitch)
            return static_cast<u64>(y) * info.stride + x_bytes;
        return getBlockLinearOffset(x_bytes, y, info.layer, info.width * bpp,
                                    info.height, info.block_height_gobs_log2,
                                    info.block_depth_gobs_log2);
    };
    auto* src = reinterpret_cast<const u8*>(src_desc.ptr);
    auto* dst = reinterpret_cast<u8*>(dst_desc.ptr);
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            const u64 src_offset =
                offset(regs.src, (src_x + x) * bpp, src_y + y);
            const u64 dst_offset = offset(
                regs.dst, (dst_x + x) * bpp, dst_y + y);
            if (src_offset + bpp > src_desc.size ||
                dst_offset + bpp > dst_desc.size)
                continue;
            // 16 byte chunks never cross a GOB line
            std::memcpy(dst + dst_offset, src + src_offset, bpp);
        }
    }

    // The texture cache is already locked by copy. The blit's host result is
    // an alias of compressed blocks, never write it back over the copy.
    const auto dst_range =
        ztd::Range<uptr>::fromSize(dst_desc.ptr, dst_desc.size);
    texture_cache.markGpuWritesInGuest(dst_range);
    texture_cache.invalidateMemory(dst_range);
}

#pragma GCC diagnostic pop

renderer::TextureDescriptor
TwoD::getTextureDescriptor(const Texture2DInfo& info) const {
    return renderer::TextureDescriptor(
        tls_crnt_gmmu->unmapAddr(static_cast<u64>(info.addr) & 0xffffffffffull),
        info.depth > 1 ? MTL::TextureType3D
                       : MTL::TextureType2D,
        renderer::toTextureFormat(info.format),
        info.layout == MemoryLayout::Pitch, info.stride, info.width,
        info.height, std::max(info.depth, 1u), 1, 1, 0x0,
        info.block_height_gobs_log2, info.block_depth_gobs_log2);
}

renderer::ITextureView*
TwoD::getTexture(const Texture2DInfo& info, renderer::TextureUsage usage,
                 std::optional<renderer::TextureFormat> format_override) {
    auto descriptor = getTextureDescriptor(info);
    if (format_override)
        descriptor.format = *format_override;

    // TODO: texture view (layer)

    return gpu.getRenderer().getTextureCache().find(tls_crnt_command_buffer,
                                                    descriptor, usage);
}

} // namespace hydra::hw::tegra_x1::gpu::engines
