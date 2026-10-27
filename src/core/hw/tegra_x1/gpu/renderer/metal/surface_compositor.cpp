#include "core/hw/tegra_x1/gpu/renderer/metal/surface_compositor.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/blit_pipeline_cache.hpp"
#include "common/config.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/display_stats.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

SurfaceCompositor::SurfaceCompositor(Renderer& renderer_,
                                     CA::MetalDrawable* drawable_)
    : renderer{renderer_}, drawable{drawable_},
      target{drawable_->texture()} {
    // Frame generation and sharpening read the composited frame
    auto& presenter = renderer.getFramePresenter();
    if (FramePresenter::isEnabled()) {
        if (auto* canvas = presenter.getCanvas(
                static_cast<u32>(target->width()),
                static_cast<u32>(target->height()), target->pixelFormat()))
            target = canvas;
    } else {
        presenter.reset();
    }

    // Render pass
    render_pass_descriptor = MTL::RenderPassDescriptor::alloc()->init();
    auto color_attachment =
        render_pass_descriptor->colorAttachments()->object(0);
    color_attachment->setTexture(target);
    color_attachment->setLoadAction(MTL::LoadActionClear);
    color_attachment->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 1.0));
    color_attachment->setStoreAction(MTL::StoreActionStore);
    render_pass_descriptor->setRenderTargetArrayLength(1);
}

SurfaceCompositor::~SurfaceCompositor() { render_pass_descriptor->release(); }

bool SurfaceCompositor::reusesPresentedFrame(bool new_frame) const {
    // Frame generation shows the frames it holds until a new one arrives, the
    // canvas is only read for new frames
    return !new_frame && target != drawable->texture() &&
           renderer.getFramePresenter().holdsFrames();
}

RenderEncoder*
SurfaceCompositor::getEncoder(CommandBuffer* command_buffer) {
    auto* encoder = command_buffer->getRenderCommandEncoder(render_pass_descriptor);
    if (!render_pass_started) {
        // Only the first pass clears the drawable
        render_pass_started = true;
        render_pass_descriptor->colorAttachments()->object(0)->setLoadAction(
            MTL::LoadActionLoad);
    }
    return encoder;
}

void SurfaceCompositor::drawTexture(ICommandBuffer* command_buffer,
                                    const ITextureView* texture,
                                    const FloatRect2D src_rect,
                                    const FloatRect2D dst_rect,
                                    bool transparent, f32 opacity) {
    auto command_buffer_impl = static_cast<CommandBuffer*>(command_buffer);
    auto texture_impl = static_cast<const TextureView*>(texture);
    const auto& descriptor = texture->getBase()->getDescriptor();
    const auto filter = CONFIG_INSTANCE.getUpscalingFilter();
    ScopedPerfTimer encode_timer(PerfStats::instance().present_encode_ns,
                                 PerfStats::isEnabled());
    // Frame generation searches motion at the size the game rendered at
    if (target != drawable->texture() && dst_rect.size.x() > 0.0f &&
        dst_rect.size.y() > 0.0f)
        renderer.getFramePresenter().setInputScale(
            std::abs(src_rect.size.x()) / dst_rect.size.x(),
            std::abs(src_rect.size.y()) / dst_rect.size.y());
    if (PerfStats::isEnabled()) {
        auto& stats = PerfStats::instance();
        stats.present_input_width = static_cast<u32>(std::abs(src_rect.size.x()));
        stats.present_input_height = static_cast<u32>(std::abs(src_rect.size.y()));
        stats.present_output_width = static_cast<u32>(dst_rect.size.x());
        stats.present_output_height = static_cast<u32>(dst_rect.size.y());
    }

    // MetalFX scales the whole texture so that the source rectangle becomes
    // the size of the destination. The blit below then samples it 1:1 with the
    // same normalized coordinates (crop and flips included).
    MTL::Texture* source = texture_impl->getTexture();
    if (filter == UpscalingFilter::MetalFx && src_rect.size.x() != 0.0f &&
        src_rect.size.y() != 0.0f) {
        const auto scale_x = dst_rect.size.x() / std::abs(src_rect.size.x());
        const auto scale_y = dst_rect.size.y() / std::abs(src_rect.size.y());
        if (scale_x > 1.0f || scale_y > 1.0f) {
            const auto output_width = static_cast<u32>(std::lround(
                static_cast<f32>(descriptor.width) * std::max(scale_x, 1.0f)));
            const auto output_height = static_cast<u32>(std::lround(
                static_cast<f32>(descriptor.height) * std::max(scale_y, 1.0f)));
            // MetalFX encodes its own pass
            if (auto* upscaled = renderer.getSpatialUpscaler().upscale(
                    command_buffer_impl, source, output_width, output_height))
                source = upscaled;
        }
    }

    auto encoder = getEncoder(command_buffer_impl);

    // Draw
    encoder->setRenderPipelineState(renderer.getBlitPipelineCache().find(
        {.pixel_format = target->pixelFormat(),
         .transparent = transparent}));
    encoder->setViewport(
        MTL::Viewport{.originX = static_cast<f64>(dst_rect.origin.x()),
                      .originY = static_cast<f64>(dst_rect.origin.y()),
                      .width = static_cast<f64>(dst_rect.size.x()),
                      .height = static_cast<f64>(dst_rect.size.y()),
                      .znear = 0.0,
                      .zfar = 1.0});

    u32 zero = 0;
    encoder->setVertexBytes(&zero, sizeof(zero), 0);

    // Src rect
    const auto src_width = descriptor.width;
    const auto src_height = descriptor.height;
    BlitParams params = {
        .src_offset = {src_rect.origin.x() / static_cast<f32>(src_width),
                       src_rect.origin.y() / static_cast<f32>(src_height)},
        .src_scale = {src_rect.size.x() / static_cast<f32>(src_width),
                      src_rect.size.y() / static_cast<f32>(src_height)},
        .opacity = opacity,
    };

    encoder->setFragmentBytes(&params, sizeof(params), 0);
    encoder->setFragmentTexture(source, static_cast<NS::UInteger>(0));
    encoder->setFragmentSamplerState(filter == UpscalingFilter::Nearest
                                         ? renderer.getNearestSampler()
                                         : renderer.getLinearSampler(),
                                     static_cast<NS::UInteger>(0));
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                            static_cast<NS::UInteger>(0),
                            static_cast<NS::UInteger>(3));
}

void SurfaceCompositor::present(ICommandBuffer* command_buffer,
                                bool new_frame) {
    auto command_buffer_impl = static_cast<CommandBuffer*>(command_buffer);
    ScopedPerfTimer encode_timer(PerfStats::instance().present_encode_ns,
                                 PerfStats::isEnabled());
    if (PerfStats::isEnabled()) {
        PerfStats::instance().presents++;
        // Everything this command buffer holds is presentation work
        command_buffer_impl->addCompletedHandler([](const GpuCompletion& done) {
            if (!done.succeeded || done.gpu_end_time <= done.gpu_start_time)
                return;
            auto& stats = PerfStats::instance();
            stats.present_gpu_ns += static_cast<u64>(
                (done.gpu_end_time - done.gpu_start_time) * 1e9);
            stats.present_gpu_count++;
        });
    }

    // The canvas is cleared even when no layer drew, unless the presenter
    // does not read it
    if (!reusesPresentedFrame(new_frame)) {
        getEncoder(command_buffer_impl);
        command_buffer_impl->endEncoding();
    }
    // 0: a game frame shown for the first time, 1: generated, 2: repeated
    int kind = new_frame ? 0 : 2;
    if (target != drawable->texture()) {
        auto& presenter = renderer.getFramePresenter();
        presenter.present(command_buffer_impl, drawable->texture(), new_frame);
        switch (presenter.lastShown()) {
        case FramePresenter::Shown::GameFrame: kind = 0; break;
        case FramePresenter::Shown::Generated: kind = 1; break;
        case FramePresenter::Shown::Repeated: kind = 2; break;
        }
    }
    // What the display really shows, from the drawable's presentation (the
    // frame rate in the window title)
    drawable->addPresentedHandler([kind](MTL::Drawable* shown) {
        auto& stats = DisplayStats::instance();
        if (shown->presentedTime() <= 0.0)
            stats.dropped++;
        else if (kind == 0)
            stats.game_frames++;
        else if (kind == 1)
            stats.generated_frames++;
        else
            stats.repeated_frames++;
    });
    command_buffer_impl->presentDrawable(drawable);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
