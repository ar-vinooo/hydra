#include "core/hw/tegra_x1/gpu/resource/resource_registry.hpp"
#include "core/hw/tegra_x1/gpu/renderer/draw_queue.hpp"

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
    // Identity of `registry` for the hot checks: comparing it plus
    // `registry.expired()` avoids the atomic increments of locking the
    // weak pointer on every lookup
    const RegistryState* registry_identity{nullptr};
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
    // The resource changes in place: queued draws that use it are encoded
    // first, as if they had been encoded when sealed
    renderer::syncDrawQueue();
    const auto previous =
        record->generation.fetch_add(1, std::memory_order_acq_rel);
    if (previous == std::numeric_limits<u64>::max()) {
        record->retire();
        record.reset();
    }
}

ResourceRegistry::ResourceRegistry()
    : state{std::make_shared<RegistryState>()} {}

bool ResourceRegistry::belongsTo(const ResourceRecord& record) const {
    // A registry at a reused address is told apart by the expired pointer
    // of the destroyed one
    return record.registry_identity == state.get() &&
           !record.registry.expired();
}

ResourceRegistry::~ResourceRegistry() = default;

ResourceHandle ResourceRegistry::track(ResourceLifetime& lifetime,
                                       ResourceKind kind, const void* object) {
    if (!object)
        throw std::invalid_argument("Cannot register a null GPU resource");
    auto& record = lifetime.record;
    // Already registered here: only creating a record needs the registry
    if (record && belongsTo(*record) && record->kind == kind &&
        record->object == object) [[likely]] {
        ResourceHandle handle;
        handle.id = record->id;
        handle.generation = record->generation.load(std::memory_order_acquire);
        handle.record = record;
        return handle;
    }
    std::scoped_lock lock(state->mutex);
    if (record && !belongsTo(*record)) {
        if (!record->registry.expired())
            throw std::logic_error("GPU resource belongs to another registry");
        record.reset();
    }
    if (!record) {
        if (state->next_id == std::numeric_limits<u64>::max())
            throw std::overflow_error("GPU resource IDs exhausted");
        record = std::make_shared<ResourceRecord>();
        record->registry = state;
        record->registry_identity = state.get();
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
    const auto* record = handle.record.get();
    if (!record || !belongsTo(*record) ||
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
