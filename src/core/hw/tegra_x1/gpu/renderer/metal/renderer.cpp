#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/geometry.hpp"
#include "core/hw/tegra_x1/gpu/renderer/tessellation.hpp"
#include "core/hw/tegra_x1/gpu/renderer/draw_queue.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/command_ir/compute_limits.hpp"
#include "core/hw/tegra_x1/gpu/management/metal_submission.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"

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

namespace {

Mtl4Context* createMtl4Context(MTL::Device* device) {
    if (CONFIG_INSTANCE.getMetalApi() != MetalApi::Metal4) {
        LOG_INFO(MetalRenderer, "Encoding with Metal 3");
        return nullptr;
    }
    auto* context = Mtl4Context::create(device);
    if (context != nullptr)
        LOG_INFO(MetalRenderer, "Encoding with Metal 4");
    else
        LOG_WARN(MetalRenderer,
                 "The GPU or OS does not support Metal 4, encoding with Metal 3");
    return context;
}

} // namespace

Renderer::Renderer()
    : device{MTL::CreateSystemDefaultDevice()},
      command_queue{device->newCommandQueue()},
      mtl4_context{std::shared_ptr<Mtl4Context>(createMtl4Context(device))},
      upload_buffer_pool(
          std::make_shared<UploadBufferPool>(device, 64 * 1024 * 1024, 4096)),
      completions(std::make_shared<CompletionQueue>()),
      occlusion(std::make_unique<OcclusionCounter>(device, completions)),
      binary_archive_cache(device), shader_library_cache(device),
      spatial_upscaler(device, mtl4_context.get()),
      frame_presenter(device, mtl4_context.get()),
      depth_stencil_state_cache(device),
      blit_pipeline_cache(device), clear_color_pipeline_cache(device),
      clear_depth_pipeline_cache(device) {
    // Compile shaders and pipelines of different threads (draws, prewarm) in
    // parallel
    device->setShouldMaximizeConcurrentCompilation(true);

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
    // Metal 4 binds samplers through argument tables
    sampler_state_descriptor->setSupportArgumentBuffers(true);
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

    if (DrawQueue::isEnabled())
        draw_queue = std::make_unique<DrawQueue>(*this);
}

Renderer::~Renderer() {
    // Encodes what is left, before the objects the draws use go away
    draw_queue.reset();

    for (auto& [function, pipeline] : compute_pipelines) {
        if (pipeline != nullptr)
            pipeline->release();
    }

    linear_sampler->release();
    nearest_sampler->release();

    depth_stencil_state_always_and_write->release();
    depth_stencil_state_clear_stencil->release();
    depth_stencil_state_disabled->release();

    // The pipeline prewarm uses the shader library cache, which is destroyed
    // first
    binary_archive_cache.stopPrewarm();

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
    if (mtl4_context)
        mtl4_context->addLayer(ca_layer);
    // TODO: set pixel format
}

ISurfaceCompositor* Renderer::acquireNextSurfaceHot1() {
    // Drawable
    if (ca_layer == nullptr)
        return nullptr;

#ifdef ZTD_PLATFORM_MACOS
    // FG presentation has its own clock, independent of guest vsync/render FPS.
    ca_layer->setDisplaySyncEnabled(
        CONFIG_INSTANCE.getFrameGeneration() == FrameGeneration::Off);
#endif
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
    if (disabled || range.getSize() > BUFFER_SNAPSHOT_MAX_SIZE)
        return nullptr;
    ScopedGpuWork work(GpuWork::Buffer);
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

    // A blit to the target of the open pass continues it instead of storing
    // and loading the target
    auto encoder = command_buffer_impl->getDirectRenderCommandEncoder(
        render_pass_descriptor, depth_stencil_state_disabled);

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
    // The 2D engine floors exact fixed point positions. Sample positions on a
    // texel boundary (downscaling by an integer factor from pixel centres)
    // would fall on either side of it with float rounding of the normalised
    // coordinates, so nearest sampling nudges them towards the exact result.
    constexpr f32 NEAREST_BIAS = 1.0f / 512.0f;
    const f32 bias = options.nearest ? NEAREST_BIAS : 0.0f;
    BlitParams params = {
        .src_offset = {(static_cast<f32>(src_origin.x) + bias) / src_width,
                       (static_cast<f32>(src_origin.y) + bias) / src_height},
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
    auto* command_buffer = mtl4_context
                               ? new CommandBuffer(mtl4_context, completions)
                               : new CommandBuffer(command_queue, completions);
    command_buffer->setVisibilitySource(
        [counter = occlusion.get()](CommandBuffer& buffer) {
            return counter->pageForNewPass(buffer);
        });
    return command_buffer;
}

RenderPassBase*
Renderer::createRenderPassHot12(const RenderPassDescriptor& descriptor) {
    return new RenderPass(descriptor);
}

void Renderer::bindRenderPassHot13(const RenderPassBase* render_pass) {
    state.render_pass_owner = render_pass ? render_pass->getResourceOwner() : nullptr;
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

    // Nothing is bound to clear at this index
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
        .depth_stencil_pixel_format = getDepthStencilPixelFormat(),
        .sample_count = state.render_pass != nullptr
                            ? state.render_pass->getSampleCount()
                            : 1u};
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

    // No depth target is bound to clear
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
            toMtlPixelFormat(texture->getDescriptor().format),
        .sample_count = state.render_pass != nullptr
                            ? state.render_pass->getSampleCount()
                            : 1u};
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
            toMtlPixelFormat(texture->getDescriptor().format),
        .sample_count = state.render_pass != nullptr
                            ? state.render_pass->getSampleCount()
                            : 1u};
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

void Renderer::logPassProfile(f64 frames) {
    auto* profiler = GpuPassProfiler::get(device);
    if (!profiler)
        return;
    GpuPassProfiler::Total kinds[3];
    const auto top = profiler->takeTop(6, kinds);
    frames = std::max(frames, 1.0);
    const auto ms = [&](u64 ns) { return static_cast<f64>(ns) / 1e6 / frames; };
    const auto per_frame = [&](u64 count) {
        return static_cast<f64>(count) / frames;
    };
    LOG_INFO(MetalRenderer,
             "GPU PASSES: render {:.1f}/frame ({:.2f} ms), compute {:.1f} "
             "({:.2f} ms), blit {:.1f} ({:.2f} ms) | sum {:.2f} ms/frame",
             per_frame(kinds[0].count), ms(kinds[0].ns),
             per_frame(kinds[1].count), ms(kinds[1].ns),
             per_frame(kinds[2].count), ms(kinds[2].ns),
             ms(kinds[0].ns + kinds[1].ns + kinds[2].ns));
    for (usize i = 0; i < top.size(); i++)
        LOG_INFO(MetalRenderer, "GPU TOP {}: {:.2f} ms/frame, {:.1f}/frame | {}",
                 i + 1, ms(top[i].second.ns), per_frame(top[i].second.count),
                 top[i].first);
}

void Renderer::setTitleId(u64 title_id) {
    // Sources and recipes do not depend on the GPU or the OS, unlike the
    // binaries in the binary archive cache
    const auto directory = std::filesystem::path(
                               CONFIG_INSTANCE.getAppDataPath()) /
                           "cache" / "metal" / "titles" /
                           fmt::format("{:016x}", title_id);
    shader_library_cache.setDirectory(directory / "msl");
    binary_archive_cache.startPrewarm(directory / "pipelines",
                                      shader_library_cache);
}

PipelineBase* Renderer::createPipelineHot18(const PipelineDescriptor& descriptor) {
    return new Pipeline(device, descriptor, binary_archive_cache);
}

void Renderer::bindPipelineHot19(const PipelineBase* pipeline) {
    state.pipeline_owner = pipeline ? pipeline->getResourceOwner() : nullptr;
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

    command_buffer_impl->addCompletedHandler(
        [=](const GpuCompletion& completion) {
            if (!completion.succeeded) {
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
                // Keep exact alpha for numerical mask/blend analysis. The PNG
                // below is intentionally opaque so dark intermediate buffers
                // remain visible in image viewers.
                std::ofstream(path + ".rgba8", std::ios::binary)
                    .write(reinterpret_cast<const char*>(data),
                           static_cast<std::streamsize>(pixel_count * 4));
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
    // Stages without a host shader type (compute and others) have no
    // graphics bindings
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
                      range.getBegin() - begin, range.getSize()}
        .withGuestRange(range);
}

void Renderer::bindStorageBuffer(const BufferView& buffer,
                                 ShaderType shader_type, u32 index) {
    syncDrawQueue();
    if (shader_type == ShaderType::Count)
        return;
    state.storage_buffers[static_cast<u32>(shader_type)][index] = buffer;
}

void Renderer::bindTextureHot35(ITextureView* texture, SamplerBase* sampler,
                           ShaderType shader_type, u32 index) {
    // Stages without a host shader type (compute and others) have no
    // graphics bindings
    if (shader_type == ShaderType::Count)
        return;

    if (texture != nullptr && sampler != nullptr &&
        getIntegerTextureComponentType(texture->getDescriptor().format) != nullptr) {
        auto descriptor = sampler->getDescriptor();
        descriptor.min_filter = descriptor.mag_filter =
            MTL::SamplerMinMagFilterNearest;
        descriptor.mip_filter = MTL::SamplerMipFilterNearest;
        descriptor.max_anisotropy = 1;
        std::scoped_lock lock(getSamplerCache().getMutex());
        sampler = getSamplerCache().find(descriptor);
    }
    state.textures[static_cast<u32>(shader_type)][index] = {
        .texture_view = static_cast<TextureView*>(texture),
        .sampler = static_cast<Sampler*>(sampler),
        .texture_owner = texture ? texture->getResourceOwner() : nullptr,
        .sampler_owner = sampler ? sampler->getResourceOwner() : nullptr};
}

std::vector<TextureBinding>
Renderer::getTextureBindings(ShaderType shader_type) const {
    syncDrawQueue();
    std::vector<TextureBinding> result;
    if (shader_type == ShaderType::Count)
        return result;
    const auto& bindings = state.textures[static_cast<u32>(shader_type)];
    for (u32 index = 0; index < bindings.size(); ++index) {
        const auto& binding = bindings[index];
        if (binding.texture_view || binding.sampler)
            result.push_back({index, const_cast<TextureView*>(binding.texture_view),
                              const_cast<Sampler*>(binding.sampler)});
    }
    return result;
}

void Renderer::unbindUniformBuffersHot36(ShaderType shader_type) {
    // Stages without a host shader type (compute and others) have no
    // graphics bindings
    if (shader_type == ShaderType::Count)
        return;

    state.uniform_buffers[static_cast<u32>(shader_type)] = {};
}

void Renderer::unbindStorageBuffers(ShaderType shader_type) {
    syncDrawQueue();
    if (shader_type == ShaderType::Count)
        return;
    state.storage_buffers[static_cast<u32>(shader_type)] = {};
}

void Renderer::unbindTexturesHot37(ShaderType shader_type) {
    // Stages without a host shader type (compute and others) have no
    // graphics bindings
    if (shader_type == ShaderType::Count)
        return;

    state.textures[static_cast<u32>(shader_type)] = {};
}

bool Renderer::synchronize(ICommandBuffer* command_buffer) {
    syncDrawQueue();
    auto* commands = static_cast<CommandBuffer*>(command_buffer);
    commands->debugCommitAndWait();
    return commands->lastSubmissionSucceeded();
}

bool Renderer::synchronizeGuestWrites(ICommandBuffer* command_buffer) {
    syncDrawQueue();
    if (!CommandBuffer::hasUnsynchronizedGuestWrites())
        return true;
    return synchronize(command_buffer);
}

void Renderer::queueOcclusionReport(ICommandBuffer* command_buffer,
                                    ReportPublisher publish,
                                    std::vector<ztd::Range<uptr>> writes) {
    syncDrawQueue();
    if (PerfStats::isEnabled())
        PerfStats::instance().reports_queued++;
    const auto trace = capture::GuestEvents::instance().beginReport(
        capture::currentGuestEventOrigin());
    if (trace != 0) {
        auto original = std::move(publish);
        publish = [original = std::move(original),
                   trace](std::optional<u64> value) {
            original(value);
            capture::GuestEvents::instance().completeReport(trace,
                                                            value.has_value());
        };
    }
    occlusion->queueReport(*static_cast<CommandBuffer*>(command_buffer),
                           std::move(publish), std::move(writes));
}

void Renderer::queueGuestWrite(ICommandBuffer* command_buffer,
                               std::function<void(bool succeeded)> write,
                               std::vector<ztd::Range<uptr>> writes) {
    syncDrawQueue();
    auto* impl = static_cast<CommandBuffer*>(command_buffer);
    const auto trace = capture::GuestEvents::instance().beginReport(
        capture::currentGuestEventOrigin());
    if (trace != 0) {
        auto original = std::move(write);
        write = [original = std::move(original), trace](bool succeeded) {
            original(succeeded);
            capture::GuestEvents::instance().completeReport(trace, succeeded);
        };
    }
    // The callbacks of the submission follow the write
    impl->noteQueuedCompletionWork();
    completions->push(impl->shareCompletionToken(), std::move(write),
                      std::move(writes), true);
}

void Renderer::flushReports(ICommandBuffer* command_buffer,
                            ztd::Range<uptr> range) {
    syncDrawQueue();
    if (!completions->hasPendingWrites())
        return;
    const auto sequence = completions->lastWriteTo(range);
    if (sequence == 0 || completions->wait(sequence, std::chrono::milliseconds(0)))
        return;
    if (PerfStats::isEnabled())
        PerfStats::instance().report_waits++;
    // The report waits for the commands of the submission it was queued in
    if (command_buffer != nullptr) {
        // The consumer that asked for the wait is named if it did
        ScopedCheckpointReason reason(
            command_buffer,
            command_buffer->checkpoint_reason == CheckpointReason::Other
                ? CheckpointReason::ReportFlush
                : command_buffer->checkpoint_reason);
        synchronize(command_buffer);
    }
    ScopedGpuWork work(GpuWork::Wait);
    if (!completions->wait(sequence, std::chrono::seconds(2)))
        LOG_WARN(MetalRenderer, "A queued report did not reach guest memory "
                                "within 2 seconds");
}

void Renderer::drainReports() {
    syncDrawQueue();
    if (!completions->hasPendingOrdered())
        return;
    // A marker behind everything queued so far
    const auto sequence = completions->push(nullptr, [](bool) {});
    if (!completions->wait(sequence, std::chrono::seconds(5)))
        LOG_WARN(MetalRenderer, "Queued reports did not finish within 5 seconds");
}

std::optional<u64> Renderer::prepareOcclusionQuery(CommandBuffer* command_buffer) {
    return occlusion->prepareDraw(*command_buffer,
                                  state.render_pass->getRenderPassDescriptor());
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

    if (state.pipeline->hasTessellation()) {
        drawTessellation(command_buffer_impl, start, count, false, 0,
                         base_instance, instance_count);
        return;
    }
    if (state.pipeline->hasGeometry()) {
        drawGeometry(command_buffer_impl, primitive_type, start, count, false,
                     0, base_instance, instance_count);
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
    if (state.triangle_fill_mode != MTL::TriangleFillModeFill)
        encoder->setTriangleFillMode(MTL::TriangleFillModeFill);
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
    if (state.pipeline->hasTessellation()) {
        drawTessellation(command_buffer_impl, start, count, true, base_vertex,
                         base_instance, instance_count);
        return;
    }
    if (state.pipeline->hasGeometry()) {
        drawGeometry(command_buffer_impl, primitive_type, start, count, true,
                     base_vertex, base_instance, instance_count);
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
    if (state.triangle_fill_mode != MTL::TriangleFillModeFill)
        encoder->setTriangleFillMode(MTL::TriangleFillModeFill);
    if (visibility_offset)
        encoder->setVisibilityResultMode(MTL::VisibilityResultModeDisabled, 0);
    if (capture_group) {
        encoder->popDebugGroup();
        capture_draw_label.clear();
    }
}

void Renderer::bindStageResources(CommandBuffer* command_buffer,
                                  const Shader* shader, ShaderType state_type,
                                  ShaderType host_type) {
    const auto& mapping = shader->getDescriptor().resource_mapping;
    const auto type = static_cast<u32>(state_type);
    for (u32 i = 0; i < CONST_BUFFER_BINDING_COUNT; i++) {
        const auto& buffer = state.uniform_buffers[type][i];
        if (mapping.uniform_buffers[i] == invalid<u32>() ||
            buffer.getBase() == nullptr)
            continue;
        command_buffer->setBuffer(
            static_cast<Buffer*>(buffer.getBase())->getBuffer(),
            buffer.getOffset(), host_type, i);
    }
    for (const auto& storage : mapping.storage_buffers) {
        const auto& buffer = state.storage_buffers[type][storage.host_index];
        if (buffer.getBase() == nullptr)
            continue;
        if (storage.written)
            command_buffer->noteGuestMemoryWrites();
        command_buffer->setBuffer(
            static_cast<Buffer*>(buffer.getBase())->getBuffer(),
            buffer.getOffset(), host_type, storage.host_index);
    }
    const auto sampler_encoder = shader->getSamplerEncoder();
    for (const auto& [binding, index] : mapping.textures) {
        (void)binding;
        const auto& texture = state.textures[type][index];
        command_buffer->setTexture(texture.texture_view != nullptr
                                       ? texture.texture_view->getTexture()
                                       : nullptr,
                                   host_type, index);
        if (texture.sampler != nullptr && index < SAMPLER_COUNT &&
            sampler_encoder == nullptr)
            command_buffer->setSampler(texture.sampler->getSampler(), host_type,
                                       index);
    }
    if (sampler_encoder == nullptr)
        return;
    auto* buffer = allocateTextureUploadBuffer(sampler_encoder->encodedLength());
    const auto native = static_cast<Buffer*>(buffer)->getBuffer();
    sampler_encoder->setArgumentBuffer(native, 0);
    for (const auto& [binding, index] : mapping.textures) {
        (void)binding;
        const auto sampler = state.textures[type][index].sampler;
        sampler_encoder->setSamplerState(
            sampler != nullptr ? sampler->getSampler() : nullptr, index);
    }
    command_buffer->setBuffer(native, 0, host_type, mapping.sampler_buffer_index);
    retireTextureUploadBuffer(command_buffer, buffer);
}

std::optional<Renderer::VertexRange>
Renderer::getDrawVertexRange(bool indexed, u32 start, u32 count,
                             u32 base_vertex) {
    if (!indexed)
        return VertexRange{start, count, nullptr, 0};
    auto* index_buffer =
        static_cast<Buffer*>(state.index_buffer.getBase())->getBuffer();
    const u64 index_offset = state.index_buffer.getOffset();
    const auto* data = static_cast<const u8*>(index_buffer->contents());
    if (data == nullptr) {
        ONCE(LOG_WARN(MetalRenderer, "Geometry or tessellation draw with an "
                                     "index buffer the CPU cannot read"));
        return std::nullopt;
    }
    data += index_offset;
    u32 lowest = invalid<u32>();
    u32 highest = 0;
    for (u32 i = start; i < start + count; i++) {
        u32 index = 0;
        switch (state.index_type) {
        case engines::IndexType::UInt8:
            index = data[i];
            break;
        case engines::IndexType::UInt16:
            index = reinterpret_cast<const u16*>(data)[i];
            break;
        default:
            index = reinterpret_cast<const u32*>(data)[i];
            break;
        }
        lowest = std::min(lowest, index);
        highest = std::max(highest, index);
    }
    return VertexRange{lowest + base_vertex, highest - lowest + 1, index_buffer,
                       index_offset};
}

void Renderer::encodeVertexCapture(CommandBuffer* command_buffer,
                                   const VertexRange& range,
                                   MTL::Buffer* records, MTL::Buffer* params,
                                   bool indexed, u32 start, u32 count,
                                   u32 base_vertex, u32 base_instance) {
    const auto& vertex_mapping =
        static_cast<const Shader*>(
            state.pipeline->getDescriptor()
                .shaders[static_cast<u32>(ShaderType::Vertex)])
            ->getDescriptor()
            .resource_mapping;
    auto encoder = command_buffer->getRenderCommandEncoderUnchecked();
    command_buffer->setRenderPipelineState(state.pipeline->getCapturePipeline());
    command_buffer->setBuffer(records, 0, ShaderType::Vertex,
                              vertex_mapping.capture_buffer_index);
    command_buffer->setBuffer(params, 0, ShaderType::Vertex,
                              vertex_mapping.capture_params_index);
    if (indexed)
        encoder->drawIndexedPrimitives(
            MTL::PrimitiveTypePoint, count, toMtlIndexType(state.index_type),
            range.index_buffer,
            range.index_offset +
                u64(start) * engines::getIndexTypeSize(state.index_type),
            1, base_vertex, base_instance);
    else
        encoder->drawPrimitives(MTL::PrimitiveTypePoint, start, count, 1,
                                base_instance);
    encoder->vertexBufferBarrier();
}

void Renderer::drawTessellation(CommandBuffer* command_buffer, u32 start,
                                u32 count, bool indexed, u32 base_vertex,
                                u32 base_instance, u32 instance_count) {
    const auto* pipeline = state.pipeline;
    const auto& descriptor = pipeline->getDescriptor();
    const auto* vertex_shader = static_cast<const Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::Vertex)]);
    const auto* control_shader = static_cast<const Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::TessControl)]);
    const auto* evaluation_shader = static_cast<const Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::TessEvaluation)]);
    const u32 patch_size = std::max(descriptor.tessellation_patch_size, 1u);
    const u32 patches = count / patch_size;
    const u32 control_points =
        control_shader->getDescriptor().resource_mapping.tess_output_vertices;
    const auto domain = tessellation::getDomain(descriptor.tessellation_parameters);
    if (patches == 0 || control_points == 0 ||
        vertex_shader->getDescriptor().resource_mapping.capture_buffer_index ==
            invalid<u32>())
        return;
    auto encoder = command_buffer->getRenderCommandEncoderUnchecked();
    if (!encoder->supportsTessellation()) {
        ONCE(LOG_WARN(MetalRenderer, "Tessellation needs the Metal 3 encoder, "
                                     "tessellated draws are skipped"));
        return;
    }
    const auto range = getDrawVertexRange(indexed, start, count, base_vertex);
    if (!range)
        return;

    const u64 record_bytes = u64(range->slots) * geometry::RECORD_SIZE;
    const u64 control_point_bytes =
        u64(patches) * control_points * geometry::RECORD_SIZE;
    const u64 patch_bytes = u64(patches) * tessellation::PATCH_SIZE;
    const u64 factor_bytes = u64(patches) * tessellation::getFactorSize(domain);
    constexpr u64 MAX_BYTES = 512ull << 20;
    if (record_bytes + control_point_bytes + patch_bytes + factor_bytes >
        MAX_BYTES) {
        ONCE(LOG_WARN(MetalRenderer,
                      "Tessellated draw needs {} MiB of buffers, skipped",
                      (record_bytes + control_point_bytes + patch_bytes +
                       factor_bytes) >>
                          20));
        return;
    }
    auto* records = allocateTextureUploadBuffer(record_bytes);
    auto* control = allocateTextureUploadBuffer(control_point_bytes);
    auto* patch_data = allocateTextureUploadBuffer(patch_bytes);
    auto* factors = allocateTextureUploadBuffer(factor_bytes);
    constexpr u64 PARAMS_OFFSET = 256;
    auto* params = allocateTextureUploadBuffer(PARAMS_OFFSET +
                                               sizeof(geometry::Params));
    const auto native = [](BufferBase* buffer) {
        return static_cast<Buffer*>(buffer)->getBuffer();
    };
    auto* params_data = static_cast<u8*>(native(params)->contents());
    const geometry::CaptureParams capture_params{range->min_vertex};
    std::memcpy(params_data, &capture_params, sizeof(capture_params));
    const geometry::Params patch_params{
        patches,
        static_cast<u32>(indexed ? state.index_type : engines::IndexType::None),
        start,
        static_cast<i32>(base_vertex),
        range->min_vertex,
        count};
    std::memcpy(params_data + PARAMS_OFFSET, &patch_params,
                sizeof(patch_params));

    const auto visibility_offset = prepareOcclusionQuery(command_buffer);
    for (u32 instance = 0; instance < instance_count; instance++) {
        // The vertex shader's resources again: the other stages of the
        // previous instance replaced them
        bindDrawState(command_buffer);
        encoder = command_buffer->getRenderCommandEncoderUnchecked();
        if (instance != 0)
            encoder->vertexBufferBarrier();

        // 1. The vertex shader writes the input control points' records
        encodeVertexCapture(command_buffer, *range, native(records),
                            native(params), indexed, start, count, base_vertex,
                            base_instance + instance);

        // 2. The control shader writes control points and patch attributes
        command_buffer->setRenderPipelineState(
            pipeline->getTessControlPipeline());
        bindStageResources(command_buffer, control_shader,
                           ShaderType::TessControl, ShaderType::Vertex);
        command_buffer->setBuffer(native(records), 0, ShaderType::Vertex,
                                  geometry::INPUT_BUFFER);
        command_buffer->setBuffer(indexed ? range->index_buffer : native(records),
                                  indexed ? range->index_offset : 0,
                                  ShaderType::Vertex,
                                  geometry::INDEX_DATA_BUFFER);
        command_buffer->setBuffer(native(control), 0, ShaderType::Vertex,
                                  tessellation::CONTROL_POINT_BUFFER);
        command_buffer->setBuffer(native(patch_data), 0, ShaderType::Vertex,
                                  tessellation::PATCH_BUFFER);
        command_buffer->setBuffer(native(params), PARAMS_OFFSET,
                                  ShaderType::Vertex, geometry::PARAMS_BUFFER);
        encoder->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0),
                                NS::UInteger(u64(patches) * control_points));
        encoder->vertexBufferBarrier();

        // 3. The patch levels become Metal tessellation factors
        command_buffer->setRenderPipelineState(pipeline->getTessFactorPipeline());
        command_buffer->setBuffer(native(factors), 0, ShaderType::Vertex,
                                  tessellation::FACTOR_BUFFER);
        encoder->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0),
                                NS::UInteger(patches));
        encoder->vertexBufferBarrier();

        // 4. The evaluation shader runs per tessellated vertex
        command_buffer->setRenderPipelineState(pipeline->getPipeline());
        bindStageResources(command_buffer, evaluation_shader,
                           ShaderType::TessEvaluation, ShaderType::Vertex);
        command_buffer->setBuffer(native(control), 0, ShaderType::Vertex,
                                  tessellation::EVALUATION_CONTROL_POINT_BUFFER);
        command_buffer->setBuffer(native(patch_data), 0, ShaderType::Vertex,
                                  tessellation::PATCH_BUFFER);
        encoder->setTessellationFactorBuffer(native(factors), 0, 0);
        if (visibility_offset)
            encoder->setVisibilityResultMode(MTL::VisibilityResultModeCounting,
                                             *visibility_offset);
        encoder->drawPatches(control_points, 0, patches, 1,
                             base_instance + instance);
        if (visibility_offset)
            encoder->setVisibilityResultMode(MTL::VisibilityResultModeDisabled,
                                             0);
    }
    if (state.triangle_fill_mode != MTL::TriangleFillModeFill)
        command_buffer->getRenderCommandEncoderUnchecked()->setTriangleFillMode(
            MTL::TriangleFillModeFill);
    for (auto* buffer : {records, control, patch_data, factors, params})
        retireTextureUploadBuffer(command_buffer, buffer);
}

void Renderer::drawGeometry(CommandBuffer* command_buffer,
                            engines::PrimitiveType primitive_type, u32 start,
                            u32 count, bool indexed, u32 base_vertex,
                            u32 base_instance, u32 instance_count) {
    const auto* pipeline = state.pipeline;
    const auto& descriptor = pipeline->getDescriptor();
    const auto* vertex_shader = static_cast<const Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::Vertex)]);
    const auto* geometry_shader = static_cast<const Shader*>(
        descriptor.shaders[static_cast<u32>(ShaderType::Geometry)]);
    const auto& vertex_mapping = vertex_shader->getDescriptor().resource_mapping;
    const auto& geometry_mapping =
        geometry_shader->getDescriptor().resource_mapping;
    const auto output =
        static_cast<geometry::Output>(geometry_mapping.geometry_output);
    const u32 max_vertices = geometry_mapping.geometry_max_output_vertices;
    const u32 invocations = std::max(geometry_mapping.geometry_invocations, 1u);
    const u32 max_indices = geometry::getMaxOutputIndices(output, max_vertices);
    const u32 primitives = geometry::getPrimitiveCount(primitive_type, count);
    if (primitives == 0 || max_indices == 0 ||
        vertex_mapping.capture_buffer_index == invalid<u32>())
        return;

    const auto range = getDrawVertexRange(indexed, start, count, base_vertex);
    if (!range)
        return;
    const u32 min_vertex = range->min_vertex;
    const u32 vertex_slots = range->slots;
    MTL::Buffer* index_buffer = range->index_buffer;
    const u64 index_offset = range->index_offset;

    const u64 threads = u64(primitives) * invocations;
    const u64 record_bytes = u64(vertex_slots) * geometry::RECORD_SIZE;
    const u64 output_bytes =
        threads * (max_vertices + 1) * geometry::RECORD_SIZE;
    const u64 index_bytes = threads * max_indices * sizeof(u32);
    constexpr u64 MAX_BYTES = 512ull << 20;
    if (record_bytes + output_bytes + index_bytes > MAX_BYTES) {
        ONCE(LOG_WARN(MetalRenderer,
                      "Geometry shader draw needs {} MiB of buffers, skipped",
                      (record_bytes + output_bytes + index_bytes) >> 20));
        return;
    }
    auto* records = allocateTextureUploadBuffer(record_bytes);
    auto* emitted = allocateTextureUploadBuffer(output_bytes);
    auto* indices = allocateTextureUploadBuffer(index_bytes);
    // The capture parameters, then the geometry parameters at 256 bytes
    constexpr u64 PARAMS_OFFSET = 256;
    auto* params = allocateTextureUploadBuffer(PARAMS_OFFSET +
                                               sizeof(geometry::Params));
    const auto native = [](BufferBase* buffer) {
        return static_cast<Buffer*>(buffer)->getBuffer();
    };
    auto* params_data = static_cast<u8*>(native(params)->contents());
    const geometry::CaptureParams capture_params{min_vertex};
    std::memcpy(params_data, &capture_params, sizeof(capture_params));
    const geometry::Params geometry_params{
        static_cast<u32>(primitives),
        static_cast<u32>(indexed ? state.index_type : engines::IndexType::None),
        start,
        static_cast<i32>(base_vertex),
        min_vertex,
        count};
    std::memcpy(params_data + PARAMS_OFFSET, &geometry_params,
                sizeof(geometry_params));

    MTL::PrimitiveType list_type = MTL::PrimitiveTypeTriangle;
    if (output == geometry::Output::Points)
        list_type = MTL::PrimitiveTypePoint;
    else if (output == geometry::Output::LineStrip)
        list_type = MTL::PrimitiveTypeLine;

    const auto visibility_offset = prepareOcclusionQuery(command_buffer);
    for (u32 instance = 0; instance < instance_count; instance++) {
        // The vertex shader's resources (and vertex buffers) again: the
        // geometry shader of the previous instance replaced them
        bindDrawState(command_buffer);
        auto encoder = command_buffer->getRenderCommandEncoderUnchecked();
        // The previous instance's draw reads the buffers rewritten below
        if (instance != 0)
            encoder->vertexBufferBarrier();

        // 1. The vertex shader writes its output records
        encodeVertexCapture(command_buffer, *range, native(records),
                            native(params), indexed, start, count, base_vertex,
                            base_instance + instance);

        // 2. The geometry shader emits vertices and list indices
        command_buffer->setRenderPipelineState(pipeline->getGeometryPipeline());
        bindStageResources(command_buffer, geometry_shader, ShaderType::Geometry,
                           ShaderType::Vertex);
        command_buffer->setBuffer(native(records), 0, ShaderType::Vertex,
                                  geometry::INPUT_BUFFER);
        command_buffer->setBuffer(indexed ? index_buffer : native(records),
                                  indexed ? index_offset : 0,
                                  ShaderType::Vertex,
                                  geometry::INDEX_DATA_BUFFER);
        command_buffer->setBuffer(native(emitted), 0, ShaderType::Vertex,
                                  geometry::OUTPUT_BUFFER);
        command_buffer->setBuffer(native(indices), 0, ShaderType::Vertex,
                                  geometry::OUTPUT_INDEX_BUFFER);
        command_buffer->setBuffer(native(params), PARAMS_OFFSET,
                                  ShaderType::Vertex, geometry::PARAMS_BUFFER);
        encoder->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0),
                                NS::UInteger(threads));
        encoder->vertexBufferBarrier();

        // 3. The passthrough draws them with the fragment shader
        command_buffer->setRenderPipelineState(pipeline->getPipeline());
        if (visibility_offset)
            encoder->setVisibilityResultMode(MTL::VisibilityResultModeCounting,
                                             *visibility_offset);
        encoder->drawIndexedPrimitives(list_type,
                                       NS::UInteger(threads * max_indices),
                                       MTL::IndexTypeUInt32, native(indices), 0);
        if (visibility_offset)
            encoder->setVisibilityResultMode(MTL::VisibilityResultModeDisabled,
                                             0);
    }
    if (state.triangle_fill_mode != MTL::TriangleFillModeFill)
        command_buffer->getRenderCommandEncoderUnchecked()->setTriangleFillMode(
            MTL::TriangleFillModeFill);
    for (auto* buffer : {records, emitted, indices, params})
        retireTextureUploadBuffer(command_buffer, buffer);
}

f64 Renderer::debugSyncCommandBufferHot40(ICommandBuffer* command_buffer) {
    return static_cast<CommandBuffer*>(command_buffer)->debugCommitAndWait();
}

bool Renderer::dispatchComputeHot41(ICommandBuffer* command_buffer,
                                    const ComputeDispatch& dispatch) {
    ScopedGpuWork work(GpuWork::Encode);
    if (!command_buffer)
        return false;
    if (command_ir::emptyComputeGrid(dispatch.grid_size.width,
                                     dispatch.grid_size.height,
                                     dispatch.grid_size.depth) ||
        command_ir::emptyComputeGrid(dispatch.block_size.width,
                                     dispatch.block_size.height,
                                     dispatch.block_size.depth))
        return true;
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);

    const auto shader = static_cast<Shader*>(dispatch.shader);
    if (shader == nullptr || shader->getFunction() == nullptr)
        return false;

    // Pipeline
    auto [it, inserted] =
        compute_pipelines.try_emplace(shader->getFunction(), nullptr);
    if (inserted) {
        NS::Error* error = nullptr;
        ScopedGpuWork shader_work(GpuWork::Shader);
        it->second =
            device->newComputePipelineState(shader->getFunction(), &error);
        if (it->second == nullptr)
            LOG_ERROR(MetalRenderer, "Failed to create compute pipeline: {}",
                      error ? error->localizedDescription()->utf8String()
                            : "unknown error");
    }
    const auto pipeline = it->second;
    if (pipeline == nullptr)
        return false;

    const auto& block = dispatch.block_size;
    const auto& grid = dispatch.grid_size;
    if (!command_ir::validComputeGrid(grid.width, grid.height, grid.depth))
        return false;
    if (!command_ir::validComputeBlock(
            block.width, block.height, block.depth,
            pipeline->maxTotalThreadsPerThreadgroup())) {
        ONCE(LOG_ERROR(MetalRenderer,
                       "Compute block {}x{}x{} exceeds the maximum of {} "
                       "threads",
                       block.width, block.height, block.depth,
                       pipeline->maxTotalThreadsPerThreadgroup()));
        return false;
    }

    if (PerfStats::isEnabled()) {
        PerfStats::instance().compute_dispatches++;
        PerfStats::instance().compute_threads +=
            u64(grid.width) * grid.height * grid.depth * block.width * block.height *
            block.depth;
    }

    static const bool profile = std::getenv("HYDRA_PROFILE_COMPUTE") != nullptr;
    if (profile) {
        command_buffer_impl->debugCommitAndWait();
        if (!command_buffer_impl->lastSubmissionSucceeded())
            return false;
    }
    for (const auto& binding : dispatch.storage_buffers)
        if (binding.index >= BUFFER_COUNT)
            return false;
    const auto sampler_encoder = shader->getSamplerEncoder();
    for (const auto& binding : dispatch.textures)
        if (binding.index >= TEXTURE_COUNT ||
            (!sampler_encoder && binding.sampler &&
             binding.index >= SAMPLER_COUNT))
            return false;
    for (const auto& binding : dispatch.images)
        if (binding.index >= TEXTURE_COUNT)
            return false;
    auto encoder = command_buffer_impl->getComputeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
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
        if (binding.written)
            command_buffer_impl->noteGuestMemoryWrites();
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

    // Profile the actual dispatch once, in queue order. Re-encoding the
    // kernel on a second buffer would execute storage/image side effects twice.
    if (profile) {
        const auto ms = command_buffer_impl->debugCommitAndWait();
        if (!command_buffer_impl->lastSubmissionSucceeded())
            return false;
        static std::map<u64, double> max_ms;
        auto& max = max_ms[dispatch.debug_program_addr];
        if (ms > max * 1.5 + 0.01) {
            max = ms;
            LOG_INFO(MetalRenderer,
                     "DEBUG compute {:#x} grid {}x{}x{} block {}x{}x{}: "
                     "{:.3f} ms (status {})",
                     dispatch.debug_program_addr, grid.width, grid.height,
                     grid.depth, block.width, block.height, block.depth, ms,
                     static_cast<int>(MTL::CommandBufferStatusCompleted));
        }
    }
    return true;
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

RenderEncoder*
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
                                ShaderType shader_type, u32 index,
                                bool written) {
    const auto& buffer =
        state.storage_buffers[static_cast<u32>(shader_type)][index];
    if (buffer.getBase() == nullptr)
        return;
    // Storage buffers alias guest memory: what the shader writes reaches the
    // CPU after the next checkpoint
    if (written)
        command_buffer->noteGuestMemoryWrites();
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
        // A pipeline compiling in the background is expected to be missing
        if (!state.pipeline->isCompiling())
            ONCE(LOG_WARN(MetalRenderer, "Pipeline not present, skipping draw"));
        return false;
    }

    return true;
}

void Renderer::bindDrawStateHot53(CommandBuffer* command_buffer) {
    ScopedGpuWork work(GpuWork::Encode);
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
    // Reset after the draw (see endPolygonLines): internal draws fill
    if (state.triangle_fill_mode != MTL::TriangleFillModeFill)
        command_buffer->getRenderCommandEncoderUnchecked()->setTriangleFillMode(
            state.triangle_fill_mode);

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
                             storage.host_index, storage.written);
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
