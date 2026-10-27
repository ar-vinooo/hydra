#pragma once

#include <atomic>
#include <map>

#include "core/horizon/kernel/const.hpp"
#include "core/hw/tegra_x1/cpu/memory.hpp"

namespace hydra {
class System;
}

namespace hydra::hw::tegra_x1::cpu {

struct MemoryRegion {
    vaddr_t va;
    uptr size;
    horizon::kernel::MemoryState state;
};

struct DeviceMappingState {
    struct Span { vaddr_t guest; uptr host; u64 size; };
    std::mutex mutex;
    std::map<const void*, std::vector<Span>> spaces;
};

// This class doesn't really represent a real MMU, but rather the page table, as
// each process has its own
class IMmu {
  public:
    explicit IMmu(System& system_) : system{system_} {}
    virtual ~IMmu() = default;

    virtual void map(vaddr_t dst_va, ztd::Range<uptr> range,
                     const horizon::kernel::MemoryState state) = 0;
    void map(vaddr_t dst_va, IMemory* memory,
             const horizon::kernel::MemoryState state) {
        map(dst_va,
            ztd::Range<uptr>::fromSize(memory->getPtr(), memory->getSize()),
            state);
    }
    virtual void map(vaddr_t dst_va, ztd::Range<vaddr_t> range) = 0;
    virtual void unmap(ztd::Range<vaddr_t> range) = 0;
    virtual void protect(ztd::Range<vaddr_t> range,
                         horizon::kernel::MemoryPermission perm) = 0;

    virtual uptr unmapAddr(vaddr_t va) const = 0;
    virtual MemoryRegion queryRegion(vaddr_t va) const = 0;
    virtual void setMemoryAttribute(ztd::Range<vaddr_t> range,
                                    horizon::kernel::MemoryAttribute mask,
                                    horizon::kernel::MemoryAttribute value) = 0;

    horizon::kernel::MemoryInfo queryMemory(vaddr_t va) const;
    const std::shared_ptr<DeviceMappingState>& getDeviceMappings() const { return device_mappings; }
    vaddr_t findFreeMemory(ztd::Range<vaddr_t> region, u64 size) const;

    // Write tracking
    void enableWriteTracking(ztd::Range<vaddr_t> range) {
        setWriteTrackingEnabled(range, true);
    }
    void disableWriteTracking(ztd::Range<vaddr_t> range) {
        setWriteTrackingEnabled(range, false);
    }
    virtual bool trackWrite(ztd::Range<vaddr_t> range);
    // Called before each draw and dispatch
    virtual void flushTrackedPages();
    // Applies GPU cache invalidations queued by trackWrite and rearms the
    // written pages. Called by the GPU thread before each PFIFO entry, so CPU
    // threads never wait on the cache locks a draw holds.
    void applyPendingInvalidations();
    // Queues an invalidation of host memory changed outside the guest (e.g.
    // cleared by the host), applied like those of trackWrite
    void queueHostInvalidation(ztd::Range<uptr> range);

    // GPU-written memory: the GPU's contents are not in guest memory, so CPU
    // accesses must fault and have them written back first. Takes host
    // pointers, as used by the GPU caches.
    virtual void protectGpuWritten([[maybe_unused]] ztd::Range<uptr> range) {}
    // Whether CPU accesses to GPU written memory fault and write the GPU
    // contents back first (protectGpuWritten)
    virtual bool protectsGpuWrites() const { return false; }

    // Read
    template <typename T>
    bool tryRead(vaddr_t va, T& out_value) const {
        const auto ptr = unmapAddr(va);
        if (ptr == 0x0) [[unlikely]]
            return false;

        out_value = *reinterpret_cast<T*>(ptr);
        return true;
    }

    template <typename T>
    T read(vaddr_t va) const {
        T value;
        ASSERT_DEBUG(tryRead(va, value), Cpu, "Failed to unmap va 0x{:08x}",
                     va);
        return value;
    }

    // Write
    template <typename T>
    bool tryWrite(vaddr_t va, T value) const {
        const auto ptr = unmapAddr(va);
        if (ptr == 0x0) [[unlikely]]
            return false;

        *reinterpret_cast<T*>(ptr) = value;
        return true;
    }

    template <typename T>
    void write(vaddr_t va, T value) const {
        ASSERT_DEBUG(tryWrite(va, value), Cpu, "Failed to unmap va 0x{:08x}",
                     va);
    }

    // Stores only if memory still holds the value of the exclusive load
    template <typename T>
    bool writeExclusive(vaddr_t va, T value, T expected) const {
        auto ptr = unmapAddr(va);
        ASSERT_DEBUG(ptr != 0x0, Cpu, "Failed to unmap va 0x{:08x}", va);
        if (ptr == 0x0) [[unlikely]]
            return false;

        return atomicCompareExchangeStrong(reinterpret_cast<T*>(ptr), expected,
                                           value);
    }

  protected:
    // Write tracking
    virtual void setWriteTrackingEnabled(ztd::Range<vaddr_t> range,
                                         bool enable) = 0;
    virtual bool trySuspendWriteTracking(ztd::Range<vaddr_t> range) = 0;
    virtual void resumeWriteTracking(ztd::Range<vaddr_t> range) = 0;

  protected:
    System& getSystem() const { return system; }

  private:
    System& system;
    std::shared_ptr<DeviceMappingState> device_mappings = std::make_shared<DeviceMappingState>();

    std::mutex write_tracking_mutex;
    std::vector<ztd::Range<vaddr_t>> tracked_pages;
    std::vector<ztd::Range<uptr>> pending_invalidations;
    std::atomic<bool> has_pending_invalidations{false};
};

} // namespace hydra::hw::tegra_x1::cpu
