#include "core/hw/tegra_x1/gpu/command_ir/compute.hpp"

#include "core/hw/tegra_x1/gpu/command_ir/compute_limits.hpp"
#include "core/hw/tegra_x1/gpu/renderer/sampler_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::command_ir {
namespace {
bool empty(const renderer::ComputeDispatch& dispatch) {
    return emptyComputeGrid(dispatch.grid_size.width, dispatch.grid_size.height,
                            dispatch.grid_size.depth) ||
           emptyComputeGrid(dispatch.block_size.width,
                            dispatch.block_size.height,
                            dispatch.block_size.depth);
}
std::vector<u64> layout(const renderer::ComputeDispatch& dispatch) {
    std::vector<u64> result;
    const auto pointer = [&](const void* object) {
        result.push_back(reinterpret_cast<uptr>(object));
    };
    const auto buffer = [&](const renderer::BufferView& view) {
        pointer(view.getBase());
        result.insert(result.end(), {view.getOffset(), view.getSize(),
                                     view.getGuestRange().getBegin(),
                                     view.getGuestRange().getEnd()});
    };
    pointer(dispatch.shader);
    result.insert(result.end(),
                  {dispatch.grid_size.width, dispatch.grid_size.height,
                   dispatch.grid_size.depth, dispatch.block_size.width,
                   dispatch.block_size.height, dispatch.block_size.depth});
    for (const auto& view : dispatch.uniform_buffers)
        buffer(view);
    result.push_back(dispatch.storage_buffers.size());
    for (const auto& binding : dispatch.storage_buffers) {
        result.push_back(binding.index);
        result.push_back(binding.written);
        buffer(binding.buffer);
    }
    result.push_back(dispatch.textures.size());
    for (const auto& binding : dispatch.textures) {
        result.push_back(binding.index);
        pointer(binding.texture);
        pointer(binding.sampler);
    }
    result.push_back(dispatch.images.size());
    for (const auto& binding : dispatch.images) {
        result.push_back(binding.index);
        pointer(binding.texture);
    }
    return result;
}
} // namespace

bool sealCompute(renderer::IRenderer& renderer, ComputeCommand& command) {
    auto& resources = command.resources;
    resources.sealed = false;
    // Keep the previous leases until their replacement has been tracked.
    auto previous_owners = std::move(resources.owners);
    resources.refs.clear();
    resources.owners.clear();
    resources.contents = {};
    command.sealed_layout.clear();
    const auto& dispatch = command.dispatch;
    if (empty(dispatch)) {
        command.sealed_layout = layout(dispatch);
        resources.sealed = true;
        return resources.valid(renderer);
    }
    if (!dispatch.shader ||
        dispatch.shader->getDescriptor().type !=
            renderer::ShaderType::Compute ||
        !validComputeGrid(dispatch.grid_size.width, dispatch.grid_size.height,
                          dispatch.grid_size.depth) ||
        !validComputeGrid(dispatch.block_size.width, dispatch.block_size.height,
                          dispatch.block_size.depth))
        return false;
    resources.track(renderer, dispatch.shader, resource::ResourceKind::Shader);
    const auto version = renderer.getContentVersions().current();
    const auto buffer = [&](const renderer::BufferView& view, bool written) {
        if (!view.isValid())
            return true;
        auto* base = view.getBase();
        if (view.getSize() == 0 || view.getOffset() > base->getSize() ||
            view.getSize() > base->getSize() - view.getOffset())
            return false;
        resources.track(renderer, base, resource::ResourceKind::Buffer);
        resources.contents.addRead(version, view.getGuestRange());
        if (written)
            resources.contents.addWrite(view.getGuestRange());
        return true;
    };
    for (const auto& view : dispatch.uniform_buffers)
        if (!buffer(view, false))
            return false;
    for (const auto& binding : dispatch.storage_buffers)
        if (!binding.buffer.isValid() ||
            !buffer(binding.buffer, binding.written))
            return false;
    const auto texture = [&](renderer::ITextureView* view, bool written) {
        resources.trackView(renderer, view);
        if (view && view->getBase()) {
            const auto range = view->getBase()->getDescriptor().getRange();
            resources.contents.addRead(version, range);
            if (written)
                resources.contents.addWrite(range);
        }
    };
    for (const auto& binding : dispatch.textures) {
        texture(binding.texture, false);
        resources.track(renderer, binding.sampler,
                        resource::ResourceKind::Sampler);
    }
    // Without read/write qualifiers in the resolved image binding, both
    // accesses are conservative: image atomics/read-modify-write need both.
    for (const auto& binding : dispatch.images)
        if (!binding.texture || !binding.texture->getBase())
            return false;
        else
            texture(binding.texture, true);
    command.sealed_layout = layout(dispatch);
    resources.sealed = true;
    if (!resources.valid(renderer)) {
        resources.sealed = false;
        return false;
    }
    return true;
}

bool validateCompute(renderer::IRenderer& renderer,
                     const ComputeCommand& command) {
    return command.resources.valid(renderer) &&
           command.sealed_layout == layout(command.dispatch);
}

bool executeCompute(renderer::IRenderer& renderer,
                    renderer::ICommandBuffer* commands,
                    const ComputeCommand& command) {
    if (!commands || !validateCompute(renderer, command))
        return false;
    if (empty(command.dispatch))
        return true;
    const auto& origin = command.resources.origin;
    capture::recordGuestCommand(
        "compute", {origin.channel, origin.sequence, origin.source_address},
        command.resources.contents);
    if (!renderer.dispatchCompute(commands, command.dispatch))
        return false;
    // The descriptor's leases cover encoding. Attach them to the final native
    // submission after any backend preparation checkpoint has consumed the
    // previous buffer's retained resources.
    for (const auto& owner : command.resources.owners)
        commands->retainResource(owner);
    renderer.recordGpuWrites(
        commands, command.resources.contents.writes,
        {origin.channel, origin.sequence, origin.source_address});
    return true;
}

} // namespace hydra::hw::tegra_x1::gpu::command_ir
