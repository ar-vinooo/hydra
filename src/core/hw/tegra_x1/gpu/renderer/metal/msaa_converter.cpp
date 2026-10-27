#include "core/hw/tegra_x1/gpu/renderer/metal/msaa_converter.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/encoder.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

MsaaConverter& MsaaConverter::get(MTL::Device* device) {
    static std::mutex mutex;
    static std::map<MTL::Device*, std::unique_ptr<MsaaConverter>> converters;
    std::scoped_lock lock(mutex);
    auto& converter = converters[device];
    if (!converter)
        converter.reset(new MsaaConverter(device));
    return *converter;
}

MsaaConverter::Kind MsaaConverter::getKind(MTL::PixelFormat format) {
    switch (format) {
    case MTL::PixelFormatDepth16Unorm:
    case MTL::PixelFormatDepth32Float:
        return Kind::Depth;
    case MTL::PixelFormatStencil8:
    case MTL::PixelFormatX32_Stencil8:
        return Kind::Stencil;
    case MTL::PixelFormatDepth24Unorm_Stencil8:
    case MTL::PixelFormatDepth32Float_Stencil8:
        return Kind::DepthStencil;
    case MTL::PixelFormatR8Uint:
    case MTL::PixelFormatR16Uint:
    case MTL::PixelFormatR32Uint:
    case MTL::PixelFormatRG8Uint:
    case MTL::PixelFormatRG16Uint:
    case MTL::PixelFormatRG32Uint:
    case MTL::PixelFormatRGBA8Uint:
    case MTL::PixelFormatRGB10A2Uint:
    case MTL::PixelFormatRGBA16Uint:
    case MTL::PixelFormatRGBA32Uint:
        return Kind::Uint;
    case MTL::PixelFormatR8Sint:
    case MTL::PixelFormatR16Sint:
    case MTL::PixelFormatR32Sint:
    case MTL::PixelFormatRG8Sint:
    case MTL::PixelFormatRG16Sint:
    case MTL::PixelFormatRG32Sint:
    case MTL::PixelFormatRGBA8Sint:
    case MTL::PixelFormatRGBA16Sint:
    case MTL::PixelFormatRGBA32Sint:
        return Kind::Sint;
    default:
        return Kind::Float;
    }
}

MsaaConverter::Pipeline
MsaaConverter::getPipeline(MTL::PixelFormat target_format, u32 target_samples,
                           u32 guest_samples, u32 host_samples, bool pack) {
    std::scoped_lock lock(mutex);
    const auto key = std::make_tuple(target_format, target_samples,
                                     guest_samples, host_samples, pack);
    if (const auto it = pipelines.find(key); it != pipelines.end())
        return it->second;

    const auto layout = getMsaaLayoutForSamples(guest_samples);
    const u32 block_width = 1u << layout.samples_x_log2;
    const u32 block_height = 1u << layout.samples_y_log2;
    // The guest sample of each texel of a block, and the texel of each sample
    std::string texels;
    std::vector<u32> indices(block_width * block_height, 0);
    for (u32 i = 0; i < guest_samples; i++) {
        const auto [x, y] = getMsaaSampleTexel(guest_samples, i);
        texels += fmt::format("uint2({}, {}),", x, y);
        indices[y * block_width + x] = i;
    }
    std::string index_list;
    for (const auto index : indices)
        index_list += fmt::format("{},", index);

    const auto kind = getKind(target_format);
    std::string declarations;
    std::string output;
    std::string body;
    // The pixel of a texel, the sample of the texel (expand), or the texel
    // of a sample (pack)
    const std::string coordinates =
        pack ? "uint2 texel = (uint2(in.position.xy) << uint2(SX, SY)) | "
               "SAMPLE_TEXEL[sample_id];\n"
             : "uint2 position = uint2(in.position.xy);\n"
               "uint2 pixel = position >> uint2(SX, SY);\n"
               "uint2 local = position & uint2(BW - 1, BH - 1);\n"
               "uint sample = SAMPLE_INDEX[local.y * BW + local.x] % "
               "HOST_SAMPLES;\n";
    const auto read = [&](const char* texture) {
        return pack ? fmt::format("{}.read(texel)", texture)
                    : fmt::format("{}.read(pixel, sample)", texture);
    };
    const char* ms = pack ? "" : "_ms";
    switch (kind) {
    case Kind::Float:
    case Kind::Uint:
    case Kind::Sint: {
        const char* type = kind == Kind::Float ? "float"
                           : kind == Kind::Uint ? "uint"
                                                : "int";
        declarations = fmt::format("texture2d{}<{}> source [[texture(0)]]", ms, type);
        output = fmt::format("{}4", type);
        body = fmt::format("return {};", read("source"));
        break;
    }
    case Kind::Depth:
        declarations = fmt::format("depth2d{}<float> source [[texture(0)]]", ms);
        output = "DepthOut";
        body = fmt::format("DepthOut out; out.depth = {}; return out;", read("source"));
        break;
    case Kind::Stencil:
        declarations = fmt::format("texture2d{}<uint> stencil [[texture(1)]]", ms);
        output = "StencilOut";
        body = fmt::format("StencilOut out; out.stencil = {}.r; return out;",
                           read("stencil"));
        break;
    case Kind::DepthStencil:
        declarations = fmt::format("depth2d{0}<float> source [[texture(0)]], "
                                   "texture2d{0}<uint> stencil [[texture(1)]]",
                                   ms);
        output = "DepthStencilOut";
        body = fmt::format("DepthStencilOut out; out.depth = {}; out.stencil = "
                           "{}.r; return out;",
                           read("source"), read("stencil"));
        break;
    }
    const auto source = fmt::format(
        R"(#include <metal_stdlib>
using namespace metal;
constant uint SX = {};
constant uint SY = {};
constant uint BW = {};
constant uint BH = {};
constant uint HOST_SAMPLES = {};
constant uint2 SAMPLE_TEXEL[] = {{ {} }};
constant uint SAMPLE_INDEX[] = {{ {} }};
struct VertexOut {{ float4 position [[position]]; }};
struct DepthOut {{ float depth [[depth(any)]]; }};
struct StencilOut {{ uint stencil [[stencil]]; }};
struct DepthStencilOut {{ float depth [[depth(any)]]; uint stencil [[stencil]]; }};
vertex VertexOut vertex_main(uint vid [[vertex_id]]) {{
    const float2 position = float2(float((vid << 1) & 2), float(vid & 2));
    VertexOut out;
    out.position = float4(position * 2.0 - 1.0, 0.0, 1.0);
    return out;
}}
fragment {} fragment_main(VertexOut in [[stage_in]], {}{}) {{
    {}
    {}
}}
)",
        layout.samples_x_log2, layout.samples_y_log2, block_width, block_height,
        host_samples, texels, index_list, output, declarations,
        pack ? ", uint sample_id [[sample_id]]" : "", coordinates, body);

    auto* library = createLibraryFromSource(device, source);
    ASSERT(library != nullptr, MetalRenderer, "MSAA conversion shader failed to compile");
    auto* vertex_function = library->newFunction(toNsString("vertex_main"));
    auto* fragment_function = library->newFunction(toNsString("fragment_main"));
    library->release();

    NS_STACK_SCOPED auto* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(vertex_function);
    descriptor->setFragmentFunction(fragment_function);
    descriptor->setRasterSampleCount(target_samples);
    switch (kind) {
    case Kind::Depth:
        descriptor->setDepthAttachmentPixelFormat(target_format);
        break;
    case Kind::Stencil:
        descriptor->setStencilAttachmentPixelFormat(target_format);
        break;
    case Kind::DepthStencil:
        descriptor->setDepthAttachmentPixelFormat(target_format);
        descriptor->setStencilAttachmentPixelFormat(target_format);
        break;
    default:
        descriptor->colorAttachments()->object(0)->setPixelFormat(target_format);
        break;
    }
    NS::Error* error = nullptr;
    Pipeline pipeline;
    pipeline.state = device->newRenderPipelineState(descriptor, &error);
    ASSERT(pipeline.state != nullptr, MetalRenderer,
           "MSAA conversion pipeline failed: {}",
           error != nullptr ? error->localizedDescription()->utf8String() : "");
    vertex_function->release();
    fragment_function->release();

    // Depth and stencil come from the shader, every sample is written
    NS_STACK_SCOPED auto* depth_stencil = MTL::DepthStencilDescriptor::alloc()->init();
    if (kind == Kind::Depth || kind == Kind::DepthStencil) {
        depth_stencil->setDepthCompareFunction(MTL::CompareFunctionAlways);
        depth_stencil->setDepthWriteEnabled(true);
    }
    if (kind == Kind::Stencil || kind == Kind::DepthStencil) {
        NS_STACK_SCOPED auto* stencil = MTL::StencilDescriptor::alloc()->init();
        stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
        stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
        stencil->setStencilFailureOperation(MTL::StencilOperationReplace);
        stencil->setDepthFailureOperation(MTL::StencilOperationReplace);
        stencil->setReadMask(0xff);
        stencil->setWriteMask(0xff);
        depth_stencil->setFrontFaceStencil(stencil);
        depth_stencil->setBackFaceStencil(stencil);
    }
    pipeline.depth_stencil = device->newDepthStencilState(depth_stencil);
    pipelines.emplace(key, pipeline);
    return pipeline;
}

void MsaaConverter::convert(CommandBuffer* command_buffer, MTL::Texture* source,
                            MTL::Texture* target, u32 guest_samples,
                            bool pack) {
    MTL::Texture* multisampled = pack ? target : source;
    const u32 host_samples = static_cast<u32>(multisampled->sampleCount());
    const auto format = target->pixelFormat();
    const auto kind = getKind(format);
    const auto pipeline = getPipeline(format, static_cast<u32>(target->sampleCount()),
                                      guest_samples, host_samples, pack);
    const auto slices = static_cast<u32>(target->arrayLength());
    const bool target_array =
        target->textureType() == MTL::TextureType2DArray ||
        target->textureType() == MTL::TextureType2DMultisampleArray;
    const bool source_array =
        source->textureType() == MTL::TextureType2DArray ||
        source->textureType() == MTL::TextureType2DMultisampleArray;
    const auto slice_type = [](MTL::Texture* texture) {
        return texture->sampleCount() > 1 ? MTL::TextureType2DMultisample
                                          : MTL::TextureType2D;
    };
    // Stencil is read through a view of the stencil plane
    const auto stencil_format = [](MTL::PixelFormat combined) {
        return combined == MTL::PixelFormatDepth24Unorm_Stencil8
                   ? MTL::PixelFormatX24_Stencil8
               : combined == MTL::PixelFormatStencil8
                   ? MTL::PixelFormatStencil8
                   : MTL::PixelFormatX32_Stencil8;
    };
    for (u32 slice = 0; slice < std::min<u32>(slices, static_cast<u32>(source->arrayLength())); slice++) {
        const auto view = [&](MTL::Texture* texture, bool array,
                              MTL::PixelFormat view_format) {
            auto* result = texture->newTextureView(
                view_format, slice_type(texture), NS::Range(0, 1),
                NS::Range(array ? slice : 0, 1));
            command_buffer->retainResource(std::shared_ptr<void>(
                result, [](void* object) {
                    static_cast<MTL::Texture*>(object)->release();
                }));
            return result;
        };
        auto* target_view = view(target, target_array, format);

        NS_STACK_SCOPED auto* pass = MTL::RenderPassDescriptor::alloc()->init();
        if (kind == Kind::Depth || kind == Kind::DepthStencil) {
            auto* depth = pass->depthAttachment();
            depth->setTexture(target_view);
            depth->setLoadAction(MTL::LoadActionDontCare);
            depth->setStoreAction(MTL::StoreActionStore);
        }
        if (kind == Kind::Stencil || kind == Kind::DepthStencil) {
            auto* stencil = pass->stencilAttachment();
            stencil->setTexture(target_view);
            stencil->setLoadAction(MTL::LoadActionDontCare);
            stencil->setStoreAction(MTL::StoreActionStore);
        }
        if (kind != Kind::Depth && kind != Kind::Stencil &&
            kind != Kind::DepthStencil) {
            auto* color = pass->colorAttachments()->object(0);
            color->setTexture(target_view);
            color->setLoadAction(MTL::LoadActionDontCare);
            color->setStoreAction(MTL::StoreActionStore);
        }

        auto* encoder = command_buffer->getDirectRenderCommandEncoder(
            pass, pipeline.depth_stencil);
        encoder->setRenderPipelineState(pipeline.state);
        encoder->setDepthStencilState(pipeline.depth_stencil);
        encoder->setViewport(MTL::Viewport(
            0.0, 0.0, static_cast<f64>(target_view->width()),
            static_cast<f64>(target_view->height()), 0.0, 1.0));
        encoder->setScissorRect(
            MTL::ScissorRect(0, 0, target_view->width(), target_view->height()));
        if (kind != Kind::Stencil)
            encoder->setFragmentTexture(
                view(source, source_array, source->pixelFormat()),
                static_cast<NS::UInteger>(0));
        if (kind == Kind::Stencil || kind == Kind::DepthStencil)
            encoder->setFragmentTexture(
                view(source, source_array, stencil_format(source->pixelFormat())),
                static_cast<NS::UInteger>(1));
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                static_cast<NS::UInteger>(0),
                                static_cast<NS::UInteger>(3));
    }
    command_buffer->track(source);
    command_buffer->track(target);
}

void MsaaConverter::expand(CommandBuffer* command_buffer,
                           MTL::Texture* multisampled, MTL::Texture* scaled,
                           u32 guest_samples) {
    convert(command_buffer, multisampled, scaled, guest_samples, false);
}

void MsaaConverter::pack(CommandBuffer* command_buffer, MTL::Texture* scaled,
                         MTL::Texture* multisampled, u32 guest_samples) {
    convert(command_buffer, scaled, multisampled, guest_samples, true);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
