#include "core/hw/tegra_x1/gpu/resource/mapping_capture.hpp"

#include "core/hw/tegra_x1/gpu/gmmu.hpp"

namespace hydra::hw::tegra_x1::gpu::resource {
namespace {
// Kept in the host, not duplicated in each live GPU module.
thread_local MappingCaptureScope* active_capture = nullptr;
} // namespace

MappingCaptureScope::MappingCaptureScope(
    GMmu* owner_, std::vector<MappingDependency>& dependencies_)
    : owner{owner_}, dependencies{dependencies_}, previous{active_capture} {
    active_capture = this;
}
MappingCaptureScope::~MappingCaptureScope() { active_capture = previous; }

void recordMappingLookup(const GMmu* owner, uptr base,
                         const AddressSpace& mapping) {
    for (auto* capture = active_capture; capture; capture = capture->previous) {
        if (capture->owner != owner)
            continue;
        bool recorded = false;
        for (const auto& dep : capture->dependencies)
            if (dep.base == base && dep.ptr == mapping.ptr &&
                dep.size == mapping.size && dep.guest == mapping.guest) {
                recorded = true;
                break;
            }
        if (!recorded)
            capture->dependencies.push_back(
                {base, mapping.ptr, mapping.size, mapping.guest});
    }
}

void reuseMappingDependencies(
    const GMmu* owner, const std::vector<MappingDependency>& dependencies) {
    for (const auto& dep : dependencies)
        recordMappingLookup(owner, dep.base, {dep.ptr, dep.size, dep.guest});
}

bool validateMappings(const GMmu& owner,
                      const std::vector<MappingDependency>& dependencies) {
    return owner.matchesMappings(dependencies);
}

} // namespace hydra::hw::tegra_x1::gpu::resource
