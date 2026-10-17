#include "core/hw/tegra_x1/gpu/renderer/render_pass_cache.hpp"

#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/render_pass_base.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

RenderPassBase*
RenderPassCache::createHot0(const RenderPassDescriptor& descriptor) {
    return renderer.createRenderPass(descriptor);
}

u32 RenderPassCache::hashHot1(const RenderPassDescriptor& descriptor) {
    ztd::hash::XxHash32 hash;

    // TODO: improve this
    // TODO: also hash metadata about clears
    for (const auto& color_target : descriptor.color_targets)
        hash.add(color_target.texture);
    hash.add(descriptor.depth_stencil_target.texture);

    return hash.toHashCode();
}

// The fields of the hash
bool RenderPassCache::equals(const RenderPassDescriptor& a,
                             const RenderPassDescriptor& b) {
    for (usize i = 0; i < std::size(a.color_targets); i++) {
        if (a.color_targets[i].texture != b.color_targets[i].texture)
            return false;
    }
    return a.depth_stencil_target.texture == b.depth_stencil_target.texture;
}

void RenderPassCache::destroyElement(RenderPassBase* render_pass) {
    delete render_pass;
}

} // namespace hydra::hw::tegra_x1::gpu::renderer
