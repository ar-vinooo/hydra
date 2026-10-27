#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/surface_compositor.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class CommandBuffer;
class RenderEncoder;
class Renderer;

class SurfaceCompositor final : public ISurfaceCompositor {
  public:
    SurfaceCompositor(Renderer& renderer_, CA::MetalDrawable* drawable_);
    ~SurfaceCompositor() override;

    void drawTexture(ICommandBuffer* command_buffer,
                     const ITextureView* texture, const FloatRect2D src_rect,
                     const FloatRect2D dst_rect, bool transparent,
                     f32 opacity) override;
    bool reusesPresentedFrame(bool new_frame) const override;
    void present(ICommandBuffer* command_buffer, bool new_frame) override;

  private:
    Renderer& renderer;
    CA::MetalDrawable* drawable;
    // The drawable's texture, or the frame presenter's canvas
    MTL::Texture* target;
    MTL::RenderPassDescriptor* render_pass_descriptor;
    // A render pass after the first one keeps what earlier ones drew
    bool render_pass_started{false};

    RenderEncoder* getEncoder(CommandBuffer* command_buffer);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
