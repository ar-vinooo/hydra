#pragma once

#include "core/hw/tegra_x1/gpu/resource/resource_lifetime.hpp"

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class ShaderBase : public resource::TrackedResource {
  public:
    explicit ShaderBase(ShaderDescriptor descriptor_)
        : descriptor(std::move(descriptor_)) {}
    virtual ~ShaderBase() = default;

    // Getters
    const ShaderDescriptor& getDescriptor() const { return descriptor; }

  private:
    const ShaderDescriptor descriptor;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
