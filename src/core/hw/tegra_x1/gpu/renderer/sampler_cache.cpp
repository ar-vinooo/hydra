#include "core/hw/tegra_x1/gpu/renderer/sampler_cache.hpp"

#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/sampler_base.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

SamplerBase* SamplerCache::createHot0(const SamplerDescriptor& descriptor) {
    auto sampler = renderer.createSampler(descriptor);
    return sampler;
}

u32 SamplerCache::hashHot1(const SamplerDescriptor& descriptor) {
    ztd::hash::XxHash32 hash;
    hash.add(descriptor.min_filter);
    hash.add(descriptor.mag_filter);
    hash.add(descriptor.mip_filter);
    hash.add(descriptor.address_mode_s);
    hash.add(descriptor.address_mode_t);
    hash.add(descriptor.address_mode_r);
    hash.add(descriptor.depth_compare_op);
    hash.add(descriptor.border_color_u);
    hash.add(descriptor.min_lod);
    hash.add(descriptor.max_lod);
    hash.add(descriptor.lod_bias);
    hash.add(descriptor.max_anisotropy);
    hash.add(descriptor.integer_texture);

    return hash.toHashCode();
}

// The fields of the hash
bool SamplerCache::equals(const SamplerDescriptor& a,
                          const SamplerDescriptor& b) {
    return a.min_filter == b.min_filter && a.mag_filter == b.mag_filter &&
           a.mip_filter == b.mip_filter &&
           a.address_mode_s == b.address_mode_s &&
           a.address_mode_t == b.address_mode_t &&
           a.address_mode_r == b.address_mode_r &&
           a.depth_compare_op == b.depth_compare_op &&
           simd_all(a.border_color_u == b.border_color_u) && a.min_lod == b.min_lod &&
           a.max_lod == b.max_lod && a.lod_bias == b.lod_bias &&
           a.max_anisotropy == b.max_anisotropy &&
           a.integer_texture == b.integer_texture;
}

void SamplerCache::destroyElement(SamplerBase* sampler) { delete sampler; }

} // namespace hydra::hw::tegra_x1::gpu::renderer
