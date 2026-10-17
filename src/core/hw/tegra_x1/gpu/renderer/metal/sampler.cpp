#include "core/hw/tegra_x1/gpu/renderer/metal/sampler.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

Sampler::Sampler(MTL::Device* device, const SamplerDescriptor& descriptor)
    : SamplerBase(descriptor) {
    MTL::SamplerDescriptor* desc = MTL::SamplerDescriptor::alloc()->init();
    desc->setSupportArgumentBuffers(true);
    desc->setMinFilter(toMtlSamplerMinMagFilter(descriptor.min_filter));
    desc->setMagFilter(toMtlSamplerMinMagFilter(descriptor.mag_filter));
    desc->setMipFilter(toMtlSamplerMipFilter(descriptor.mip_filter));
    desc->setLodMinClamp(descriptor.min_lod);
    desc->setLodMaxClamp(descriptor.max_lod);
    // Bias is combined with the instruction bias in generated MSL.
    if (descriptor.min_filter != SamplerFilter::Nearest &&
        descriptor.mip_filter == SamplerMipFilter::Linear)
        desc->setMaxAnisotropy(std::clamp(descriptor.max_anisotropy, 1u, 16u));
    // CLAMP_OGL clamps to the border when filtering linearly (like Eden)
    const bool linear = descriptor.min_filter != SamplerFilter::Nearest ||
                        descriptor.mag_filter != SamplerFilter::Nearest;
    const auto address_mode = [linear](SamplerAddressMode mode) {
        if (mode == SamplerAddressMode::Clamp && linear)
            return MTL::SamplerAddressModeClampToBorderColor;
        return toMtlSamplerAddressMode(mode);
    };
    desc->setSAddressMode(address_mode(descriptor.address_mode_s));
    desc->setTAddressMode(address_mode(descriptor.address_mode_t));
    desc->setRAddressMode(address_mode(descriptor.address_mode_r));
    if (descriptor.depth_compare_op != engines::CompareOp::Invalid)
        desc->setCompareFunction(toMtlCompareFunc(descriptor.depth_compare_op));

    // Border color: Metal has transparent black, opaque black and opaque
    // white. Use the closest one (a color between them is approximated).
    const auto border_color = std::bit_cast<float4>(descriptor.border_color_u);
    const f32 alpha = std::clamp(border_color.w(), 0.0f, 1.0f);
    const f32 luminance = (std::clamp(border_color.x(), 0.0f, 1.0f) +
                           std::clamp(border_color.y(), 0.0f, 1.0f) +
                           std::clamp(border_color.z(), 0.0f, 1.0f)) /
                          3.0f;
    const auto distance = [&](f32 white, f32 target_alpha) {
        return (luminance - white) * (luminance - white) +
               (alpha - target_alpha) * (alpha - target_alpha);
    };
    MTL::SamplerBorderColor mtl_border_color =
        MTL::SamplerBorderColorTransparentBlack;
    f32 best = distance(0.0f, 0.0f);
    if (const f32 d = distance(0.0f, 1.0f); d < best) {
        best = d;
        mtl_border_color = MTL::SamplerBorderColorOpaqueBlack;
    }
    if (const f32 d = distance(1.0f, 1.0f); d < best) {
        best = d;
        mtl_border_color = MTL::SamplerBorderColorOpaqueWhite;
    }
    if (best > 0.0f) {
        ONCE(LOG_WARN(MetalRenderer,
                      "Unsupported border color {}, using the closest preset",
                      border_color));
    }
    desc->setBorderColor(mtl_border_color);

    mtl_sampler = device->newSamplerState(desc);
}

Sampler::~Sampler() { mtl_sampler->release(); }

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
