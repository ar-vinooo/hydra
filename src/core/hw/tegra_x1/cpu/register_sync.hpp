#pragma once

#include "core/hw/tegra_x1/cpu/register_write_filter.hpp"

namespace hydra::hw::tegra_x1::cpu {

// Parts of ThreadState that a VM exit may leave unread
enum class RegisterGroup : u8 {
    None = 0,
    // X0-X7 (SVC arguments and results), FP, LR, SP, PC and PSTATE
    Low = 1 << 0,
    // X8-X28, FPCR and FPSR
    High = 1 << 1,
    // V0-V31
    Simd = 1 << 2,
    All = Low | High | Simd,
};

constexpr RegisterGroup operator|(RegisterGroup a, RegisterGroup b) {
    return static_cast<RegisterGroup>(static_cast<u8>(a) | static_cast<u8>(b));
}
constexpr RegisterGroup operator&(RegisterGroup a, RegisterGroup b) {
    return static_cast<RegisterGroup>(static_cast<u8>(a) & static_cast<u8>(b));
}
constexpr RegisterGroup operator~(RegisterGroup a) {
    return static_cast<RegisterGroup>(~static_cast<u8>(a)) & RegisterGroup::All;
}
constexpr bool hasGroup(RegisterGroup groups, RegisterGroup group) {
    return (groups & group) != RegisterGroup::None;
}

// Keeps a ThreadState in sync with a vCPU while reading only the registers
// that each VM exit needs. The other registers stay in the vCPU ("stale")
// until complete() reads them.
//
// The baseline holds what the vCPU had at the last exit or restore. A stale
// register keeps the same old value in the state and in the baseline, so
// restore() leaves the vCPU's value alone. Code must complete() before it
// reads or changes a stale register.
//
// `Vcpu` accesses the registers: general(i) and setGeneral(i, value) for
// X0-X28, FP (29) and LR (30), simd(i) and setSimd(i, value), and sp, pc,
// pstate, fpcr and fpsr with their setters.
template <typename Vcpu>
class RegisterSync {
  public:
    explicit RegisterSync(Vcpu vcpu_) : vcpu{vcpu_} {}

    // A new vCPU: the next restore writes every register
    void reset() {
        has_baseline = false;
        stale = RegisterGroup::None;
    }

    // After a VM exit: reads `groups`, everything else becomes stale
    void exit(ThreadState& state, RegisterGroup groups) {
        if (!has_baseline)
            groups = RegisterGroup::All;
        read(state, groups, false);
        stale = ~groups;
        has_baseline = true;
    }

    // Reads the stale registers, keeping the changes made to the state
    void complete(ThreadState& state) {
        if (stale == RegisterGroup::None)
            return;
        read(state, stale, true);
        stale = RegisterGroup::None;
    }

    // Before running the vCPU: writes what changed in the state. Returns the
    // number of general and SIMD registers written.
    u32 restore(const ThreadState& state) {
        const ThreadState* base = has_baseline ? &baseline : nullptr;
        const u32 writes = restoreRegisterBank(
            state, base,
            [&](u8 index, u64 value) { vcpu.setGeneral(index, value); },
            [&](u8 index, u128 value) { vcpu.setSimd(index, value); });
        if (!base || state.sp != base->sp)
            vcpu.setSp(state.sp);
        if (!base || state.pc != base->pc)
            vcpu.setPc(state.pc);
        if (!base || state.pstate != base->pstate)
            vcpu.setPstate(state.pstate);
        if (!base || state.fpcr != base->fpcr)
            vcpu.setFpcr(state.fpcr);
        if (!base || state.fpsr != base->fpsr)
            vcpu.setFpsr(state.fpsr);
        baseline = state;
        has_baseline = true;
        return writes;
    }

    RegisterGroup getStale() const { return stale; }

  private:
    Vcpu vcpu;
    ThreadState baseline{};
    bool has_baseline{false};
    RegisterGroup stale{RegisterGroup::None};

    // Unless `keep_changes`, the state takes the vCPU's value even where it
    // differs from the baseline
    template <typename T, typename Read>
    static void sync(T& value, T& base, bool keep_changes, Read&& read) {
        const T current = read();
        if (!keep_changes || value == base)
            value = current;
        base = current;
    }

    void read(ThreadState& state, RegisterGroup groups, bool keep_changes) {
        auto& base = baseline;
        if (hasGroup(groups, RegisterGroup::Low)) {
            for (u8 i = 0; i < 8; i++)
                sync(state.r[i], base.r[i], keep_changes,
                     [&] { return vcpu.general(i); });
            sync(state.fp, base.fp, keep_changes,
                 [&] { return vcpu.general(29); });
            sync(state.lr, base.lr, keep_changes,
                 [&] { return vcpu.general(30); });
            sync(state.sp, base.sp, keep_changes, [&] { return vcpu.sp(); });
            sync(state.pc, base.pc, keep_changes, [&] { return vcpu.pc(); });
            sync(state.pstate, base.pstate, keep_changes,
                 [&] { return vcpu.pstate(); });
        }
        if (hasGroup(groups, RegisterGroup::High)) {
            for (u8 i = 8; i < 29; i++)
                sync(state.r[i], base.r[i], keep_changes,
                     [&] { return vcpu.general(i); });
            sync(state.fpcr, base.fpcr, keep_changes,
                 [&] { return vcpu.fpcr(); });
            sync(state.fpsr, base.fpsr, keep_changes,
                 [&] { return vcpu.fpsr(); });
        }
        if (hasGroup(groups, RegisterGroup::Simd)) {
            for (u8 i = 0; i < 32; i++)
                sync(state.v[i], base.v[i], keep_changes,
                     [&] { return vcpu.simd(i); });
        }
    }
};

} // namespace hydra::hw::tegra_x1::cpu
