#include "core/hw/tegra_x1/cpu/hypervisor/stage2_tracker.hpp"

#include "core/hw/tegra_x1/cpu/hypervisor/physical_memory.hpp"

namespace hydra::hw::tegra_x1::cpu::hypervisor {

template <typename F>
void Stage2Tracker::forEachPage(ztd::Range<uptr> range, F&& func) {
    if (range.getSize() == 0)
        return;
    const uptr begin = alignDown(range.getBegin(), static_cast<uptr>(APPLE_PAGE_SIZE));
    const uptr end = align(range.getEnd(), static_cast<uptr>(APPLE_PAGE_SIZE));
    for (uptr host = begin; host < end; host += APPLE_PAGE_SIZE) {
        // Host memory that isn't guest memory can't be accessed by the guest
        const paddr_t page = PhysicalMemory::instance().tryFromHost(host);
        if (page != 0x0)
            func(page);
    }
}

void Stage2Tracker::applyProtection(paddr_t page, const PageState& state) {
    hv_memory_flags_t flags = HV_MEMORY_EXEC;
    if (!state.gpu_written) {
        flags |= HV_MEMORY_READ;
        if (!state.write_armed)
            flags |= HV_MEMORY_WRITE;
    }
    HV_ASSERT_SUCCESS(hv_vm_protect(page, APPLE_PAGE_SIZE, flags));
}

void Stage2Tracker::setWriteTracked(ztd::Range<uptr> range, bool tracked) {
    std::scoped_lock lock(mutex);
    forEachPage(range, [&](paddr_t page) {
        auto& state = pages[page];
        state.write_tracked = tracked;
        state.write_armed = tracked;
        applyProtection(page, state);
    });
}

void Stage2Tracker::protectGpuWritten(ztd::Range<uptr> range) {
    std::scoped_lock lock(mutex);
    forEachPage(range, [&](paddr_t page) {
        auto& state = pages[page];
        state.gpu_epoch++;
        if (state.gpu_written)
            return;
        state.gpu_written = true;
        applyProtection(page, state);
    });
}

void Stage2Tracker::rearmWriteTracking() {
    std::scoped_lock lock(mutex);
    for (const auto page : written_pages) {
        auto& state = pages[page];
        if (!state.write_tracked || state.write_armed)
            continue;
        state.write_armed = true;
        applyProtection(page, state);
    }
    written_pages.clear();
}

bool Stage2Tracker::handleFault(paddr_t address, bool is_write) {
    const paddr_t page = alignDown(address, APPLE_PAGE_SIZE);
    bool flush;
    u32 gpu_epoch;
    Callbacks crnt_callbacks;
    {
        std::scoped_lock lock(mutex);
        const auto it = pages.find(page);
        if (it == pages.end())
            return false;
        // Another thread may have handled the fault already, retry then
        flush = it->second.gpu_written;
        gpu_epoch = it->second.gpu_epoch;
        crnt_callbacks = callbacks;
    }

    const uptr host = PhysicalMemory::instance().toHost(page);
    const auto host_range = ztd::Range<uptr>::fromSize(host, APPLE_PAGE_SIZE);
    // The page stays inaccessible until guest memory is up to date
    if (flush && crnt_callbacks.flush_gpu_writes)
        crnt_callbacks.flush_gpu_writes(host_range);
    if (is_write && crnt_callbacks.invalidate_gpu_caches)
        crnt_callbacks.invalidate_gpu_caches(host_range);

    std::scoped_lock lock(mutex);
    auto& state = pages[page];
    if (flush && state.gpu_epoch == gpu_epoch)
        state.gpu_written = false;
    fault_count++;
    if (flush)
        flush_count++;
    if (fault_count % 2000 == 0)
        LOG_INFO(Hypervisor, "Stage 2 tracking: {} faults, {} GPU write backs",
                 fault_count, flush_count);
    if (is_write && state.write_tracked) {
        state.write_armed = false;
        written_pages.push_back(page);
    }
    applyProtection(page, state);
    return true;
}

} // namespace hydra::hw::tegra_x1::cpu::hypervisor
