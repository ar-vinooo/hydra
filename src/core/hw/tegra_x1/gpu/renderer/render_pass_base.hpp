#pragma once

#include "core/hw/tegra_x1/gpu/resource/resource_lifetime.hpp"

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class RenderPassBase : public resource::TrackedResource {
  public:
    explicit RenderPassBase(const RenderPassDescriptor& descriptor_)
        : descriptor{descriptor_} {
        for (const auto& target : descriptor.color_targets)
            if (target.texture)
                attachment_owners.push_back(target.texture->getResourceOwner());
        if (descriptor.depth_stencil_target.texture)
            attachment_owners.push_back(
                descriptor.depth_stencil_target.texture->getResourceOwner());
    }
    virtual ~RenderPassBase() = default;

    // Getters
    const RenderPassDescriptor& getDescriptor() const { return descriptor; }

  protected:
    const RenderPassDescriptor descriptor;
    std::vector<std::shared_ptr<void>> attachment_owners;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
