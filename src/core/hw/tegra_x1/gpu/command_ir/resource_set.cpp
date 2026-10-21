#include "core/hw/tegra_x1/gpu/command_ir/resource_set.hpp"

#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/pfifo.hpp"
#include "core/hw/tegra_x1/gpu/renderer/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::command_ir {

using resource::ResourceKind;

CommandOrigin currentCommandOrigin() {
    const auto* state = tls_crnt_decode_state;
    if (!state || state->active_sequence == 0)
        return {};
    return {state->channel_id, state->active_sequence,
            state->dispatch_source_address};
}

void ResourceSet::clear() {
    origin = {};
    refs.clear();
    owners.clear();
    address_space = nullptr;
    address_space_handle = {};
    mapping_generation = 0;
    mapping_dependencies.clear();
    contents = {};
    sealed = false;
}

void ResourceSet::capture(renderer::IRenderer& renderer, GMmu* gmmu) {
    clear();
    origin = currentCommandOrigin();
    address_space = gmmu;
    if (gmmu) {
        address_space_handle = renderer.getResourceRegistry().track(
            gmmu->getResourceLifetime(), ResourceKind::AddressSpace, gmmu);
        mapping_generation = gmmu->getMappingGeneration();
    }
}

void ResourceSet::trackObject(renderer::IRenderer& renderer, const void* object,
                              resource::ResourceLifetime* lifetime,
                              ResourceKind kind, std::shared_ptr<void> owner) {
    if (!object)
        return;
    refs.push_back({object, kind,
                    renderer.getResourceRegistry().track(*lifetime, kind, object)});
    if (owner && std::find(owners.begin(), owners.end(), owner) == owners.end())
        owners.push_back(std::move(owner));
}

void ResourceSet::trackView(renderer::IRenderer& renderer,
                            renderer::ITextureView* view) {
    track(renderer, view, ResourceKind::TextureView);
    if (view)
        track(renderer, view->getBase(), ResourceKind::Texture);
}

bool ResourceSet::valid(renderer::IRenderer& renderer) const {
    if (!sealed)
        return false;
    auto& registry = renderer.getResourceRegistry();
    for (const auto& ref : refs)
        if (!registry.matches(ref.handle, ref.kind, ref.object))
            return false;
    if (!address_space)
        return address_space_handle.getId() == 0;
    return registry.matches(address_space_handle, ResourceKind::AddressSpace,
                            address_space) &&
           !address_space->gpuWorkCancelled() &&
           (address_space->getMappingGeneration() == mapping_generation ||
            resource::validateMappings(*address_space, mapping_dependencies));
}

bool ResourceSet::contentsCurrent(renderer::IRenderer& renderer) const {
    return sealed && contents.readsCurrent(renderer.getContentVersions());
}

} // namespace hydra::hw::tegra_x1::gpu::command_ir
