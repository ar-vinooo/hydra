#include "core/hw/tegra_x1/cpu/hypervisor/thread.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/stage2_tracker.hpp"

#include <mach/mach_time.h>
#include <thread>

#include "core/debugger/debugger_manager.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/cpu.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/mmu.hpp"
#include "core/hw/tegra_x1/cpu/register_write_filter.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/wall_clock.hpp"

#define MMU (*static_cast<Mmu*>(mmu))

namespace hydra::hw::tegra_x1::cpu::hypervisor {

namespace {

constexpr u32 PSTATE_SS = (1u << 21);
constexpr u32 MDSCR_EL1_SS = (1u << 0);
constexpr u32 MDSCR_EL1_MDE = (1u << 15);

constexpr u32 DBGBCR_E = (1u << 0);     // Enable bit
constexpr u32 DBGBCR_BAS = (0xfu << 5); // Byte address select

constexpr u64 INTERRUPT_TIME = 16ull * 1000ull * 1000ull; // 16ms

enum class ExceptionClass {
    Unknown = 0b000000,
    TrappedWfeWfiWfetWfit = 0b000001,
    TrappedMcrMrcCp15 = 0b000011,
    TrappedMcrrMrrcCp15 = 0b000100,
    TrappedMcrMrcCp14 = 0b000101,
    TrappedLdcStc = 0b000110,
    TrappedSveFpSimd = 0b000111,
    TrappedVmrs = 0b001000,
    TrappedPAuth = 0b001001,
    TrappedLd64bSt64bSt64bvSt64bv0 = 0b001010,
    TrappedMrrcCp14 = 0b001100,
    IllegalExecutionState = 0b001110,
    SvcAarch32 = 0b010001,
    HvcAarch32 = 0b010010,
    SmcAarch32 = 0b010011,
    SvcAarch64 = 0b010101,
    HvcAarch64 = 0b010110,
    SmcAarch64 = 0b010111,
    TrappedMsrMrsSystem = 0b011000,
    TrappedSve = 0b011001,
    TrappedEretEretaaEretab = 0b011010,
    PointerAuthenticationFailure = 0b011100,
    ImplementationDefinedEl3 = 0b011111,
    InstructionAbortLowerEl = 0b100000,
    InstructionAbortSameEl = 0b100001,
    PcAlignmentFault = 0b100010,
    DataAbortLowerEl = 0b100100,
    DataAbortSameEl = 0b100101,
    SpAlignmentFault = 0b100110,
    TrappedFpExceptionAarch32 = 0b101000,
    TrappedFpExceptionAarch64 = 0b101100,
    SErrorInterrupt = 0b101111,
    BreakpointLowerEl = 0b110000,
    BreakpointSameEl = 0b110001,
    SoftwareStepLowerEl = 0b110010,
    SoftwareStepSameEl = 0b110011,
    WatchpointLowerEl = 0b110100,
    WatchpointSameEl = 0b110101,
    BkptAarch32 = 0b111000,
    VectorCatchAarch32 = 0b111010,
    BrkAarch64 = 0b111100,
};

} // namespace

} // namespace hydra::hw::tegra_x1::cpu::hypervisor

ENABLE_ENUM_FORMATTING(
    hydra::hw::tegra_x1::cpu::hypervisor::ExceptionClass, Unknown, "unknown",
    TrappedWfeWfiWfetWfit, "trapped WFE WFI WFET WFIT", TrappedMrrcCp14,
    "trapped MRRC CP14", IllegalExecutionState, "illegal execution state",
    SvcAarch32, "svc AArch32", HvcAarch32, "hvc AArch32", SmcAarch32,
    "smc AArch32", SvcAarch64, "svc AArch64", HvcAarch64, "hvc AArch64",
    SmcAarch64, "smc AArch64", TrappedMsrMrsSystem, "trapped MSR/MRS system",
    TrappedSve, "trapped SVE", TrappedEretEretaaEretab,
    "trapped ERET/ERETAA/ERETAB", PointerAuthenticationFailure,
    "pointer authentication failure", ImplementationDefinedEl3,
    "implementation defined EL3", InstructionAbortLowerEl,
    "instruction abort lower EL", InstructionAbortSameEl,
    "instruction abort same EL", PcAlignmentFault, "PC alignment fault",
    DataAbortLowerEl, "data abort lower EL", DataAbortSameEl,
    "data abort same EL", SpAlignmentFault, "SP alignment fault",
    TrappedFpExceptionAarch32, "trapped FP exception AArch32",
    TrappedFpExceptionAarch64, "trapped FP exception AArch64", SErrorInterrupt,
    "SError interrupt", BreakpointLowerEl, "breakpoint lower EL",
    BreakpointSameEl, "breakpoint same EL", SoftwareStepLowerEl,
    "software step lower EL", SoftwareStepSameEl, "software step same EL",
    WatchpointLowerEl, "watchpoint lower EL", WatchpointSameEl,
    "watchpoint same EL", BkptAarch32, "BKPT AArch32", VectorCatchAarch32,
    "vector catch AArch32", BrkAarch64, "BRK AArch64")

namespace hydra::hw::tegra_x1::cpu::hypervisor {

namespace {

std::atomic<u32> live_vcpu_count{0};

// Start giving vCPUs back well before the hard limit of 64.
// HYDRA_VCPU_RELEASE_THRESHOLD overrides it (0 exercises the path always).
u32 getVcpuReleaseThreshold() {
    static const u32 threshold = [] {
        if (const char* env = std::getenv("HYDRA_VCPU_RELEASE_THRESHOLD"))
            return static_cast<u32>(std::atoi(env));
        return 40u;
    }();
    return threshold;
}

bool mayBlock(u64 svc_id) {
    switch (svc_id) {
    case 0x0b: // SleepThread
    case 0x18: // WaitSynchronization
    case 0x1a: // ArbitrateLock
    case 0x1c: // WaitProcessWideKeyAtomic
    case 0x21: // SendSyncRequest
    case 0x34: // WaitForAddress
    case 0x43: // ReplyAndReceive
        return true;
    default:
        return false;
    }
}

} // namespace

Thread::Thread(WallClock& wall_clock, Cpu& cpu_, IMmu* mmu,
               const ThreadCallbacks& callbacks, IMemory* tls_mem,
               vaddr_t tls_mem_base_)
    : IThread(wall_clock, mmu, callbacks, tls_mem), cpu{cpu_},
      tls_mem_base{tls_mem_base_} {
    // VTimer
    struct mach_timebase_info info;
    mach_timebase_info(&info);

    interrupt_time_delta_ticks =
        ((INTERRUPT_TIME * info.denom) + (info.numer - 1)) / info.numer;

    // DEBUG: HYDRA_HOOK_PCS=0x...,0x... logs the first hit of each address on
    // every thread (one-shot hardware breakpoints, at most 6)
    if (const char* hooks = std::getenv("HYDRA_HOOK_PCS")) {
        std::string_view list(hooks);
        u32 slot = 0;
        while (!list.empty() && slot < 6) {
            const auto comma = list.find(',');
            const auto item = list.substr(0, comma);
            breakpoints[slot++] = std::strtoull(std::string(item).c_str(), nullptr, 16);
            list = comma == std::string_view::npos ? "" : list.substr(comma + 1);
        }
    }

    createVcpu();

    // PSTATE until the first run writes `state`: EL0t (M = 0) with D, A, I
    // and F masked (0x3c0)
    setReg(HV_REG_CPSR, 0x3c0);
}

Thread::~Thread() { releaseVcpu(false); }

void Thread::createVcpu() {
    // vCPUs may be exhausted for a moment; wait for a blocked thread to give
    // one back
    while (true) {
        const auto res = hv_vcpu_create(&vcpu, &exit, nullptr);
        if (res == HV_SUCCESS)
            break;
        ASSERT(res == HV_NO_RESOURCES, Hypervisor,
               "hv_vcpu_create(&vcpu, &exit, nullptr) failed: 0x{:x}",
               u64(res));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    has_vcpu = true;
    live_vcpu_count++;

    setSysReg(HV_SYS_REG_MAIR_EL1, 0xfful);
    setSysReg(HV_SYS_REG_TCR_EL1, 0x12B5193519ul);
    setSysReg(HV_SYS_REG_SCTLR_EL1, 0x34D5D925ul);

    // Enable FP and SIMD instructions
    setSysReg(HV_SYS_REG_CPACR_EL1, 0b11 << 20);

    // Trap EL0 counter, frequency and timer accesses (EL0PCTEN, EL0VCTEN,
    // EL0VTEN and EL0PTEN clear), so that both CNTPCT_EL0 and CNTVCT_EL0 are
    // emulated at GUEST_CNTFRQ instead of exposing the host counter
    setSysReg(HV_SYS_REG_CNTKCTL_EL1, 0x0);

    // Trampoline
    setSysReg(HV_SYS_REG_VBAR_EL1, KERNEL_REGION_BASE);

    setSysReg(HV_SYS_REG_TTBR0_EL1, MMU.getUserPageTable().getBase());
    setSysReg(HV_SYS_REG_TTBR1_EL1, cpu.getKernelPageTable().getBase());

    // Setup TLS pointer
    setSysReg(HV_SYS_REG_TPIDRRO_EL0, tls_mem_base);

    // Trap debug access
    HV_ASSERT_SUCCESS(hv_vcpu_set_trap_debug_exceptions(vcpu, true));
    // HYP_ASSERT_SUCCESS(hv_vcpu_set_trap_debug_reg_accesses(vcpu, true));

    // Registers that are not part of `state`
    setReg(HV_REG_PC, saved_pc);
    setSysReg(HV_SYS_REG_SPSR_EL1, saved_spsr);
    setSysReg(HV_SYS_REG_TPIDR_EL0, saved_tpidr);

    // Breakpoints
    bool any_breakpoint = false;
    for (u32 slot = 0; slot < MAX_BREAKPOINTS; ++slot) {
        if (breakpoints[slot] == 0x0)
            continue;
        any_breakpoint = true;
        setSysReg(hv_sys_reg_t(HV_SYS_REG_DBGBVR0_EL1 + slot * 8),
                  breakpoints[slot]);
        setSysReg(hv_sys_reg_t(HV_SYS_REG_DBGBCR0_EL1 + slot * 8),
                  DBGBCR_E | (0x3 << 1) | DBGBCR_BAS | (0x0 << 20));
    }
    if (any_breakpoint)
        setSysReg(HV_SYS_REG_MDSCR_EL1,
                  getSysReg(HV_SYS_REG_MDSCR_EL1) | MDSCR_EL1_MDE);

    setupVTimer();

    // Every register has to be written on the next run
    exit_register_state.reset();
}

void Thread::releaseVcpu(bool save_state) {
    if (!has_vcpu)
        return;
    // A new vCPU may run on a core with older translations
    tlb_generation_seen = 0;

    // Only the owning thread may access the registers
    if (save_state) {
        saved_pc = getReg(HV_REG_PC);
        saved_spsr = getSysReg(HV_SYS_REG_SPSR_EL1);
        saved_tpidr = getSysReg(HV_SYS_REG_TPIDR_EL0);
    }

    hv_vcpu_destroy(vcpu);
    has_vcpu = false;
    live_vcpu_count--;
}

void Thread::run() {
    // Main run loop
    while (true) {
        if (!has_vcpu)
            createVcpu();
        processMessages();

        // Run
        deserializeState();
        const bool perf = gpu::PerfStats::isEnabled();
        const auto run_start = perf ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
        HV_ASSERT_SUCCESS(hv_vcpu_run(vcpu));
        if (perf)
            countExit(run_start);
        exception = (getReg(HV_REG_PC) >= cpu.getKernelPageTable().getBase());
        serializeState(canSkipSimdOnExit());

        // Handle exit
        if (exit->reason == HV_EXIT_REASON_EXCEPTION) {
            u64 syndrome = exit->exception.syndrome;
            const auto hv_ec =
                static_cast<ExceptionClass>((syndrome >> 26) & 0x3f);

            switch (hv_ec) {
            case ExceptionClass::HvcAarch64: {
                const auto esr =
                    static_cast<u32>(getSysReg(HV_SYS_REG_ESR_EL1));
                const auto ec = static_cast<ExceptionClass>((esr >> 26) & 0x3f);
                const u64 far = getSysReg(HV_SYS_REG_FAR_EL1);

                switch (ec) {
                case ExceptionClass::SvcAarch64:
                    // Don't hold on to a vCPU while blocked if they run low
                    if (live_vcpu_count >= getVcpuReleaseThreshold() &&
                        mayBlock(esr & 0xffff)) {
                        // The registers are recreated from `state`
                        serializeSimd();
                        releaseVcpu();
                    }
                    if (perf) {
                        const auto svc_id = (esr & 0xffff) & 0x7f;
                        const auto svc_start = std::chrono::steady_clock::now();
                        callbacks.svc_handler(this, esr & 0xffff);
                        auto& stats = gpu::PerfStats::instance();
                        stats.svc_count[svc_id]++;
                        stats.svc_ns[svc_id] += static_cast<u64>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - svc_start)
                                .count());
                    } else {
                        callbacks.svc_handler(this, esr & 0xffff);
                    }
                    break;
                case ExceptionClass::TrappedMsrMrsSystem: {
                    instructionTrap(esr);

                    state.pc += 4;
                    break;
                }
                case ExceptionClass::DataAbortLowerEl: {
                    // The access size, when the syndrome holds it (ISV,
                    // SAS); pair and vector accesses do not report one
                    const bool size_valid = (esr & (1u << 24)) != 0;
                    const u64 access_size =
                        size_valid ? (1ull << ((esr >> 22) & 0x3)) : 16;
                    if (far < ADDRESS_SPACE_SIZE &&
                        MMU.trackWrite(
                            ztd::Range<vaddr_t>::fromSize(far, access_size))) {
                        // The page is writable now, this core may still
                        // hold it read-only
                        local_tlb_flush = true;
                        break;
                    }

                    // A real fault: the debugger may show every register
                    serializeSimd();
                    bool far_valid = (esr & 0x00000400) == 0;
                    ASSERT_DEBUG(far_valid, Hypervisor, "FAR not valid");

                    if (!CONFIG_INSTANCE.getRecoverFromSegfault() &&
                        deliverUserException(esr, far))
                        break;
                    if (CONFIG_INSTANCE.getRecoverFromSegfault()) {
                        LOG_ERROR(
                            Hypervisor,
                            "Data abort (PC: 0x{:08x}, address: 0x{:08x})",
                            state.pc, far);

                        // Just move on to the next instruction
                        state.pc += 4;
                    } else {
                        GET_CURRENT_PROCESS_DEBUGGER().breakOnThisThread(
                            "Data abort (PC: 0x{:08x}, address: 0x{:08x})",
                            state.pc, far);
                    }
                    break;
                }
                default:
                    // Undefined instructions, alignment faults, instruction
                    // aborts, ...: Horizon hands them to the process
                    serializeSimd();
                    if (deliverUserException(esr, far))
                        break;
                    LOG_ERROR(
                        Hypervisor,
                        "Unknown HVC code (EC: {}, ESR: 0x{:08x}, PC: "
                        "0x{:08x}, FAR: "
                        "0x{:08x}, VA: 0x{:08x}, PA: 0x{:08x}, instruction: "
                        "0x{:08x})",
                        ec, esr, state.pc, getSysReg(HV_SYS_REG_FAR_EL1),
                        exit->exception.virtual_address,
                        exit->exception.physical_address,
                        MMU.read<u32>(state.pc));

                    GET_CURRENT_PROCESS_DEBUGGER().breakOnThisThread(
                        "unknown HVC code");
                    break;
                }

                // Return through the trampoline, invalidating TLBs as needed
                const auto trampoline = selectTrampoline();
                if (has_vcpu)
                    setReg(HV_REG_PC, trampoline);
                else
                    saved_pc = trampoline;
                break;
            }
            case ExceptionClass::TrappedWfeWfiWfetWfit:
                // Guest spin loops wait with WFE/WFI, which get trapped.
                // There is no event to wait for, so give the host core away
                // and continue after the instruction.
                std::this_thread::yield();
                state.pc += 4;
                break;
            case ExceptionClass::SmcAarch64:
                LOG_FATAL(Hypervisor, "SMC");
                break;
            case ExceptionClass::BrkAarch64:
                // Without a debugger, BRK is a user exception on Horizon
                if (deliverUserException(static_cast<u32>(syndrome), 0x0))
                    break;
                GET_CURRENT_PROCESS_DEBUGGER().breakOnThisThread(
                    "BRK instruction");
                return;
            case ExceptionClass::DataAbortLowerEl: {
                // Stage 2 permission fault from memory tracking. The access
                // is retried once the page is accessible.
                const bool is_write = (syndrome & (1ull << 6)) != 0;
                if (Stage2Tracker::instance().handleFault(
                        exit->exception.physical_address, is_write))
                    break;
                LOG_FATAL(Hypervisor,
                          "Unhandled stage 2 data abort (PC: {:#x}, VA: {:#x}, "
                          "PA: {:#x}, ESR: {:#x})",
                          getReg(HV_REG_PC), exit->exception.virtual_address,
                          exit->exception.physical_address, syndrome);
                break;
            }
            case ExceptionClass::BreakpointLowerEl: {
                // DEBUG: one-shot hooks from HYDRA_HOOK_PCS
                static const bool hook_mode =
                    std::getenv("HYDRA_HOOK_PCS") != nullptr;
                if (hook_mode) {
                    const u64 pc = getReg(HV_REG_PC);
                    LOG_INFO(Hypervisor,
                             "DEBUG HOOK pc={:#x} lr={:#x} x0={:#x} x1={:#x} "
                             "x2={:#x} x3={:#x}",
                             pc, state.lr, state.r[0], state.r[1], state.r[2],
                             state.r[3]);
                    // Disable the hit breakpoint, step one instruction and
                    // re-enable it so every call is logged
                    for (u32 slot = 0; slot < MAX_BREAKPOINTS; ++slot) {
                        if (breakpoints[slot] != pc)
                            continue;
                        setSysReg(hv_sys_reg_t(HV_SYS_REG_DBGBCR0_EL1 + slot * 8),
                                  0x0);
                        hook_rearm_slot = slot;
                        setSysReg(HV_SYS_REG_MDSCR_EL1,
                                  getSysReg(HV_SYS_REG_MDSCR_EL1) | MDSCR_EL1_SS);
                        state.pstate |= PSTATE_SS;
                    }
                    break;
                }
                // Callback
                callbacks.breakpoint_hit();
                break;
            }
            case ExceptionClass::SoftwareStepLowerEl: {
                // DEBUG: re-arm a HYDRA_HOOK_PCS breakpoint
                if (hook_rearm_slot < MAX_BREAKPOINTS && !exception) {
                    const auto slot = hook_rearm_slot;
                    hook_rearm_slot = MAX_BREAKPOINTS;
                    setSysReg(HV_SYS_REG_MDSCR_EL1,
                              getSysReg(HV_SYS_REG_MDSCR_EL1) & ~u64(MDSCR_EL1_SS));
                    setSysReg(hv_sys_reg_t(HV_SYS_REG_DBGBCR0_EL1 + slot * 8),
                              DBGBCR_E | (0x3 << 1) | DBGBCR_BAS | (0x0 << 20));
                    break;
                }
                // HACK: single-stepping in exceptions behaves very weirdly
                if (exception) {
                    state.pstate |= PSTATE_SS;
                    break;
                }

                // Disable SW step
                u64 mdscr_el1 = getSysReg(HV_SYS_REG_MDSCR_EL1);
                mdscr_el1 &= ~MDSCR_EL1_SS;
                setSysReg(HV_SYS_REG_MDSCR_EL1, mdscr_el1);

                // Callback
                callbacks.supervisor_pause();
                break;
            }
            default:
                GET_CURRENT_PROCESS_DEBUGGER().breakOnThisThread(
                    "Unexpected VM exception 0x{:08x} (EC: {}, ESR: "
                    "0x{:08x}, PC: 0x{:08x}, "
                    "VA: "
                    "0x{:08x}, PA: 0x{:08x}, "
                    "instruction: "
                    "0x{:08x})",
                    syndrome, hv_ec, getSysReg(HV_SYS_REG_ESR_EL1), state.pc,
                    exit->exception.virtual_address,
                    exit->exception.physical_address, MMU.read<u32>(state.pc));
                break;
            }
        } else if (exit->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
            updateVTimer();
        } else {
            // TODO: don't cast to u32
            LOG_ERROR(Hypervisor, "Unexpected VM exit reason {}",
                      static_cast<u32>(exit->reason));

            GET_CURRENT_PROCESS_DEBUGGER().breakOnThisThread(
                "unexpected VM exit reason");
            break;
        }

        if (callbacks.stop_requested())
            break;
    }
}

void Thread::countExit(std::chrono::steady_clock::time_point run_start) {
    auto& stats = gpu::PerfStats::instance();
    stats.guest_run_ns += static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - run_start)
            .count());
    auto kind = gpu::CpuExit::Other;
    if (exit->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
        kind = gpu::CpuExit::VTimer;
    } else if (exit->reason == HV_EXIT_REASON_EXCEPTION) {
        const auto hv_ec = static_cast<ExceptionClass>(
            (exit->exception.syndrome >> 26) & 0x3f);
        if (hv_ec == ExceptionClass::DataAbortLowerEl) {
            kind = gpu::CpuExit::Stage2;
        } else if (hv_ec == ExceptionClass::HvcAarch64) {
            const auto esr = static_cast<u32>(getSysReg(HV_SYS_REG_ESR_EL1));
            switch (static_cast<ExceptionClass>((esr >> 26) & 0x3f)) {
            case ExceptionClass::SvcAarch64:
                kind = gpu::CpuExit::Svc;
                break;
            case ExceptionClass::TrappedMsrMrsSystem:
                kind = gpu::CpuExit::SystemRegister;
                break;
            case ExceptionClass::DataAbortLowerEl:
                kind = gpu::CpuExit::DataAbort;
                break;
            default:
                break;
            }
        }
    }
    stats.cpu_exits[static_cast<usize>(kind)]++;
}

bool Thread::deliverUserException(u32 esr, u64 far) {
    if (CONFIG_INSTANCE.getGdbEnabled() || !callbacks.user_exception)
        return false;
    GuestException guest_exception{.esr = esr, .far = far};
    if (exception && has_vcpu) {
        guest_exception.afsr0 =
            static_cast<u32>(getSysReg(HV_SYS_REG_AFSR0_EL1));
        guest_exception.afsr1 =
            static_cast<u32>(getSysReg(HV_SYS_REG_AFSR1_EL1));
    }
    return callbacks.user_exception(this, guest_exception);
}

u64 Thread::selectTrampoline() {
    const auto generation = getTlbGeneration();
    uptr offset = EXCEPTION_TRAMPOLINE_OFFSET;
    if (generation != tlb_generation_seen) {
        // Read before invalidating: a change after this is seen next time
        tlb_generation_seen = generation;
        local_tlb_flush = false;
        offset = EXCEPTION_TRAMPOLINE_SHARED_TLB_OFFSET;
    } else if (local_tlb_flush) {
        local_tlb_flush = false;
        offset = EXCEPTION_TRAMPOLINE_LOCAL_TLB_OFFSET;
    }
    return KERNEL_REGION_BASE + offset;
}

void Thread::setupVTimer() {
    // The vtimer only preempts the guest and runs in host ticks. It compares
    // against the vCPU's virtual count (host count - vtimer offset), so the
    // offset has to be taken into account.
    u64 vtimer_offset;
    HV_ASSERT_SUCCESS(hv_vcpu_get_vtimer_offset(vcpu, &vtimer_offset));
    setSysReg(HV_SYS_REG_CNTV_CTL_EL0, 1);
    setSysReg(HV_SYS_REG_CNTV_CVAL_EL0, mach_absolute_time() - vtimer_offset +
                                            interrupt_time_delta_ticks);
}

void Thread::updateVTimer() {
    setupVTimer();
    hv_vcpu_set_vtimer_mask(vcpu, false);
}

bool Thread::canSkipSimdOnExit() {
    static const bool disable_filter =
        std::getenv("HYDRA_DISABLE_REGISTER_WRITE_FILTER") != nullptr;
    // Without a baseline or filter every register is written back from `state`
    if (disable_filter || !exit_register_state || !has_vcpu)
        return false;
    // The vCPU keeps the live SIMD registers; only exits whose handler may
    // read them (debugging) need them in `state`
    if (exit->reason == HV_EXIT_REASON_VTIMER_ACTIVATED)
        return true;
    if (exit->reason != HV_EXIT_REASON_EXCEPTION)
        return false;
    switch (static_cast<ExceptionClass>((exit->exception.syndrome >> 26) &
                                        0x3f)) {
    case ExceptionClass::DataAbortLowerEl: // Stage 2 memory tracking
        return true;
    case ExceptionClass::HvcAarch64:
        break;
    default:
        return false;
    }
    const auto esr = static_cast<u32>(getSysReg(HV_SYS_REG_ESR_EL1));
    switch (static_cast<ExceptionClass>((esr >> 26) & 0x3f)) {
    case ExceptionClass::SvcAarch64:
    case ExceptionClass::TrappedMsrMrsSystem: // CNTPCT_EL0/CNTFRQ_EL0 reads
    case ExceptionClass::DataAbortLowerEl:    // Stage 1 write tracking
        return true;
    default:
        return false;
    }
}

void Thread::serializeSimd() {
    if (!simd_stale)
        return;
    for (u8 i = 0; i < 32; i++)
        state.v[i] = getSimdFpReg(i);
    if (exit_register_state)
        for (u8 i = 0; i < 32; i++)
            exit_register_state->v[i] = state.v[i];
    simd_stale = false;
}

void Thread::serializeState(bool skip_simd) {
    for (u8 i = 0; i < 29; i++)
        state.r[i] = getReg(hv_reg_t(HV_REG_X0 + i));
    state.fp = getReg(HV_REG_FP);
    state.lr = getReg(HV_REG_LR);
    state.sp = getSysReg(HV_SYS_REG_SP_EL0);
    if (exception)
        state.pc = getSysReg(HV_SYS_REG_ELR_EL1);
    else
        state.pc = getReg(HV_REG_PC);
    state.pstate = static_cast<u32>(getReg(HV_REG_CPSR));
    if (!skip_simd) {
        for (u8 i = 0; i < 32; i++)
            state.v[i] = getSimdFpReg(i);
        simd_stale = false;
    } else {
        simd_stale = true;
    }
    state.fpcr = static_cast<u32>(getReg(HV_REG_FPCR));
    state.fpsr = static_cast<u32>(getReg(HV_REG_FPSR));
    exit_register_state = state;
}

void Thread::deserializeState() {
    static const bool disable_filter =
        std::getenv("HYDRA_DISABLE_REGISTER_WRITE_FILTER") != nullptr;
    const auto writes = restoreRegisterBank(
        state,
        !disable_filter && exit_register_state ? &*exit_register_state
                                               : nullptr,
        [&](u8 index, u64 value) {
            const auto reg = index == 29   ? HV_REG_FP
                             : index == 30 ? HV_REG_LR
                                           : hv_reg_t(HV_REG_X0 + index);
            setReg(reg, value);
        },
        [&](u8 index, u128 value) { setSimdFpReg(index, value); });
    if (gpu::PerfStats::isEnabled()) {
        gpu::PerfStats::instance().register_writes += writes;
        gpu::PerfStats::instance().register_write_skips += 63 - writes;
    }
    // Like the register bank, skip registers the vCPU still holds from the
    // last exit
    const ThreadState* base =
        !disable_filter && exit_register_state ? &*exit_register_state
                                               : nullptr;
    if (!base || state.sp != base->sp)
        setSysReg(HV_SYS_REG_SP_EL0, state.sp);
    if (exception)
        setSysReg(HV_SYS_REG_ELR_EL1, state.pc);
    else
        setReg(HV_REG_PC, state.pc);
    if (!base || state.pstate != base->pstate)
        setReg(HV_REG_CPSR, state.pstate);
    if (!base || state.fpcr != base->fpcr)
        setReg(HV_REG_FPCR, state.fpcr);
    if (!base || state.fpsr != base->fpsr)
        setReg(HV_REG_FPSR, state.fpsr);
}

void Thread::instructionTrap(u32 esr) {
    bool read = (esr & 1) != 0;
    uint rt = (esr >> 5) & 0x1f;

    // Rt 29/30 are FP/LR, which live outside `r`; 31 is XZR
    const auto write_rt = [&](u64 value) {
        if (rt < 29)
            state.r[rt] = value;
        else if (rt == 29)
            state.fp = value;
        else if (rt == 30)
            state.lr = value;
    };

    if (read) {
        // Op0 Op2 Op1 CRn 00000 CRm
        switch ((esr >> 1) & 0x1ffe0f) {
        case 0b11'000'011'1110'00000'0000: // CNTFRQ_EL0
            write_rt(GUEST_CNTFRQ);
            break;
        case 0b11'001'011'1110'00000'0000: // CNTPCT_EL0
        case 0b11'010'011'1110'00000'0000: // CNTVCT_EL0
            // The guest has no virtual offset, so both counters match
            write_rt(wall_clock.getCntpct());
            break;
        default:
            LOG_FATAL(Hypervisor,
                      "Unhandled system register read (ESR: 0x{:08x})", esr);
            break;
        }
    } else {
        LOG_FATAL(Hypervisor, "Unhandled system register write (ESR: 0x{:08x})",
                  esr);
    }
}

void Thread::processMessages() {
    // Checked on every VM exit; debugger messages are rare
    if (!has_messages.load(std::memory_order_acquire))
        return;
    std::scoped_lock lock(msg_mutex);
    has_messages.store(false, std::memory_order_relaxed);
    while (!msg_queue.empty()) {
        auto message = msg_queue.front();
        msg_queue.pop();
        switch (message.type) {
        case ThreadMessageType::InsertBreakpoint: {
            const auto addr = message.payload.insert_breakpoint.addr;

            // Enable breakpoints
            // TODO: only do once?
            u64 mdscr_el1 = getSysReg(HV_SYS_REG_MDSCR_EL1);
            mdscr_el1 |= MDSCR_EL1_MDE;
            setSysReg(HV_SYS_REG_MDSCR_EL1, mdscr_el1);

            // Find a breakpoint slot
            bool found = false;
            for (u32 slot = 0; slot < MAX_BREAKPOINTS; ++slot) {
                if (breakpoints[slot] == 0x0) {
                    breakpoints[slot] = addr;

                    // DBGBVR
                    setSysReg(hv_sys_reg_t(HV_SYS_REG_DBGBVR0_EL1 + slot * 8),
                              addr);

                    // DBGBCR
                    setSysReg(hv_sys_reg_t(HV_SYS_REG_DBGBCR0_EL1 + slot * 8),
                              DBGBCR_E | (0x3 << 1) | DBGBCR_BAS | (0x0 << 20));

                    found = true;
                    break;
                }
            }

            if (!found)
                LOG_ERROR(
                    Hypervisor,
                    "Failed to set breakpoint at address 0x{:08x}: no free "
                    "breakpoint slots",
                    addr);
            break;
        }
        case ThreadMessageType::RemoveBreakpoint: {
            const auto addr = message.payload.remove_breakpoint.addr;

            // Disable breakpoints
            // TODO: disable when no breakpoints are active?
            u64 mdscr_el1 = getSysReg(HV_SYS_REG_MDSCR_EL1);
            mdscr_el1 &= ~MDSCR_EL1_MDE;
            setSysReg(HV_SYS_REG_MDSCR_EL1, mdscr_el1);

            // Find the breakpoint slot
            bool found = false;
            for (u32 slot = 0; slot < MAX_BREAKPOINTS; ++slot) {
                if (breakpoints[slot] == addr) {
                    breakpoints[slot] = 0x0;

                    // DBGBVR
                    setSysReg(hv_sys_reg_t(HV_SYS_REG_DBGBVR0_EL1 + slot * 8),
                              0x0);

                    // DBGBCR
                    setSysReg(hv_sys_reg_t(HV_SYS_REG_DBGBCR0_EL1 + slot * 8),
                              0x0);

                    found = true;
                    break;
                }
            }

            if (!found)
                LOG_ERROR(
                    Hypervisor,
                    "Failed to remove breakpoint at address 0x{:08x}: no such "
                    "breakpoint",
                    addr);
            break;
        }
        case ThreadMessageType::SingleStep: {
            // Enable SW step
            u64 mdscr_el1 = getSysReg(HV_SYS_REG_MDSCR_EL1);
            mdscr_el1 |= MDSCR_EL1_SS;
            setSysReg(HV_SYS_REG_MDSCR_EL1, mdscr_el1);

            // Set PSTATE.SS
            state.pstate |= PSTATE_SS;
            break;
        }
        default:
            LOG_FATAL(Hypervisor, "Unhandled message type");
            break;
        }
    }
}

} // namespace hydra::hw::tegra_x1::cpu::hypervisor
