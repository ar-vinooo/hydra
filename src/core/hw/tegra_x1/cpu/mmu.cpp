#include "core/hw/tegra_x1/cpu/mmu.hpp"

#include "core/system.hpp"

namespace hydra::hw::tegra_x1::cpu {

horizon::kernel::MemoryInfo IMmu::queryMemory(vaddr_t va) const {
    horizon::kernel::MemoryInfo info;
    info.size = 0x0;

    // Resize to the left
    auto region = queryRegion(va);
    do {
        // Resize
        info.addr = region.va;
        info.size = region.size;
        info.state = region.state;
        if (info.addr == 0x0)
            break;

        // Next
        region = queryRegion(info.addr - 1);
    } while (region.state == info.state);

    // Resize to the right
    region = queryRegion(info.addr + info.size);
    while (region.state == info.state) {
        // Resize
        info.size += region.size;

        // Next
        vaddr_t addr = info.addr + info.size;
        if (addr >= horizon::kernel::ADDRESS_SPACE.getEnd())
            break;

        region = queryRegion(addr);
    }

    // HACK
    if (info.state.type == horizon::kernel::MemoryType::Free) {
        info.ipc_ref_count = 0;
        info.device_ref_count = 0;
    } else {
        info.ipc_ref_count = 1;
        info.device_ref_count = 1;
    }

    return info;
}

vaddr_t IMmu::findFreeMemory(ztd::Range<vaddr_t> region, u64 size) const {
    size = align(size, GUEST_PAGE_SIZE);
    auto crnt_region = ztd::Range<vaddr_t>::fromSize(region.getBegin(), size);
    while (region.contains(crnt_region)) {
        const auto info = queryMemory(crnt_region.getBegin());
        const auto mem_range = ztd::Range<vaddr_t>(
            std::max(info.addr, region.getBegin()), info.addr + info.size);
        if (info.state.type == horizon::kernel::MemoryType::Free &&
            mem_range.contains(crnt_region))
            return mem_range.getBegin();

        crnt_region += mem_range.getSize();
    }

    return 0x0;
}

bool IMmu::trackWrite(ztd::Range<vaddr_t> range) {
    const auto aligned_range =
        ztd::Range<vaddr_t>(alignDown(range.getBegin(), GUEST_PAGE_SIZE),
                            align(range.getEnd(), GUEST_PAGE_SIZE));
    if (!trySuspendWriteTracking(aligned_range))
        return false;

    // Notify the GPU. Queued: invalidating here would wait for the cache
    // locks that the GPU thread holds for whole draws.
    // TODO: what about non-contiguous regions?
    const auto ptr = unmapAddr(aligned_range.getBegin());
    {
        std::scoped_lock lock(write_tracking_mutex);
        tracked_pages.push_back(aligned_range);
        pending_invalidations.push_back(
            ztd::Range<uptr>::fromSize(ptr, aligned_range.getSize()));
        has_pending_invalidations.store(true, std::memory_order_release);
    }

    return true;
}

void IMmu::applyPendingInvalidations() {
    if (!has_pending_invalidations.load(std::memory_order_acquire))
        return;
    std::vector<ztd::Range<uptr>> ranges;
    {
        std::scoped_lock lock(write_tracking_mutex);
        ranges.swap(pending_invalidations);
        has_pending_invalidations.store(false, std::memory_order_relaxed);
    }
    auto& renderer = system.getGpu().getRenderer();
    for (const auto& range : ranges)
        renderer.invalidateMemory(range);
}

void IMmu::flushTrackedPages() {
    // Pending invalidations are applied by PFIFO before each entry: this is
    // called with the cache mutexes held, so invalidating here would deadlock.
    // Ranges stay queued, so a write after the resume is not lost
    std::scoped_lock lock(write_tracking_mutex);
    for (const auto& range : tracked_pages)
        resumeWriteTracking(range);
    tracked_pages.clear();
}

} // namespace hydra::hw::tegra_x1::cpu
