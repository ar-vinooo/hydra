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
    desc->setMipFilter(descriptor.mip_filter);
    desc->setLodMinClamp(descriptor.min_lod);
    desc->setLodMaxClamp(descriptor.max_lod);
    // Bias is combined with the instruction bias in generated MSL.
    desc->setMaxAnisotropy(getEffectiveSamplerAnisotropy(descriptor));
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

    // Metal only has the three presets. Other colors (and mirrored-to-border
    // addressing) are emulated by the shader, which does not use this sampler
    // for those bindings; the transparent preset is the neutral base there.
    MTL::SamplerBorderColor mtl_border_color =
        MTL::SamplerBorderColorTransparentBlack;
    switch (getSamplerBorderPreset(descriptor)) {
    case SamplerBorderPreset::OpaqueBlack:
        mtl_border_color = MTL::SamplerBorderColorOpaqueBlack;
        break;
    case SamplerBorderPreset::OpaqueWhite:
        mtl_border_color = MTL::SamplerBorderColorOpaqueWhite;
        break;
    default:
        break;
    }
    desc->setBorderColor(mtl_border_color);

    mtl_sampler = device->newSamplerState(desc);
}

Sampler::~Sampler() { mtl_sampler->release(); }

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
