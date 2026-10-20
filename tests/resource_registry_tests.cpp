#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/resource/content_version.hpp"
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
        {
            // Content versions: identity is unchanged while the bytes change
            using R = ztd::Range<uptr>;
            ContentVersions versions;
            check(versions.current() == 0 && versions.latest(R(0x1000, 0x2000)) == 0,
                  "Untouched memory has version 0");
            const auto v1 = versions.write(R(0x1000, 0x2000));
            const auto v2 = versions.write(R(0x1800, 0x1900));
            check(v1 == 1 && v2 == 2 && versions.current() == 2,
                  "Writes advance one global counter");
            check(versions.latest(R(0x1000, 0x1100)) == 1 &&
                      versions.latest(R(0x1800, 0x1801)) == 2 &&
                      versions.latest(R(0x1900, 0x2000)) == 1,
                  "A write overlays only the part it covers");
            check(versions.latest(R(0x0, 0x1000)) == 0 &&
                      versions.latest(R(0x2000, 0x3000)) == 0,
                  "Adjacent ranges are untouched");
            check(versions.latest(R(0x1000, 0x2000)) == 2,
                  "A range reports its newest overlapping write");
            const auto v3 = versions.write(R(0x0, 0x5000));
            check(versions.latest(R(0x1800, 0x1900)) == v3 && versions.size() == 1,
                  "A covering write replaces the ranges inside it");
            versions.write(R(0x100, 0x200));
            versions.write(R(0x150, 0x400));
            check(versions.latest(R(0x100, 0x150)) == 4 &&
                      versions.latest(R(0x3f0, 0x3ff)) == 5 &&
                      versions.latest(R(0x400, 0x500)) == 3,
                  "Overlapping writes split older ranges");

            // Rewriting one range again restamps it in place
            ContentVersions again;
            const auto first = again.write(R(0x500, 0x600));
            const auto second = again.write(R(0x500, 0x600));
            check(second > first && again.size() == 1 &&
                      again.latest(R(0x500, 0x501)) == second,
                  "Rewriting the same range advances its version without growth");
            again.write(R(0x500, 0x580));
            check(again.latest(R(0x580, 0x600)) == second &&
                      again.latest(R(0x500, 0x580)) == second + 1,
                  "A shorter rewrite still splits the range");

            ContentManifest reader, writer;
            reader.addRead(versions, R(0x10000, 0x11000));
            check(reader.readsCurrent(versions), "Fresh reads are current");
            versions.write(R(0x10800, 0x10804));
            check(!reader.readsCurrent(versions),
                  "A write into a read range makes it stale");
            ContentManifest unrelated;
            unrelated.addRead(versions, R(0x20000, 0x21000));
            versions.write(R(0x10000, 0x10010));
            check(unrelated.readsCurrent(versions),
                  "Writes elsewhere do not stale a read");
            writer.addWrite(R(0x10400, 0x10500));
            ContentManifest next_reader, next_writer, independent;
            next_reader.addRead(versions, R(0x10450, 0x10460));
            next_writer.addWrite(R(0x10000, 0x10100));
            independent.addRead(versions, R(0x30000, 0x30100));
            independent.addWrite(R(0x30000, 0x30100));
            check(next_reader.dependsOn(writer), "Read after write is a dependency");
            check(!next_writer.dependsOn(writer),
                  "Disjoint writes do not depend on each other");
            next_writer.addWrite(R(0x104ff, 0x10600));
            check(next_writer.dependsOn(writer), "Write after write is a dependency");
            check(writer.dependsOn(next_reader), "Write after read is a dependency");
            check(!independent.dependsOn(writer) && !writer.dependsOn(independent),
                  "Unrelated commands may be reordered");

            // Past the range limit knowledge is dropped conservatively
            ContentVersions bounded;
            ContentManifest early;
            bounded.write(R(0x100, 0x110));
            early.addRead(bounded, R(0x900000, 0x900010));
            for (uptr i = 0; i <= ContentVersions::MAX_RANGES; i++)
                bounded.write(R(0x1000 + i * 32, 0x1000 + i * 32 + 16));
            check(bounded.size() <= ContentVersions::MAX_RANGES &&
                      !early.readsCurrent(bounded),
                  "Eviction reports change instead of losing it");
            ContentManifest after;
            after.addRead(bounded, R(0x900000, 0x900010));
            check(after.readsCurrent(bounded),
                  "Reads prepared after eviction are current");

            // Concurrent writers keep every version distinct and ordered
            ContentVersions shared;
            std::array<u64, 4> last{};
            std::vector<std::thread> threads;
            for (u32 t = 0; t < 4; t++)
                threads.emplace_back([&, t] {
                    for (u32 i = 0; i < 2000; i++)
                        last[t] = shared.write(R(0x1000 * t, 0x1000 * t + 0x800));
                });
            for (auto& thread : threads)
                thread.join();
            check(shared.current() == 8000, "Concurrent writes are all counted");
            for (u32 t = 0; t < 4; t++)
                check(shared.latest(R(0x1000 * t, 0x1000 * t + 0x800)) == last[t],
                      "Each range holds its last writer's version");
        }
        std::cout << "PASS: resource identity, generations, address reuse, "
                     "retirement, mapping dependencies and content versions\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
