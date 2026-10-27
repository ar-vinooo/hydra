// RegisterSync (cpu/register_sync.hpp) against a reference model: a fake vCPU
// whose registers the "guest" changes between exits, exits that read random
// groups (None is a fast exit), handlers that change the registers they may
// read, complete() and vCPU recreation. The vCPU must always end up with
// exactly the host's changes, and the host must never see a stale register.
#include "core/hw/tegra_x1/cpu/register_sync.hpp"

#include <iostream>
#include <random>

using namespace hydra;
using namespace hydra::hw::tegra_x1::cpu;

namespace {

// Field indices: 0-28 X, 29 FP, 30 LR, 31 SP, 32 PC, 33 PSTATE, 34 FPCR,
// 35 FPSR, 36-67 V
constexpr int FIELDS = 68;

RegisterGroup groupOf(int f) {
    if (f < 8 || (f >= 29 && f <= 33))
        return RegisterGroup::Low;
    if (f < 29 || f == 34 || f == 35)
        return RegisterGroup::High;
    return RegisterGroup::Simd;
}

u128 get(const ThreadState& s, int f) {
    if (f < 29)
        return s.r[f];
    switch (f) {
    case 29:
        return s.fp;
    case 30:
        return s.lr;
    case 31:
        return s.sp;
    case 32:
        return s.pc;
    case 33:
        return s.pstate;
    case 34:
        return s.fpcr;
    case 35:
        return s.fpsr;
    default:
        return s.v[f - 36];
    }
}

void set(ThreadState& s, int f, u128 value) {
    if (f < 29) {
        s.r[f] = static_cast<u64>(value);
        return;
    }
    switch (f) {
    case 29:
        s.fp = static_cast<u64>(value);
        break;
    case 30:
        s.lr = static_cast<u64>(value);
        break;
    case 31:
        s.sp = static_cast<u64>(value);
        break;
    case 32:
        s.pc = static_cast<u64>(value);
        break;
    case 33:
        s.pstate = static_cast<u32>(value);
        break;
    case 34:
        s.fpcr = static_cast<u32>(value);
        break;
    case 35:
        s.fpsr = static_cast<u32>(value);
        break;
    default:
        s.v[f - 36] = value;
        break;
    }
}

struct FakeVcpu {
    ThreadState regs{};
    u64 reads = 0;
};

struct Access {
    FakeVcpu* v;
    u64 general(u8 i) const {
        v->reads++;
        return static_cast<u64>(get(v->regs, i));
    }
    void setGeneral(u8 i, u64 x) const { set(v->regs, i, x); }
    u64 sp() const { return v->reads++, v->regs.sp; }
    void setSp(u64 x) const { v->regs.sp = x; }
    u64 pc() const { return v->reads++, v->regs.pc; }
    void setPc(u64 x) const { v->regs.pc = x; }
    u32 pstate() const { return v->reads++, v->regs.pstate; }
    void setPstate(u32 x) const { v->regs.pstate = x; }
    u32 fpcr() const { return v->reads++, v->regs.fpcr; }
    void setFpcr(u32 x) const { v->regs.fpcr = x; }
    u32 fpsr() const { return v->reads++, v->regs.fpsr; }
    void setFpsr(u32 x) const { v->regs.fpsr = x; }
    u128 simd(u8 i) const { return v->reads++, v->regs.v[i]; }
    void setSimd(u8 i, u128 x) const { v->regs.v[i] = x; }
};

u64 failures = 0;
void check(bool ok, const char* what, long step, int field = -1) {
    if (!ok && failures++ < 20)
        std::cerr << "FAIL step " << step << " field " << field << ": " << what
                  << '\n';
}

u128 randomValue(std::mt19937_64& rng, int f) {
    // Small values make values equal by chance (no write needed) common
    u128 value = (rng() % 4 == 0) ? rng() % 4 : rng();
    if (f >= 36)
        value = (value << 64) | rng();
    if (f >= 33 && f <= 35)
        value &= 0xffffffffu;
    return value;
}

} // namespace

int main() {
    std::mt19937_64 rng(12345);
    FakeVcpu vcpu;
    RegisterSync<Access> sync{Access{&vcpu}};
    ThreadState state{};
    for (int f = 0; f < FIELDS; f++)
        set(state, f, randomValue(rng, f));

    // Changed by the host since the last restore, to this value
    bool pending[FIELDS] = {};
    u128 intended[FIELDS] = {};
    // In a group read since the last exit
    bool readable[FIELDS] = {};
    const RegisterGroup choices[] = {
        RegisterGroup::None, RegisterGroup::Low,
        RegisterGroup::Low | RegisterGroup::Simd,
        RegisterGroup::Low | RegisterGroup::High, RegisterGroup::All};
    const u64 group_reads[] = {0, 13, 13 + 32, 13 + 23, 13 + 23 + 32};

    // A new vCPU holds garbage until the first restore
    sync.reset();
    for (int f = 0; f < FIELDS; f++) {
        set(vcpu.regs, f, randomValue(rng, f));
        pending[f] = true;
        intended[f] = get(state, f);
    }

    for (long step = 0; step < 200000; step++) {
        // The vCPU must get exactly the host's changes
        ThreadState expected = vcpu.regs;
        for (int f = 0; f < FIELDS; f++)
            if (pending[f])
                set(expected, f, intended[f]);
        sync.restore(state);
        for (int f = 0; f < FIELDS; f++) {
            check(get(vcpu.regs, f) == get(expected, f),
                  "vCPU register after restore", step, f);
            pending[f] = false;
        }

        // The guest runs and changes some registers
        for (int f = 0; f < FIELDS; f++)
            if (rng() % 8 == 0)
                set(vcpu.regs, f, randomValue(rng, f));

        const int choice = static_cast<int>(rng() % 5);
        const auto groups = choices[choice];
        const u64 reads_before = vcpu.reads;
        sync.exit(state, groups);
        check(vcpu.reads - reads_before == group_reads[choice],
              "registers read by the exit", step);
        for (int f = 0; f < FIELDS; f++) {
            readable[f] = hasGroup(groups, groupOf(f));
            if (readable[f])
                check(get(state, f) == get(vcpu.regs, f), "read register",
                      step, f);
        }

        // The handler changes registers it may read. Rarely also a stale one
        // (against the contract): the change must still win.
        for (int f = 0; f < FIELDS; f++) {
            if (readable[f] && rng() % 6 == 0) {
                set(state, f, randomValue(rng, f));
                pending[f] = true;
                intended[f] = get(state, f);
            } else if (!readable[f] && rng() % 64 == 0) {
                set(state, f, get(state, f) + 1);
                pending[f] = true;
                intended[f] = get(state, f);
            }
        }

        // Something needs the whole state (debugger, context capture, user
        // exception), then may change any register
        if (rng() % 4 == 0) {
            const u64 reads = vcpu.reads;
            sync.complete(state);
            u64 stale_fields = 0;
            for (int f = 0; f < FIELDS; f++) {
                if (!readable[f])
                    stale_fields++;
                check(get(state, f) ==
                          (pending[f] ? intended[f] : get(vcpu.regs, f)),
                      "register after complete", step, f);
                readable[f] = true;
            }
            check(vcpu.reads - reads == stale_fields,
                  "complete reads only the stale registers", step);
            for (int f = 0; f < FIELDS; f++) {
                if (rng() % 10 == 0) {
                    set(state, f, randomValue(rng, f));
                    pending[f] = true;
                    intended[f] = get(state, f);
                }
            }
            // The vCPU is given back and created again (a blocking SVC while
            // few vCPUs are left)
            if (rng() % 8 == 0) {
                sync.reset();
                for (int f = 0; f < FIELDS; f++) {
                    set(vcpu.regs, f, randomValue(rng, f));
                    pending[f] = true;
                    intended[f] = get(state, f);
                }
            }
        }
    }

    if (failures != 0) {
        std::cerr << "FAIL: " << failures << " mismatches\n";
        return 1;
    }
    std::cout << "PASS: lazy register reads, fast exits, completion and "
                 "restores match the reference model\n";
    return 0;
}
