#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

#include <cmath>
#include <fstream>

#include <objc/message.h>
#include <objc/runtime.h>

#include <stb_image_write.h>

#include "common/config.hpp"
#include "core/hw/tegra_x1/gpu/engines/3d.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/pipeline.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/render_pass.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/sampler.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/shader.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/surface_compositor.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"

// TODO: define in a separate file
/*
const std::string utility_shader_source = R"(
#include <metal_stdlib>
using namespace metal;
)";
*/

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

Renderer::Renderer()
    : device{MTL::CreateSystemDefaultDevice()},
      command_queue{device->newCommandQueue()},
      upload_buffer_pool(std::make_shared<UploadBufferPool>(device)),
      binary_archive_cache(device), shader_library_cache(device),
      depth_stencil_state_cache(device),
      blit_pipeline_cache(device), clear_color_pipeline_cache(device),
      clear_depth_pipeline_cache(device) {
    // Resources

    // Depth stencil states

    // Always + write
    auto depth_stencil_descriptor =
        MTL::DepthStencilDescriptor::alloc()->init();
    depth_stencil_descriptor->setDepthWriteEnabled(true);
    depth_stencil_state_always_and_write =
        device->newDepthStencilState(depth_stencil_descriptor);
    depth_stencil_descriptor->release();

    // No depth or stencil test (color clears)
    {
        auto desc = MTL::DepthStencilDescriptor::alloc()->init();
        depth_stencil_state_disabled = device->newDepthStencilState(desc);
        desc->release();
    }

    // Clear stencil (depth untouched)
    {
        auto desc = MTL::DepthStencilDescriptor::alloc()->init();
        auto stencil = MTL::StencilDescriptor::alloc()->init();
        stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
        stencil->setStencilFailureOperation(MTL::StencilOperationReplace);
        stencil->setDepthFailureOperation(MTL::StencilOperationReplace);
        stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
        stencil->setWriteMask(0xff);
        desc->setFrontFaceStencil(stencil);
        desc->setBackFaceStencil(stencil);
        depth_stencil_state_clear_stencil = device->newDepthStencilState(desc);
        stencil->release();
        desc->release();
    }

    // Sampler states

    // Nearest
    auto sampler_state_descriptor = MTL::SamplerDescriptor::alloc()->init();
    nearest_sampler = device->newSamplerState(sampler_state_descriptor);

    // Linear
    sampler_state_descriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
    sampler_state_descriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
    linear_sampler = device->newSamplerState(sampler_state_descriptor);
    sampler_state_descriptor->release();

    // Info
    info = {
        .supports_quads_primitive = false,
    };
}

Renderer::~Renderer() {
    for (auto& [function, pipeline] : compute_pipelines) {
        if (pipeline != nullptr)
            pipeline->release();
    }

    linear_sampler->release();
    nearest_sampler->release();

    depth_stencil_state_always_and_write->release();
    depth_stencil_state_clear_stencil->release();
    depth_stencil_state_disabled->release();

    // TODO: destroy caches here?

    command_queue->release();
    device->release();
}

void Renderer::setSurfaceHot0(void* surface) {
    ca_layer = reinterpret_cast<CA::MetalLayer*>(surface);
    ca_layer->setDevice(device);
    // The console display ignores alpha. Games often clear to transparent
    // black, which would otherwise show whatever is behind the view.
    // (metal-cpp has no wrapper for CALayer.opaque)
    reinterpret_cast<void (*)(void*, SEL, BOOL)>(objc_msgSend)(
        ca_layer, sel_registerName("setOpaque:"), YES);
    // TODO: set pixel format
}

ISurfaceCompositor* Renderer::acquireNextSurfaceHot1() {
    // Drawable
    if (ca_layer == nullptr)
        return nullptr;

    ca_drawable = ca_layer->nextDrawable();
    if (ca_drawable == nullptr)
        return nullptr;

    return new SurfaceCompositor(*this, ca_drawable);
}

BufferBase* Renderer::createBufferHot2(u64 size) {
    return new Buffer(device, size);
}

// Shared by snapshot allocation and its last-owner deleter. A snapshot remains
// ineligible for reuse until both CPU views and prior GPU commands release it.
std::shared_ptr<UploadBufferPool> snapshotBufferPool(MTL::Device* device) {
    static std::mutex mutex;
    static std::map<MTL::Device*, std::weak_ptr<UploadBufferPool>> pools;
    std::scoped_lock lock(mutex);
    auto pool = pools[device].lock();
    if (!pool) {
        pool = std::make_shared<UploadBufferPool>(device, 64 * 1024 * 1024, 8192);
        pools[device] = pool;
    }
    return pool;
}

BufferBase* Renderer::createCpuBufferSnapshotHot3(ztd::Range<uptr> range) {
    static const bool disabled =
        std::getenv("HYDRA_DISABLE_BUFFER_SNAPSHOTS") != nullptr;
    if (disabled || range.getSize() > 64 * 1024)
        return nullptr;
    BufferBase* snapshot =
        new Buffer(snapshotBufferPool(device)->acquire(range.getSize()));
    snapshot->copyFrom(range.getBegin());
    if (PerfStats::isEnabled()) {
        PerfStats::instance().buffer_snapshots++;
        PerfStats::instance().buffer_snapshot_bytes += range.getSize();
    }
    return snapshot;
}

BufferBase* Renderer::allocateTemporaryBufferHot4(const u64 size) {
    // TODO: use a buffer allocator instead
    auto buffer = device->newBuffer(size, MTL::ResourceStorageModeShared);
    return new Buffer(buffer);
}

BufferBase* Renderer::allocateTextureUploadBufferHot5(u64 size) {
    static const bool disabled =
        std::getenv("HYDRA_DISABLE_UPLOAD_POOL") != nullptr;
    if (disabled) {
        if (PerfStats::isEnabled())
            PerfStats::instance().upload_buffers_created++;
        return allocateTemporaryBuffer(size);
    }
    return new Buffer(upload_buffer_pool->acquire(size));
}

void Renderer::retireTextureUploadBufferHot6(ICommandBuffer* command_buffer,
                                         BufferBase* buffer) {
    static const bool disabled =
        std::getenv("HYDRA_DISABLE_UPLOAD_POOL") != nullptr;
    if (!disabled) {
        auto* native = static_cast<Buffer*>(buffer)->getBuffer()->retain();
        static_cast<CommandBuffer*>(command_buffer)
            ->retireUploadBuffer(upload_buffer_pool, native);
    }
    delete buffer;
}

void Renderer::freeTemporaryBufferHot7(BufferBase* buffer) {
    auto buffer_impl = static_cast<Buffer*>(buffer);

    // TODO: use a buffer allocator instead
    delete buffer_impl;
}

ITexture* Renderer::createTextureHot8(const TextureDescriptor& descriptor) {
    return new Texture(device, descriptor);
}

void Renderer::blitTextureHot9(ICommandBuffer* command_buffer, ITextureView* src,
                           simd::float3 src_origin, MTL::Size src_size, u32 src_level,
                           u32 src_layer, ITextureView* dst, simd::float3 dst_origin,
                           MTL::Size dst_size, u32 dst_level, u32 dst_layer,
                           u32 level_count, u32 layer_count, const BlitOptions& options) {
    // TODO: what about 3D textures?

    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);
    const auto src_impl = static_cast<TextureView*>(src);
    const auto dst_impl = static_cast<TextureView*>(dst);

    // Depth stencil textures can't be render targets of the color blit
    // pipeline, so copy them directly. Scaling is not supported in this case
    const auto src_mtl = src_impl->getTexture();
    const auto dst_mtl = dst_impl->getTexture();
    const auto is_depth_stencil = [](MTL::PixelFormat format) {
        switch (format) {
        case MTL::PixelFormatDepth16Unorm:
        case MTL::PixelFormatDepth32Float:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatStencil8:
            return true;
        default:
            return false;
        }
    };
    if (is_depth_stencil(dst_mtl->pixelFormat()) ||
        is_depth_stencil(src_mtl->pixelFormat())) {
        if (src_mtl->pixelFormat() != dst_mtl->pixelFormat()) {
            ONCE(LOG_NOT_IMPLEMENTED(
                MetalRenderer, "Depth stencil blit between different formats"));
            return;
        }
        if (src_size.width != dst_size.width || src_size.height != dst_size.height)
            ONCE(
                LOG_WARN(MetalRenderer,
                         "Scaled depth stencil blit, copying without scaling"));

        // Clamp the region to both textures
        const auto src_x = static_cast<u32>(src_origin.x);
        const auto src_y = static_cast<u32>(src_origin.y);
        const auto dst_x = static_cast<u32>(dst_origin.x);
        const auto dst_y = static_cast<u32>(dst_origin.y);
        const auto width = static_cast<u32>(std::min<u64>(
            {dst_size.width,
             src_mtl->width() - std::min<u64>(src_x, src_mtl->width()),
             dst_mtl->width() - std::min<u64>(dst_x, dst_mtl->width())}));
        const auto height = static_cast<u32>(std::min<u64>(
            {dst_size.height,
             src_mtl->height() - std::min<u64>(src_y, src_mtl->height()),
             dst_mtl->height() - std::min<u64>(dst_y, dst_mtl->height())}));
        if (width == 0 || height == 0)
            return;

        auto blit_encoder = command_buffer_impl->getBlitCommandEncoder();
        blit_encoder->copyFromTexture(
            src_mtl, src_layer, 0, MTL::Origin(src_x, src_y, 0),
            MTL::Size(width, height, 1), dst_mtl, dst_layer, 0,
            MTL::Origin(dst_x, dst_y, 0));
        return;
    }

    const auto is_2d = [](MTL::TextureType type) {
        return type == MTL::TextureType2D || type == MTL::TextureType2DArray;
    };
    const auto integral_origin = [](simd::float3 origin) {
        return std::isfinite(origin.x) && std::isfinite(origin.y) &&
               origin.x >= 0 && origin.y >= 0 && origin.z == 0 &&
               std::floor(origin.x) == origin.x &&
               std::floor(origin.y) == origin.y;
    };
    if (src_mtl != dst_mtl && src_mtl->pixelFormat() == dst_mtl->pixelFormat() &&
        !isTextureFormatCompressed(src->getBase()->getDescriptor().format) &&
        !isTextureFormatCompressed(dst->getBase()->getDescriptor().format) &&
        is_2d(src_mtl->textureType()) && is_2d(dst_mtl->textureType()) &&
        src_mtl->sampleCount() == 1 && dst_mtl->sampleCount() == 1 &&
        level_count == 1 && layer_count == 1 &&
        src_level < src_mtl->mipmapLevelCount() &&
        dst_level < dst_mtl->mipmapLevelCount() &&
        src_layer < src_mtl->arrayLength() && dst_layer < dst_mtl->arrayLength() &&
        src_size.width == dst_size.width && src_size.height == dst_size.height &&
        src_size.depth == 1 && dst_size.depth == 1 &&
        (options.src_width == 0.0f ||
         options.src_width == static_cast<f32>(src_size.width)) &&
        (options.src_height == 0.0f ||
         options.src_height == static_cast<f32>(src_size.height)) &&
        integral_origin(src_origin) && integral_origin(dst_origin)) {
        const auto src_width = std::max<u64>(src_mtl->width() >> src_level, 1);
        const auto src_height = std::max<u64>(src_mtl->height() >> src_level, 1);
        const auto dst_width = std::max<u64>(dst_mtl->width() >> dst_level, 1);
        const auto dst_height = std::max<u64>(dst_mtl->height() >> dst_level, 1);
        if (static_cast<f64>(src_origin.x) <= static_cast<f64>(src_width) &&
            static_cast<f64>(src_origin.y) <= static_cast<f64>(src_height) &&
            static_cast<f64>(dst_origin.x) <= static_cast<f64>(dst_width) &&
            static_cast<f64>(dst_origin.y) <= static_cast<f64>(dst_height)) {
            const auto sx = static_cast<u64>(src_origin.x);
            const auto sy = static_cast<u64>(src_origin.y);
            const auto dx = static_cast<u64>(dst_origin.x);
            const auto dy = static_cast<u64>(dst_origin.y);
            if (src_size.width <= src_width - sx && src_size.height <= src_height - sy &&
                dst_size.width <= dst_width - dx && dst_size.height <= dst_height - dy) {
                if (src_size.width == 0 || src_size.height == 0) return;
                auto encoder = command_buffer_impl->getBlitCommandEncoder();
                encoder->copyFromTexture(src_mtl, src_layer, src_level,
                    MTL::Origin(sx, sy, 0), MTL::Size(src_size.width, src_size.height, 1),
                    dst_mtl, dst_layer, dst_level, MTL::Origin(dx, dy, 0));
                if (auto& visual = VisualDiagnostics::instance(); visual.active())
                    visual.event("native_texture_copy", {
                        {"width", std::to_string(src_size.width)},
                        {"height", std::to_string(src_size.height)},
                        {"components", "all_raw_bits_including_alpha"}});
                return;
            }
        }
    }

    // Render pass
    NS_STACK_SCOPED auto render_pass_descriptor =
        MTL::RenderPassDescriptor::alloc()->init();
    auto color_attachment =
        render_pass_descriptor->colorAttachments()->object(0);
    color_attachment->setTexture(dst_impl->getTexture());
    color_attachment->setLoadAction(
        MTL::LoadActionLoad); // TODO: use don't care if blitting to the whole
                              // texture
    color_attachment->setStoreAction(MTL::StoreActionStore);
    render_pass_descriptor->setRenderTargetArrayLength(
        getRenderTargetArrayLength(dst_impl->getTexture()));

    auto encoder =
        command_buffer_impl->createRenderCommandEncoder(render_pass_descriptor);

    // Draw
    const auto data_type = [](ITextureView* view) {
        const auto type = getIntegerTextureComponentType(
            view->getBase()->getDescriptor().format);
        return type == nullptr   ? MTL::DataTypeFloat
               : type[0] == 'u' ? MTL::DataTypeUInt
                                : MTL::DataTypeInt;
    };
    const auto src_type = data_type(src);
    const auto dst_type = data_type(dst);
    encoder->setRenderPipelineState(blit_pipeline_cache.find(
        {.pixel_format = dst_impl->getTexture()->pixelFormat(),
         .transparent = false,
         .copy_alpha = true,
         .src_type = src_type,
         .dst_type = dst_type}));
    encoder->setViewport(MTL::Viewport(
        static_cast<f64>(dst_origin.x), static_cast<f64>(dst_origin.y),
        static_cast<f64>(dst_size.width), static_cast<f64>(dst_size.height), 0.0,
        1.0));
    encoder->setVertexBytes(&dst_layer, sizeof(dst_layer), 0);
    const auto src_width = static_cast<f32>(src_impl->getTexture()->width());
    const auto src_height = static_cast<f32>(src_impl->getTexture()->height());
    const auto extent_x =
        options.src_width != 0.0f ? options.src_width
                                  : static_cast<f32>(src_size.width);
    const auto extent_y =
        options.src_height != 0.0f ? options.src_height
                                   : static_cast<f32>(src_size.height);
    BlitParams params = {
        .src_offset = {static_cast<f32>(src_origin.x) / src_width,
                       static_cast<f32>(src_origin.y) / src_height},
        .src_scale = {extent_x / src_width, extent_y / src_height},
        .opacity = 1.0f,
    };
    encoder->setFragmentBytes(&params, sizeof(params), 0);
    encoder->setFragmentTexture(src_impl->getTexture(),
                                static_cast<NS::UInteger>(0));
    encoder->setFragmentSamplerState(
        options.nearest ? nearest_sampler : linear_sampler,
        static_cast<NS::UInteger>(0));

    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                            static_cast<NS::UInteger>(0),
                            static_cast<NS::UInteger>(3));
}

SamplerBase* Renderer::createSamplerHot10(const SamplerDescriptor& descriptor) {
    return new Sampler(device, descriptor);
}

ICommandBuffer* Renderer::createCommandBufferHot11() {
    return new CommandBuffer(command_queue);
}

RenderPassBase*
Renderer::createRenderPassHot12(const RenderPassDescriptor& descriptor) {
    return new RenderPass(descriptor);
}

void Renderer::bindRenderPassHot13(const RenderPassBase* render_pass) {
    state.render_pass = static_cast<const RenderPass*>(render_pass);
}

void Renderer::beginClear(CommandBuffer* command_buffer,
                          MTL::DepthStencilState* depth_stencil_state) {
    const auto width = state.render_pass->getWidth();
    const auto height = state.render_pass->getHeight();
    const MTL::Viewport viewport(0.0, 0.0, static_cast<f64>(width),
                                 static_cast<f64>(height), 0.0, 1.0);
    const auto& rect = state.clear.rect;
    const NS::UInteger x = std::min<NS::UInteger>(rect.x, width);
    const NS::UInteger y = std::min<NS::UInteger>(rect.y, height);
    const MTL::ScissorRect scissor(x, y,
                                   std::min<NS::UInteger>(rect.width, width - x),
                                   std::min<NS::UInteger>(rect.height, height - y));
    command_buffer->setViewports(std::span(&viewport, 1));
    command_buffer->setScissors(std::span(&scissor, 1));
    command_buffer->setDepthBias(0.0f, 0.0f, 0.0f);
    command_buffer->setCullMode(MTL::CullModeNone);
    command_buffer->setDepthStencilState(depth_stencil_state);
}

void Renderer::clearColorHot14(ICommandBuffer* command_buffer, u32 render_target_id,
                          u32 layer, u8 mask, const simd::uint4 color) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);
    auto texture =
        static_cast<TextureView*>(state.render_pass->getDescriptor()
                                      .color_targets[render_target_id]
                                      .texture);

    // HACK
    if (texture == nullptr) {
        ONCE(LOG_WARN(MetalRenderer, "Invalid color target at index {}",
                      render_target_id));
        return;
    }

    auto encoder = getRenderCommandEncoder(command_buffer_impl);
    if (state.clear.rect.width == 0 || state.clear.rect.height == 0)
        return;

    ClearColorPipelineDescriptor pipeline_descriptor{
        .render_target_id = render_target_id,
        .mask = mask,
        .depth_stencil_pixel_format = getDepthStencilPixelFormat()};
    getColorPixelFormats(pipeline_descriptor.pixel_formats);
    command_buffer_impl->setRenderPipelineState(
        clear_color_pipeline_cache.find(pipeline_descriptor));
    beginClear(command_buffer_impl, depth_stencil_state_disabled);
    encoder->setVertexBytes(&layer, sizeof(layer), 0);
    encoder->setFragmentBytes(&color, sizeof(color), 0);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                            static_cast<NS::UInteger>(0),
                            static_cast<NS::UInteger>(3));
}

void Renderer::clearDepthHot15(ICommandBuffer* command_buffer, u32 layer,
                          const float value) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);
    auto texture = static_cast<TextureView*>(
        state.render_pass->getDescriptor().depth_stencil_target.texture);

    // HACK
    if (texture == nullptr) {
        ONCE(LOG_WARN(MetalRenderer, "Invalid depth target"));
        return;
    }

    const auto format = texture->getDescriptor().format;
    if (!toMtlPixelFormatInfo(format).has_depth) {
        ONCE(LOG_WARN(MetalRenderer,
                      "Texture format {} does not have a depth component",
                      format));
        return;
    }

    auto encoder = getRenderCommandEncoder(command_buffer_impl);
    if (state.clear.rect.width == 0 || state.clear.rect.height == 0)
        return;

    ClearDepthPipelineDescriptor pipeline_descriptor{
        .depth_stencil_pixel_format =
            toMtlPixelFormat(texture->getDescriptor().format)};
    getColorPixelFormats(pipeline_descriptor.color_pixel_formats);
    command_buffer_impl->setRenderPipelineState(
        clear_depth_pipeline_cache.find(pipeline_descriptor));
    beginClear(command_buffer_impl, depth_stencil_state_always_and_write);
    struct {
        u32 layer_id;
        float value;
    } params = {.layer_id = layer, .value = value};
    encoder->setVertexBytes(&params, sizeof(params), 0);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                            static_cast<NS::UInteger>(0),
                            static_cast<NS::UInteger>(3));
}

void Renderer::clearStencilHot16(ICommandBuffer* command_buffer, u32 layer,
                            const u32 value) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);
    auto texture = static_cast<TextureView*>(
        state.render_pass->getDescriptor().depth_stencil_target.texture);
    if (texture == nullptr || !hasStencilAttachment())
        return;

    auto encoder = getRenderCommandEncoder(command_buffer_impl);
    if (state.clear.rect.width == 0 || state.clear.rect.height == 0 ||
        (state.clear.stencil_write_mask & 0xff) == 0)
        return;

    // Draw a full screen triangle that replaces the stencil value through
    // the write mask, while keeping depth intact
    ClearDepthPipelineDescriptor pipeline_descriptor{
        .depth_stencil_pixel_format =
            toMtlPixelFormat(texture->getDescriptor().format)};
    getColorPixelFormats(pipeline_descriptor.color_pixel_formats);
    command_buffer_impl->setRenderPipelineState(
        clear_depth_pipeline_cache.find(pipeline_descriptor));
    const StencilFaceState replace{
        .func = engines::CompareOp::Always,
        .fail_op = MTL::StencilOperationReplace,
        .depth_fail_op = MTL::StencilOperationReplace,
        .pass_op = MTL::StencilOperationReplace,
        .ref = value,
        .func_mask = 0xff,
        .write_mask = state.clear.stencil_write_mask & 0xff,
    };
    beginClear(command_buffer_impl,
               depth_stencil_state_cache.find({
                   .depth_test_enabled = false,
                   .depth_write_enabled = false,
                   .depth_compare_op = engines::CompareOp::Always,
                   .stencil_enabled = true,
                   .stencil_front = replace,
                   .stencil_back = replace,
               }));
    command_buffer_impl->setStencilReferences(value & 0xff, value & 0xff);
    struct {
        u32 layer_id;
        float value;
    } params = {.layer_id = layer, .value = 0.0f};
    encoder->setVertexBytes(&params, sizeof(params), 0);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                            static_cast<NS::UInteger>(0),
                            static_cast<NS::UInteger>(3));
}

ShaderBase* Renderer::createShaderHot17(const ShaderDescriptor& descriptor) {
    return new Shader(device, descriptor, shader_library_cache);
}

PipelineBase* Renderer::createPipelineHot18(const PipelineDescriptor& descriptor) {
    return new Pipeline(device, descriptor, binary_archive_cache);
}

void Renderer::bindPipelineHot19(const PipelineBase* pipeline) {
    state.pipeline = static_cast<const Pipeline*>(pipeline);
}

void Renderer::setDepthTestEnabledHot20(bool enabled) {
    state.depth_test_enabled = enabled;
}

void Renderer::setDepthWriteEnabledHot21(bool enabled) {
    state.depth_write_enabled = enabled;
}

void Renderer::debugDumpTextureHot22(ICommandBuffer* command_buffer,
                                ITextureView* view, const std::string& path) {
    auto texture = static_cast<TextureView*>(view)->getTexture();
    const u32 level_count =
        (texture->textureType() == MTL::TextureTypeCube ||
         texture->textureType() == MTL::TextureTypeCubeArray)
            ? std::min(static_cast<u32>(texture->mipmapLevelCount()), 5u)
            : 1u;
    for (u32 level = 0; level < level_count; level++)
        debugDumpTextureLevel(
            command_buffer, view, level,
            level == 0 ? path : fmt::format("{}_mip{}.png", path, level));
}

void Renderer::debugDumpTextureLevelHot23(ICommandBuffer* command_buffer,
                                     ITextureView* view, u32 level,
                                     const std::string& path) {
    auto command_buffer_impl = static_cast<CommandBuffer*>(command_buffer);
    auto texture = static_cast<TextureView*>(view)->getTexture();
    const auto texture_type = texture->textureType();
    if (texture_type != MTL::TextureType2D &&
        texture_type != MTL::TextureType2DArray &&
        texture_type != MTL::TextureTypeCube &&
        texture_type != MTL::TextureTypeCubeArray)
        return;
    // Cube faces are stitched horizontally
    const u32 slice_count =
        texture_type == MTL::TextureTypeCube ? 6
        : texture_type == MTL::TextureTypeCubeArray
            ? std::min(static_cast<u32>(texture->arrayLength()) * 6, 96u)
            : 1;

    const auto pixel_format = texture->pixelFormat();
    const bool is_bc1 = pixel_format == MTL::PixelFormatBC1_RGBA ||
                        pixel_format == MTL::PixelFormatBC1_RGBA_sRGB;
    u32 bpp = 0;
    switch (pixel_format) {
    case MTL::PixelFormatBC1_RGBA:
    case MTL::PixelFormatBC1_RGBA_sRGB:
        break;
    case MTL::PixelFormatR8Unorm:
        bpp = 1;
        break;
    case MTL::PixelFormatRG8Unorm:
    case MTL::PixelFormatR16Float:
    case MTL::PixelFormatDepth16Unorm:
        bpp = 2;
        break;
    case MTL::PixelFormatRGBA8Unorm:
    case MTL::PixelFormatRGBA8Unorm_sRGB:
    case MTL::PixelFormatBGRA8Unorm:
    case MTL::PixelFormatBGRA8Unorm_sRGB:
    case MTL::PixelFormatR32Float:
    case MTL::PixelFormatRG16Float:
    case MTL::PixelFormatRG16Unorm:
    case MTL::PixelFormatDepth32Float:
    case MTL::PixelFormatRGB10A2Unorm:
    case MTL::PixelFormatRG11B10Float:
        bpp = 4;
        break;
    case MTL::PixelFormatRGBA16Float:
        bpp = 8;
        break;
    default:
        return;
    }

    const u32 width = std::max(static_cast<u32>(texture->width()) >> level, 1u);
    const u32 height =
        std::max(static_cast<u32>(texture->height()) >> level, 1u);
    const u32 bytes_per_row =
        is_bc1 ? std::max((width + 3) / 4, 1u) * 8 : width * bpp;
    const u32 rows = is_bc1 ? std::max((height + 3) / 4, 1u) : height;
    const usize image_size = static_cast<usize>(bytes_per_row) * rows;
    if (!VisualDiagnostics::instance().reserveImage(path,
            std::max<uint64_t>(image_size, u64(width) * height * 4) * slice_count))
        return;
    auto buffer = device->newBuffer(image_size * slice_count,
                                    MTL::ResourceStorageModeShared);

    if (!buffer) return;
    auto encoder = command_buffer_impl->getBlitCommandEncoder();
    for (u32 slice = 0; slice < slice_count; slice++)
        encoder->copyFromTexture(texture, slice, level, MTL::Origin(0, 0, 0),
                                 MTL::Size(width, height, 1), buffer,
                                 image_size * slice, bytes_per_row, image_size);

    command_buffer_impl->getCommandBuffer()->addCompletedHandler(
        [=](MTL::CommandBuffer* completed) {
            if (completed->status() != MTL::CommandBufferStatusCompleted) {
                buffer->release();
                return;
            }
            const auto all_data =
                reinterpret_cast<const u8*>(buffer->contents());

            // RGBA16F layered textures (cubemaps): stitch the slices tone
            // mapped and write per slice statistics
            if (pixel_format == MTL::PixelFormatRGBA16Float &&
                slice_count > 1) {
                const usize pixels = static_cast<usize>(width) * height;
                const u32 out_width = width * slice_count;
                std::vector<u8> out(static_cast<usize>(out_width) * height * 4,
                                    255);
                std::string stats;
                for (u32 slice = 0; slice < slice_count; slice++) {
                    const auto* px = reinterpret_cast<const _Float16*>(
                        all_data + image_size * slice);
                    f32 max = 0.0f;
                    f64 sum = 0.0;
                    u32 nans = 0, infs = 0;
                    for (usize i = 0; i < pixels; i++) {
                        for (u32 c = 0; c < 3; c++) {
                            const f32 v = static_cast<f32>(px[i * 4 + c]);
                            if (std::isnan(v)) {
                                nans++;
                                continue;
                            }
                            if (std::isinf(v)) {
                                infs++;
                                continue;
                            }
                            max = std::max(max, v);
                            sum += static_cast<f64>(v);
                            const f32 t = std::max(v, 0.0f);
                            out[(static_cast<usize>(i / width) * out_width +
                                 slice * width + i % width) *
                                    4 +
                                c] = static_cast<u8>(t / (1.0f + t) * 255.0f);
                        }
                    }
                    stats += fmt::format("slice {} max {} mean {} nan {} inf "
                                         "{}\n",
                                         slice, max, sum / static_cast<f64>(pixels * 3),
                                         nans, infs);
                }
                stbi_write_png(path.c_str(), static_cast<int>(out_width),
                               static_cast<int>(height), 4, out.data(),
                               static_cast<int>(out_width * 4));
                std::ofstream(path + ".txt") << stats;
                buffer->release();
                return;
            }

            // BC1: decode every slice into a horizontally stitched image
            if (is_bc1) {
                const u32 out_width = width * slice_count;
                std::vector<u8> out(static_cast<usize>(out_width) * height * 4,
                                    255);
                const auto expand565 = [](u16 c, u8* rgb) {
                    rgb[0] = static_cast<u8>(((c >> 11) & 0x1f) * 255 / 31);
                    rgb[1] = static_cast<u8>(((c >> 5) & 0x3f) * 255 / 63);
                    rgb[2] = static_cast<u8>((c & 0x1f) * 255 / 31);
                };
                for (u32 slice = 0; slice < slice_count; slice++) {
                    const u8* blocks = all_data + image_size * slice;
                    for (u32 by = 0; by < rows; by++) {
                        for (u32 bx = 0; bx < bytes_per_row / 8; bx++) {
                            const u8* block =
                                blocks + by * bytes_per_row + bx * 8;
                            u16 c0, c1;
                            u32 indices;
                            std::memcpy(&c0, block, 2);
                            std::memcpy(&c1, block + 2, 2);
                            std::memcpy(&indices, block + 4, 4);
                            u8 palette[4][3];
                            expand565(c0, palette[0]);
                            expand565(c1, palette[1]);
                            for (u32 c = 0; c < 3; c++) {
                                if (c0 > c1) {
                                    palette[2][c] = static_cast<u8>(
                                        (2 * palette[0][c] + palette[1][c]) /
                                        3);
                                    palette[3][c] = static_cast<u8>(
                                        (palette[0][c] + 2 * palette[1][c]) /
                                        3);
                                } else {
                                    palette[2][c] = static_cast<u8>(
                                        (palette[0][c] + palette[1][c]) / 2);
                                    palette[3][c] = 0;
                                }
                            }
                            for (u32 py = 0; py < 4; py++) {
                                for (u32 px = 0; px < 4; px++) {
                                    const u32 x = bx * 4 + px;
                                    const u32 y = by * 4 + py;
                                    if (x >= width || y >= height)
                                        continue;
                                    const u32 index =
                                        (indices >> (2 * (py * 4 + px))) & 0x3;
                                    u8* dst = &out[(static_cast<usize>(y) *
                                                        out_width +
                                                    slice * width + x) *
                                                   4];
                                    dst[0] = palette[index][0];
                                    dst[1] = palette[index][1];
                                    dst[2] = palette[index][2];
                                }
                            }
                        }
                    }
                }
                stbi_write_png(path.c_str(), static_cast<int>(out_width),
                               static_cast<int>(height), 4, out.data(),
                               static_cast<int>(out_width * 4));
                buffer->release();
                return;
            }

            const auto data = all_data;
            const usize pixel_count = static_cast<usize>(width) * height;
            std::vector<u8> rgba(pixel_count * 4, 255);

            // Single and dual channel data is normalized to its range
            const auto read_channel = [&](usize i, u32 c) -> f32 {
                switch (pixel_format) {
                case MTL::PixelFormatR8Unorm:
                    return data[i] / 255.0f;
                case MTL::PixelFormatRG8Unorm:
                    return data[i * 2 + c] / 255.0f;
                case MTL::PixelFormatR16Float:
                    return static_cast<f32>(
                        reinterpret_cast<const _Float16*>(data)[i]);
                case MTL::PixelFormatDepth16Unorm:
                    return reinterpret_cast<const u16*>(data)[i] / 65535.0f;
                case MTL::PixelFormatR32Float:
                case MTL::PixelFormatDepth32Float:
                    return reinterpret_cast<const f32*>(data)[i];
                case MTL::PixelFormatRG16Float:
                    return static_cast<f32>(
                        reinterpret_cast<const _Float16*>(data)[i * 2 + c]);
                default:
                    return 0.0f;
                }
            };

            switch (pixel_format) {
            case MTL::PixelFormatRG16Unorm:
                // Full precision next to the PNG
                std::ofstream(path + ".raw", std::ios::binary)
                    .write(reinterpret_cast<const char*>(data),
                           static_cast<std::streamsize>(pixel_count * 4));
                // Raw high bytes, e.g. UE4 velocity: 128 is no motion
                for (usize i = 0; i < pixel_count; i++) {
                    const auto* px = reinterpret_cast<const u16*>(data) + i * 2;
                    rgba[i * 4 + 0] = static_cast<u8>(px[0] >> 8);
                    rgba[i * 4 + 1] = static_cast<u8>(px[1] >> 8);
                    rgba[i * 4 + 2] = 0;
                }
                break;
            case MTL::PixelFormatRGBA8Unorm:
            case MTL::PixelFormatRGBA8Unorm_sRGB:
                std::memcpy(rgba.data(), data, pixel_count * 4);
                for (usize i = 0; i < pixel_count; i++)
                    rgba[i * 4 + 3] = 255;
                break;
            case MTL::PixelFormatBGRA8Unorm:
            case MTL::PixelFormatBGRA8Unorm_sRGB:
                for (usize i = 0; i < pixel_count; i++) {
                    rgba[i * 4 + 0] = data[i * 4 + 2];
                    rgba[i * 4 + 1] = data[i * 4 + 1];
                    rgba[i * 4 + 2] = data[i * 4 + 0];
                }
                break;
            case MTL::PixelFormatRGB10A2Unorm:
                for (usize i = 0; i < pixel_count; i++) {
                    const u32 v = reinterpret_cast<const u32*>(data)[i];
                    rgba[i * 4 + 0] = static_cast<u8>((v & 0x3ff) >> 2);
                    rgba[i * 4 + 1] = static_cast<u8>(((v >> 10) & 0x3ff) >> 2);
                    rgba[i * 4 + 2] = static_cast<u8>(((v >> 20) & 0x3ff) >> 2);
                }
                break;
            case MTL::PixelFormatRG11B10Float: {
                // Unsigned floats (5 bit exponent), tone mapped as x / (1 + x)
                const auto decode = [](u32 bits, u32 mantissa_bits) -> f32 {
                    const u32 exponent = bits >> mantissa_bits;
                    const u32 mantissa = bits & ((1u << mantissa_bits) - 1);
                    const f32 scale = static_cast<f32>(1u << mantissa_bits);
                    if (exponent == 0)
                        return (static_cast<f32>(mantissa) / scale) *
                               std::ldexp(1.0f, -14);
                    if (exponent == 31)
                        return mantissa != 0 ? 0.0f : 65504.0f;
                    return (1.0f + static_cast<f32>(mantissa) / scale) *
                           std::ldexp(1.0f, static_cast<int>(exponent) - 15);
                };
                for (usize i = 0; i < pixel_count; i++) {
                    const u32 v = reinterpret_cast<const u32*>(data)[i];
                    const f32 channels[3] = {decode(v & 0x7ff, 6),
                                             decode((v >> 11) & 0x7ff, 6),
                                             decode((v >> 22) & 0x3ff, 5)};
                    for (u32 c = 0; c < 3; c++)
                        rgba[i * 4 + c] = static_cast<u8>(
                            channels[c] / (1.0f + channels[c]) * 255.0f);
                }
                break;
            }
            case MTL::PixelFormatRGBA16Float:
                for (usize i = 0; i < pixel_count * 4; i++) {
                    const f32 v = static_cast<f32>(
                        reinterpret_cast<const _Float16*>(data)[i]);
                    rgba[i] = (i % 4 == 3)
                                  ? 255
                                  : static_cast<u8>(std::clamp(v, 0.0f, 1.0f) *
                                                    255.0f);
                }
                break;
            default: {
                const u32 channels =
                    (pixel_format == MTL::PixelFormatRG8Unorm ||
                     pixel_format == MTL::PixelFormatRG16Float)
                        ? 2
                        : 1;
                for (u32 c = 0; c < channels; c++) {
                    f32 min = std::numeric_limits<f32>::max();
                    f32 max = std::numeric_limits<f32>::lowest();
                    for (usize i = 0; i < pixel_count; i++) {
                        const f32 v = read_channel(i, c);
                        if (!std::isfinite(v))
                            continue;
                        min = std::min(min, v);
                        max = std::max(max, v);
                    }
                    const f32 range = max > min ? max - min : 1.0f;
                    for (usize i = 0; i < pixel_count; i++) {
                        f32 v = read_channel(i, c);
                        if (!std::isfinite(v))
                            v = min;
                        const u8 b = static_cast<u8>(
                            std::clamp((v - min) / range, 0.0f, 1.0f) * 255.0f);
                        if (channels == 1) {
                            rgba[i * 4 + 0] = rgba[i * 4 + 1] =
                                rgba[i * 4 + 2] = b;
                        } else {
                            rgba[i * 4 + c] = b;
                            rgba[i * 4 + 2] = 0;
                        }
                    }
                }
                break;
            }
            }

            stbi_write_png(path.c_str(), static_cast<int>(width),
                           static_cast<int>(height), 4, rgba.data(),
                           static_cast<int>(width * 4));
            buffer->release();
        });
}

void Renderer::setStencilStateHot24(const StencilState& stencil) {
    state.stencil = stencil;
}

bool Renderer::hasStencilAttachmentHot25() const {
    switch (getDepthStencilPixelFormat()) {
    case MTL::PixelFormatDepth24Unorm_Stencil8:
    case MTL::PixelFormatDepth32Float_Stencil8:
    case MTL::PixelFormatStencil8:
    case MTL::PixelFormatX24_Stencil8:
    case MTL::PixelFormatX32_Stencil8:
        return true;
    default:
        return false;
    }
}

void Renderer::setCullStateHot26(bool enabled, engines::CullFaceMode mode,
                            engines::Winding front_face) {
    // The winding also selects two-sided stencil and front facing
    state.front_face_winding = toMtlWinding(front_face);
    // Front and back culling skips the draw in the engine
    state.cull_mode = enabled ? toMtlCullMode(mode) : MTL::CullModeNone;
}

void Renderer::setDepthBiasHot27(f32 constant, f32 slope, f32 clamp) {
    state.depth_bias_constant = constant;
    state.depth_bias_slope = slope;
    state.depth_bias_clamp = clamp;
}

void Renderer::setBlendConstantHot28(const simd::float4 color) {
    state.blend_constant = color;
}

void Renderer::setDepthCompareOpHot29(engines::CompareOp op) {
    state.depth_compare_op = op;
}

void Renderer::setViewportHot30(u32 index, const MTL::Viewport& viewport) {
    state.viewports[index] = viewport;
}

void Renderer::setScissorHot31(u32 index, const MTL::ScissorRect& scissor) {
    state.scissors[index] = scissor;
}

void Renderer::bindVertexBufferHot32(const BufferView& buffer, u32 index) {
    state.vertex_buffers[index] = buffer;
}

void Renderer::bindIndexBufferHot33(const BufferView& index_buffer,
                               engines::IndexType index_type) {
    state.index_buffer = index_buffer;
    state.index_type = index_type;
}

void Renderer::bindUniformBufferHot34(const BufferView& buffer,
                                 ShaderType shader_type, u32 index) {
    // HACK
    if (shader_type == ShaderType::Count)
        return;

    state.uniform_buffers[static_cast<u32>(shader_type)][index] = buffer;
}

BufferView Renderer::getGuestMemoryBuffer(ztd::Range<uptr> range) {
    // Metal wraps whole pages. The shader only touches the bound range, so
    // the rest of the first and last page is never accessed
    const auto page_size = static_cast<uptr>(sysconf(_SC_PAGESIZE));
    const uptr begin = range.getBegin() & ~(page_size - 1);
    const uptr end = (range.getEnd() + page_size - 1) & ~(page_size - 1);

    std::scoped_lock lock(guest_memory_buffers_mutex);
    auto& entry = guest_memory_buffers[begin];
    if (entry.buffer == nullptr || entry.end < end) {
        auto* native = device->newBuffer(reinterpret_cast<void*>(begin),
                                         end - begin,
                                         MTL::ResourceStorageModeShared,
                                         nullptr);
        if (native == nullptr) {
            guest_memory_buffers.erase(begin);
            LOG_ERROR(MetalRenderer, "Cannot wrap guest memory {:#x}..{:#x}",
                      range.getBegin(), range.getEnd());
            return {};
        }
        entry = {std::make_shared<Buffer>(native), end};
        // Entries are only dropped here, bound views keep theirs alive
        if (guest_memory_buffers.size() > 256) {
            const auto crnt = entry;
            guest_memory_buffers.clear();
            guest_memory_buffers[begin] = crnt;
        }
    }
    return BufferView{guest_memory_buffers[begin].buffer,
                      range.getBegin() - begin, range.getSize()};
}

void Renderer::bindStorageBuffer(const BufferView& buffer,
                                 ShaderType shader_type, u32 index) {
    if (shader_type == ShaderType::Count)
        return;
    state.storage_buffers[static_cast<u32>(shader_type)][index] = buffer;
}

void Renderer::bindTextureHot35(ITextureView* texture, SamplerBase* sampler,
                           ShaderType shader_type, u32 index) {
    // HACK
    if (shader_type == ShaderType::Count)
        return;

    if (texture != nullptr && sampler != nullptr &&
        getIntegerTextureComponentType(texture->getDescriptor().format) != nullptr) {
        auto descriptor = sampler->getDescriptor();
        descriptor.min_filter = descriptor.mag_filter =
            MTL::SamplerMinMagFilterNearest;
        descriptor.mip_filter = MTL::SamplerMipFilterNearest;
        descriptor.max_anisotropy = 1;
        sampler = getSamplerCache().find(descriptor);
    }
    state.textures[static_cast<u32>(shader_type)][index] = {
        .texture_view = static_cast<TextureView*>(texture),
        .sampler = static_cast<Sampler*>(sampler)};
}

void Renderer::unbindUniformBuffersHot36(ShaderType shader_type) {
    // HACK
    if (shader_type == ShaderType::Count)
        return;

    state.uniform_buffers[static_cast<u32>(shader_type)] = {};
}

void Renderer::unbindStorageBuffers(ShaderType shader_type) {
    if (shader_type == ShaderType::Count)
        return;
    state.storage_buffers[static_cast<u32>(shader_type)] = {};
}

void Renderer::unbindTexturesHot37(ShaderType shader_type) {
    // HACK
    if (shader_type == ShaderType::Count)
        return;

    state.textures[static_cast<u32>(shader_type)] = {};
}

bool Renderer::synchronize(ICommandBuffer* command_buffer) {
    auto* commands = static_cast<CommandBuffer*>(command_buffer);
    commands->debugCommitAndWait();
    return commands->lastSubmissionSucceeded();
}

void Renderer::resetOcclusionCounter() {
    occlusion_count = 0;
    occlusion_valid = true;
    occlusion_pages.clear();
}

std::optional<u64> Renderer::getOcclusionCounter(ICommandBuffer* command_buffer) {
    if (!occlusion_valid)
        return std::nullopt;
    if (!synchronize(command_buffer)) {
        occlusion_valid = false;
        return std::nullopt;
    }
    for (const auto& page : occlusion_pages) {
        const auto* counts = static_cast<const u64*>(page.buffer->contents());
        for (u32 i = 0; i < page.count; i++)
            occlusion_count += counts[i];
    }
    occlusion_pages.clear();
    return occlusion_count;
}

std::optional<u64> Renderer::prepareOcclusionQuery(CommandBuffer* command_buffer) {
    if (!occlusion_enabled || !occlusion_valid)
        return std::nullopt;
    if (occlusion_pages.empty() ||
        occlusion_pages.back().count == OCCLUSION_PAGE_SIZE ||
        !command_buffer->hasRenderPass(state.render_pass->getRenderPassDescriptor()) ||
        command_buffer->getVisibilityResultBuffer() != occlusion_pages.back().buffer.get()) {
        // Queries can stay enabled indefinitely without a report/reset.
        if (occlusion_pages.size() >= MAX_OCCLUSION_PAGES &&
            !getOcclusionCounter(command_buffer))
            return std::nullopt;
        auto* buffer = device->newBuffer(OCCLUSION_PAGE_SIZE * sizeof(u64),
                                          MTL::ResourceStorageModeShared);
        ASSERT(buffer != nullptr, MetalRenderer, "Cannot allocate visibility buffer");
        std::memset(buffer->contents(), 0, OCCLUSION_PAGE_SIZE * sizeof(u64));
        occlusion_pages.push_back(
            {std::shared_ptr<MTL::Buffer>(buffer, [](MTL::Buffer* value) {
                 value->release();
             }), 0});
    }
    auto& page = occlusion_pages.back();
    command_buffer->setVisibilityResultBuffer(page.buffer.get());
    return static_cast<u64>(page.count++) * sizeof(u64);
}

void Renderer::drawHot38(ICommandBuffer* command_buffer,
                    const engines::PrimitiveType primitive_type,
                    const u32 start, const u32 count, const u32 base_instance,
                    const u32 instance_count) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);

    // Check for errors
    if (!canDraw()) {
        if (auto& visual = VisualDiagnostics::instance(); visual.selected())
            visual.event("metal_draw_rejected", {{"reason", "pipeline or render pass unavailable"}});
        return;
    }

    const auto visibility_offset = prepareOcclusionQuery(command_buffer_impl);
    bindDrawState(command_buffer_impl);

    auto encoder = command_buffer_impl->getRenderCommandEncoderUnchecked();

    if (auto& visual = VisualDiagnostics::instance(); visual.selected()) {
        const auto group = fmt::format("hydra s{} p{} d{}", visual.submissionId(), visual.frameId(), visual.drawIndex());
        encoder->insertDebugSignpost(toNsString(group));
        visual.event("metal_draw_encoded", {{"count", std::to_string(count)}, {"signpost", group}});
    }
    const bool capture_group = isCapturing() && !capture_draw_label.empty();
    if (capture_group)
        encoder->pushDebugGroup(toNsString(capture_draw_label));
    if (visibility_offset)
        encoder->setVisibilityResultMode(MTL::VisibilityResultModeCounting,
                                         *visibility_offset);
    // Draw
    encoder->drawPrimitives(toMtlPrimitiveType(primitive_type), start, count,
                            instance_count, base_instance);
    if (visibility_offset)
        encoder->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, 0);
    if (capture_group) {
        encoder->popDebugGroup();
        capture_draw_label.clear();
    }
}

void Renderer::drawIndexedHot39(ICommandBuffer* command_buffer,
                           const engines::PrimitiveType primitive_type,
                           const u32 start, const u32 count,
                           const u32 base_vertex, const u32 base_instance,
                           const u32 instance_count) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);

    // Check for errors
    if (!canDraw()) {
        if (auto& visual = VisualDiagnostics::instance(); visual.selected())
            visual.event("metal_draw_rejected", {{"reason", "pipeline or render pass unavailable"}});
        return;
    }

    const auto visibility_offset = prepareOcclusionQuery(command_buffer_impl);
    bindDrawState(command_buffer_impl);

    auto encoder = command_buffer_impl->getRenderCommandEncoderUnchecked();

    if (auto& visual = VisualDiagnostics::instance(); visual.selected()) {
        const auto group = fmt::format("hydra s{} p{} d{}", visual.submissionId(), visual.frameId(), visual.drawIndex());
        encoder->insertDebugSignpost(toNsString(group));
        visual.event("metal_draw_encoded", {{"count", std::to_string(count)}, {"signpost", group}});
    }
    if (visibility_offset)
        encoder->setVisibilityResultMode(MTL::VisibilityResultModeCounting,
                                         *visibility_offset);
    // Draw
    auto index_buffer_mtl =
        static_cast<Buffer*>(state.index_buffer.getBase())->getBuffer();
    // TODO: is start used correctly?
    const auto index_buffer_offset =
        static_cast<u64>(start) * engines::getIndexTypeSize(state.index_type) +
        state.index_buffer.getOffset();
    const bool capture_group = isCapturing() && !capture_draw_label.empty();
    if (capture_group)
        encoder->pushDebugGroup(toNsString(capture_draw_label));
    encoder->drawIndexedPrimitives(toMtlPrimitiveType(primitive_type), count,
                                   toMtlIndexType(state.index_type),
                                   index_buffer_mtl, index_buffer_offset,
                                   instance_count, base_vertex, base_instance);
    if (visibility_offset)
        encoder->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, 0);
    if (capture_group) {
        encoder->popDebugGroup();
        capture_draw_label.clear();
    }
}

f64 Renderer::debugSyncCommandBufferHot40(ICommandBuffer* command_buffer) {
    return static_cast<CommandBuffer*>(command_buffer)->debugCommitAndWait();
}

void Renderer::dispatchComputeHot41(ICommandBuffer* command_buffer,
                               const ComputeDispatch& dispatch) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);

    const auto shader = static_cast<Shader*>(dispatch.shader);
    if (shader == nullptr || shader->getFunction() == nullptr)
        return;

    // Pipeline
    auto [it, inserted] =
        compute_pipelines.try_emplace(shader->getFunction(), nullptr);
    if (inserted) {
        NS::Error* error = nullptr;
        it->second =
            device->newComputePipelineState(shader->getFunction(), &error);
        if (it->second == nullptr)
            LOG_ERROR(MetalRenderer, "Failed to create compute pipeline: {}",
                      error ? error->localizedDescription()->utf8String()
                            : "unknown error");
    }
    const auto pipeline = it->second;
    if (pipeline == nullptr)
        return;

    const auto& block = dispatch.block_size;
    const auto& grid = dispatch.grid_size;
    if (block.width * block.height * block.depth == 0 ||
        grid.width * grid.height * grid.depth == 0)
        return;
    if (block.width * block.height * block.depth >
        pipeline->maxTotalThreadsPerThreadgroup()) {
        ONCE(LOG_ERROR(MetalRenderer,
                       "Compute block {}x{}x{} exceeds the maximum of {} "
                       "threads",
                       block.width, block.height, block.depth,
                       pipeline->maxTotalThreadsPerThreadgroup()));
        return;
    }

    if (PerfStats::isEnabled()) {
        PerfStats::instance().compute_dispatches++;
        PerfStats::instance().compute_threads +=
            u64(grid.width) * grid.height * grid.depth * block.width * block.height *
            block.depth;
    }

    auto encoder = command_buffer_impl->getComputeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    const auto sampler_encoder = shader->getSamplerEncoder();
    BufferBase* sampler_buffer = nullptr;
    if (sampler_encoder != nullptr) {
        sampler_buffer = allocateTextureUploadBuffer(sampler_encoder->encodedLength());
        const auto native = static_cast<Buffer*>(sampler_buffer)->getBuffer();
        sampler_encoder->setArgumentBuffer(native, 0);
    }

    // Resources
    for (u32 i = 0; i < CONST_BUFFER_BINDING_COUNT; i++) {
        const auto& buffer = dispatch.uniform_buffers[i];
        if (buffer.getBase() == nullptr)
            continue;
        encoder->setBuffer(static_cast<Buffer*>(buffer.getBase())->getBuffer(),
                           buffer.getOffset(), i);
    }
    for (const auto& binding : dispatch.storage_buffers) {
        if (binding.buffer.getBase() == nullptr)
            continue;
        encoder->setBuffer(
            static_cast<Buffer*>(binding.buffer.getBase())->getBuffer(),
            binding.buffer.getOffset(), binding.index);
    }
    for (const auto& binding : dispatch.textures) {
        encoder->setTexture(
            binding.texture != nullptr
                ? static_cast<TextureView*>(binding.texture)->getTexture()
                : nullptr,
            binding.index);
        if (sampler_encoder != nullptr)
            sampler_encoder->setSamplerState(
                binding.sampler != nullptr
                    ? static_cast<Sampler*>(binding.sampler)->getSampler()
                    : nullptr,
                binding.index);
        else if (binding.sampler != nullptr)
            encoder->setSamplerState(
                static_cast<Sampler*>(binding.sampler)->getSampler(),
                binding.index);
    }
    for (const auto& binding : dispatch.images) {
        encoder->setTexture(
            binding.texture != nullptr
                ? static_cast<TextureView*>(binding.texture)->getTexture()
                : nullptr,
            binding.index);
    }
    if (sampler_buffer != nullptr) {
        encoder->setBuffer(
            static_cast<Buffer*>(sampler_buffer)->getBuffer(), 0,
            shader->getDescriptor().resource_mapping.sampler_buffer_index);
        retireTextureUploadBuffer(command_buffer_impl, sampler_buffer);
    }

    encoder->dispatchThreadgroups(MTL::Size(grid.width, grid.height, grid.depth),
                                  MTL::Size(block.width, block.height, block.depth));

    // DEBUG: measure the GPU time of each dispatch in isolation
    static const bool profile = std::getenv("HYDRA_PROFILE_COMPUTE") != nullptr;
    if (profile) {
        command_buffer_impl->endEncoding();
        auto cb = command_queue->commandBuffer();
        auto enc = cb->computeCommandEncoder();
        enc->setComputePipelineState(pipeline);
        for (u32 i = 0; i < CONST_BUFFER_BINDING_COUNT; i++) {
            const auto& buffer = dispatch.uniform_buffers[i];
            if (buffer.getBase() != nullptr)
                enc->setBuffer(
                    static_cast<Buffer*>(buffer.getBase())->getBuffer(),
                    buffer.getOffset(), i);
        }
        for (const auto& binding : dispatch.storage_buffers) {
            if (binding.buffer.getBase() != nullptr)
                enc->setBuffer(
                    static_cast<Buffer*>(binding.buffer.getBase())->getBuffer(),
                    binding.buffer.getOffset(), binding.index);
        }
        for (const auto& binding : dispatch.textures) {
            if (binding.texture != nullptr)
                enc->setTexture(
                    static_cast<TextureView*>(binding.texture)->getTexture(),
                    binding.index);
            if (binding.sampler != nullptr)
                enc->setSamplerState(
                    static_cast<Sampler*>(binding.sampler)->getSampler(),
                    binding.index);
        }
        for (const auto& binding : dispatch.images) {
            if (binding.texture != nullptr)
                enc->setTexture(
                    static_cast<TextureView*>(binding.texture)->getTexture(),
                    binding.index);
        }
        enc->dispatchThreadgroups(MTL::Size(grid.width, grid.height, grid.depth),
                                  MTL::Size(block.width, block.height, block.depth));
        enc->endEncoding();
        cb->commit();
        cb->waitUntilCompleted();
        const auto ms = (cb->GPUEndTime() - cb->GPUStartTime()) * 1000.0;
        static std::map<u64, double> max_ms;
        auto& max = max_ms[dispatch.debug_program_addr];
        if (ms > max * 1.5 + 0.01) {
            max = ms;
            LOG_INFO(MetalRenderer,
                     "DEBUG compute {:#x} grid {}x{}x{} block {}x{}x{}: "
                     "{:.3f} ms (status {})",
                     dispatch.debug_program_addr, grid.width, grid.height,
                     grid.depth, block.width, block.height, block.depth, ms,
                     static_cast<int>(cb->status()));
        }
    }
}

void Renderer::getColorPixelFormatsHot42(
    MTL::PixelFormat out_pixel_formats[COLOR_TARGET_COUNT]) const {
    for (u32 i = 0; i < COLOR_TARGET_COUNT; i++) {
        const auto texture =
            state.render_pass != nullptr
                ? state.render_pass->getDescriptor().color_targets[i].texture
                : nullptr;
        out_pixel_formats[i] =
            texture != nullptr
                ? toMtlPixelFormat(texture->getDescriptor().format)
                : MTL::PixelFormatInvalid;
    }
}

MTL::PixelFormat Renderer::getDepthStencilPixelFormatHot43() const {
    if (state.render_pass == nullptr)
        return MTL::PixelFormatInvalid;
    const auto texture =
        state.render_pass->getDescriptor().depth_stencil_target.texture;
    if (texture == nullptr)
        return MTL::PixelFormatInvalid;
    return toMtlPixelFormat(texture->getDescriptor().format);
}

MTL::RenderCommandEncoder*
Renderer::getRenderCommandEncoderHot44(CommandBuffer* command_buffer) const {
    return command_buffer->getRenderCommandEncoder(
        state.render_pass->getRenderPassDescriptor());
}

void Renderer::setRenderPipelineStateHot45(CommandBuffer* command_buffer) const {
    command_buffer->setRenderPipelineState(state.pipeline->getPipeline());
}

void Renderer::setDepthStencilStateHot46(CommandBuffer* command_buffer) {
    // Depth testing without a depth attachment is invalid in Metal
    const bool has_depth =
        getDepthStencilPixelFormat() != MTL::PixelFormatInvalid;
    DepthStencilStateDescriptor descriptor{
        .depth_test_enabled = state.depth_test_enabled && has_depth,
        .depth_write_enabled = state.depth_write_enabled && has_depth,
        .depth_compare_op = state.depth_compare_op,
        .stencil_enabled = state.stencil.enabled && hasStencilAttachment(),
        .stencil_front = state.stencil.front,
        .stencil_back = state.stencil.back,
    };

    command_buffer->setDepthStencilState(
        depth_stencil_state_cache.find(descriptor));
}

void Renderer::setVertexBufferHot47(CommandBuffer* command_buffer, u32 index) {
    ASSERT_DEBUG(index < VERTEX_ARRAY_COUNT, MetalRenderer,
                 "Invalid vertex buffer index {}", index);

    const auto buffer = state.vertex_buffers[index];
    if (buffer.getBase() == nullptr)
        return;

    command_buffer->setBuffer(
        static_cast<Buffer*>(buffer.getBase())->getBuffer(), buffer.getOffset(),
        ShaderType::Vertex, getVertexBufferIndex(index));
}

void Renderer::setUniformBufferHot48(CommandBuffer* command_buffer,
                                ShaderType shader_type, u32 index) {
    // TODO: get the index from resource mapping

    ASSERT_DEBUG(index < CONST_BUFFER_BINDING_COUNT, MetalRenderer,
                 "Invalid uniform buffer index {}", index);

    const auto buffer =
        state.uniform_buffers[static_cast<u32>(shader_type)][index];
    if (buffer.getBase() == nullptr)
        return;

    command_buffer->setBuffer(
        static_cast<Buffer*>(buffer.getBase())->getBuffer(), buffer.getOffset(),
        shader_type, index);
}

void Renderer::setStorageBuffer(CommandBuffer* command_buffer,
                                ShaderType shader_type, u32 index) {
    const auto& buffer =
        state.storage_buffers[static_cast<u32>(shader_type)][index];
    if (buffer.getBase() == nullptr)
        return;
    command_buffer->setBuffer(
        static_cast<Buffer*>(buffer.getBase())->getBuffer(), buffer.getOffset(),
        shader_type, index);
}

void Renderer::setTextureHot49(CommandBuffer* command_buffer, ShaderType shader_type,
                          u32 index) {
    const auto texture = state.textures[static_cast<u32>(shader_type)][index];
    // Unbind empty slots, otherwise the shader would sample a stale texture
    // from a previous draw. Null textures read as zero, like a null descriptor
    command_buffer->setTexture(texture.texture_view != nullptr
                                   ? texture.texture_view->getTexture()
                                   : nullptr,
                               shader_type, index);
    const auto shader = static_cast<Shader*>(
        state.pipeline->getDescriptor().shaders[static_cast<u32>(shader_type)]);
    if (texture.sampler != nullptr && index < SAMPLER_COUNT &&
        (shader == nullptr || shader->getSamplerEncoder() == nullptr))
        command_buffer->setSampler(texture.sampler->getSampler(), shader_type,
                                   index);
}

void Renderer::beginCaptureHot50() {
    auto capture_manager = MTL::CaptureManager::sharedCaptureManager();
    auto desc = MTL::CaptureDescriptor::alloc()->init();
    desc->setCaptureObject(device);

    // Check if a debugger with support for Gpu capture is attached
    if (capture_manager->supportsDestination(
            MTL::CaptureDestinationDeveloperTools)) {
        desc->setDestination(MTL::CaptureDestinationDeveloperTools);
    } else {
        const char* env_dir = std::getenv("HYDRA_GPU_CAPTURE_DIR");
        const std::string gpu_capture_dir =
            env_dir != nullptr
                ? std::string(env_dir)
                : fmt::format("{}/gpu_captures",
                              CONFIG_INSTANCE.getAppDataPath());
        std::error_code mkdir_error;
        std::filesystem::create_directories(gpu_capture_dir, mkdir_error);
        if (gpu_capture_dir.empty()) {
            LOG_ERROR(
                MetalRenderer,
                "No GPU capture directory specified, cannot do a Gpu capture");
            return;
        }

        // Check if the Gpu trace document destination is available
        if (!capture_manager->supportsDestination(
                MTL::CaptureDestinationGPUTraceDocument)) {
            LOG_ERROR(MetalRenderer, "GPU trace document destination is not "
                                     "available, cannot do a GPU capture");
            return;
        }

        // Get current date and time as a string
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&now_time), "%Y.%m.%d_%H:%M:%S");
        std::string now_str = oss.str();

        std::string capture_path =
            fmt::format("{}/hydra_{}.gputrace", gpu_capture_dir, now_str);
        desc->setDestination(MTL::CaptureDestinationGPUTraceDocument);
        desc->setOutputURL(toNsurl(capture_path));
        LOG_INFO(MetalRenderer, "GPU capture to {}", capture_path);
    }

    NS::Error* error = nullptr;
    capture_manager->startCapture(desc, &error);
    if (error != nullptr) {
        LOG_ERROR(MetalRenderer, "Failed to start GPU capture: {}",
                  error->localizedDescription()->utf8String());
    }
}

void Renderer::endCaptureHot51() {
    auto captureManager = MTL::CaptureManager::sharedCaptureManager();
    captureManager->stopCapture();
    LOG_INFO(MetalRenderer, "GPU capture stopped");
}

bool Renderer::canDrawHot52() const {
    if (state.pipeline->getPipeline() == nullptr) {
        ONCE(LOG_WARN(MetalRenderer, "Pipeline not present, skipping draw"));
        return false;
    }

    return true;
}

void Renderer::bindDrawStateHot53(CommandBuffer* command_buffer) {
    const auto start = PerfStats::isEnabled()
                           ? std::chrono::steady_clock::now()
                           : std::chrono::steady_clock::time_point{};
    getRenderCommandEncoder(command_buffer);

    // States
    setRenderPipelineState(command_buffer);
    setDepthStencilState(command_buffer);

    // Culling
    command_buffer->setCullMode(state.cull_mode);
    command_buffer->setFrontFaceWinding(state.front_face_winding);
    command_buffer->getRenderCommandEncoderUnchecked()->setDepthClipMode(
        state.depth_clip_mode);

    // Stencil reference
    command_buffer->setStencilReferences(state.stencil.front.ref & 0xff,
                                         state.stencil.back.ref & 0xff);

    // Depth bias
    command_buffer->setDepthBias(state.depth_bias_constant,
                                 state.depth_bias_slope,
                                 state.depth_bias_clamp);

    // Blend constant
    command_buffer->setBlendColor(
        state.blend_constant.x, state.blend_constant.y,
        state.blend_constant.z, state.blend_constant.w);

    // Viewport and scissor
    MTL::Viewport viewports[VIEWPORT_COUNT];
    MTL::ScissorRect scissors[VIEWPORT_COUNT];
    for (u32 i = 0; i < VIEWPORT_COUNT; i++) {
        viewports[i] = state.viewports[i];

        // Scissor
        // Metal requires the scissor to lie within the render pass
        scissors[i] = state.scissors[i];
        if (state.render_pass) {
            const NS::UInteger rp_width = state.render_pass->getWidth();
            const NS::UInteger rp_height = state.render_pass->getHeight();
            auto& scissor = scissors[i];
            scissor.x = std::min(scissor.x, rp_width);
            scissor.y = std::min(scissor.y, rp_height);
            scissor.width = std::min(scissor.width, rp_width - scissor.x);
            scissor.height = std::min(scissor.height, rp_height - scissor.y);
        }
    }
    command_buffer->setViewports(viewports);
    command_buffer->setScissors(scissors);

    // Resources
    for (u32 i = 0; i < VERTEX_ARRAY_COUNT; i++)
        setVertexBuffer(command_buffer, i);
    // Bind only resources declared by the active stage. In particular, do not
    // walk every texture slot (and its hot-reload trampoline) for every draw.
    // Used slots still pass through setTexture even when null, clearing stale
    // bindings before a shader can read them.
    for (u32 shader_type = 0; shader_type < GRAPHICS_SHADER_TYPE_COUNT;
         shader_type++) {
        const auto shader = static_cast<Shader*>(
            state.pipeline->getDescriptor().shaders[shader_type]);
        if (shader == nullptr)
            continue;
        const auto& mapping = shader->getDescriptor().resource_mapping;
        for (u32 i = 0; i < CONST_BUFFER_BINDING_COUNT; i++) {
            if (mapping.uniform_buffers[i] != invalid<u32>())
                setUniformBuffer(command_buffer, ShaderType(shader_type), i);
        }
        for (const auto& storage : mapping.storage_buffers)
            setStorageBuffer(command_buffer, ShaderType(shader_type),
                             storage.host_index);
        for (const auto& [binding, index] : mapping.textures) {
            (void)binding;
            setTexture(command_buffer, ShaderType(shader_type), index);
        }
        if (shader->getSamplerEncoder() == nullptr)
            continue;
        const auto sampler_encoder = shader->getSamplerEncoder();
        auto* buffer = allocateTextureUploadBuffer(sampler_encoder->encodedLength());
        const auto native = static_cast<Buffer*>(buffer)->getBuffer();
        sampler_encoder->setArgumentBuffer(native, 0);
        for (const auto& [binding, index] :
             shader->getDescriptor().resource_mapping.textures) {
            (void)binding;
            const auto sampler = state.textures[shader_type][index].sampler;
            sampler_encoder->setSamplerState(
                sampler != nullptr ? sampler->getSampler() : nullptr, index);
        }
        command_buffer->setBuffer(
            native, 0, ShaderType(shader_type),
            shader->getDescriptor().resource_mapping.sampler_buffer_index);
        retireTextureUploadBuffer(command_buffer, buffer);
    }
    if (PerfStats::isEnabled())
        PerfStats::instance().draw_state_ns += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
