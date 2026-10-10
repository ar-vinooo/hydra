#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

struct ClearDepthPipelineDescriptor {
    MTL::PixelFormat depth_stencil_pixel_format;
    // Formats of all render pass color attachments (not written to)
    MTL::PixelFormat color_pixel_formats[COLOR_TARGET_COUNT];
};

class ClearDepthPipelineCache
    : public CacheBase<ClearDepthPipelineCache, MTL::RenderPipelineState*,
                       ClearDepthPipelineDescriptor> {
  public:
    explicit ClearDepthPipelineCache(MTL::Device* device_);

    void destroy();

    MTL::RenderPipelineState*
    create(const ClearDepthPipelineDescriptor& descriptor);
    void update([[maybe_unused]] MTL::RenderPipelineState* pipeline) {}
    static u32 hash(const ClearDepthPipelineDescriptor& descriptor);

    static void destroyElement(MTL::RenderPipelineState* pipeline);

  private:
    MTL::Device* device;

    MTL::RenderPipelineDescriptor* pipeline_descriptor;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
