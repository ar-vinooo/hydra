#pragma once

#include "core/hw/tegra_x1/gpu/resource/resource_lifetime.hpp"

#include <optional>

namespace hydra::hw::tegra_x1::gpu::resource {

enum class ResourceKind {
    Buffer,
    Texture,
    TextureView,
    Sampler,
    Shader,
    Pipeline,
    RenderPass,
    AddressSpace,
};

struct ResourceInfo {
    u64 id;
    u64 generation;
    ResourceKind kind;
    const void* object;
};

struct RegistryState;

class ResourceHandle {
  public:
    u64 getId() const { return id; }
    u64 getGeneration() const { return generation; }

  private:
    friend class ResourceRegistry;
    u64 id{0};
    u64 generation{0};
    std::weak_ptr<ResourceRecord> record;
};

class ResourceRegistry {
  public:
    ResourceRegistry();
    ~ResourceRegistry();
    ResourceRegistry(const ResourceRegistry&) = delete;
    ResourceRegistry& operator=(const ResourceRegistry&) = delete;

    ResourceHandle track(ResourceLifetime& lifetime, ResourceKind kind,
                         const void* object);
    std::optional<ResourceInfo> inspect(const ResourceHandle& handle) const;
    bool matches(const ResourceHandle& handle, ResourceKind kind,
                 const void* object) const;
    usize size() const;

  private:
    std::shared_ptr<RegistryState> state;

    bool belongsTo(const ResourceRecord& record) const;
};

} // namespace hydra::hw::tegra_x1::gpu::resource
