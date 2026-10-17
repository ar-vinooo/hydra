#include "core/hw/tegra_x1/gpu/gmmu.hpp"

#include "core/hw/tegra_x1/cpu/mmu.hpp"

#include <atomic>
#include <cstdlib>

namespace hydra::hw::tegra_x1::gpu {

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

uptr GMmu::unmapAddr(uptr gpu_addr) const {
    uptr base;
    const auto as = findAddrImpl(gpu_addr, base);
    ASSERT_DEBUG(as.ptr != 0x0, Gpu, "Address 0x{:08x} is not host mapped",
                 gpu_addr);

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

uptr GMmu::createAddressSpace(ztd::Range<vaddr_t> range, uptr gpu_addr) {
    std::unique_lock lock(mutex);
    if (range.getBegin() != 0x0) {
        if (gpu_addr == invalid<uptr>()) {
            gpu_addr = address_space_base;
            address_space_base += align(range.getSize(), GPU_PAGE_SIZE);
        }

        mmu->enableWriteTracking(ztd::Range<vaddr_t>(
            alignDown(range.getBegin(), cpu::GUEST_PAGE_SIZE),
            align(range.getEnd(), cpu::GUEST_PAGE_SIZE)));
        carveOut(gpu_addr, gpu_addr + range.getSize());

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
        return gpu_addr;
    }

    // Reserved GPU address space without guest backing reads as zero, like
    // sparse memory
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    const uptr ptr = reinterpret_cast<uptr>(calloc(1, range.getSize()));

    AddressSpace as;
    as.ptr = ptr;
    as.size = range.getSize();

    if (gpu_addr == invalid<uptr>()) {
        gpu_addr = address_space_base;
        address_space_base += align(range.getSize(), GPU_PAGE_SIZE);
        mapLocked(gpu_addr, as);
    } else {
        mapIntoGaps(gpu_addr, as);
    }

    return gpu_addr;
}

void GMmu::carveOut(uptr begin, uptr end) {
    for (auto it = mapped_ranges.begin(); it != mapped_ranges.end();) {
        const uptr other_begin = it->first;
        const AddressSpace other = it->second;
        const uptr other_end = other_begin + other.size;
        if (other_end <= begin || other_begin >= end) {
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
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    mapLocked(gpu_addr, {reinterpret_cast<uptr>(calloc(1, size)), size});
}

} // namespace hydra::hw::tegra_x1::gpu
