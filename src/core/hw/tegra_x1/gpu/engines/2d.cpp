#include "core/hw/tegra_x1/gpu/engines/2d.hpp"

#include "core/hw/tegra_x1/gpu/gpu.hpp"
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

    const auto src_width = static_cast<u32>(pixels.dst_width * dudx);
    const auto src_height = static_cast<u32>(pixels.dst_height * dvdy);

    gpu.getRenderer().blitTexture(
        tls_crnt_command_buffer, src,
        {static_cast<f32>(src_x0), static_cast<f32>(src_y0), 0.0f},
        {src_width, src_height, 1}, 0, regs.src.layer, dst,
        {static_cast<f32>(pixels.dst_x0), static_cast<f32>(pixels.dst_y0),
         0.0f},
        {pixels.dst_width, pixels.dst_height, 1}, 0, regs.dst.layer, 1, 1);
}

#pragma GCC diagnostic pop

renderer::TextureDescriptor
TwoD::getTextureDescriptor(const Texture2DInfo& info) const {
    // TODO: is depth always layer count? How are levels handled?
    return renderer::TextureDescriptor(
        tls_crnt_gmmu->unmapAddr(info.addr), renderer::TextureType::_2D,
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
