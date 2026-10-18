#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"

#include <cstdio>
#include <cstdlib>

#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/upload_buffer_pool.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

CommandBuffer::CommandBuffer(MTL::CommandQueue* command_queue_)
    : command_queue{command_queue_} {
    TMP_AUTORELEASE_POOL_BEGIN();
    command_buffer = command_queue->commandBuffer()->retain();
    TMP_AUTORELEASE_POOL_END();
}

CommandBuffer::~CommandBuffer() {
    endEncoding();
    if (visibility_buffer != nullptr)
        visibility_buffer->release();
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
        PerfStats::instance().render_encoders += render_encoders;
        PerfStats::instance().blit_encoders += blit_encoders;
        PerfStats::instance().command_buffers++;
        command_buffer->addCompletedHandler([](MTL::CommandBuffer* cb) {
            const auto dt = cb->GPUEndTime() - cb->GPUStartTime();
            if (dt > 0.0)
                PerfStats::instance().gpu_ns +=
                    static_cast<u64>(dt * 1'000'000'000.0);
        });
    }
    if (!completion_callbacks.empty()) {
        command_buffer->addCompletedHandler(
            [callbacks = std::move(completion_callbacks)](MTL::CommandBuffer*) {
                for (const auto& callback : callbacks)
                    callback();
            });
        completion_callbacks.clear();
    }
    // GPU faults and timeouts are otherwise silent. This runs on a Metal
    // dispatch thread the logger does not know about, so print directly.
    command_buffer->addCompletedHandler([](MTL::CommandBuffer* cb) {
        if (cb->status() != MTL::CommandBufferStatusError)
            return;
        const auto error = cb->error();
        std::fprintf(stderr, "[Metal] Command buffer failed (code %ld): %s\n",
                     error ? static_cast<long>(error->code()) : -1L,
                     error ? error->localizedDescription()->utf8String()
                           : "unknown error");
    });
    command_buffer->commit();
    // Command buffers execute in commit order on the same queue, so there is no
    // need to wait for the GPU. HYDRA_GPU_SYNC=1 restores the old behaviour
    // for debugging.
    static const bool gpu_sync = std::getenv("HYDRA_GPU_SYNC") != nullptr;
    if (gpu_sync)
        command_buffer->waitUntilCompleted();
    command_buffer->release();
}

void CommandBuffer::retireUploadBufferHot0(std::shared_ptr<UploadBufferPool> pool,
                                       MTL::Buffer* buffer) {
    ASSERT(!upload_pool || upload_pool == pool, MetalRenderer,
           "An upload command buffer cannot mix buffer pools");
    upload_pool = std::move(pool);
    retired_upload_buffers.push_back(buffer);
}

void CommandBuffer::setVisibilityResultBuffer(MTL::Buffer* buffer) {
    if (buffer == visibility_buffer)
        return;
    endEncoding();
    if (visibility_buffer != nullptr)
        visibility_buffer->release();
    visibility_buffer = buffer != nullptr ? buffer->retain() : nullptr;
}

MTL::RenderCommandEncoder* CommandBuffer::getRenderCommandEncoderHot1(
    MTL::RenderPassDescriptor* render_pass_descriptor) {
    if (render_pass_descriptor == encoder_state.render_pass)
        return getRenderCommandEncoderUnchecked();

    // Creating the encoder already resets all cached render state.
    return createRenderCommandEncoder(render_pass_descriptor);
}

MTL::RenderCommandEncoder* CommandBuffer::createRenderCommandEncoderHot2(
    MTL::RenderPassDescriptor* render_pass_descriptor) {
    endEncoding();

    TMP_AUTORELEASE_POOL_BEGIN();
    auto* descriptor = render_pass_descriptor;
    if (visibility_buffer != nullptr) {
        descriptor = render_pass_descriptor->copy();
        descriptor->setVisibilityResultBuffer(visibility_buffer);
    }
    command_encoder = command_buffer->renderCommandEncoder(descriptor)->retain();
    if (descriptor != render_pass_descriptor)
        descriptor->release();
    TMP_AUTORELEASE_POOL_END();

    encoder_type = EncoderType::Render;
    ++render_encoders;
    encoder_state.render_pass = render_pass_descriptor;
    encoder_state.render = {};

    return getRenderCommandEncoderUnchecked();
}

MTL::BlitCommandEncoder* CommandBuffer::getBlitCommandEncoderHot3() {
    if (encoder_type == EncoderType::Blit)
        return getBlitCommandEncoderUnchecked();

    endEncoding();

    TMP_AUTORELEASE_POOL_BEGIN();
    command_encoder = command_buffer->blitCommandEncoder()->retain();
    TMP_AUTORELEASE_POOL_END();

    encoder_type = EncoderType::Blit;
    ++blit_encoders;

    return getBlitCommandEncoderUnchecked();
}

MTL::ComputeCommandEncoder* CommandBuffer::getComputeCommandEncoderHot4() {
    if (encoder_type == EncoderType::Compute)
        return static_cast<MTL::ComputeCommandEncoder*>(command_encoder);

    endEncoding();

    TMP_AUTORELEASE_POOL_BEGIN();
    // Serial dispatches see the results of previous ones
    command_encoder = command_buffer->computeCommandEncoder()->retain();
    TMP_AUTORELEASE_POOL_END();

    encoder_type = EncoderType::Compute;

    return static_cast<MTL::ComputeCommandEncoder*>(command_encoder);
}

f64 CommandBuffer::debugCommitAndWaitHot5() {
    const bool profile = PerfStats::isEnabled();
    const auto start = profile ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    endEncoding();
    command_buffer->commit();
    command_buffer->waitUntilCompleted();
    last_submission_succeeded =
        command_buffer->status() == MTL::CommandBufferStatusCompleted;
    if (!last_submission_succeeded) {
        const auto error = command_buffer->error();
        LOG_ERROR(MetalRenderer, "GPU checkpoint failed: {}",
                  error ? error->localizedDescription()->utf8String() : "unknown error");
    }
    for (auto* buffer : retired_upload_buffers) {
        if (last_submission_succeeded)
            upload_pool->recycle(buffer);
        else
            buffer->release();
    }
    retired_upload_buffers.clear();
    const auto ms =
        (command_buffer->GPUEndTime() - command_buffer->GPUStartTime()) *
        1000.0;
    if (profile) {
        const auto now = std::chrono::steady_clock::now();
        static thread_local auto last = start;
        static thread_local u64 checkpoints = 0;
        static thread_local f64 wall_ms = 0.0;
        static thread_local f64 gpu_ms = 0.0;
        ++checkpoints;
        wall_ms += std::chrono::duration<f64, std::milli>(now - start).count();
        gpu_ms += std::max(ms, 0.0);
        PerfStats::instance().command_buffers++;
        if (ms > 0.0)
            PerfStats::instance().gpu_ns += static_cast<u64>(ms * 1'000'000.0);
        const auto elapsed = std::chrono::duration<f64>(now - last).count();
        if (elapsed >= 2.0) {
            LOG_INFO(MetalRenderer, "CHECKPOINT: {} in {:.2f}s, {:.2f} ms CPU blocking, "
                                    "{:.2f} ms GPU execution, {:.3f} ms average blocking",
                     checkpoints, elapsed, wall_ms, gpu_ms,
                     wall_ms / static_cast<f64>(checkpoints));
            checkpoints = 0;
            wall_ms = gpu_ms = 0.0;
            last = now;
        }
    }
    command_buffer->release();
    TMP_AUTORELEASE_POOL_BEGIN();
    command_buffer = command_queue->commandBuffer()->retain();
    TMP_AUTORELEASE_POOL_END();
    return ms;
}

void CommandBuffer::endEncodingHot6() {
    if (encoder_type == EncoderType::None)
        return;

    command_encoder->endEncoding();
    command_encoder->release();
    command_encoder = nullptr;
    encoder_type = EncoderType::None;

    // A new render pass resets visibility storage, so it needs a fresh page.
    if (visibility_buffer != nullptr) {
        visibility_buffer->release();
        visibility_buffer = nullptr;
    }
    // Reset the render pass
    encoder_state.render_pass = nullptr;
}

void CommandBuffer::setRenderPipelineStateHot7(MTL::RenderPipelineState* pipeline) {
    auto& bound_pipeline = encoder_state.render.pipeline;
    if (pipeline == bound_pipeline)
        return;

    getRenderCommandEncoderUnchecked()->setRenderPipelineState(pipeline);
    bound_pipeline = pipeline;
}

void CommandBuffer::setDepthStencilStateHot8(
    MTL::DepthStencilState* depth_stencil_state) {
    auto& bound_depth_stencil_state = encoder_state.render.depth_stencil_state;
    if (depth_stencil_state == bound_depth_stencil_state)
        return;

    getRenderCommandEncoderUnchecked()->setDepthStencilState(
        depth_stencil_state);
    bound_depth_stencil_state = depth_stencil_state;
}

void CommandBuffer::setCullModeHot9(MTL::CullMode cull_mode) {
    auto& bound_cull_mode = encoder_state.render.cull_mode;
    if (cull_mode == bound_cull_mode)
        return;

    getRenderCommandEncoderUnchecked()->setCullMode(cull_mode);
    bound_cull_mode = cull_mode;
}

void CommandBuffer::setFrontFaceWindingHot10(MTL::Winding front_face_winding) {
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

void CommandBuffer::setDepthBiasHot11(float constant, float slope, float clamp) {
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

void CommandBuffer::setBlendColorHot12(float r, float g, float b, float a) {
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

void CommandBuffer::setStencilReferencesHot13(u32 front, u32 back) {
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

void CommandBuffer::setViewportsHot14(std::span<const MTL::Viewport> values) {
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

void CommandBuffer::setScissorsHot15(std::span<const MTL::ScissorRect> values) {
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

void CommandBuffer::setBufferHot16(MTL::Buffer* buffer, u64 offset,
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

void CommandBuffer::setTextureHot17(MTL::Texture* texture, ShaderType shader_type,
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

void CommandBuffer::setSamplerHot18(MTL::SamplerState* sampler,
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
