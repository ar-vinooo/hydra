#pragma once

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class ICommandBuffer;
class ITextureView;

class ISurfaceCompositor {
  public:
    virtual ~ISurfaceCompositor() = default;

    virtual void drawTexture(ICommandBuffer* command_buffer,
                             const ITextureView* texture,
                             const FloatRect2D src_rect,
                             const FloatRect2D dst_rect, bool transparent,
                             f32 opacity = 1.0f) = 0;
    // Whether a refresh without a new guest frame shows a frame the
    // presenter already holds, so the layers need not be drawn (or upscaled)
    // again
    virtual bool reusesPresentedFrame(bool new_frame) const {
        (void)new_frame;
        return false;
    }
    // `new_frame` is whether a layer got a new guest frame for this refresh
    virtual void present(ICommandBuffer* command_buffer, bool new_frame) = 0;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
