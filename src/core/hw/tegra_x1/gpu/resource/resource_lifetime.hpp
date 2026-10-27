#pragma once

#include <memory>
#include <mutex>

namespace hydra::hw::tegra_x1::gpu::resource {

struct ResourceRecord;
class ResourceRegistry;

class ResourceLifetime {
  public:
    ResourceLifetime() = default;
    ~ResourceLifetime();
    ResourceLifetime(const ResourceLifetime&) = delete;
    ResourceLifetime& operator=(const ResourceLifetime&) = delete;
    void invalidate();

  private:
    friend class ResourceRegistry;
    std::shared_ptr<ResourceRecord> record;
};

class TrackedResource {
  public:
    virtual ~TrackedResource() = default;
    // Cache ownership is created by host code; packets keep a lease.
    std::shared_ptr<void> createResourceOwner();
    std::shared_ptr<void> getResourceOwner() const {
        std::scoped_lock lock(owner_mutex);
        return owner.lock();
    }
    void setResourceOwner(const std::shared_ptr<void>& value) {
        std::scoped_lock lock(owner_mutex);
        owner = value;
    }
    ResourceLifetime& getResourceLifetime() { return lifetime; }

  private:
    ResourceLifetime lifetime;
    mutable std::mutex owner_mutex;
    std::weak_ptr<void> owner;
};

} // namespace hydra::hw::tegra_x1::gpu::resource
