#include "core/hw/tegra_x1/gpu/renderer/metal/clear_depth_pipeline_cache.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

ClearDepthPipelineCache::ClearDepthPipelineCache(MTL::Device* device_)
    : device{device_} {
    // Source
    auto shader_source = R"(
        #include <metal_stdlib>
        using namespace metal;

        struct VertexClearDepthOut {
            float4 position [[position]];
            uint layer_id [[render_target_array_index]];
        };

        constant float2 vertices[3] = {
            float2(-1.0, -3.0),
            float2(-1.0,  1.0),
            float2( 3.0,  1.0)
        };

        struct VertexClearDepthParams {
            uint layer_id;
            float value;
        };

        vertex VertexClearDepthOut vertex_clear_depth(ushort vid [[vertex_id]], constant VertexClearDepthParams& params [[buffer(0)]]) {
            VertexClearDepthOut out;
            out.position = float4(vertices[vid], params.value, 1.0);
            out.layer_id = params.layer_id;

            return out;
        }
    )";

    // Function
    auto vertex_clear_depth =
        createFunctionFromSource(device, shader_source, "vertex_clear_depth");

    // Pipeline descriptor
    pipeline_descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    pipeline_descriptor->setVertexFunction(vertex_clear_depth);
    pipeline_descriptor->setInputPrimitiveTopology(
        MTL::PrimitiveTopologyClassTriangle);
}

void ClearDepthPipelineCache::destroy() { pipeline_descriptor->release(); }

MTL::RenderPipelineState*
ClearDepthPipelineCache::createHot0(
    const ClearDepthPipelineDescriptor& descriptor) {
    // Pipeline
    setDepthStencilPixelFormat(pipeline_descriptor,
                               descriptor.depth_stencil_pixel_format);
    pipeline_descriptor->setRasterSampleCount(descriptor.sample_count);
    for (u32 i = 0; i < COLOR_TARGET_COUNT; i++) {
        auto attachment = pipeline_descriptor->colorAttachments()->object(i);
        attachment->setPixelFormat(descriptor.color_pixel_formats[i]);
        attachment->setWriteMask(MTL::ColorWriteMaskNone);
    }

    NS::Error* error;
    auto pipeline = device->newRenderPipelineState(pipeline_descriptor, &error);
    if (error != nullptr) {
        LOG_ERROR(MetalRenderer, "Failed to create clear depth pipeline: {}",
                  error->localizedDescription()->utf8String());
        return nullptr;
    }

    return pipeline;
}

u32 ClearDepthPipelineCache::hashHot1(
    const ClearDepthPipelineDescriptor& descriptor) {
    ztd::hash::XxHash32 hash;
    hash.add(descriptor.depth_stencil_pixel_format);
    for (const auto pixel_format : descriptor.color_pixel_formats)
        hash.add(pixel_format);
    hash.add(descriptor.sample_count);
    return hash.toHashCode();
}

// The fields of the hash
bool ClearDepthPipelineCache::equals(const ClearDepthPipelineDescriptor& a,
                                     const ClearDepthPipelineDescriptor& b) {
    return a.depth_stencil_pixel_format == b.depth_stencil_pixel_format &&
           std::equal(std::begin(a.color_pixel_formats),
                      std::end(a.color_pixel_formats),
                      std::begin(b.color_pixel_formats)) &&
           a.sample_count == b.sample_count;
}

void ClearDepthPipelineCache::destroyElement(
    MTL::RenderPipelineState* pipeline) {
    pipeline->release();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
