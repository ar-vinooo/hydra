#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/capture/guest_events.hpp"

#include <cstdio>
#include <cstdlib>

#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/upload_buffer_pool.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

CommandBuffer::CommandBuffer(MTL::CommandQueue* command_queue_,
                             std::shared_ptr<CompletionQueue> completions_)
    : command_queue{command_queue_}, completions{std::move(completions_)} {
    TMP_AUTORELEASE_POOL_BEGIN();
    command_buffer = command_queue->commandBuffer()->retain();
    TMP_AUTORELEASE_POOL_END();
}

CommandBuffer::~CommandBuffer() {
    endEncoding();
    // Register content completion before the callbacks that publish guest
    // submission fences. Checkpoints attach it in commitSubmission instead.
    attachGpuCompletionCallbacks();
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
        if (completions &&
            (queued_completion_work || completions->hasPendingOrdered())) {
            // Reports queued through this buffer reach guest memory first, and
            // the callbacks of earlier submissions (their fences) come before
            // these: a fence only counts, so a later one would satisfy the
            // wait for an earlier one
            completions->push(
                shareCompletionToken(),
                [callbacks = std::move(completion_callbacks)](bool succeeded) {
                    for (const auto& callback : callbacks)
                        callback(succeeded);
                },
                {}, true);
        } else {
            command_buffer->addCompletedHandler(
                [callbacks = std::move(completion_callbacks)](
                    MTL::CommandBuffer* cb) {
                    const bool succeeded =
                        cb->status() == MTL::CommandBufferStatusCompleted;
                    for (const auto& callback : callbacks)
                        callback(succeeded);
                });
        }
        completion_callbacks.clear();
    }
    // GPU faults and timeouts are otherwise silent. This runs on a Metal
    // dispatch thread the logger does not know about, so print directly.
    command_buffer->addCompletedHandler([](MTL::CommandBuffer* cb) {
        completed_buffers.fetch_add(1, std::memory_order_acq_rel);
        if (cb->status() != MTL::CommandBufferStatusError)
            return;
        const auto error = cb->error();
        std::fprintf(stderr, "[Metal] Command buffer failed (code %ld): %s\n",
                     error ? static_cast<long>(error->code()) : -1L,
                     error ? error->localizedDescription()->utf8String()
                           : "unknown error");
    });
    submitted_buffers.fetch_add(1, std::memory_order_acq_rel);
    commitSubmission();
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

const std::shared_ptr<CompletionToken>& CommandBuffer::shareCompletionToken() {
    if (!completion_token) {
        completion_token = std::make_shared<CompletionToken>();
        // The work depending on the token needs the buffer to be committed,
        // even if nothing else was encoded
        has_commands = true;
        command_buffer->addCompletedHandler(
            [token = completion_token,
             queue = completions](MTL::CommandBuffer* cb) {
                token->complete(cb->status() ==
                                MTL::CommandBufferStatusCompleted);
                if (queue)
                    queue->notify();
            });
    }
    return completion_token;
}

CompletionToken* CommandBuffer::getCompletionToken() {
    return shareCompletionToken().get();
}

void CommandBuffer::setVisibilityResultBuffer(MTL::Buffer* buffer) {
    if (buffer == visibility_buffer)
        return;
    endEncoding();
    if (visibility_buffer != nullptr)
        visibility_buffer->release();
    visibility_buffer = buffer != nullptr ? buffer->retain() : nullptr;
}

namespace {

// Whether a pass on `next` only continues the open pass on `current`: the
// same attachments, all of them loaded and stored the same way. Ending the
// open pass and starting a new one would store and load every attachment for
// the same result.
bool continuesRenderPass(MTL::RenderPassDescriptor* current,
                         MTL::RenderPassDescriptor* next) {
    if (current->renderTargetWidth() != next->renderTargetWidth() ||
        current->renderTargetHeight() != next->renderTargetHeight() ||
        current->renderTargetArrayLength() != next->renderTargetArrayLength() ||
        current->defaultRasterSampleCount() != next->defaultRasterSampleCount())
        return false;
    bool any = false;
    const auto same = [&](MTL::RenderPassAttachmentDescriptor* a,
                          MTL::RenderPassAttachmentDescriptor* b) {
        if (a->texture() != b->texture())
            return false;
        if (b->texture() == nullptr)
            return true;
        any = true;
        return a->level() == b->level() && a->slice() == b->slice() &&
               a->depthPlane() == b->depthPlane() &&
               a->resolveTexture() == b->resolveTexture() &&
               a->storeAction() == b->storeAction() &&
               b->loadAction() == MTL::LoadActionLoad;
    };
    for (u32 i = 0; i < COLOR_TARGET_COUNT; i++)
        if (!same(current->colorAttachments()->object(i),
                  next->colorAttachments()->object(i)))
            return false;
    return same(current->depthAttachment(), next->depthAttachment()) &&
           same(current->stencilAttachment(), next->stencilAttachment()) && any;
}

} // namespace

MTL::RenderCommandEncoder* CommandBuffer::getRenderCommandEncoderHot1(
    MTL::RenderPassDescriptor* render_pass_descriptor) {
    if (render_pass_descriptor == encoder_state.render_pass)
        return getRenderCommandEncoderUnchecked();

    // DEBUG: HYDRA_NO_PASS_MERGE=1 always starts a new pass, for comparisons
    static const bool merge = std::getenv("HYDRA_NO_PASS_MERGE") == nullptr;
    if (merge && encoder_type == EncoderType::Render &&
        encoder_state.render_pass != nullptr &&
        continuesRenderPass(encoder_state.render_pass, render_pass_descriptor)) {
        encoder_state.render_pass = render_pass_descriptor;
        if (PerfStats::isEnabled())
            PerfStats::instance().render_pass_merges++;
        return getRenderCommandEncoderUnchecked();
    }

    // Creating the encoder already resets all cached render state.
    return createRenderCommandEncoder(render_pass_descriptor);
}

MTL::RenderCommandEncoder* CommandBuffer::createRenderCommandEncoderHot2(
    MTL::RenderPassDescriptor* render_pass_descriptor) {
    endEncoding();

    TMP_AUTORELEASE_POOL_BEGIN();
    auto* descriptor = render_pass_descriptor;
    // Sampling GPU timestamps changes the descriptor
    const bool sample = GpuPassProfiler::get(command_buffer->device()) != nullptr;
    if (visibility_buffer != nullptr || sample) {
        descriptor = render_pass_descriptor->copy();
        if (visibility_buffer != nullptr)
            descriptor->setVisibilityResultBuffer(visibility_buffer);
    }
    if (PerfStats::isEnabled())
        profileRenderPass(descriptor);
    command_encoder = command_buffer->renderCommandEncoder(descriptor)->retain();
    if (descriptor != render_pass_descriptor)
        descriptor->release();
    TMP_AUTORELEASE_POOL_END();

    encoder_type = EncoderType::Render;
    has_commands = true;
    ++render_encoders;
    encoder_state.render_pass = render_pass_descriptor;
    encoder_state.render = {};

    return getRenderCommandEncoderUnchecked();
}

u32 CommandBuffer::reservePassSamples(GpuPassProfiler::Kind kind,
                                      std::string label) {
    auto* profiler = GpuPassProfiler::get(command_buffer->device());
    if (!profiler)
        return invalid<u32>();
    const auto sample = profiler->reserve();
    if (sample != invalid<u32>())
        profiled_passes.push_back({kind, sample, std::move(label)});
    return sample;
}

void CommandBuffer::profileRenderPass(MTL::RenderPassDescriptor* descriptor) {
    // Approximate size of the attached level and slices: the whole
    // allocation without its other mip levels
    // A texture view may report the allocation of its whole parent, so the
    // size is computed from the attached level
    const auto attachment_bytes = [](MTL::RenderPassAttachmentDescriptor* attachment,
                                     MTL::Texture* texture) -> u64 {
        u64 bytes_per_pixel;
        switch (texture->pixelFormat()) {
        case MTL::PixelFormatR8Unorm:
        case MTL::PixelFormatR8Snorm:
        case MTL::PixelFormatR8Uint:
        case MTL::PixelFormatR8Sint:
        case MTL::PixelFormatStencil8:
            bytes_per_pixel = 1;
            break;
        case MTL::PixelFormatRG8Unorm:
        case MTL::PixelFormatRG8Snorm:
        case MTL::PixelFormatRG8Uint:
        case MTL::PixelFormatRG8Sint:
        case MTL::PixelFormatR16Unorm:
        case MTL::PixelFormatR16Float:
        case MTL::PixelFormatR16Uint:
        case MTL::PixelFormatR16Sint:
        case MTL::PixelFormatB5G6R5Unorm:
        case MTL::PixelFormatA1BGR5Unorm:
        case MTL::PixelFormatBGR5A1Unorm:
        case MTL::PixelFormatDepth16Unorm:
            bytes_per_pixel = 2;
            break;
        case MTL::PixelFormatRG16Unorm:
        case MTL::PixelFormatRG16Float:
        case MTL::PixelFormatRGBA16Unorm:
        case MTL::PixelFormatRGBA16Snorm:
        case MTL::PixelFormatRGBA16Float:
        case MTL::PixelFormatRGBA16Uint:
        case MTL::PixelFormatRGBA16Sint:
        case MTL::PixelFormatRG32Float:
        case MTL::PixelFormatRG32Uint:
        case MTL::PixelFormatRG32Sint:
        case MTL::PixelFormatDepth32Float_Stencil8:
            bytes_per_pixel = texture->pixelFormat() == MTL::PixelFormatRG16Unorm ||
                                      texture->pixelFormat() == MTL::PixelFormatRG16Float
                                  ? 4
                                  : 8;
            break;
        case MTL::PixelFormatRGBA32Float:
        case MTL::PixelFormatRGBA32Uint:
        case MTL::PixelFormatRGBA32Sint:
            bytes_per_pixel = 16;
            break;
        default: // 32-bit colour and depth formats
            bytes_per_pixel = 4;
            break;
        }
        const auto level = attachment->level();
        const u64 width = std::max<u64>(texture->width() >> level, 1);
        const u64 height = std::max<u64>(texture->height() >> level, 1);
        return width * height * bytes_per_pixel *
               std::max<u64>(texture->sampleCount(), 1);
    };
    std::array<MTL::Texture*, COLOR_TARGET_COUNT + 2> attachments{};
    u64 load_bytes = 0, store_bytes = 0;
    bool all_loaded = true;
    std::string loads, stores, formats;
    u32 width = 0, height = 0;
    const auto visit = [&](MTL::RenderPassAttachmentDescriptor* attachment,
                           usize slot, std::string_view name) {
        auto* texture = attachment->texture();
        if (!texture)
            return;
        attachments[slot] = texture;
        if (width == 0) {
            width = static_cast<u32>(texture->width());
            height = static_cast<u32>(texture->height());
        }
        const auto bytes = attachment_bytes(attachment, texture);
        formats += fmt::format(" {}:{}", name,
                               static_cast<u64>(texture->pixelFormat()));
        if (attachment->loadAction() == MTL::LoadActionLoad) {
            load_bytes += bytes;
            loads += fmt::format(" {}", name);
        } else {
            all_loaded = false;
        }
        if (attachment->storeAction() == MTL::StoreActionStore ||
            attachment->storeAction() ==
                MTL::StoreActionStoreAndMultisampleResolve) {
            store_bytes += bytes;
            stores += fmt::format(" {}", name);
        }
    };
    for (u32 i = 0; i < COLOR_TARGET_COUNT; i++)
        visit(descriptor->colorAttachments()->object(i), i,
              fmt::format("c{}", i));
    visit(descriptor->depthAttachment(), COLOR_TARGET_COUNT, "d");
    visit(descriptor->stencilAttachment(), COLOR_TARGET_COUNT + 1, "s");

    auto& stats = PerfStats::instance();
    stats.render_passes++;
    stats.render_pass_load_bytes += load_bytes;
    stats.render_pass_store_bytes += store_bytes;
    const bool continuation = all_loaded && attachments == previous_attachments &&
                              width != 0;
    if (continuation) {
        stats.render_pass_continuations++;
        stats.render_pass_continuation_causes[render_pass_gap]++;
    }
    previous_attachments = attachments;
    render_pass_gap = 0;

    const auto sample = reservePassSamples(
        GpuPassProfiler::Kind::Render,
        fmt::format("render {}x{}{} | load{} | store{}", width, height, formats,
                    loads.empty() ? " -" : loads,
                    stores.empty() ? " -" : stores));
    if (sample == invalid<u32>())
        return;
    auto* attachment = descriptor->sampleBufferAttachments()->object(0);
    attachment->setSampleBuffer(
        GpuPassProfiler::get(command_buffer->device())->getSampleBuffer());
    attachment->setStartOfVertexSampleIndex(sample);
    attachment->setEndOfVertexSampleIndex(MTL::CounterDontSample);
    attachment->setStartOfFragmentSampleIndex(MTL::CounterDontSample);
    attachment->setEndOfFragmentSampleIndex(sample + 1);
}

MTL::BlitCommandEncoder* CommandBuffer::getBlitCommandEncoderHot3() {
    if (encoder_type == EncoderType::Blit)
        return getBlitCommandEncoderUnchecked();

    endEncoding();

    TMP_AUTORELEASE_POOL_BEGIN();
    if (const auto sample = reservePassSamples(GpuPassProfiler::Kind::Blit, "blit");
        sample != invalid<u32>()) {
        auto* pass = MTL::BlitPassDescriptor::blitPassDescriptor();
        auto* attachment = pass->sampleBufferAttachments()->object(0);
        attachment->setSampleBuffer(
            GpuPassProfiler::get(command_buffer->device())->getSampleBuffer());
        attachment->setStartOfEncoderSampleIndex(sample);
        attachment->setEndOfEncoderSampleIndex(sample + 1);
        command_encoder = command_buffer->blitCommandEncoder(pass)->retain();
    } else {
        command_encoder = command_buffer->blitCommandEncoder()->retain();
    }
    TMP_AUTORELEASE_POOL_END();

    encoder_type = EncoderType::Blit;
    render_pass_gap = 1;
    has_commands = true;
    ++blit_encoders;

    return getBlitCommandEncoderUnchecked();
}

MTL::ComputeCommandEncoder* CommandBuffer::getComputeCommandEncoderHot4() {
    if (encoder_type == EncoderType::Compute)
        return static_cast<MTL::ComputeCommandEncoder*>(command_encoder);

    endEncoding();

    TMP_AUTORELEASE_POOL_BEGIN();
    // Serial dispatches see the results of previous ones
    if (const auto sample =
            reservePassSamples(GpuPassProfiler::Kind::Compute, "compute");
        sample != invalid<u32>()) {
        auto* pass = MTL::ComputePassDescriptor::computePassDescriptor();
        pass->setDispatchType(MTL::DispatchTypeSerial);
        auto* attachment = pass->sampleBufferAttachments()->object(0);
        attachment->setSampleBuffer(
            GpuPassProfiler::get(command_buffer->device())->getSampleBuffer());
        attachment->setStartOfEncoderSampleIndex(sample);
        attachment->setEndOfEncoderSampleIndex(sample + 1);
        command_encoder = command_buffer->computeCommandEncoder(pass)->retain();
    } else {
        command_encoder = command_buffer->computeCommandEncoder()->retain();
    }
    TMP_AUTORELEASE_POOL_END();

    encoder_type = EncoderType::Compute;
    render_pass_gap = 2;
    has_commands = true;

    return static_cast<MTL::ComputeCommandEncoder*>(command_encoder);
}

f64 CommandBuffer::debugCommitAndWaitHot5() {
    ScopedGpuWork work(GpuWork::Wait);
    const bool profile = PerfStats::isEnabled();
    const auto start = profile ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    endEncoding();
    // Nothing encoded since the last checkpoint and no earlier submission in
    // flight: there is nothing to commit or wait for
    if (!has_commands && retired_upload_buffers.empty() &&
        submitted_buffers.load(std::memory_order_acquire) ==
            completed_buffers.load(std::memory_order_acquire)) {
        last_submission_succeeded = true;
        runGpuCompletionCallbacks(true);
        guest_write_synced.store(guest_write_epoch.load(std::memory_order_acquire),
                                 std::memory_order_release);
        if (profile)
            PerfStats::instance().checkpoint_skips++;
        capture::GuestEvents::instance().event(
            "checkpoint", capture::currentGuestEventOrigin(), 0, 0, 0, 1);
        return 0.0;
    }
    const u64 write_epoch = guest_write_epoch.load(std::memory_order_acquire);
    submitted_buffers.fetch_add(1, std::memory_order_acq_rel);
    commitSubmission();
    command_buffer->waitUntilCompleted();
    completed_buffers.fetch_add(1, std::memory_order_acq_rel);
    checkpoint_total.fetch_add(1, std::memory_order_acq_rel);
    has_commands = false;
    last_submission_succeeded =
        command_buffer->status() == MTL::CommandBufferStatusCompleted;
    if (!last_submission_succeeded) {
        const auto error = command_buffer->error();
        LOG_ERROR(MetalRenderer, "GPU checkpoint failed: {}",
                  error ? error->localizedDescription()->utf8String() : "unknown error");
    }
    // Everything encoded before the commit finished (queue order), so its
    // writes to guest memory are visible
    for (u64 synced = guest_write_synced.load(std::memory_order_acquire);
         synced < write_epoch &&
         !guest_write_synced.compare_exchange_weak(
             synced, write_epoch, std::memory_order_acq_rel);) {
    }
    if (completion_token) {
        // Metal calls the handler registered for the token later on; the work
        // waiting for it does not have to wait for that
        completion_token->complete(last_submission_succeeded);
        completion_token.reset();
        if (completions)
            completions->notify();
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
        const auto blocked = std::chrono::duration<f64, std::milli>(now - start);
        wall_ms += blocked.count();
        gpu_ms += std::max(ms, 0.0);
        auto& stats = PerfStats::instance();
        const auto reason = static_cast<usize>(checkpoint_reason);
        stats.checkpoint_count[reason]++;
        stats.checkpoint_ns[reason] += static_cast<u64>(blocked.count() * 1e6);
        stats.command_buffers++;
        if (ms > 0.0)
            stats.gpu_ns += static_cast<u64>(ms * 1'000'000.0);
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
    capture::GuestEvents::instance().event(
        "checkpoint", capture::currentGuestEventOrigin(), 0, 0, 0,
        last_submission_succeeded ? 1 : 0);
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
