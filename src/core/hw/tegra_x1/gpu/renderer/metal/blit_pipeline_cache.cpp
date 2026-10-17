#include "core/hw/tegra_x1/gpu/renderer/metal/blit_pipeline_cache.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

BlitPipelineCache::BlitPipelineCache(MTL::Device* device_) : device{device_} {
    // Source
    auto shader_source = R"(
        #include <metal_stdlib>
        using namespace metal;

        struct VertexBlitOut {
            float4 position [[position]];
            uint layer_id [[render_target_array_index]];
            float2 tex_coord;
        };

        constant float2 vertices[3] = {
            float2(-1.0, -3.0),
            float2(-1.0,  1.0),
            float2( 3.0,  1.0)
        };

        vertex VertexBlitOut vertex_blit(ushort vid [[vertex_id]], constant uint& layer_id [[buffer(0)]]) {
            VertexBlitOut out;
            out.position = float4(vertices[vid], 0.0, 1.0);
            out.layer_id = layer_id;
            out.tex_coord = vertices[vid] * 0.5 + 0.5;
            out.tex_coord.y = 1.0 - out.tex_coord.y;

            return out;
        }
    )";

    // Function
    auto vertex_blit =
        createFunctionFromSource(device, shader_source, "vertex_blit");

    // Pipeline descriptor
    pipeline_descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    pipeline_descriptor->setVertexFunction(vertex_blit);
    pipeline_descriptor->setInputPrimitiveTopology(
        MTL::PrimitiveTopologyClassTriangle);
}

void BlitPipelineCache::destroy() { pipeline_descriptor->release(); }

MTL::RenderPipelineState*
BlitPipelineCache::createHot0(const BlitPipelineDescriptor& descriptor) {
    // Source
    auto shader_source = R"(
        #include <metal_stdlib>
        using namespace metal;

        struct VertexBlitOut {
            float4 position [[position]];
            uint layer_id [[render_target_array_index]];
            float2 tex_coord;
        };

        struct BlitParams {
            float2 src_offset;
            float2 src_scale;
            float opacity;
        };

        fragment DST4 fragment_blit(VertexBlitOut in [[stage_in]], constant BlitParams& params [[buffer(0)]], texture2d<SRC_T> tex [[texture(0)]], sampler samplr [[sampler(0)]]) {
            float2 tex_coord = params.src_offset + in.tex_coord * params.src_scale;
            SAMPLE
            OPACITY
            return DST4(color);
        }
    )";
    // Integer textures can't be filtered, they are read texel by texel
    static const char* const types[] = {"float", "uint", "int"};
    std::string source = shader_source;
    const auto replace = [&](const std::string& from, const std::string& to) {
        for (auto pos = source.find(from); pos != std::string::npos;
             pos = source.find(from, pos + to.size()))
            source.replace(pos, from.size(), to);
    };
    const std::string src_type = types[descriptor.src_type];
    replace("SRC_T", src_type);
    replace("DST4", std::string(types[descriptor.dst_type]) + "4");
    if (descriptor.src_type == 0) {
        replace("SAMPLE", "float4 color = tex.sample(samplr, tex_coord);");
    } else {
        replace("SAMPLE",
                "int2 size = int2(tex.get_width(), tex.get_height());"
                " int2 texel = clamp(int2(floor(tex_coord * float2(size))),"
                " int2(0), size - 1);"
                " " + src_type + "4 color = tex.read(uint2(texel));");
    }
    replace("OPACITY", descriptor.src_type == 0 && descriptor.dst_type == 0
                           ? "color.a *= params.opacity;"
                           : "");

    // Function
    auto fragment_blit =
        createFunctionFromSource(device, source.c_str(), "fragment_blit");

    // Pipeline
    pipeline_descriptor->setFragmentFunction(fragment_blit);
    auto color_attachment = pipeline_descriptor->colorAttachments()->object(0);
    color_attachment->setPixelFormat(descriptor.pixel_format);
    // Guest texture copies include alpha. The console display ignores alpha;
    // keep the drawable's cleared alpha of
    // 1 so transparent game pixels do not show the window behind the view
    // (CALayer.opaque is not honored for layers hosted by SwiftUI views).
    color_attachment->setWriteMask(
        descriptor.copy_alpha
            ? MTL::ColorWriteMaskAll
            : (MTL::ColorWriteMaskRed | MTL::ColorWriteMaskGreen |
               MTL::ColorWriteMaskBlue));
    if (descriptor.transparent) {
        color_attachment->setBlendingEnabled(true);
        color_attachment->setRgbBlendOperation(MTL::BlendOperationAdd);
        color_attachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
        color_attachment->setDestinationRGBBlendFactor(
            MTL::BlendFactorOneMinusSourceAlpha);
        color_attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
        color_attachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        color_attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
    } else {
        color_attachment->setBlendingEnabled(false);
    }

    fragment_blit->release();

    NS::Error* error;
    auto pipeline = device->newRenderPipelineState(pipeline_descriptor, &error);
    if (error != nullptr) {
        LOG_ERROR(MetalRenderer, "Failed to create blit pipeline: {}",
                  error->localizedDescription()->utf8String());
        return nullptr;
    }

    return pipeline;
}

u32 BlitPipelineCache::hashHot1(const BlitPipelineDescriptor& descriptor) {
    return static_cast<u32>(descriptor.pixel_format) |
           (static_cast<u32>(descriptor.transparent) << 12) |
           (static_cast<u32>(descriptor.copy_alpha) << 13) |
           (static_cast<u32>(descriptor.src_type) << 14) |
           (static_cast<u32>(descriptor.dst_type) << 16);
}

void BlitPipelineCache::destroyElement(MTL::RenderPipelineState* pipeline) {
    pipeline->release();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
