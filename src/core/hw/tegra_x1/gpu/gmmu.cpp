#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/capture/guest_events.hpp"
#include "core/hw/tegra_x1/gpu/resource/mapping_capture.hpp"

#include "core/hw/tegra_x1/cpu/mmu.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <set>

#include <sys/mman.h>
#include <unistd.h>

namespace hydra::hw::tegra_x1::gpu {

GMmu::GMmu(cpu::IMmu* mmu_) : mmu(mmu_),
    device_mappings(mmu ? mmu->getDeviceMappings() : nullptr) {}

std::vector<std::pair<uptr, AddressSpace>> GMmu::snapshotMappings() const {
    std::shared_lock lock(mutex);
    return {mapped_ranges.begin(), mapped_ranges.end()};
}

void GMmu::publishDeviceMappings(uptr excluded) {
    if (!device_mappings)
        return;
    std::vector<cpu::DeviceMappingState::Span> spans;
    spans.reserve(mapped_ranges.size());
    for (const auto& [base, as] : mapped_ranges)
        if (as.guest != 0 && base != excluded)
            spans.push_back({as.guest, as.ptr, as.size});
    std::scoped_lock lock(device_mappings->mutex);
    if (spans.empty())
        device_mappings->spaces.erase(this);
    else
        device_mappings->spaces[this] = std::move(spans);
}

void GMmu::unmapImpl(uptr base, AddressSpace as) {
    if (as.guest != 0)
        publishDeviceMappings(base);
}

GMmu::~GMmu() {
    capture::GuestEvents::instance().addressSpaceDestroyed(capture_id);
    if (device_mappings) {
        std::scoped_lock lock(device_mappings->mutex);
        device_mappings->spaces.erase(this);
    }
    for (const auto& [ptr, block] : zero_blocks)
        munmap(reinterpret_cast<void*>(ptr), block.size);
}

AddressSpace GMmu::allocateZeroBlock(u64 size) {
    if (size == 0)
        return {0x0, 0};

    const u64 block_size = align(size, static_cast<u64>(getpagesize()));

    // Reuse the smallest unmapped block that fits
    auto it = free_zero_blocks.lower_bound(block_size);
    if (it != free_zero_blocks.end()) {
        const uptr ptr = it->second;
        free_zero_blocks.erase(it);
        auto& block = zero_blocks.at(ptr);
        block.free = false;
        // The pages may hold what the GPU wrote before they were returned.
        // Cleared in place: Metal buffers may still wrap these addresses, so
        // the mapping must stay the same.
        madvise(reinterpret_cast<void*>(ptr), block.size, MADV_FREE_REUSE);
        std::memset(reinterpret_cast<void*>(ptr), 0, block.size);
        // Cached copies of the old contents are stale
        if (mmu != nullptr)
            mmu->queueHostInvalidation(
                ztd::Range<uptr>::fromSize(ptr, block.size));
        return {ptr, size};
    }

    // Anonymous pages read as zero and only take memory once written
    void* ptr = mmap(nullptr, block_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(ptr != MAP_FAILED, Gpu,
           "Failed to allocate {:#x} bytes of sparse GPU memory", size);
    zero_blocks[reinterpret_cast<uptr>(ptr)] = {block_size, false};
    return {reinterpret_cast<uptr>(ptr), size};
}

void GMmu::recycleZeroBlocks() {
    if (zero_blocks.size() == free_zero_blocks.size())
        return;

    // Blocks still (partially) mapped
    std::set<uptr> referenced;
    for (const auto& [base, as] : mapped_ranges) {
        if (as.guest != 0 || as.ptr == 0x0)
            continue;
        auto it = zero_blocks.upper_bound(as.ptr);
        if (it == zero_blocks.begin())
            continue;
        --it;
        if (as.ptr - it->first < it->second.size)
            referenced.insert(it->first);
    }

    for (auto& [ptr, block] : zero_blocks) {
        if (block.free || referenced.contains(ptr))
            continue;

        // Return the memory to the system. The mapping itself stays: Metal
        // buffers wrapping these addresses would keep the old pages if it
        // was replaced. Cleared when reused.
        madvise(reinterpret_cast<void*>(ptr), block.size, MADV_FREE_REUSABLE);
        block.free = true;
        free_zero_blocks.emplace(block.size, ptr);
    }
}

std::vector<u8> GMmu::readMappedBytes(uptr gpu_addr, usize max_size) {
    std::vector<u8> bytes;
    bytes.reserve(max_size);
    while (bytes.size() < max_size) {
        uptr base;
        const auto as = findAddr(gpu_addr, base);
        if (!as || as->ptr == 0)
            break;
        const auto offset = gpu_addr - base;
        const auto count = std::min<usize>(as->size - offset,
                                           max_size - bytes.size());
        const auto* ptr = reinterpret_cast<const u8*>(as->ptr + offset);
        bytes.insert(bytes.end(), ptr, ptr + count);
        gpu_addr += count;
    }
    return bytes;
}

bool GMmu::matchesMappings(
    const std::vector<resource::MappingDependency>& dependencies) const {
    std::shared_lock lock(mutex);
    for (const auto& dep : dependencies) {
        uptr base = 0;
        u64 size = 0;
        const auto* mapping = const_cast<GMmu*>(this)->findAddrLocked(dep.base, base, size);
        if (!mapping || base != dep.base || mapping->ptr != dep.ptr ||
            mapping->size != dep.size || mapping->guest != dep.guest)
            return false;
    }
    return true;
}

std::optional<AddressSpace> GMmu::findAddr(uptr addr, uptr& out_base) const {
    const auto result = GenericMmu<GMmu, AddressSpace>::findAddr(addr, out_base);
    if (result)
        resource::recordMappingLookup(this, out_base, *result);
    return result;
}

AddressSpace GMmu::findAddrImpl(uptr addr, uptr& out_base) const {
    const auto result = findAddr(addr, out_base);
    DEBUGGER_ASSERT_DEBUG(result, Mmu, "Failed to find impl for addr 0x{:08x}", addr);
    return result.value_or(AddressSpace{});
}

uptr GMmu::unmapAddr(uptr gpu_addr) const {
    uptr base;
    const auto as = findAddrImpl(gpu_addr, base);
    ASSERT_DEBUG(as.ptr != 0x0, Gpu, "Address 0x{:08x} is not host mapped",
                 gpu_addr);
    // `base` is not set for an unmapped address: no pointer into host memory
    if (as.ptr == 0x0)
        return 0x0;

    const uptr ptr = as.ptr + (gpu_addr - base);
    // DEBUG: the host pointer is resolved once at map time. Report mappings
    // whose guest virtual memory now resolves elsewhere (HYDRA_GMMU_CHECK=1; HYDRA_GMMU_FRESH=1
    // uses the current translation instead).
    static const bool check = std::getenv("HYDRA_GMMU_CHECK") != nullptr;
    if (check && as.guest != 0) {
        const uptr current = mmu->unmapAddr(as.guest + (gpu_addr - base));
        if (current != ptr) {
            static std::atomic<u32> reports{0};
            if (reports.fetch_add(1, std::memory_order_relaxed) < 32)
                LOG_WARN(Gpu,
                         "DEBUG stale GPU mapping: gpu {:#x} guest {:#x} "
                         "cached host {:#x} current host {:#x}",
                         gpu_addr, as.guest + (gpu_addr - base), ptr, current);
            static const bool fresh = std::getenv("HYDRA_GMMU_FRESH") != nullptr;
            if (fresh && current != 0)
                return current;
        }
    }
    return ptr;
}

uptr GMmu::createAddressSpace(ztd::Range<vaddr_t> range, uptr gpu_addr, u64 replace_size) {
    std::unique_lock lock(mutex);
    if (range.getBegin() != 0x0) {
        if (gpu_addr == invalid<uptr>()) {
            gpu_addr = address_space_base;
            address_space_base += align(range.getSize(), GPU_PAGE_SIZE);
        }

        carveOut(gpu_addr, gpu_addr + std::max(range.getSize(), replace_size));
        if (range.getSize() == 0) {
            publishDeviceMappings();
            return gpu_addr;
        }
        mmu->enableWriteTracking(ztd::Range<vaddr_t>(
            alignDown(range.getBegin(), cpu::GUEST_PAGE_SIZE),
            align(range.getEnd(), cpu::GUEST_PAGE_SIZE)));

        // Guest virtual memory can span separate physical allocations. Resolve
        // each page instead of treating the first host pointer as the backing
        // for the entire nvmap allocation.
        vaddr_t run_begin = range.getBegin();
        uptr run_ptr = mmu->unmapAddr(run_begin);
        vaddr_t cursor = std::min(
            align(run_begin + 1, cpu::GUEST_PAGE_SIZE), range.getEnd());
        while (cursor < range.getEnd()) {
            const uptr ptr = mmu->unmapAddr(cursor);
            if (ptr != run_ptr + (cursor - run_begin)) {
                mapLocked(gpu_addr + (run_begin - range.getBegin()),
                          {run_ptr, cursor - run_begin, run_begin});
                LOG_DEBUG(Gpu, "Noncontiguous GPU backing at {:#x}",
                          gpu_addr + (cursor - range.getBegin()));
                run_begin = cursor;
                run_ptr = ptr;
            }
            cursor = std::min(cursor + cpu::GUEST_PAGE_SIZE, range.getEnd());
        }
        mapLocked(gpu_addr + (run_begin - range.getBegin()),
                  {run_ptr, range.getEnd() - run_begin, run_begin});
        // DEBUG (HYDRA_GMMU_CHECK): report other GPU mappings of the same
        // guest memory. Two live resources must not share backing.
        if (std::getenv("HYDRA_GMMU_CHECK") != nullptr) {
            static std::atomic<u32> reports{0};
            const vaddr_t guest_begin = range.getBegin();
            const vaddr_t guest_end = range.getEnd();
            for (const auto& [other_gpu, other] : mapped_ranges) {
                if (other.guest == 0 || other_gpu == gpu_addr)
                    continue;
                if (other_gpu >= gpu_addr && other_gpu < gpu_addr + range.getSize())
                    continue;
                const vaddr_t other_end = other.guest + other.size;
                if (other.guest < guest_end && other_end > guest_begin &&
                    reports.fetch_add(1, std::memory_order_relaxed) < 64)
                    LOG_WARN(Gpu,
                             "DEBUG shared GPU backing: new gpu {:#x} guest "
                             "{:#x}+{:#x} overlaps gpu {:#x} guest {:#x}+{:#x}",
                             gpu_addr, guest_begin, range.getSize(), other_gpu,
                             other.guest, other.size);
            }
        }
        publishDeviceMappings();
        return gpu_addr;
    }

    // Reserved GPU address space without guest backing reads as zero, like
    // sparse memory
    const AddressSpace as = allocateZeroBlock(range.getSize());

    if (gpu_addr == invalid<uptr>()) {
        gpu_addr = address_space_base;
        address_space_base += align(range.getSize(), GPU_PAGE_SIZE);
        mapLocked(gpu_addr, as);
    } else {
        mapIntoGaps(gpu_addr, as);
    }

    return gpu_addr;
}

void GMmu::carveOut(uptr begin, uptr end, bool host_only) {
    for (auto it = mapped_ranges.begin(); it != mapped_ranges.end();) {
        const uptr other_begin = it->first;
        const AddressSpace other = it->second;
        const uptr other_end = other_begin + other.size;
        if (other_end <= begin || other_begin >= end ||
            (host_only && other.guest != 0)) {
            ++it;
            continue;
        }

        it = mapped_ranges.erase(it);
        if (other_begin < begin)
            mapped_ranges[other_begin] = {other.ptr, begin - other_begin,
                                          other.guest};
        if (other_end > end)
            mapped_ranges[end] = {
                other.ptr + (end - other_begin), other_end - end,
                other.guest != 0 ? other.guest + (end - other_begin) : 0};
    }
    generation.fetch_add(1, std::memory_order_release);
    recycleZeroBlocks();
}

void GMmu::mapIntoGaps(uptr base, AddressSpace as) {
    const uptr end = base + as.size;
    uptr cursor = base;

    // Find the first mapping that may overlap
    auto it = mapped_ranges.upper_bound(base);
    if (it != mapped_ranges.begin())
        --it;

    std::vector<std::pair<uptr, AddressSpace>> gaps;
    for (; it != mapped_ranges.end() && it->first < end; ++it) {
        const uptr other_begin = it->first;
        const uptr other_end = other_begin + it->second.size;
        if (other_end <= cursor)
            continue;
        if (other_begin > cursor)
            gaps.push_back({cursor, {as.ptr + (cursor - base),
                                     other_begin - cursor,
                                     as.guest != 0 ? as.guest + (cursor - base) : 0}});
        cursor = std::max(cursor, other_end);
    }
    if (cursor < end)
        gaps.push_back({cursor, {as.ptr + (cursor - base), end - cursor,
                                 as.guest != 0 ? as.guest + (cursor - base) : 0}});

    for (const auto& [gap_base, gap] : gaps)
        mapLocked(gap_base, gap);
}

void GMmu::unbindToSparse(uptr gpu_addr, u64 size) {
    std::unique_lock lock(mutex);
    carveOut(gpu_addr, gpu_addr + size);
    mapLocked(gpu_addr, allocateZeroBlock(size));
    publishDeviceMappings();
}

void GMmu::reserveSparse(uptr gpu_addr, u64 size) {
    if (size == 0)
        return;
    std::unique_lock lock(mutex);
    mapIntoGaps(gpu_addr, allocateZeroBlock(size));
}

void GMmu::unmapRange(uptr gpu_addr, u64 size, bool restore_sparse) {
    if (size == 0)
        return;
    std::unique_lock lock(mutex);
    carveOut(gpu_addr, gpu_addr + size);
    if (restore_sparse)
        mapIntoGaps(gpu_addr, allocateZeroBlock(size));
    publishDeviceMappings();
}

std::vector<ztd::Range<vaddr_t>> GMmu::rebaseHostRange(uptr old_begin, uptr old_end,
                                                       uptr new_begin, u64 new_size) {
    std::vector<ztd::Range<vaddr_t>> guest_ranges;
    std::unique_lock lock(mutex);
    std::vector<std::pair<uptr, AddressSpace>> pieces;
    for (auto it = mapped_ranges.begin(); it != mapped_ranges.end();) {
        const uptr gpu = it->first;
        const AddressSpace as = it->second;
        const uptr as_end = as.ptr + as.size;
        if (as.guest == 0 || as_end <= old_begin || as.ptr >= old_end) {
            ++it;
            continue;
        }
        it = mapped_ranges.erase(it);
        const uptr in_begin = std::max(as.ptr, old_begin);
        const uptr in_end = std::min(as_end, old_end);
        if (in_begin > as.ptr)
            pieces.push_back({gpu, {as.ptr, in_begin - as.ptr, as.guest}});
        const u64 offset = in_begin - old_begin;
        if (offset < new_size) {
            const u64 size = std::min<u64>(in_end - in_begin, new_size - offset);
            const uptr piece_gpu = gpu + (in_begin - as.ptr);
            const vaddr_t piece_guest = as.guest + (in_begin - as.ptr);
            pieces.push_back({piece_gpu, {new_begin + offset, size, piece_guest}});
            guest_ranges.push_back(ztd::Range<vaddr_t>::fromSize(piece_guest, size));
        }
        if (as_end > in_end)
            pieces.push_back({gpu + (in_end - as.ptr),
                              {in_end, as_end - in_end, as.guest + (in_end - as.ptr)}});
    }
    for (const auto& [gpu, as] : pieces)
        mapLocked(gpu, as);
    generation.fetch_add(1, std::memory_order_release);
    publishDeviceMappings();
    return guest_ranges;
}

void GMmu::releaseSparse(uptr gpu_addr, u64 size) {
    if (size == 0)
        return;
    std::unique_lock lock(mutex);
    carveOut(gpu_addr, gpu_addr + size, true);
}

} // namespace hydra::hw::tegra_x1::gpu
