#include "core/hw/tegra_x1/gpu/renderer/metal/render_pass.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

namespace {

// DEBUG: HYDRA_EXPERIMENT_DONTCARE=color_load,depth_load,depth_store drops
// those memory transfers to measure the bandwidth they cost. The picture is
// wrong, it is not meant for playing.
struct DontCareExperiment {
    bool color_load{false};
    bool depth_load{false};
    bool depth_store{false};
};

const DontCareExperiment& getDontCareExperiment() {
    static const DontCareExperiment experiment = [] {
        DontCareExperiment result;
        if (const char* env = std::getenv("HYDRA_EXPERIMENT_DONTCARE")) {
            const std::string_view list(env);
            result.color_load = list.contains("color_load");
            result.depth_load = list.contains("depth_load");
            result.depth_store = list.contains("depth_store");
        }
        return result;
    }();
    return experiment;
}

} // namespace

RenderPass::RenderPass(const RenderPassDescriptor& descriptor)
    : RenderPassBase(descriptor) {
    render_pass_descriptor = MTL::RenderPassDescriptor::alloc()->init();

    // Shaders may write render_target_array_index, so the array length must
    // be set. Use the smallest layer count among all attachments.
    u32 array_length = invalid<u32>();
    const auto add_attachment = [&](MTL::Texture* texture) {
        array_length =
            std::min(array_length, getRenderTargetArrayLength(texture));
        width = std::min(width, static_cast<u32>(texture->width()));
        height = std::min(height, static_cast<u32>(texture->height()));
    };

    const auto& experiment = getDontCareExperiment();

    // Color targets
    for (u32 i = 0; i < COLOR_TARGET_COUNT; i++) {
        const auto& color_target = descriptor.color_targets[i];
        if (color_target.texture == nullptr)
            continue;

        auto color_attachment =
            render_pass_descriptor->colorAttachments()->object(i);
        color_attachment->setTexture(
            static_cast<TextureView*>(descriptor.color_targets[i].texture)
                ->getTexture());
        add_attachment(color_attachment->texture());
        if (color_target.load_action_clear) {
            color_attachment->setLoadAction(MTL::LoadActionClear);
            color_attachment->setClearColor(MTL::ClearColor(
                static_cast<f64>(color_target.clear_data.color[0]),
                static_cast<f64>(color_target.clear_data.color[1]),
                static_cast<f64>(color_target.clear_data.color[2]),
                static_cast<f64>(color_target.clear_data.color[3])));
        } else {
            color_attachment->setLoadAction(experiment.color_load
                                                ? MTL::LoadActionDontCare
                                                : MTL::LoadActionLoad);
        }
        color_attachment->setStoreAction(MTL::StoreActionStore);
    }

    // Depth stencil target
    if (descriptor.depth_stencil_target.texture != nullptr) {
        const auto& depth_stencil_target = descriptor.depth_stencil_target;
        const auto& format_info = toMtlPixelFormatInfo(
            depth_stencil_target.texture->getDescriptor().format);

        // Depth
        if (format_info.has_depth) {
            auto depth_attachment = render_pass_descriptor->depthAttachment();
            depth_attachment->setTexture(
                static_cast<TextureView*>(depth_stencil_target.texture)
                    ->getTexture());
            add_attachment(depth_attachment->texture());
            if (depth_stencil_target.load_action_clear &&
                depth_stencil_target.clear_data.clear_depth) {
                depth_attachment->setLoadAction(MTL::LoadActionClear);
                depth_attachment->setClearDepth(
                    static_cast<f64>(depth_stencil_target.clear_data.depth));
            } else {
                depth_attachment->setLoadAction(experiment.depth_load
                                                    ? MTL::LoadActionDontCare
                                                    : MTL::LoadActionLoad);
            }
            depth_attachment->setStoreAction(experiment.depth_store
                                                 ? MTL::StoreActionDontCare
                                                 : MTL::StoreActionStore);
        }

        // Stencil
        if (format_info.has_stencil) {
            auto stencil_attachment =
                render_pass_descriptor->stencilAttachment();
            stencil_attachment->setTexture(
                static_cast<TextureView*>(depth_stencil_target.texture)
                    ->getTexture());
            add_attachment(stencil_attachment->texture());
            if (depth_stencil_target.load_action_clear &&
                depth_stencil_target.clear_data.clear_stencil) {
                stencil_attachment->setLoadAction(MTL::LoadActionClear);
                stencil_attachment->setClearStencil(
                    depth_stencil_target.clear_data.stencil);
            } else {
                stencil_attachment->setLoadAction(experiment.depth_load
                                                      ? MTL::LoadActionDontCare
                                                      : MTL::LoadActionLoad);
            }
            stencil_attachment->setStoreAction(experiment.depth_store
                                                   ? MTL::StoreActionDontCare
                                                   : MTL::StoreActionStore);
        }
    }

    if (array_length != invalid<u32>())
        render_pass_descriptor->setRenderTargetArrayLength(array_length);
}

RenderPass::~RenderPass() { render_pass_descriptor->release(); }

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
