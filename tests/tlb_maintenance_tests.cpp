// Guest code on a real vCPU sees host page table changes: a write tracked
// page faults again after it is rearmed, and a remapped page reads its new
// memory. Both need the TLB invalidation on returning to the guest.
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "core/horizon/kernel/process.hpp"
#include "core/horizon/kernel/process_manager.hpp"
#include "core/horizon/os.hpp"
#include "core/hw/tegra_x1/cpu/cpu.hpp"
#include "core/hw/tegra_x1/cpu/memory.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/cpu/thread.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/system.hpp"

using namespace hydra;
using namespace hydra::horizon;

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

constexpr u64 PAGE = 0x4000;
constexpr u32 ITERATIONS = 500;

} // namespace

void testTlbMaintenance(System& system) {
    auto* process = system.getOs().getKernel().getProcessManager().createProcess(
        "TLB maintenance test");
    auto* mmu = process->getMmu();

    // L: str x2, [x0]; add x2, x2, #1; ldr x3, [x1]; svc #0x7f; b L
    vaddr_t code_va;
    const auto code = process->createMemory(
        kernel::EXECUTABLE_REGION, PAGE, kernel::MemoryType::Code,
        kernel::MemoryPermission::ReadExecute, code_va);
    const u32 program[] = {0xf9000002, 0x91000442, 0xf9400023, 0xd4000fe1,
                           0x17fffffc};
    std::memcpy(reinterpret_cast<void*>(code), program, sizeof(program));

    vaddr_t data_va;
    const auto data = process->createMemory(
        kernel::HEAP_REGION, PAGE, kernel::MemoryType::Normal_4_0_0,
        kernel::MemoryPermission::ReadWrite, data_va);
    const auto data_range = ztd::Range<vaddr_t>::fromSize(data_va, PAGE);
    mmu->enableWriteTracking(data_range);

    // Two backings for one address, swapped on every iteration
    vaddr_t remap_va;
    const auto first = process->createMemory(
        kernel::HEAP_REGION, PAGE, kernel::MemoryType::Normal_4_0_0,
        kernel::MemoryPermission::ReadWrite, remap_va);
    std::unique_ptr<hw::tegra_x1::cpu::IMemory> second(
        system.getCpu().allocateMemory(PAGE));
    *reinterpret_cast<u64*>(first) = 0x1111;
    *reinterpret_cast<u64*>(second->getPtr()) = 0x2222;
    const kernel::MemoryState state{.type = kernel::MemoryType::Normal_4_0_0,
                                    .attr = kernel::MemoryAttribute::None,
                                    .perm = kernel::MemoryPermission::ReadWrite};
    const auto remap_range = ztd::Range<vaddr_t>::fromSize(remap_va, PAGE);

    auto& versions = system.getGpu().getRenderer().getContentVersions();
    const auto data_host = ztd::Range<uptr>::fromSize(data, PAGE);
    u64 last_version = versions.latest(data_host);

    vaddr_t tls_va;
    auto* tls = process->createTlsMemory(tls_va);
    vaddr_t stack_va;
    process->createMemory(kernel::HEAP_REGION, PAGE,
                          kernel::MemoryType::Normal_4_0_0,
                          kernel::MemoryPermission::ReadWrite, stack_va);

    std::atomic<bool> stop{false};
    u32 iteration = 0;
    bool on_second = false;
    std::string failure;
    std::thread runner([&] {
        try {
            std::unique_ptr<hw::tegra_x1::cpu::IThread> thread(
                system.getCpu().createThread(
                    system.getWallClock(), mmu,
                    {.svc_handler =
                         [&](hw::tegra_x1::cpu::IThread* guest, u64 id) {
                             auto& regs = guest->getState();
                             check(id == 0x7f, "The guest makes its SVC");
                             // The store faulted: its invalidation is queued
                             mmu->applyPendingInvalidations();
                             const auto version = versions.latest(data_host);
                             check(version > last_version,
                                   "Every write after rearming faults again");
                             last_version = version;
                             check(*reinterpret_cast<u64*>(data) ==
                                       u64(iteration) + 1,
                                   "Tracked writes reach memory");
                             check(regs.r[3] ==
                                       (on_second ? 0x2222u : 0x1111u),
                                   "A remapped page reads its new memory");
                             mmu->unmap(remap_range);
                             on_second = !on_second;
                             mmu->map(remap_va,
                                      on_second
                                          ? ztd::Range<uptr>::fromSize(
                                                second->getPtr(), PAGE)
                                          : ztd::Range<uptr>::fromSize(first,
                                                                       PAGE),
                                      state);
                             if (++iteration == ITERATIONS)
                                 stop = true;
                         },
                     .stop_requested = [&] { return stop.load(); },
                     .supervisor_pause = [] {},
                     .breakpoint_hit = [] {}},
                    tls, tls_va));
            auto& regs = thread->getState();
            regs.pc = code_va;
            regs.sp = stack_va + PAGE;
            regs.r[0] = data_va;
            regs.r[1] = remap_va;
            regs.r[2] = 1;
            thread->run();
        } catch (const std::exception& error) {
            failure = error.what();
            stop = true;
        }
    });
    runner.join();
    if (!failure.empty())
        throw std::runtime_error(failure);
    check(iteration == ITERATIONS, "The guest ran every iteration");
    std::cout << "PASS: guest writes fault again after rearming and remapped "
                 "pages read their new memory (" << ITERATIONS
              << " iterations)\n";
}
