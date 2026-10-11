#pragma once

#include "core/hw/tegra_x1/cpu/hypervisor/physical_memory.hpp"
#include "core/hw/tegra_x1/cpu/memory.hpp"

namespace hydra::hw::tegra_x1::cpu::hypervisor {

class Memory : public IMemory {
  public:
    explicit Memory(u64 size) : IMemory(size) { allocate(); }
    ~Memory() override { free(); }

    uptr getPtr() const override { return ptr; }
    paddr_t getPhysicalAddress() const { return physical_address; }

  protected:
    void resizeImpl() override {
        free();
        allocate();
    }

  private:
    uptr ptr;
    u64 allocated_size = 0;
    paddr_t physical_address = 0;

    // Helpers
    u64 getSizeAligned() const { return align(getSize(), APPLE_PAGE_SIZE); }

    void allocate() {
        const auto size = getSizeAligned();
        ptr = allocateVmMemory(size);

        LOG_DEBUG(Hypervisor, "Mapping memory ptr: 0x{:x}, size: 0x{:x}", ptr,
                  size);

        allocated_size = size;
        physical_address = PhysicalMemory::instance().add(ptr, size);
        // Map
        HV_ASSERT_SUCCESS(
            hv_vm_map(reinterpret_cast<void*>(ptr), physical_address, size,
                      HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    }

    void free() {
        // Unmap
        HV_ASSERT_SUCCESS(hv_vm_unmap(physical_address, allocated_size));

        PhysicalMemory::instance().remove(physical_address);
        freeVmMemory(ptr, allocated_size);
    }
};

// Host memory owned by someone else, only registered with the VM
class ExternalMemory : public IMemory {
  public:
    ExternalMemory(uptr ptr_, u64 size) : IMemory(size), ptr{ptr_} {
        ASSERT_ALIGNMENT(ptr, APPLE_PAGE_SIZE, Hypervisor, "ptr");
        ASSERT_ALIGNMENT(getSize(), APPLE_PAGE_SIZE, Hypervisor, "size");
        physical_address = PhysicalMemory::instance().add(ptr, getSize());
        HV_ASSERT_SUCCESS(
            hv_vm_map(reinterpret_cast<void*>(ptr), physical_address,
                      getSize(),
                      HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    }
    ~ExternalMemory() override {
        HV_ASSERT_SUCCESS(hv_vm_unmap(physical_address, getSize()));
        PhysicalMemory::instance().remove(physical_address);
    }

    uptr getPtr() const override { return ptr; }

  protected:
    void resizeImpl() override {
        LOG_FATAL(Hypervisor, "External memory cannot be resized");
    }

  private:
    uptr ptr;
    paddr_t physical_address;
};

} // namespace hydra::hw::tegra_x1::cpu::hypervisor
