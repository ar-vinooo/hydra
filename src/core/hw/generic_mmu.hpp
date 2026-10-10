#pragma once

#include "core/debugger/debugger_manager.hpp"

namespace hydra::hw {

// TODO: get rid of this bs
template <typename Subclass, typename Impl>
class GenericMmu {
    friend Subclass;

  private:
    GenericMmu() noexcept = default;

  public:
    void map(uptr base, Impl impl) {
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
    }

    void unmap(uptr base) {
        auto it = mapped_ranges.find(base);
        ASSERT_DEBUG(it != mapped_ranges.end(), Mmu,
                     "Failed to unmap with base 0x{:08x}", base);
        THIS->unmapImpl(base, it->second);
        mapped_ranges.erase(it);
    }

    /*
    void Remap(uptr base) {
        auto impl = mapped_ranges.at(base);
        THIS->UnmapImpl(base, impl);
        THIS->MapImpl(base, impl);
    }
    */

    Impl* findAddrImplRef(uptr addr, uptr& out_base) {
        if (!has_overlapping_ranges) {
            auto it = mapped_ranges.upper_bound(addr);
            if (it == mapped_ranges.begin())
                return nullptr;
            --it;
            if (addr - it->first >= THIS->implGetSize(it->second))
                return nullptr;
            out_base = it->first;
            return &it->second;
        }
        for (auto& [base, impl] : mapped_ranges) {
            if (addr >= base && addr - base < THIS->implGetSize(impl)) {
                out_base = base;
                return &impl;
            }
        }

        return nullptr;
    }

    const Impl& findAddrImpl(uptr addr, uptr& out_base) const {
        auto impl =
            const_cast<GenericMmu*>(this)->findAddrImplRef(addr, out_base);
        DEBUGGER_ASSERT_DEBUG(impl, Mmu,
                              "Failed to find impl for addr 0x{:08x}", addr);

        return *impl;
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
    std::map<uptr, Impl> mapped_ranges;
    bool has_overlapping_ranges{false};
};

} // namespace hydra::hw
