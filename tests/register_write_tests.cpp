#include "core/hw/tegra_x1/cpu/register_write_filter.hpp"
#include <Hypervisor/Hypervisor.h>
#include <iostream>
#include <stdexcept>
using namespace hydra;
using namespace hydra::hw::tegra_x1::cpu;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        check(hv_vm_create(nullptr) == HV_SUCCESS, "Create real Hypervisor VM");
        hv_vcpu_t vcpu;
        hv_vcpu_exit_t* exit;
        check(hv_vcpu_create(&vcpu, &exit, nullptr) == HV_SUCCESS,
              "Create VCPU");
        auto write_general = [&](u8 index, u64 value) {
            const auto reg = index == 29   ? HV_REG_FP
                             : index == 30 ? HV_REG_LR
                                           : hv_reg_t(HV_REG_X0 + index);
            check(hv_vcpu_set_reg(vcpu, reg, value) == HV_SUCCESS,
                  "Write general register");
        };
        auto write_simd = [&](u8 index, u128 value) {
            check(hv_vcpu_set_simd_fp_reg(
                      vcpu,
                      static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + index),
                      std::bit_cast<hv_simd_fp_uchar16_t>(value)) == HV_SUCCESS,
                  "Write SIMD register");
        };
        auto read_bank = [&] {
            ThreadState bank{};
            for (u8 i = 0; i < 29; ++i)
                check(hv_vcpu_get_reg(vcpu, hv_reg_t(HV_REG_X0 + i),
                                      &bank.r[i]) == HV_SUCCESS,
                      "Read general register");
            check(hv_vcpu_get_reg(vcpu, HV_REG_FP, &bank.fp) == HV_SUCCESS,
                  "Read FP");
            check(hv_vcpu_get_reg(vcpu, HV_REG_LR, &bank.lr) == HV_SUCCESS,
                  "Read LR");
            for (u8 i = 0; i < 32; ++i) {
                hv_simd_fp_uchar16_t value;
                check(hv_vcpu_get_simd_fp_reg(
                          vcpu,
                          static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i),
                          &value) == HV_SUCCESS,
                      "Read SIMD register");
                bank.v[i] = std::bit_cast<u128>(value);
            }
            return bank;
        };
        ThreadState desired{};
        for (u8 i = 0; i < 29; ++i)
            desired.r[i] = 0xfedcba9800000000ull + i;
        desired.fp = 29;
        desired.lr = 30;
        for (u8 i = 0; i < 32; ++i)
            desired.v[i] = (u128(0xabcdef9876543210ull + i) << 64) |
                           (0xfedcba0123456789ull - i);
        check(restoreRegisterBank(desired, nullptr, write_general,
                                  write_simd) == 63,
              "Initial state fully restored");
        auto baseline = read_bank();
        check(restoreRegisterBank(desired, &baseline, write_general,
                                  write_simd) == 0,
              "Unchanged bank needs no register writes");
        // Simulate guest execution changing a register between exits. The
        // baseline is reread from the VCPU, never taken from the previous
        // input.
        write_general(5, 123);
        baseline = read_bank();
        check(restoreRegisterBank(desired, &baseline, write_general,
                                  write_simd) == 1,
              "Guest-side mutation is restored from a fresh exit baseline");
        baseline = read_bank();
        desired.r[0] = 42;
        desired.fp = 99;
        desired.lr = 88;
        desired.v[31] ^= u128(1) << 100;
        check(restoreRegisterBank(desired, &baseline, write_general,
                                  write_simd) == 4,
              "SVC/debugger changes to general and high SIMD bits restored");
        const auto actual = read_bank();
        for (u8 i = 0; i < 29; ++i)
            check(actual.r[i] == desired.r[i], "General registers exact");
        check(actual.fp == desired.fp && actual.lr == desired.lr,
              "FP/LR exact");
        for (u8 i = 0; i < 32; ++i)
            check(actual.v[i] == desired.v[i], "SIMD registers exact");
        check(hv_vcpu_destroy(vcpu) == HV_SUCCESS, "Destroy VCPU");
        check(hv_vm_destroy() == HV_SUCCESS, "Destroy VM");
        std::cout << "PASS: real Hypervisor register restores, unchanged "
                     "filtering, fresh exit baseline and 128-bit SIMD\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
