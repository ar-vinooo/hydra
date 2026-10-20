#pragma once

#include <vector>

namespace hydra::hw::tegra_x1::gpu {
class GMmu;
struct AddressSpace;
} // namespace hydra::hw::tegra_x1::gpu

namespace hydra::hw::tegra_x1::gpu::resource {

struct MappingDependency {
    uptr base;
    uptr ptr;
    u64 size;
    u64 guest;
};

class MappingCaptureScope {
  public:
    MappingCaptureScope(GMmu* owner_,
                        std::vector<MappingDependency>& dependencies_);
    ~MappingCaptureScope();
    MappingCaptureScope(const MappingCaptureScope&) = delete;
    MappingCaptureScope& operator=(const MappingCaptureScope&) = delete;

  private:
    friend void recordMappingLookup(const GMmu*, uptr, const AddressSpace&);
    GMmu* owner;
    std::vector<MappingDependency>& dependencies;
    MappingCaptureScope* previous;
};

void recordMappingLookup(const GMmu* owner, uptr base,
                         const AddressSpace& mapping);
void reuseMappingDependencies(
    const GMmu* owner, const std::vector<MappingDependency>& dependencies);
bool validateMappings(const GMmu& owner,
                      const std::vector<MappingDependency>& dependencies);

} // namespace hydra::hw::tegra_x1::gpu::resource
