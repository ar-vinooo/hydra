#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

TextureView::TextureView(Texture* base, const TextureViewDescriptor& descriptor)
    : ITextureView(base, descriptor) {
    const auto& pixel_format_info = toMtlPixelFormatInfo(descriptor.format);

    // Swizzle: guest components are stored in the host channels given by the
    // component indices
    const auto to_host_swizzle = [&](ImageSwizzle swizzle) {
        switch (swizzle) {
        case ImageSwizzle::R:
        case ImageSwizzle::G:
        case ImageSwizzle::B:
        case ImageSwizzle::A: {
            const u32 component = static_cast<u32>(swizzle) -
                                  static_cast<u32>(ImageSwizzle::R);
            return toMtlSwizzle(static_cast<ImageSwizzle>(
                static_cast<u32>(ImageSwizzle::R) +
                pixel_format_info.component_indices[component]));
        }
        default:
            return toMtlSwizzle(swizzle);
        }
    };
    MTL::TextureSwizzleChannels swizzle_channels_mtl(
        to_host_swizzle(descriptor.swizzle_channels.r),
        to_host_swizzle(descriptor.swizzle_channels.g),
        to_host_swizzle(descriptor.swizzle_channels.b),
        to_host_swizzle(descriptor.swizzle_channels.a));

    auto type = descriptor.type;
    // Views of a multisampled texture are multisampled
    if (base->getTexture()->sampleCount() > 1)
        type = type == MTL::TextureType2DArray ? MTL::TextureType2DMultisampleArray
                                               : MTL::TextureType2DMultisample;

    // Stencil views of combined depth stencil textures
    auto pixel_format = pixel_format_info.pixel_format;
    if (pixel_format == MTL::PixelFormatStencil8) {
        switch (base->getTexture()->pixelFormat()) {
        case MTL::PixelFormatDepth32Float_Stencil8:
            pixel_format = MTL::PixelFormatX32_Stencil8;
            break;
        case MTL::PixelFormatDepth24Unorm_Stencil8:
            pixel_format = MTL::PixelFormatX24_Stencil8;
            break;
        default:
            break;
        }
    }

    texture = base->getTexture()->newTextureView(
        pixel_format, type,
        NS::Range(descriptor.levels.getBegin(), descriptor.levels.getSize()),
        NS::Range(descriptor.layers.getBegin(), descriptor.layers.getSize()),
        swizzle_channels_mtl);
}

TextureView::~TextureView() { texture->release(); }

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
