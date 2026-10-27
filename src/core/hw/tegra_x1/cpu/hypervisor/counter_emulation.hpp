#pragma once

#include <cstring>

// Guest reads of the counter (CNTPCT_EL0 or CNTVCT_EL0) trap from EL0 to the
// exception vector for lower EL synchronous exceptions, since CNTKCTL_EL1 is
// 0. Exiting the VM for each read (Thread::instructionTrap) costs a whole VM
// exit, so the vector emulates them itself: it reads the vCPU's virtual
// counter, scales it like WallClock::getCntpct (the high half of counter *
// factor), writes the target register and returns after the read. Every other
// exception still exits to the host with the guest's registers.
//
// The result is the host's only if the virtual counter equals
// ztd::getSystemTick, which depends on the vtimer offset (see
// Thread::probeCounterEmulation). Until counter_emulation::enable, the read
// syndromes never match, so counter reads exit to the host as before.
// HYDRA_DISABLE_COUNTER_EMULATION keeps it that way.
//
// The vector saves x0 and x1 in TPIDR_EL1 and SP_EL1, which the guest cannot
// access and the host does not use.

namespace hydra::hw::tegra_x1::cpu::hypervisor {

// Offsets in the kernel memory, where the exception vectors start
constexpr uptr LOWER_EL_SYNC_VECTOR_OFFSET = 0x400;
constexpr uptr COUNTER_TABLE_OFFSET = 0x900;
constexpr uptr COUNTER_PROBE_OFFSET = 0xb80;

// The immediate of the probe's HVC
constexpr u32 COUNTER_PROBE_HVC = 0xfffe;

namespace counter_emulation {

// Syndrome of a read (MRS) of CNTPCT_EL0 (op2 1) or CNTVCT_EL0 (op2 2) into
// x0: EC 0x18, IL, op0 3, op1 3, CRn 14, CRm 0
constexpr u32 readSyndrome(u32 op2) {
    return (0x18u << 26) | (1u << 25) | (3u << 20) | (op2 << 17) |
           (3u << 14) | (14u << 10) | 1u;
}

constexpr u32 branch(uptr from, uptr to) {
    return 0x14000000u | (static_cast<u32>((to - from) >> 2) & 0x3ffffffu);
}

constexpr u32 adr(u32 rd, uptr from, uptr to) {
    const auto offset = static_cast<u32>(to - from);
    return 0x10000000u | ((offset & 0x3u) << 29) |
           (((offset >> 2) & 0x7ffffu) << 5) | rd;
}

// ldr xT, <literal>
constexpr u32 ldrLiteral(u32 rt, uptr from, uptr to) {
    return 0x58000000u | ((static_cast<u32>((to - from) >> 2) & 0x7ffffu) << 5) |
           rt;
}

// mrs xT, cntvct_el0
constexpr u32 mrsCntvct(u32 rt) { return 0xd53be040u | rt; }

constexpr u32 umulh(u32 rd, u32 rn, u32 rm) {
    return 0x9bc07c00u | (rm << 16) | (rn << 5) | rd;
}

constexpr u32 BRK = 0xd4200000u; // brk #0

// Checked against GNU as
static_assert(readSyndrome(1) == 0x6232f801u && readSyndrome(2) == 0x6234f801u);
static_assert(branch(0x900, 0xb00) == 0x14000080u &&
              branch(0x92c, 0xb28) == 0x1400007fu);
static_assert(adr(1, 0x440, 0x900) == 0x10002601u);
static_assert(ldrLiteral(0, 0x924, 0xb40) == 0x580010e0u);
static_assert(mrsCntvct(2) == 0xd53be042u && umulh(2, 2, 0) == 0x9bc07c42u);

// At LOWER_EL_SYNC_VECTOR_OFFSET
constexpr uptr EMULATE_OFFSET = LOWER_EL_SYNC_VECTOR_OFFSET + 0x38;
constexpr u32 sync_vector[] = {
    0xd518d080u, // msr tpidr_el1, x0
    0x9100003fu, // mov sp, x1
    0xd5385200u, // mrs x0, esr_el1
    0x12166800u, // and w0, w0, #0xfffffc1f (all but Rt)
    0x18000201u, // ldr w1, cntpct_read
    0x6b01001fu, // cmp w0, w1
    0x54000100u, // b.eq emulate
    0x180001c1u, // ldr w1, cntvct_read
    0x6b01001fu, // cmp w0, w1
    0x540000a0u, // b.eq emulate
    // Anything else: exit to the host
    0xd538d080u, // mrs x0, tpidr_el1
    0x910003e1u, // mov x1, sp
    0xd41fffe2u, // hvc #0xffff
    BRK,
    // emulate: jump to the counter table entry for Rt
    0xd5385200u, // mrs x0, esr_el1
    0xd3452400u, // ubfx x0, x0, #5, #5
    adr(1, EMULATE_OFFSET + 0x8, COUNTER_TABLE_OFFSET), // adr x1, counter_table
    0x8b001021u, // add x1, x1, x0, lsl #4
    0xd61f0020u, // br x1
    BRK,
    // cntpct_read, cntvct_read: enable sets them. All ones until then, which
    // no syndrome with Rt cleared is.
    0xffffffffu,
    0xffffffffu,
};
constexpr uptr READ_SYNDROMES_OFFSET =
    LOWER_EL_SYNC_VECTOR_OFFSET + sizeof(sync_vector) - 2 * sizeof(u32);
// Where the literal loads and the branches above point to
static_assert(READ_SYNDROMES_OFFSET == LOWER_EL_SYNC_VECTOR_OFFSET + 0x50);
static_assert(sync_vector[(EMULATE_OFFSET - LOWER_EL_SYNC_VECTOR_OFFSET) /
                          sizeof(u32)] == 0xd5385200u);
static_assert(sizeof(sync_vector) <= 0x80); // one vector

// The counter table has an entry of 4 instructions for every Rt. x0 and x1
// are saved, so their entries branch out; XZR only skips the read.
constexpr usize TABLE_ENTRY_SIZE = 4 * sizeof(u32);
constexpr uptr X0_OFFSET = COUNTER_TABLE_OFFSET + 32 * TABLE_ENTRY_SIZE;
constexpr uptr X1_OFFSET = X0_OFFSET + 0x14;
constexpr uptr RETURN_OFFSET = X1_OFFSET + 0x14;
constexpr uptr FACTOR_OFFSET = RETURN_OFFSET + 0x18;
constexpr u32 tail[] = {
    // x0 (saved in TPIDR_EL1)
    mrsCntvct(0),                           // mrs x0, cntvct_el0
    ldrLiteral(1, X0_OFFSET + 0x4, FACTOR_OFFSET), // ldr x1, factor
    umulh(0, 0, 1),                         // umulh x0, x0, x1
    0xd518d080u,                            // msr tpidr_el1, x0
    branch(X0_OFFSET + 0x10, RETURN_OFFSET),
    // x1 (saved in SP_EL1)
    mrsCntvct(1),                           // mrs x1, cntvct_el0
    ldrLiteral(0, X1_OFFSET + 0x4, FACTOR_OFFSET), // ldr x0, factor
    umulh(1, 1, 0),                         // umulh x1, x1, x0
    0x9100003fu,                            // mov sp, x1
    branch(X1_OFFSET + 0x10, RETURN_OFFSET),
    // Return after the read, restoring x0 and x1
    0xd5384020u, // mrs x0, elr_el1
    0x91001000u, // add x0, x0, #4
    0xd5184020u, // msr elr_el1, x0
    0xd538d080u, // mrs x0, tpidr_el1
    0x910003e1u, // mov x1, sp
    0xd69f03e0u, // eret
};
static_assert(X0_OFFSET + sizeof(tail) == FACTOR_OFFSET);
static_assert(FACTOR_OFFSET % 8 == 0 && FACTOR_OFFSET + 8 <= COUNTER_PROBE_OFFSET);

// Runs at EL1 with the vtimer offset under test
constexpr u32 probe[] = {
    mrsCntvct(0),                            // mrs x0, cntvct_el0
    0xd4000002u | (COUNTER_PROBE_HVC << 5), // hvc #COUNTER_PROBE_HVC
    BRK,
};
static_assert(probe[1] == 0xd41fffc2u);

// Writes the vector, the counter table and the probe to the kernel memory,
// with counter reads still exiting to the host
inline void write(u8* kernel_mem) {
    std::memcpy(kernel_mem + LOWER_EL_SYNC_VECTOR_OFFSET, sync_vector,
                sizeof(sync_vector));
    for (u32 rt = 0; rt < 32; rt++) {
        const uptr entry = COUNTER_TABLE_OFFSET + rt * TABLE_ENTRY_SIZE;
        u32 code[4] = {BRK, BRK, BRK, BRK};
        if (rt == 0) {
            code[0] = branch(entry, X0_OFFSET);
        } else if (rt == 1) {
            code[0] = branch(entry, X1_OFFSET);
        } else if (rt == 31) {
            code[0] = branch(entry, RETURN_OFFSET);
        } else {
            code[0] = mrsCntvct(rt);
            code[1] = ldrLiteral(0, entry + 0x4, FACTOR_OFFSET);
            code[2] = umulh(rt, rt, 0);
            code[3] = branch(entry + 0xc, RETURN_OFFSET);
        }
        std::memcpy(kernel_mem + entry, code, sizeof(code));
    }
    std::memcpy(kernel_mem + X0_OFFSET, tail, sizeof(tail));
    std::memset(kernel_mem + FACTOR_OFFSET, 0, sizeof(u64));
    std::memcpy(kernel_mem + COUNTER_PROBE_OFFSET, probe, sizeof(probe));
}

// Lets the vector emulate counter reads. `factor` is WallClock's for the
// guest counter, which must fit in 64 bits (a host counter of at least
// GUEST_CNTFRQ).
inline void enable(u8* kernel_mem, u64 factor) {
    std::memcpy(kernel_mem + FACTOR_OFFSET, &factor, sizeof(factor));
    const u32 syndromes[] = {readSyndrome(1), readSyndrome(2)};
    std::memcpy(kernel_mem + READ_SYNDROMES_OFFSET, syndromes,
                sizeof(syndromes));
}

} // namespace counter_emulation

} // namespace hydra::hw::tegra_x1::cpu::hypervisor
