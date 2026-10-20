#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/resource/mapping_capture.hpp"
#include "core/hw/tegra_x1/gpu/resource/resource_registry.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::resource;

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Object : TrackedResource {};
} // namespace

int main() {
    try {
        ResourceRegistry registry;
        ResourceHandle stale;
        alignas(Object) std::array<std::byte, sizeof(Object)> storage;
        auto* object =
            std::construct_at(reinterpret_cast<Object*>(storage.data()));
        stale = registry.track(object->getResourceLifetime(),
                               ResourceKind::Buffer, object);
        const auto repeat = registry.track(object->getResourceLifetime(),
                                           ResourceKind::Buffer, object);
        check(stale.getId() == repeat.getId() &&
                  stale.getGeneration() == repeat.getGeneration(),
              "Repeated tracking keeps identity and generation");
        object->getResourceLifetime().invalidate();
        const auto updated = registry.track(object->getResourceLifetime(),
                                            ResourceKind::Buffer, object);
        check(!registry.inspect(stale) && updated.getId() == stale.getId() &&
                  updated.getGeneration() > stale.getGeneration(),
              "Invalidation advances the generation without reviving the old "
              "handle");
        std::destroy_at(object);
        check(!registry.inspect(updated) && registry.size() == 0,
              "Destruction retires metadata");
        object = std::construct_at(reinterpret_cast<Object*>(storage.data()));
        const auto reused = registry.track(object->getResourceLifetime(),
                                           ResourceKind::Buffer, object);
        check(reused.getId() != updated.getId() && !registry.inspect(updated),
              "Same-address allocation gets a new ID");
        ResourceRegistry other;
        check(
            !other.inspect(reused),
            "Foreign registries reject handles even when numeric IDs overlap");
        bool rejected = false;
        try {
            other.track(object->getResourceLifetime(), ResourceKind::Buffer,
                        object);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        check(rejected,
              "A live resource cannot silently change renderer registries");
        check(!registry.matches(reused, ResourceKind::Texture, object),
              "Resource kind is checked");
        std::destroy_at(object);
        for (u32 i = 0; i < 5000; i++) {
            Object temporary;
            (void)registry.track(temporary.getResourceLifetime(),
                                 ResourceKind::Texture, &temporary);
            check(registry.size() == 1,
                  "Only live resources occupy registry metadata");
        }
        check(registry.size() == 0,
              "Metadata does not accumulate after resource destruction");
        {
            Object survivor;
            auto first = std::make_unique<ResourceRegistry>();
            const auto old = first->track(survivor.getResourceLifetime(),
                                          ResourceKind::Buffer, &survivor);
            first.reset();
            const auto next = registry.track(survivor.getResourceLifetime(),
                                             ResourceKind::Buffer, &survivor);
            check(!registry.inspect(old) && registry.inspect(next).has_value(),
                  "Registry teardown expires handles even when the resource "
                  "survives");
        }
        {
            GMmu mmu(nullptr), unrelated(nullptr);
            std::array<u32, 4> old{}, replacement{};
            const auto ptr = reinterpret_cast<uptr>(old.data());
            mmu.map(0x1000, {ptr, sizeof(old), 0x8000});
            unrelated.map(0x1000, {ptr, sizeof(old)});
            std::vector<MappingDependency> outer, inner;
            {
                MappingCaptureScope scope(&mmu, outer);
                (void)mmu.unmapAddr(0x1000);
                (void)mmu.unmapAddr(0x1004);
                (void)unrelated.unmapAddr(0x1000);
                {
                    MappingCaptureScope nested(&unrelated, inner);
                    (void)unrelated.unmapAddr(0x1000);
                }
                (void)mmu.unmapAddr(0x1000);
            }
            std::vector<MappingDependency> replayed, nested;
            {
                MappingCaptureScope parent(&mmu, replayed);
                {
                    MappingCaptureScope child(&mmu, nested);
                    (void)mmu.unmapAddr(0x1000);
                }
            }
            check(replayed.size() == 1 && nested.size() == 1,
                  "Nested cache resolution preserves dependencies in the "
                  "enclosing command");
            replayed.clear();
            {
                MappingCaptureScope scope(&mmu, replayed);
                reuseMappingDependencies(&mmu, nested);
            }
            check(replayed.size() == 1,
                  "Cache hits replay their lookup dependencies");
            check(outer.size() == 1 && inner.size() == 1,
                  "Lookup capture deduplicates mappings, isolates address "
                  "spaces and restores nested scopes");
            mmu.map(0x2000, {reinterpret_cast<uptr>(replacement.data()),
                             sizeof(replacement)});
            check(validateMappings(mmu, outer),
                  "Unrelated mappings remain valid");
            bool stable = true;
            std::thread writer([&] {
                for (u32 i = 0; i < 2000; i++)
                    mmu.map(0x2000, {reinterpret_cast<uptr>(replacement.data()),
                                     sizeof(replacement)});
            });
            for (u32 i = 0; i < 2000; i++)
                stable &= validateMappings(mmu, outer);
            writer.join();
            check(stable, "Concurrent updates of an unrelated mapping do not "
                          "reject valid dependencies");
            mmu.rebaseHostRange(ptr, ptr + sizeof(old),
                                reinterpret_cast<uptr>(replacement.data()),
                                sizeof(replacement));
            {
                MappingCaptureScope scope(&mmu, outer);
                check(!validateMappings(mmu, outer) && outer.size() == 1,
                      "Validation rejects a rebase without mutating its "
                      "captured dependencies");
            }
            std::vector<MappingDependency> rebased;
            {
                MappingCaptureScope scope(&mmu, rebased);
                (void)mmu.unmapAddr(0x1000);
            }
            mmu.unmapRange(0x1000, sizeof(old), false);
            check(!validateMappings(mmu, rebased),
                  "Unmapping invalidates a consumed mapping");
        }
        std::cout << "PASS: resource identity, generations, address reuse, "
                     "retirement and mapping dependencies\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
