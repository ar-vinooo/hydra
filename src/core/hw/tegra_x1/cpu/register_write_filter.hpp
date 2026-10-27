#pragma once

#include "core/hw/tegra_x1/cpu/thread.hpp"

namespace hydra::hw::tegra_x1::cpu {

// The baseline must come from the most recent VM exit, not the previous
// restore: the guest can change registers while executing in the Hypervisor.
template <typename GeneralWriter, typename SimdWriter>
u32 restoreRegisterBank(const ThreadState& desired, const ThreadState* baseline,
                        GeneralWriter&& write_general,
                        SimdWriter&& write_simd) {
    u32 writes = 0;
    for (u8 i = 0; i < 29; ++i) {
        if (!baseline || desired.r[i] != baseline->r[i]) {
            write_general(i, desired.r[i]);
            ++writes;
        }
    }
    if (!baseline || desired.fp != baseline->fp) {
        write_general(29, desired.fp);
        ++writes;
    }
    if (!baseline || desired.lr != baseline->lr) {
        write_general(30, desired.lr);
        ++writes;
    }
    for (u8 i = 0; i < 32; ++i) {
        if (!baseline || desired.v[i] != baseline->v[i]) {
            write_simd(i, desired.v[i]);
            ++writes;
        }
    }
    return writes;
}

} // namespace hydra::hw::tegra_x1::cpu
