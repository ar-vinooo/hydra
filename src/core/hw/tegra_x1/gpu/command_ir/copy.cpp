#include "core/hw/tegra_x1/gpu/command_ir/copy.hpp"

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

bool sealCopy(renderer::IRenderer& renderer, CopyCommand& command) {
    auto& set = command.resources;
    set.sealed = false;
    set.refs.clear();
    set.owners.clear();
    set.contents = {};
    if (!command.src || !command.dst)
        return false;
    set.trackView(renderer, command.src);
    set.trackView(renderer, command.dst);
    set.contents.addRead(renderer.getContentVersions(), textureRange(command.src));
    set.contents.addWrite(textureRange(command.dst));
    set.sealed = true;
    if (!set.valid(renderer)) {
        set.sealed = false;
        return false;
    }
    return true;
}

bool executeCopy(renderer::IRenderer& renderer,
                 renderer::ICommandBuffer* command_buffer,
                 const CopyCommand& command) {
    if (!command_buffer || !command.resources.valid(renderer))
        return false;
    for (const auto& owner : command.resources.owners)
        command_buffer->retainResource(owner);
    renderer.blitTexture(command_buffer, command.src, command.src_origin,
                         command.src_size, command.src_level, command.src_layer,
                         command.dst, command.dst_origin, command.dst_size,
                         command.dst_level, command.dst_layer,
                         command.level_count, command.layer_count,
                         command.options);
    for (const auto& range : command.resources.contents.writes)
        renderer.getContentVersions().write(range);
    return true;
}

} // namespace hydra::hw::tegra_x1::gpu::command_ir
