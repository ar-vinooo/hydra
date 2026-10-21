#include "core/hw/tegra_x1/gpu/command_ir/clear.hpp"

#include "core/hw/tegra_x1/gpu/renderer/render_pass_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::command_ir {

namespace {
ztd::Range<uptr> textureRange(const renderer::ITextureView* view) {
    if (!view || !view->getBase())
        return {0, 0};
    return view->getBase()->getDescriptor().getRange();
}
} // namespace

bool sealClear(renderer::IRenderer& renderer, ClearCommand& command) {
    auto& set = command.resources;
    // The address space was captured by the producer; keep it
    set.sealed = false;
    set.refs.clear();
    set.owners.clear();
    set.contents = {};
    if (!command.render_pass) {
        set.sealed = true;
        return true;
    }
    set.track(renderer, command.render_pass, resource::ResourceKind::RenderPass);
    const auto& pass = command.render_pass->getDescriptor();
    if (command.color && command.color->target < std::size(pass.color_targets)) {
        auto* view = pass.color_targets[command.color->target].texture;
        set.trackView(renderer, view);
        set.contents.addWrite(textureRange(view));
    }
    if (command.depth || command.stencil) {
        auto* view = pass.depth_stencil_target.texture;
        set.trackView(renderer, view);
        set.contents.addWrite(textureRange(view));
    }
    set.sealed = true;
    if (!set.valid(renderer)) {
        set.sealed = false;
        return false;
    }
    return true;
}

bool executeClear(renderer::IRenderer& renderer,
                  renderer::ICommandBuffer* command_buffer,
                  const ClearCommand& command) {
    if (!command.render_pass)
        return true;
    if (!command_buffer || !command.resources.valid(renderer))
        return false;
    for (const auto& owner : command.resources.owners)
        command_buffer->retainResource(owner);
    renderer.bindRenderPass(command.render_pass);
    renderer.setClearState(command.state);
    if (command.color)
        renderer.clearColor(command_buffer, command.color->target,
                            command.color->layer, command.color->mask,
                            command.color->value);
    if (command.depth)
        renderer.clearDepth(command_buffer, command.depth->layer,
                            command.depth->value);
    if (command.stencil)
        renderer.clearStencil(command_buffer, command.stencil->layer,
                              command.stencil->value);
    renderer.recordGpuWrites(command_buffer, command.resources.contents.writes);
    return true;
}

} // namespace hydra::hw::tegra_x1::gpu::command_ir
