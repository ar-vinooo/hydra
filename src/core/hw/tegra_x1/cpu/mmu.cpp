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

    // DeviceShared follows live GPU mappings, including overlapping aliases.
    info.device_ref_count = 0;
    info.state.attr &= ~horizon::kernel::MemoryAttribute::DeviceShared;
    if (info.state.type != horizon::kernel::MemoryType::Free) {
        const auto physical_region = queryRegion(va);
        const auto host = unmapAddr(va);
        auto left = info.addr;
        auto right = info.addr + info.size;
        std::scoped_lock lock(device_mappings->mutex);
        bool intersects = false;
        for (const auto& [space, spans] : device_mappings->spaces) {
            for (const auto& span : spans) {
                const auto begin = alignDown(span.guest, GUEST_PAGE_SIZE);
                const auto end = align(span.guest + span.size, GUEST_PAGE_SIZE);
                if (end <= info.addr || begin >= info.addr + info.size)
                    continue;
                intersects = true;
                if (begin <= va) left = std::max(left, begin);
                else right = std::min(right, begin);
                if (end <= va) left = std::max(left, end);
                else right = std::min(right, end);
                if (va >= begin && va < end && span.host != 0 &&
                    host == span.host - (span.guest - begin) + (va - begin))
                    ++info.device_ref_count;
            }
        }
        if (intersects) {
            info.addr = std::max(left, physical_region.va);
            info.size = std::min(right, physical_region.va + physical_region.size) - info.addr;
        }
        if (info.device_ref_count != 0)
            info.state.attr |= horizon::kernel::MemoryAttribute::DeviceShared;
    }
    if (info.state.type == horizon::kernel::MemoryType::Free) {
        info.ipc_ref_count = 0;
    } else {
        info.ipc_ref_count = 1;
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
    // locks that the GPU thread holds for whole draws. Guest pages can be
    // backed by separate host allocations, so queue each contiguous host run
    std::vector<ztd::Range<uptr>> host_runs;
    uptr run_begin = 0x0;
    uptr run_end = 0x0;
    for (vaddr_t va = aligned_range.getBegin(); va < aligned_range.getEnd();
         va += GUEST_PAGE_SIZE) {
        const uptr host = unmapAddr(va);
        if (host == 0x0 || host != run_end) {
            if (run_end != run_begin)
                host_runs.push_back(ztd::Range<uptr>(run_begin, run_end));
            run_begin = run_end = host;
        }
        if (host != 0x0)
            run_end = host + GUEST_PAGE_SIZE;
    }
    if (run_end != run_begin)
        host_runs.push_back(ztd::Range<uptr>(run_begin, run_end));
    {
        std::scoped_lock lock(write_tracking_mutex);
        tracked_pages.push_back(aligned_range);
        pending_invalidations.insert(pending_invalidations.end(),
                                     host_runs.begin(), host_runs.end());
        has_pending_invalidations.store(true, std::memory_order_release);
    }

    return true;
}

void IMmu::queueHostInvalidation(ztd::Range<uptr> range) {
    std::scoped_lock lock(write_tracking_mutex);
    pending_invalidations.push_back(range);
    has_pending_invalidations.store(true, std::memory_order_release);
}

void IMmu::applyPendingInvalidations() {
    if (!has_pending_invalidations.load(std::memory_order_acquire))
        return;
    std::vector<ztd::Range<uptr>> ranges;
    {
        std::scoped_lock lock(write_tracking_mutex);
        // Rearm the written pages before taking their invalidations: a write
        // after this faults again and is queued for the next entry. Rearming
        // on every draw instead made each CPU write fault again between
        // draws, although its invalidation is only applied here anyway.
        for (const auto& range : tracked_pages)
            resumeWriteTracking(range);
        tracked_pages.clear();
        ranges.swap(pending_invalidations);
        has_pending_invalidations.store(false, std::memory_order_relaxed);
    }
    auto& renderer = system.getGpu().getRenderer();
    for (const auto& range : ranges)
        renderer.invalidateMemory(range);
}

void IMmu::flushTrackedPages() {
    // Write tracking through trackWrite is rearmed when its invalidations are
    // applied, see applyPendingInvalidations
}

} // namespace hydra::hw::tegra_x1::cpu
