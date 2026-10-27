#include "core/horizon/display/layer.hpp"

#include "common/config.hpp"
#include "core/horizon/kernel/process.hpp"
#include "core/horizon/os.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/surface_compositor.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"
#include "core/system.hpp"

namespace hydra::horizon::display {

bool Layer::acquirePresentTexture(
    hw::tegra_x1::gpu::renderer::ICommandBuffer* command_buffer) {
    // Get the buffer to present
    auto& binder = system.getOs().getDisplayDriver().getBinder(binder_handle);

    BqBufferInput input;
    u64 gpu_fence;
    i32 slot = binder.consumeBuffer(input, gpu_fence);
    if (slot == -1)
        return false;

    // Above the console's 60, only the newest frame is shown and the older
    // ones go back to the title at once, so it renders on instead of waiting
    // for the host display
    if (CONFIG_INSTANCE.getFrameRateLimit() != 60) {
        BqBufferInput newer_input;
        u64 newer_gpu_fence;
        for (i32 newer_slot;
             (newer_slot = binder.consumeBuffer(newer_input,
                                                newer_gpu_fence)) != -1;) {
            binder.releaseCallback(slot)();
            slot = newer_slot;
            input = newer_input;
            gpu_fence = newer_gpu_fence;
        }
    }

    // Wait for the GPU to finish rendering the buffer
    system.getGpu().getPfifo().waitForCompletion(gpu_fence);
    const auto release = binder.releaseCallback(slot);
    const auto buffer = binder.getBuffer(slot);
    if (!buffer) {
        release();
        return false;
    }
    command_buffer->onCompleted(release);

    // Texture
    auto* texture = system.getGpu().getTexture(
        command_buffer, process->getMmu(), buffer->nv_buffer);
    if (texture == nullptr) {
        present_texture.reset();
        return false;
    }
    present_texture = texture;

    // Rect
    src_rect = {};
    src_rect.origin.x() = input.rect.left;
    src_rect.origin.y() =
        input.rect.top; // Convert from top left to bottom left origin
    src_rect.size.x() = input.rect.right - input.rect.left;
    src_rect.size.y() = input.rect.bottom - input.rect.top;

    // An empty crop rectangle means "no crop" (the whole buffer), as in the
    // Android BufferQueue this interface comes from
    if (src_rect.size.x() == 0)
        src_rect.size.x() = static_cast<i32>(
            present_texture.value()->getBase()->getDescriptor().width);
    if (src_rect.size.y() == 0)
        src_rect.size.y() = static_cast<i32>(
            present_texture.value()->getBase()->getDescriptor().height);

    if (any(input.transform_flags & horizon::display::TransformFlags::FlipH)) {
        src_rect.origin.x() += src_rect.size.x();
        src_rect.size.x() = -src_rect.size.x();
    }
    if (any(input.transform_flags & horizon::display::TransformFlags::FlipV)) {
        src_rect.origin.y() += src_rect.size.y();
        src_rect.size.y() = -src_rect.size.y();
    }
    if (any(input.transform_flags & horizon::display::TransformFlags::Rot90)) {
        // TODO: how does this work? Is the aspect ratio kept intact?
        ONCE(LOG_NOT_IMPLEMENTED(Other, "Rotating by 90 degrees"));
    }

    return true;
}

void Layer::present(hw::tegra_x1::gpu::renderer::ICommandBuffer* command_buffer,
                    hw::tegra_x1::gpu::renderer::ISurfaceCompositor* compositor,
                    FloatRect2D dst_rect, f32 dst_scale, bool transparent) {
    ZTD_ASSIGN_OR_RETURN(auto present_tex, present_texture);

    // Size
    if (size != LAYER_SIZE_AUTO)
        dst_rect.size = float2(size) * dst_scale;

    // Draw
    compositor->drawTexture(command_buffer, present_tex, src_rect, dst_rect,
                            transparent);
}

AccumulatedTime Layer::getAccumulatedDt() {
    return system.getOs()
        .getDisplayDriver()
        .getBinder(binder_handle)
        .getAccumulatedDt();
}

} // namespace hydra::horizon::display
