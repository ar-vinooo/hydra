#pragma once

#include <map>

#include "core/hw/generic_mmu.hpp"
#include "core/hw/tegra_x1/gpu/resource/resource_lifetime.hpp"
#include "core/hw/tegra_x1/gpu/resource/mapping_capture.hpp"
#include "core/hw/tegra_x1/gpu/const.hpp"

namespace hydra::hw::tegra_x1::cpu {
class IMmu;
struct DeviceMappingState;
}

namespace hydra::hw::tegra_x1::gpu {

struct AddressSpace {
    uptr ptr;
    u64 size;
    // Guest virtual address backing ptr (0 for host-only memory)
    u64 guest{0};
};

// TODO: free memory
class GMmu : public GenericMmu<GMmu, AddressSpace>,
             public resource::TrackedResource {
  public:
    explicit GMmu(cpu::IMmu* mmu_);
    ~GMmu();
    void cancelGpuWork() { gpu_cancelled.store(true, std::memory_order_release); }
    bool gpuWorkCancelled() const { return gpu_cancelled.load(std::memory_order_acquire); }
    u64 getCaptureId() const { return capture_id; }
    std::vector<std::pair<uptr, AddressSpace>> snapshotMappings() const;

    static u64 implGetSize(const AddressSpace& as) { return as.size; }

    std::optional<AddressSpace> findAddr(uptr addr, uptr& out_base) const;
    AddressSpace findAddrImpl(uptr addr, uptr& out_base) const;
    bool matchesMappings(const std::vector<resource::MappingDependency>& dependencies) const;
    uptr unmapAddr(uptr gpu_addr) const;

    // Copy a contiguous guest range across separate host mappings, stopping
    // at the first unmapped address.
    std::vector<u8> readMappedBytes(uptr gpu_addr, usize max_size);
    // Same into `out`, without allocating. Returns the bytes copied.
    usize readMappedBytes(uptr gpu_addr, void* out, usize max_size);

    void mapImpl([[maybe_unused]] uptr base, [[maybe_unused]] AddressSpace as) {
    }
    void unmapImpl(uptr base, AddressSpace as);

    // Address space
    uptr createAddressSpace(ztd::Range<vaddr_t> range, uptr gpu_addr, u64 replace_size = 0);

    uptr allocatePrivateAddressSpace(u64 size, uptr gpu_addr) {
        return createAddressSpace(ztd::Range<vaddr_t>::fromSize(0x0, size),
                                  gpu_addr);
    }

    uptr mapBufferToAddressSpace(ztd::Range<vaddr_t> range, uptr gpu_addr, u64 replace_size = 0) {
        return createAddressSpace(range, gpu_addr, replace_size);
    }

    // Sparse memory without backing reads as zero
    void unbindToSparse(uptr gpu_addr, u64 size);

    // Reserves GPU address space: zero backed when sparse, otherwise only the
    // address range is claimed by the caller and reads fault. Existing
    // mappings are preserved.
    void reserveSparse(uptr gpu_addr, u64 size);
    // Moves mappings backed by [old_begin, old_end) to the same offsets of
    // new_begin (mappings beyond new_size become unmapped). Returns the guest
    // ranges that now point at the new memory.
    std::vector<ztd::Range<vaddr_t>> rebaseHostRange(uptr old_begin, uptr old_end,
                                                     uptr new_begin, u64 new_size);
    // Removes the mappings in the range (sparse reservations read as zero
    // again when restore_sparse is set)
    void unmapRange(uptr gpu_addr, u64 size, bool restore_sparse);
    // Drops the zero backing of a released sparse reservation, keeping buffer
    // mappings that were placed inside it
    void releaseSparse(uptr gpu_addr, u64 size);

  private:
    // Mappings are kept disjoint: a buffer replaces whatever it overlaps,
    // while a reservation only fills the parts that are still unmapped. The
    // caller holds the mapping mutex exclusively.
    void carveOut(uptr begin, uptr end, bool host_only = false);
    // Zero backing is owned by the GMMU until it is destroyed: queued or
    // in-flight GPU work may still hold pointers into blocks that were
    // unmapped, so they are never freed earlier. Blocks that are no longer
    // mapped get fresh zero pages instead (the addresses stay valid) and are
    // reused by later allocations.
    AddressSpace allocateZeroBlock(u64 size);
    void recycleZeroBlocks();
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
    inline static std::atomic<u64> next_capture_id{1};
    const u64 capture_id{
        next_capture_id.fetch_add(1, std::memory_order_relaxed)};
    std::shared_ptr<cpu::DeviceMappingState> device_mappings;
    void publishDeviceMappings(uptr excluded = invalid<uptr>());
    std::atomic<bool> gpu_cancelled{false};
    struct ZeroBlock {
        u64 size;
        bool free;
    };
    std::map<uptr, ZeroBlock> zero_blocks;
    // Size -> base of the blocks that are not mapped
    std::multimap<u64, uptr> free_zero_blocks;

    // TODO: use a better way to allocate new memory
    uptr address_space_base{GPU_PAGE_SIZE};

  public:
    GETTER(mmu, getMmu);
};

} // namespace hydra::hw::tegra_x1::gpu
