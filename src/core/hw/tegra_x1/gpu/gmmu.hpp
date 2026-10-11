#pragma once

#include "core/hw/generic_mmu.hpp"
#include "core/hw/tegra_x1/gpu/const.hpp"

namespace hydra::hw::tegra_x1::cpu {
class IMmu;
}

namespace hydra::hw::tegra_x1::gpu {

struct AddressSpace {
    uptr ptr;
    u64 size;
};

// TODO: free memory
class GMmu : public GenericMmu<GMmu, AddressSpace> {
  public:
    explicit GMmu(cpu::IMmu* mmu_) : mmu{mmu_} {}

    static u64 implGetSize(const AddressSpace& as) { return as.size; }

    AddressSpace& unmapAddrToAddressSpace(uptr gpu_addr) {
        uptr base;
        auto addr_space = findAddrImplRef(gpu_addr, base);
        ASSERT_DEBUG(addr_space, Gpu,
                     "Address space not found for Gpu address 0x{:x}",
                     gpu_addr);

        return *addr_space;
    }

    uptr unmapAddr(uptr gpu_addr) const;

    // Copy a contiguous guest range across separate host mappings, stopping
    // at the first unmapped address.
    std::vector<u8> readMappedBytes(uptr gpu_addr, usize max_size);

    void mapImpl([[maybe_unused]] uptr base, [[maybe_unused]] AddressSpace as) {
    }
    void unmapImpl([[maybe_unused]] uptr base,
                   [[maybe_unused]] AddressSpace as) {}

    // Address space
    uptr createAddressSpace(ztd::Range<vaddr_t> range, uptr gpu_addr);

    uptr allocatePrivateAddressSpace(u64 size, uptr gpu_addr) {
        return createAddressSpace(ztd::Range<vaddr_t>::fromSize(0x0, size),
                                  gpu_addr);
    }

    uptr mapBufferToAddressSpace(ztd::Range<vaddr_t> range, uptr gpu_addr) {
        return createAddressSpace(range, gpu_addr);
    }

    // Sparse memory without backing reads as zero
    void unbindToSparse(uptr gpu_addr, u64 size);

  private:
    // Mappings are kept disjoint: a buffer replaces whatever it overlaps,
    // while a reservation only fills the parts that are still unmapped
    void carveOut(uptr begin, uptr end);
    void mapIntoGaps(uptr base, AddressSpace as);

  public:

    // TODO
    /*
    void ModifyAddressSpace(uptr ptr, u64 size, uptr gpu_addr) {
        auto& as = UnmapAddrToAddressSpace(gpu_addr);
        ASSERT_DEBUG(size == as.size, Gpu, "Size mismatch: {} != {}", size,
                     as.size)

        as.ptr = ptr;
    }
    */

  private:
    cpu::IMmu* mmu;

    // TODO: use a better way to allocate new memory
    uptr address_space_base{GPU_PAGE_SIZE};

  public:
    GETTER(mmu, getMmu);
};

} // namespace hydra::hw::tegra_x1::gpu
