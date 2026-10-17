#pragma once

#include "core/debugger/debugger_manager.hpp"

#include <array>
#include <atomic>
#include <optional>
#include <shared_mutex>

namespace hydra::hw {

// TODO: get rid of this bs
// Mappings change on service threads while the GPU thread looks them up, so
// lookups return copies
template <typename Subclass, typename Impl>
class GenericMmu {
    friend Subclass;

  private:
    GenericMmu() noexcept = default;

  public:
    void map(uptr base, Impl impl) {
        std::unique_lock lock(mutex);
        mapLocked(base, impl);
    }

    void unmap(uptr base) {
        std::unique_lock lock(mutex);
        auto it = mapped_ranges.find(base);
        ASSERT_DEBUG(it != mapped_ranges.end(), Mmu,
                     "Failed to unmap with base 0x{:08x}", base);
        THIS->unmapImpl(base, it->second);
        mapped_ranges.erase(it);
        generation.fetch_add(1, std::memory_order_release);
    }

    /*
    void Remap(uptr base) {
        auto impl = mapped_ranges.at(base);
        THIS->UnmapImpl(base, impl);
        THIS->MapImpl(base, impl);
    }
    */

    std::optional<Impl> findAddr(uptr addr, uptr& out_base) const {
        // Most lookups hit one of the few mappings used last by this thread
        thread_local LookupCache cache;
        const u64 current_generation =
            generation.load(std::memory_order_acquire);
        for (const auto& entry : cache.entries) {
            if (entry.owner == this && entry.generation == current_generation &&
                addr - entry.base < entry.size) {
                out_base = entry.base;
                return entry.impl;
            }
        }

        std::shared_lock lock(mutex);
        u64 size;
        const auto* impl = const_cast<GenericMmu*>(this)->findAddrLocked(
            addr, out_base, size);
        if (impl == nullptr)
            return std::nullopt;
        cache.entries[cache.next++ % cache.entries.size()] = {
            this, current_generation, out_base, size, *impl};
        return *impl;
    }

    Impl findAddrImpl(uptr addr, uptr& out_base) const {
        const auto impl = findAddr(addr, out_base);
        DEBUGGER_ASSERT_DEBUG(impl, Mmu,
                              "Failed to find impl for addr 0x{:08x}", addr);

        return impl.value_or(Impl{});
    }

    template <typename T>
    T load(uptr addr) const {
        return *reinterpret_cast<T*>(CONST_THIS->unmapAddr(addr));
    }

    template <typename T>
    void store(uptr addr, T value) const {
        *reinterpret_cast<T*>(CONST_THIS->UnmapAddr(addr)) = value;
    }

  private:
    struct CachedMapping {
        const GenericMmu* owner{nullptr};
        u64 generation{0};
        uptr base{0};
        u64 size{0};
        Impl impl{};
    };
    struct LookupCache {
        std::array<CachedMapping, 4> entries{};
        usize next{0};
    };

    // Callers hold the mutex exclusively
    void mapLocked(uptr base, Impl impl) {
        // Most GPU mappings are disjoint. Preserve the existing first-range
        // precedence if overlapping mappings are ever installed.
        if (!has_overlapping_ranges) {
            const auto size = THIS->implGetSize(impl);
            for (const auto& [other_base, other] : mapped_ranges) {
                if (other_base == base)
                    continue;
                const auto other_size = THIS->implGetSize(other);
                if (size != 0 && other_size != 0 &&
                    (base <= other_base ? other_base - base < size
                                        : base - other_base < other_size)) {
                    has_overlapping_ranges = true;
                    break;
                }
            }
        }
        mapped_ranges[base] = impl;
        THIS->mapImpl(base, impl);
        generation.fetch_add(1, std::memory_order_release);
    }

    // Callers hold the mutex
    Impl* findAddrLocked(uptr addr, uptr& out_base, u64& out_size) {
        if (!has_overlapping_ranges) {
            auto it = mapped_ranges.upper_bound(addr);
            if (it == mapped_ranges.begin())
                return nullptr;
            --it;
            out_size = THIS->implGetSize(it->second);
            if (addr - it->first >= out_size)
                return nullptr;
            out_base = it->first;
            return &it->second;
        }
        for (auto& [base, impl] : mapped_ranges) {
            out_size = THIS->implGetSize(impl);
            if (addr >= base && addr - base < out_size) {
                out_base = base;
                return &impl;
            }
        }

        return nullptr;
    }

    mutable std::shared_mutex mutex;
    // Changes whenever a mapping does, invalidating the lookup caches
    std::atomic<u64> generation{1};
    std::map<uptr, Impl> mapped_ranges;
    bool has_overlapping_ranges{false};
};

} // namespace hydra::hw
