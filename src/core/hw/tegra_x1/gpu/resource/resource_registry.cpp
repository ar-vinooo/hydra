#include "core/hw/tegra_x1/gpu/resource/resource_registry.hpp"

#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace hydra::hw::tegra_x1::gpu::resource {

struct RegistryState {
    std::mutex mutex;
    u64 next_id{1};
    std::unordered_map<u64, std::weak_ptr<ResourceRecord>> records;
};

struct ResourceRecord {
    std::weak_ptr<RegistryState> registry;
    u64 id;
    ResourceKind kind;
    const void* object;
    std::atomic<u64> generation{1};
    std::atomic<bool> alive{true};

    void retire() {
        alive.store(false, std::memory_order_release);
        if (const auto owner = registry.lock()) {
            std::scoped_lock lock(owner->mutex);
            owner->records.erase(id);
        }
    }
};

std::shared_ptr<void> TrackedResource::createResourceOwner() {
    if (auto existing = getResourceOwner())
        return existing;
    auto created = std::shared_ptr<TrackedResource>(this);
    setResourceOwner(created);
    return created;
}

ResourceLifetime::~ResourceLifetime() {
    if (record)
        record->retire();
}

void ResourceLifetime::invalidate() {
    if (!record)
        return;
    const auto previous =
        record->generation.fetch_add(1, std::memory_order_acq_rel);
    if (previous == std::numeric_limits<u64>::max()) {
        record->retire();
        record.reset();
    }
}

ResourceRegistry::ResourceRegistry()
    : state{std::make_shared<RegistryState>()} {}
ResourceRegistry::~ResourceRegistry() = default;

ResourceHandle ResourceRegistry::track(ResourceLifetime& lifetime,
                                       ResourceKind kind, const void* object) {
    if (!object)
        throw std::invalid_argument("Cannot register a null GPU resource");
    std::scoped_lock lock(state->mutex);
    auto& record = lifetime.record;
    if (record && record->registry.lock() != state) {
        if (!record->registry.expired())
            throw std::logic_error("GPU resource belongs to another registry");
        record.reset();
    }
    if (!record) {
        if (state->next_id == std::numeric_limits<u64>::max())
            throw std::overflow_error("GPU resource IDs exhausted");
        record = std::make_shared<ResourceRecord>();
        record->registry = state;
        record->id = state->next_id++;
        record->kind = kind;
        record->object = object;
        state->records.emplace(record->id, record);
    }
    if (record->kind != kind || record->object != object)
        throw std::logic_error(
            "GPU resource identity changed without retirement");
    ResourceHandle handle;
    handle.id = record->id;
    handle.generation = record->generation.load(std::memory_order_acquire);
    handle.record = record;
    return handle;
}

std::optional<ResourceInfo>
ResourceRegistry::inspect(const ResourceHandle& handle) const {
    const auto record = handle.record.lock();
    if (!record || record->registry.lock() != state ||
        !record->alive.load(std::memory_order_acquire) ||
        record->id != handle.id ||
        record->generation.load(std::memory_order_acquire) != handle.generation)
        return std::nullopt;
    return ResourceInfo{record->id, handle.generation, record->kind,
                        record->object};
}

bool ResourceRegistry::matches(const ResourceHandle& handle, ResourceKind kind,
                               const void* object) const {
    const auto info = inspect(handle);
    return info && info->kind == kind && info->object == object;
}

usize ResourceRegistry::size() const {
    std::scoped_lock lock(state->mutex);
    return state->records.size();
}

} // namespace hydra::hw::tegra_x1::gpu::resource
