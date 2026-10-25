#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

struct ClearDepthPipelineDescriptor {
    MTL::PixelFormat depth_stencil_pixel_format;
    // Formats of all render pass color attachments (not written to)
    MTL::PixelFormat color_pixel_formats[COLOR_TARGET_COUNT];
    u32 sample_count{1};
};

class ClearDepthPipelineCache
    : public CacheBase<ClearDepthPipelineCache, MTL::RenderPipelineState*,
                       ClearDepthPipelineDescriptor, true> {
  public:
    explicit ClearDepthPipelineCache(MTL::Device* device_);

    void destroy();

    MTL::RenderPipelineState*
    create(const ClearDepthPipelineDescriptor& descriptor);
    void update([[maybe_unused]] MTL::RenderPipelineState* pipeline) {}
    static u32 hash(const ClearDepthPipelineDescriptor& descriptor);
    static bool equals(const ClearDepthPipelineDescriptor& a,
                       const ClearDepthPipelineDescriptor& b);

    static void destroyElement(MTL::RenderPipelineState* pipeline);

  private:
    MTL::Device* device;

    MTL::RenderPipelineDescriptor* pipeline_descriptor;

  public:
    // ABI-stable implementations selected by the live module.
    MTL::RenderPipelineState*
    createHot0(const ClearDepthPipelineDescriptor& descriptor);
    static u32 hashHot1(const ClearDepthPipelineDescriptor& descriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
