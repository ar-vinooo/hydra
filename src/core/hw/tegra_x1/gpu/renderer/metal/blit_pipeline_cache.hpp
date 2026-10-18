#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

struct BlitParams {
    simd::float2 src_offset;
    simd::float2 src_scale;
    f32 opacity;
    f32 _padding; // Match MSL struct size (aligned to float2)
};

struct BlitPipelineDescriptor {
    MTL::PixelFormat pixel_format;
    bool transparent;
    bool copy_alpha = false;
    // Component type of the source and destination (float, uint or int)
    MTL::DataType src_type = MTL::DataTypeFloat;
    MTL::DataType dst_type = MTL::DataTypeFloat;
};

class BlitPipelineCache
    : public CacheBase<BlitPipelineCache, MTL::RenderPipelineState*,
                       BlitPipelineDescriptor> {
  public:
    explicit BlitPipelineCache(MTL::Device* device_);

    void destroy();

    MTL::RenderPipelineState* create(const BlitPipelineDescriptor& descriptor);
    void update([[maybe_unused]] MTL::RenderPipelineState* pipeline) {}
    u32 hash(const BlitPipelineDescriptor& descriptor);

    void destroyElement(MTL::RenderPipelineState* pipeline);

  private:
    MTL::Device* device;

    MTL::RenderPipelineDescriptor* pipeline_descriptor;

  public:
    // ABI-stable implementations selected by the live module.
    MTL::RenderPipelineState*
    createHot0(const BlitPipelineDescriptor& descriptor);
    u32 hashHot1(const BlitPipelineDescriptor& descriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
