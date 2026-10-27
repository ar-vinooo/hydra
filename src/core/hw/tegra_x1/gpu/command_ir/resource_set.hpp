#pragma once

#include "core/hw/tegra_x1/gpu/resource/content_version.hpp"
#include "core/hw/tegra_x1/gpu/resource/mapping_capture.hpp"
#include "core/hw/tegra_x1/gpu/resource/resource_registry.hpp"

#include <memory>
#include <vector>

namespace hydra::hw::tegra_x1::gpu {
class GMmu;
}

namespace hydra::hw::tegra_x1::gpu::renderer {
class IRenderer;
class ITextureView;
} // namespace hydra::hw::tegra_x1::gpu::renderer

namespace hydra::hw::tegra_x1::gpu::command_ir {

struct CommandOrigin {
    u64 channel{0};
    u64 sequence{0};
    u64 source_address{0};
};
CommandOrigin currentCommandOrigin();

struct ResourceReference {
    const void* object;
    resource::ResourceKind kind;
    resource::ResourceHandle handle;
};

// What a non-draw command depends on: resource identities with ownership
// leases, the GPU mapping it was resolved against, and the guest memory it
// reads and writes. Sealed once the command is complete; valid while none of
// them was retired, invalidated or remapped.
struct ResourceSet {
    CommandOrigin origin;
    std::vector<ResourceReference> refs;
    std::vector<std::shared_ptr<void>> owners;
    GMmu* address_space{nullptr};
    resource::ResourceHandle address_space_handle;
    u64 mapping_generation{0};
    std::vector<resource::MappingDependency> mapping_dependencies;
    resource::ContentManifest contents;
    bool sealed{false};

    // Rebinds the set to the address space the command was resolved against
    void capture(renderer::IRenderer& renderer, GMmu* gmmu);
    // Registers a live object (null is ignored)
    template <typename T>
    void track(renderer::IRenderer& renderer, T* object,
               resource::ResourceKind kind) {
        trackObject(renderer, object, object ? &object->getResourceLifetime() : nullptr,
                    kind, object ? object->getResourceOwner() : nullptr);
    }
    // A view and the texture behind it
    void trackView(renderer::IRenderer& renderer,
                   renderer::ITextureView* view);
    // Whether everything it names is still the same live object and mapping
    bool valid(renderer::IRenderer& renderer) const;
    // Whether the memory it reads is unchanged since sealing
    bool contentsCurrent(renderer::IRenderer& renderer) const;
    void clear();

  private:
    void trackObject(renderer::IRenderer& renderer, const void* object,
                     resource::ResourceLifetime* lifetime,
                     resource::ResourceKind kind,
                     std::shared_ptr<void> owner);
};

} // namespace hydra::hw::tegra_x1::gpu::command_ir
