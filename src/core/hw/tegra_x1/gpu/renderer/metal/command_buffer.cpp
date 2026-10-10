#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"

#include <cstdlib>

#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/upload_buffer_pool.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

CommandBuffer::CommandBuffer(MTL::CommandQueue* command_queue) {
    TMP_AUTORELEASE_POOL_BEGIN();
    command_buffer = command_queue->commandBuffer()->retain();
    TMP_AUTORELEASE_POOL_END();
}

CommandBuffer::~CommandBuffer() {
    endEncoding();
    if (!retired_upload_buffers.empty()) {
        command_buffer->addCompletedHandler(
            [pool = std::move(upload_pool),
             buffers =
                 std::move(retired_upload_buffers)](MTL::CommandBuffer* cb) {
                for (auto* buffer : buffers) {
                    if (cb->status() == MTL::CommandBufferStatusCompleted)
                        pool->recycle(buffer);
                    else
                        buffer->release();
                }
            });
    }
    if (PerfStats::isEnabled()) {
        PerfStats::instance().raster_updates += raster_updates;
        PerfStats::instance().raster_skips += raster_skips;
        PerfStats::instance().command_buffers++;
        command_buffer->addCompletedHandler([](MTL::CommandBuffer* cb) {
            const auto dt = cb->GPUEndTime() - cb->GPUStartTime();
            if (dt > 0.0)
                PerfStats::instance().gpu_ns +=
                    static_cast<u64>(dt * 1'000'000'000.0);
        });
    }
    command_buffer->commit();
    // Command buffers execute in commit order on the same queue, so there is no
    // need to wait for the GPU. HYDRA_GPU_SYNC=1 restores the old behaviour
    // for debugging.
    static const bool gpu_sync = std::getenv("HYDRA_GPU_SYNC") != nullptr;
    if (gpu_sync)
        command_buffer->waitUntilCompleted();
    command_buffer->release();
}

void CommandBuffer::retireUploadBuffer(std::shared_ptr<UploadBufferPool> pool,
                                       MTL::Buffer* buffer) {
    ASSERT(!upload_pool || upload_pool == pool, MetalRenderer,
           "An upload command buffer cannot mix buffer pools");
    upload_pool = std::move(pool);
    retired_upload_buffers.push_back(buffer);
}

MTL::RenderCommandEncoder* CommandBuffer::getRenderCommandEncoder(
    MTL::RenderPassDescriptor* render_pass_descriptor) {
    if (render_pass_descriptor == encoder_state.render_pass)
        return getRenderCommandEncoderUnchecked();

    // Creating the encoder already resets all cached render state.
    return createRenderCommandEncoder(render_pass_descriptor);
}

MTL::RenderCommandEncoder* CommandBuffer::createRenderCommandEncoder(
    MTL::RenderPassDescriptor* render_pass_descriptor) {
    endEncoding();

    TMP_AUTORELEASE_POOL_BEGIN();
    command_encoder =
        command_buffer->renderCommandEncoder(render_pass_descriptor)->retain();
    TMP_AUTORELEASE_POOL_END();

    encoder_type = EncoderType::Render;
    encoder_state.render_pass = render_pass_descriptor;
    encoder_state.render = {};

    return getRenderCommandEncoderUnchecked();
}

MTL::BlitCommandEncoder* CommandBuffer::getBlitCommandEncoder() {
    if (encoder_type == EncoderType::Blit)
        return getBlitCommandEncoderUnchecked();

    endEncoding();

    TMP_AUTORELEASE_POOL_BEGIN();
    command_encoder = command_buffer->blitCommandEncoder()->retain();
    TMP_AUTORELEASE_POOL_END();

    encoder_type = EncoderType::Blit;

    return getBlitCommandEncoderUnchecked();
}

void CommandBuffer::endEncoding() {
    if (encoder_type == EncoderType::None)
        return;

    command_encoder->endEncoding();
    command_encoder->release();
    command_encoder = nullptr;
    encoder_type = EncoderType::None;

    // Reset the render pass
    encoder_state.render_pass = nullptr;
}

void CommandBuffer::setRenderPipelineState(MTL::RenderPipelineState* pipeline) {
    auto& bound_pipeline = encoder_state.render.pipeline;
    if (pipeline == bound_pipeline)
        return;

    getRenderCommandEncoderUnchecked()->setRenderPipelineState(pipeline);
    bound_pipeline = pipeline;
}

void CommandBuffer::setDepthStencilState(
    MTL::DepthStencilState* depth_stencil_state) {
    auto& bound_depth_stencil_state = encoder_state.render.depth_stencil_state;
    if (depth_stencil_state == bound_depth_stencil_state)
        return;

    getRenderCommandEncoderUnchecked()->setDepthStencilState(
        depth_stencil_state);
    bound_depth_stencil_state = depth_stencil_state;
}

void CommandBuffer::setCullMode(MTL::CullMode cull_mode) {
    auto& bound_cull_mode = encoder_state.render.cull_mode;
    if (cull_mode == bound_cull_mode)
        return;

    getRenderCommandEncoderUnchecked()->setCullMode(cull_mode);
    bound_cull_mode = cull_mode;
}

void CommandBuffer::setFrontFaceWinding(MTL::Winding front_face_winding) {
    auto& bound_front_face_winding = encoder_state.render.front_face_winding;
    if (front_face_winding == bound_front_face_winding)
        return;

    getRenderCommandEncoderUnchecked()->setFrontFacingWinding(
        front_face_winding);
    bound_front_face_winding = front_face_winding;
}

namespace {
bool rasterCacheEnabled() {
    static const bool enabled =
        std::getenv("HYDRA_DISABLE_RASTER_CACHE") == nullptr;
    return enabled;
}
} // namespace

void CommandBuffer::setDepthBias(float constant, float slope, float clamp) {
    const std::array<float, 3> value{constant, slope, clamp};
    auto& cached = encoder_state.render.depth_bias;
    if (rasterCacheEnabled() && cached && *cached == value) {
        ++raster_skips;
        return;
    }
    getRenderCommandEncoderUnchecked()->setDepthBias(constant, slope, clamp);
    cached = value;
    ++raster_updates;
}

void CommandBuffer::setBlendColor(float r, float g, float b, float a) {
    const std::array<float, 4> value{r, g, b, a};
    auto& cached = encoder_state.render.blend_color;
    if (rasterCacheEnabled() && cached && *cached == value) {
        ++raster_skips;
        return;
    }
    getRenderCommandEncoderUnchecked()->setBlendColor(r, g, b, a);
    cached = value;
    ++raster_updates;
}

void CommandBuffer::setStencilReferences(u32 front, u32 back) {
    const std::array<u32, 2> value{front, back};
    auto& cached = encoder_state.render.stencil_references;
    if (rasterCacheEnabled() && cached && *cached == value) {
        ++raster_skips;
        return;
    }
    getRenderCommandEncoderUnchecked()->setStencilReferenceValues(front, back);
    cached = value;
    ++raster_updates;
}

void CommandBuffer::setViewports(std::span<const MTL::Viewport> values) {
    ASSERT(!values.empty() && values.size() <= VIEWPORT_COUNT, MetalRenderer,
           "Invalid raster rectangle count {}", values.size());
    auto& cached = encoder_state.render;
    const auto equal = [](const MTL::Viewport& a, const MTL::Viewport& b) {
        return a.originX == b.originX && a.originY == b.originY &&
               a.width == b.width && a.height == b.height &&
               a.znear == b.znear && a.zfar == b.zfar;
    };
    if (rasterCacheEnabled() && cached.viewport_count == values.size() &&
        std::equal(values.begin(), values.end(), cached.viewports.begin(),
                   equal)) {
        ++raster_skips;
        return;
    }
    getRenderCommandEncoderUnchecked()->setViewports(values.data(),
                                                     values.size());
    std::copy(values.begin(), values.end(), cached.viewports.begin());
    cached.viewport_count = values.size();
    ++raster_updates;
}

void CommandBuffer::setScissors(std::span<const MTL::ScissorRect> values) {
    ASSERT(!values.empty() && values.size() <= VIEWPORT_COUNT, MetalRenderer,
           "Invalid raster rectangle count {}", values.size());
    auto& cached = encoder_state.render;
    const auto equal = [](const MTL::ScissorRect& a,
                          const MTL::ScissorRect& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width &&
               a.height == b.height;
    };
    if (rasterCacheEnabled() && cached.scissor_count == values.size() &&
        std::equal(values.begin(), values.end(), cached.scissors.begin(),
                   equal)) {
        ++raster_skips;
        return;
    }
    getRenderCommandEncoderUnchecked()->setScissorRects(values.data(),
                                                        values.size());
    std::copy(values.begin(), values.end(), cached.scissors.begin());
    cached.scissor_count = values.size();
    ++raster_updates;
}

void CommandBuffer::setBuffer(MTL::Buffer* buffer, u64 offset,
                              ShaderType shader_type, u32 index) {
    ASSERT_DEBUG(index < BUFFER_COUNT, MetalRenderer, "Invalid buffer index {}",
                 index);

    auto& bound_buffer =
        encoder_state.render.buffers[static_cast<u32>(shader_type)][index];
    if (buffer == bound_buffer.buffer && offset == bound_buffer.offset)
        return;

    // TODO: fast path for offset only change

    switch (shader_type) {
    case ShaderType::Vertex:
        getRenderCommandEncoderUnchecked()->setVertexBuffer(buffer, offset,
                                                            index);
        break;
    case ShaderType::Fragment:
        getRenderCommandEncoderUnchecked()->setFragmentBuffer(buffer, offset,
                                                              index);
        break;
    default:
        LOG_ERROR(MetalRenderer, "Invalid shader type {}", shader_type);
        break;
    }
    bound_buffer.buffer = buffer;
    bound_buffer.offset = offset;
}

void CommandBuffer::setTexture(MTL::Texture* texture, ShaderType shader_type,
                               u32 index) {
    ASSERT_DEBUG(index < TEXTURE_COUNT, MetalRenderer,
                 "Invalid texture index {}", index);

    auto& bound_texture =
        encoder_state.render.textures[static_cast<u32>(shader_type)][index];
    if (texture == bound_texture)
        return;

    switch (shader_type) {
    case ShaderType::Vertex:
        getRenderCommandEncoderUnchecked()->setVertexTexture(texture, index);
        break;
    case ShaderType::Fragment:
        getRenderCommandEncoderUnchecked()->setFragmentTexture(texture, index);
        break;
    default:
        LOG_ERROR(MetalRenderer, "Invalid shader type {}", shader_type);
        break;
    }
    bound_texture = texture;
}

void CommandBuffer::setSampler(MTL::SamplerState* sampler,
                               ShaderType shader_type, u32 index) {
    ASSERT_DEBUG(index < TEXTURE_COUNT, MetalRenderer,
                 "Invalid texture index {}", index);

    auto& bound_sampler =
        encoder_state.render.samplers[static_cast<u32>(shader_type)][index];
    if (sampler == bound_sampler)
        return;

    switch (shader_type) {
    case ShaderType::Vertex:
        getRenderCommandEncoderUnchecked()->setVertexSamplerState(sampler,
                                                                  index);
        break;
    case ShaderType::Fragment:
        getRenderCommandEncoderUnchecked()->setFragmentSamplerState(sampler,
                                                                    index);
        break;
    default:
        LOG_ERROR(MetalRenderer, "Invalid shader type {}", shader_type);
        break;
    }
    bound_sampler = sampler;
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
