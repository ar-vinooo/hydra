#include "core/hw/tegra_x1/gpu/engines/2d.hpp"

#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::engines {

DEFINE_METHOD_TABLE(TwoD, 0x237, 1, copy, u32)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

void TwoD::copy(const u32 index, const u32 pixels_from_memory_src_y0_int) {
    auto& pixels = regs.pixels_from_memory;
    pixels.src_y0.integer = pixels_from_memory_src_y0_int;

    std::scoped_lock texture_cache_lock(
        gpu.getRenderer().getTextureCache().getMutex());

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
    const auto src_x0 = static_cast<f64>(pixels.src_x0);
    const auto src_y0 = static_cast<f64>(pixels.src_y0);

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
    const auto exact_width = static_cast<f64>(pixels.dst_width) * dudx;
    const auto exact_height = static_cast<f64>(pixels.dst_height) * dvdy;
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
        {static_cast<f32>(pixels.dst_x0), static_cast<f32>(pixels.dst_y0),
         0.0f},
        {pixels.dst_width, pixels.dst_height, 1}, 0, regs.dst.layer, 1, 1,
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
        dudx == 1.0 && dvdy == 1.0 && (bpp == 8 || bpp == 16)) {
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
                            static_cast<u32>(src_y0), src_width, src_height);
    }

    if (!depth_stencil_format && readback)
        gpu.getRenderer().getTextureCache().flushMemory(
            tls_crnt_command_buffer,
            ztd::Range<uptr>::fromSize(dst_desc.ptr, dst_desc.size));
}

void TwoD::copyGuestMemory(u32 bpp, u32 src_x, u32 src_y, u32 width,
                           u32 height) {
    const auto& pixels = regs.pixels_from_memory;
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
                regs.dst, (static_cast<u32>(pixels.dst_x0) + x) * bpp,
                static_cast<u32>(pixels.dst_y0) + y);
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
    // TODO: is depth always layer count? How are levels handled?
    return renderer::TextureDescriptor(
        tls_crnt_gmmu->unmapAddr(info.addr),
        info.depth > 1 ? renderer::TextureType::_2DArray
                       : renderer::TextureType::_2D,
        renderer::toTextureFormat(info.format),
        info.layout == MemoryLayout::Pitch, info.stride, info.width,
        info.height, 1, 1, std::max(info.depth, 1u), 0x0,
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
