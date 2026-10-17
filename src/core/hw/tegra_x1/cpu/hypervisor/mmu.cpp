#include "core/hw/tegra_x1/cpu/hypervisor/mmu.hpp"

#include <cstdlib>

#include "core/debugger/debugger_manager.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/const.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/cpu.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/memory.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/stage2_tracker.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/system.hpp"

/*
#define USER_RANGE_MEM_BASE 0x01000000
#define USER_RANGE_MEM_SIZE 0x1000000

#define KERNEL_RANGE_MEM_BASE 0x04000000
#define KERNEL_RANGE_MEM_SIZE 0x1000000
*/

namespace hydra::hw::tegra_x1::cpu::hypervisor {

namespace {

inline ApFlags toApFlags(horizon::kernel::MemoryPermission perm) {
    if (any(perm & horizon::kernel::MemoryPermission::Read)) {
        if (any(perm & horizon::kernel::MemoryPermission::Write)) {
            if (any(perm & horizon::kernel::MemoryPermission::Execute)) {
                return ApFlags::UserReadWriteExecuteKernelReadWrite;
            } else {
                return ApFlags::UserReadWriteKernelReadWrite;
            }
        } else {
            if (any(perm & horizon::kernel::MemoryPermission::Execute)) {
                return ApFlags::UserReadExecuteKernelRead;
            } else {
                return ApFlags::UserReadKernelRead;
            }
        }
    } else {
        if (any(perm & horizon::kernel::MemoryPermission::Write)) {
            if (any(perm & horizon::kernel::MemoryPermission::Execute)) {
                return ApFlags::UserReadWriteExecuteKernelReadWrite; // TODO
            } else {
                return ApFlags::UserReadWriteKernelReadWrite; // TODO
            }
        } else {
            if (any(perm & horizon::kernel::MemoryPermission::Execute)) {
                return ApFlags::UserExecuteKernelRead; // TODO
            } else {
                return ApFlags::UserNoneKernelRead; // TODO
            }
        }
    }
}

// TODO: this is a horrible way to handle this
bool page_table_regions[16] = {false};

paddr_t findFreePageTableRegion() {
    for (u32 i = 0; i < 16; i++) {
        if (!page_table_regions[i]) {
            page_table_regions[i] = true;
            return USER_PAGE_TABLE_REGION_BASE + i * PAGE_TABLE_RESERVED_SIZE;
        }
    }

    LOG_FATAL(Hypervisor, "No free page table region found");
    return 0;
}

void releasePageTableRegion(paddr_t addr) {
    ASSERT(addr >= USER_PAGE_TABLE_REGION_BASE &&
               addr <
                   USER_PAGE_TABLE_REGION_BASE + 16 * PAGE_TABLE_RESERVED_SIZE,
           Hypervisor, "Invalid page table region address 0x{:08x}", addr);
    page_table_regions[(addr - USER_PAGE_TABLE_REGION_BASE) /
                       PAGE_TABLE_RESERVED_SIZE] = false;
}

} // namespace

Mmu::Mmu(System& system)
    : IMmu(system), user_page_table(findFreePageTableRegion()) {
    Stage2Tracker::instance().setCallbacks({
        .flush_gpu_writes =
            [&system](ztd::Range<uptr> range) {
                system.getGpu().getRenderer().flushMemoryForCpu(range);
            },
        .invalidate_gpu_caches =
            [&system](ztd::Range<uptr> range) {
                system.getGpu().getRenderer().invalidateMemory(range);
            },
    });

    // Loader return address
    // TODO: this should be done in a backend agnostic way (perhaps in the
    // kernel?)
    /*
    uptr ret_mem_ptr = physical_memory_ptr + physical_memory_cur;
    user_page_table.Map(0xffff0000, physical_memory_cur, 0x1000,
                        {horizon::kernel::MemoryType::Code,
                         horizon::kernel::MemoryAttribute::None,
                         horizon::kernel::MemoryPermission::Execute},
                        ApFlags::UserExecuteKernelRead);
    physical_memory_cur += 0x1000;

    *reinterpret_cast<u32*>(ret_mem_ptr) = 0xd40000e1; // svcExitProcess
    */
}

Mmu::~Mmu() { releasePageTableRegion(user_page_table.getBase()); }

void Mmu::map(vaddr_t dst_va, ztd::Range<uptr> range,
              const horizon::kernel::MemoryState state) {
    ASSERT_ALIGNMENT(dst_va, GUEST_PAGE_SIZE, Hypervisor, "destination VA");
    ASSERT_ALIGNMENT(range.getSize(), GUEST_PAGE_SIZE, Hypervisor, "size");
    const auto physical =
        PhysicalMemory::instance().fromHost(range.getBegin(), range.getSize());
    user_page_table.map(dst_va,
                        ztd::Range<uptr>::fromSize(physical, range.getSize()),
                        state, toApFlags(state.perm));
}

// The source may be backed by several physical allocations (e.g. memory from
// MapPhysicalMemory), so map each physically contiguous run separately
void Mmu::map(vaddr_t dst_va, ztd::Range<vaddr_t> range) {
    ASSERT_ALIGNMENT(range.getBegin(), GUEST_PAGE_SIZE, Hypervisor, "begin");
    ASSERT_ALIGNMENT(range.getEnd(), GUEST_PAGE_SIZE, Hypervisor, "end");

    vaddr_t run_va = range.getBegin();
    while (run_va < range.getEnd()) {
        const auto region = user_page_table.queryRegion(run_va);
        const paddr_t run_pa = region.unmapAddr(run_va);

        // Extend the run while the next page is physically contiguous and has
        // the same state
        vaddr_t end_va = run_va + GUEST_PAGE_SIZE;
        while (end_va < range.getEnd()) {
            const auto next = user_page_table.queryRegion(end_va);
            if (next.unmapAddr(end_va) != run_pa + (end_va - run_va) ||
                !(next.state == region.state))
                break;
            end_va += GUEST_PAGE_SIZE;
        }

        // TODO: also inherit flags
        user_page_table.map(
            dst_va + (run_va - range.getBegin()),
            ztd::Range<uptr>::fromSize(run_pa, end_va - run_va), region.state,
            toApFlags(region.state.perm));
        run_va = end_va;
    }
}

void Mmu::unmap(ztd::Range<vaddr_t> range) {
    ASSERT_ALIGNMENT(range.getBegin(), GUEST_PAGE_SIZE, Hypervisor, "begin");
    ASSERT_ALIGNMENT(range.getEnd(), GUEST_PAGE_SIZE, Hypervisor, "end");
    user_page_table.unmap(range);
}

void Mmu::protect(ztd::Range<vaddr_t> range,
                  horizon::kernel::MemoryPermission perm) {
    ASSERT_ALIGNMENT(range.getBegin(), GUEST_PAGE_SIZE, Hypervisor, "begin");
    ASSERT_ALIGNMENT(range.getEnd(), GUEST_PAGE_SIZE, Hypervisor, "end");
    user_page_table.setMemoryPermission(range, perm, toApFlags(perm));
}

uptr Mmu::unmapAddr(vaddr_t va) const {
    return PhysicalMemory::instance().toHost(user_page_table.unmapAddr(va));
}

MemoryRegion Mmu::queryRegion(vaddr_t va) const {
    auto region = user_page_table.queryRegion(va);

    return {
        .va = region.va,
        .size = region.size,
        .state = region.state,
    };
}

void Mmu::setMemoryAttribute(ztd::Range<vaddr_t> range,
                             horizon::kernel::MemoryAttribute mask,
                             horizon::kernel::MemoryAttribute value) {
    user_page_table.setMemoryAttribute(range, mask, value);
}

// Stage 2 (hypervisor) tracking is opt-in with HYDRA_STAGE2_TRACKING: it
// also makes GPU writes visible to the CPU, but UE4 games still render wrong
// lighting with it. Guest page table (stage 1) tracking is the default.
static bool useStage1Tracking() {
    static const bool value = std::getenv("HYDRA_STAGE2_TRACKING") == nullptr;
    return value;
}

bool Mmu::protectsGpuWrites() const { return !useStage1Tracking(); }

void Mmu::setWriteTrackingEnabled(ztd::Range<vaddr_t> range, bool enable) {
    ASSERT_ALIGNMENT(range.getBegin(), GUEST_PAGE_SIZE, Hypervisor, "begin");
    ASSERT_ALIGNMENT(range.getEnd(), GUEST_PAGE_SIZE, Hypervisor, "end");
    if (useStage1Tracking()) {
        user_page_table.setWriteTrackingEnabled(range, enable);
        return;
    }
    // Guest pages can be backed by separate host allocations, track each
    // contiguous host run at once
    uptr run_begin = 0x0;
    uptr run_end = 0x0;
    const auto flush_run = [&] {
        if (run_end != run_begin)
            Stage2Tracker::instance().setWriteTracked(
                ztd::Range<uptr>(run_begin, run_end), enable);
        run_begin = run_end = 0x0;
    };
    for (vaddr_t va = range.getBegin(); va < range.getEnd();
         va += GUEST_PAGE_SIZE) {
        const uptr host = unmapAddr(va);
        if (host == 0x0 || host != run_end)
            flush_run();
        if (host == 0x0)
            continue;
        if (run_begin == 0x0)
            run_begin = host;
        run_end = host + GUEST_PAGE_SIZE;
    }
    flush_run();
}

// Stage 1 tracking is not used: stage 2 faults are handled by the VM thread
bool Mmu::trySuspendWriteTracking(ztd::Range<vaddr_t> range) {
    if (useStage1Tracking())
        return user_page_table.trySuspendWriteTracking(range);
    return false;
}

void Mmu::resumeWriteTracking(ztd::Range<vaddr_t> range) {
    if (useStage1Tracking())
        user_page_table.resumeWriteTracking(range);
}

bool Mmu::trackWrite(ztd::Range<vaddr_t> range) {
    if (useStage1Tracking())
        return IMmu::trackWrite(range);
    return false;
}

void Mmu::flushTrackedPages() {
    if (useStage1Tracking()) {
        IMmu::flushTrackedPages();
        return;
    }
    Stage2Tracker::instance().rearmWriteTracking();
}

void Mmu::protectGpuWritten(ztd::Range<uptr> range) {
    if (useStage1Tracking())
        return;
    Stage2Tracker::instance().protectGpuWritten(range);
}

} // namespace hydra::hw::tegra_x1::cpu::hypervisor
