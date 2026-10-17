#pragma once

#include "core/hw/tegra_x1/cpu/hypervisor/const.hpp"

namespace hydra::hw::tegra_x1::cpu::hypervisor {

// Host virtual addresses may exceed the supported guest physical address
// width. Keep those two address spaces separate for all memory allocations.
class PhysicalMemory {
  public:
    static PhysicalMemory& instance() {
        static PhysicalMemory memory;
        return memory;
    }

    paddr_t add(uptr host, u64 size) {
        std::lock_guard lock(mutex);
        paddr_t candidate = 0x200000000ull;
        for (const auto& [address, allocation] : allocations) {
            if (size <= address - candidate)
                break;
            candidate = address + allocation.size;
        }
        ASSERT(candidate < limit && size <= limit - candidate, Hypervisor,
               "Guest physical memory exhausted allocating 0x{:x} bytes", size);
        allocations.emplace(candidate, Allocation{host, size});
        return candidate;
    }

    void remove(paddr_t address) {
        std::lock_guard lock(mutex);
        allocations.erase(address);
    }

    paddr_t fromHost(uptr host, u64 size) {
        std::lock_guard lock(mutex);
        for (const auto& [address, allocation] : allocations) {
            if (host >= allocation.host &&
                host - allocation.host < allocation.size &&
                size <= allocation.size - (host - allocation.host))
                return address + (host - allocation.host);
        }
        LOG_FATAL(Hypervisor,
                  "Unregistered host memory range: 0x{:x}, size: 0x{:x}", host,
                  size);
        return 0;
    }

    // Returns 0 if the host address isn't guest memory
    paddr_t tryFromHost(uptr host) {
        std::lock_guard lock(mutex);
        for (const auto& [address, allocation] : allocations) {
            if (host >= allocation.host &&
                host - allocation.host < allocation.size)
                return address + (host - allocation.host);
        }
        return 0;
    }

    uptr toHost(paddr_t address) {
        std::lock_guard lock(mutex);
        auto it = allocations.upper_bound(address);
        if (it == allocations.begin())
            return 0;
        --it;
        if (address - it->first >= it->second.size)
            return 0;
        return it->second.host + (address - it->first);
    }

  private:
    PhysicalMemory() {
        uint32_t bits = 0;
        HV_ASSERT_SUCCESS(hv_vm_config_get_max_ipa_size(&bits));
        limit = 1ull << bits;
    }

    struct Allocation {
        uptr host;
        u64 size;
    };
    std::mutex mutex;
    std::map<paddr_t, Allocation> allocations;
    u64 limit;
};

} // namespace hydra::hw::tegra_x1::cpu::hypervisor
