#pragma once

#include "core/hw/tegra_x1/gpu/resource/resource_lifetime.hpp"

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class PipelineBase : public resource::TrackedResource {
  public:
    explicit PipelineBase(const PipelineDescriptor& descriptor_)
        : descriptor{descriptor_} {
        for (auto* shader : descriptor.shaders)
            if (shader)
                shader_owners.push_back(shader->getResourceOwner());
    }
    virtual ~PipelineBase() = default;

    // Getters
    const PipelineDescriptor& getDescriptor() const { return descriptor; }

  protected:
    const PipelineDescriptor descriptor;
    std::vector<std::shared_ptr<void>> shader_owners;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
