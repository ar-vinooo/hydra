#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

struct ClearColorPipelineDescriptor {
    // Formats of all render pass color attachments
    MTL::PixelFormat pixel_formats[COLOR_TARGET_COUNT];
    u32 render_target_id;
    u8 mask;
    MTL::PixelFormat depth_stencil_pixel_format;
    u32 sample_count{1};
};

class ClearColorPipelineCache
    : public CacheBase<ClearColorPipelineCache, MTL::RenderPipelineState*,
                       ClearColorPipelineDescriptor, true> {
  public:
    explicit ClearColorPipelineCache(MTL::Device* device_);

    void destroy();

    MTL::RenderPipelineState*
    create(const ClearColorPipelineDescriptor& descriptor);
    void update([[maybe_unused]] MTL::RenderPipelineState* pipeline) {}
    static u32 hash(const ClearColorPipelineDescriptor& descriptor);
    static bool equals(const ClearColorPipelineDescriptor& a,
                       const ClearColorPipelineDescriptor& b);

    static void destroyElement(MTL::RenderPipelineState* pipeline);

  private:
    MTL::Device* device;

    MTL::RenderPipelineDescriptor* pipeline_descriptor;

  public:
    // ABI-stable implementations selected by the live module.
    MTL::RenderPipelineState*
    createHot0(const ClearColorPipelineDescriptor& descriptor);
    static u32 hashHot1(const ClearColorPipelineDescriptor& descriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
