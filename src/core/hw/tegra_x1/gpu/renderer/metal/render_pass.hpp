#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/render_pass_base.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class RenderPass final : public RenderPassBase {
  public:
    explicit RenderPass(const RenderPassDescriptor& descriptor);
    ~RenderPass() override;

    // Getters
    MTL::RenderPassDescriptor* getRenderPassDescriptor() const {
        return render_pass_descriptor;
    }
    u32 getWidth() const { return width; }
    u32 getHeight() const { return height; }

  private:
    MTL::RenderPassDescriptor* render_pass_descriptor;
    // Smallest attachment size (scissors must not exceed it)
    u32 width{invalid<u32>()};
    u32 height{invalid<u32>()};
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
