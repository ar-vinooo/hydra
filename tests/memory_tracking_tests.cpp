#include "core/hw/tegra_x1/cpu/hypervisor/memory.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/stage2_tracker.hpp"

#include <Hypervisor/Hypervisor.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace hydra;
using namespace hydra::hw::tegra_x1::cpu::hypervisor;

namespace {

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

constexpr u32 EC_DATA_ABORT_LOWER_EL = 0x24;
constexpr u32 EC_HVC = 0x16;

} // namespace

// Runs guest code at EL1 with the MMU off (virtual = physical addresses) and
// checks that stage 2 tracking faults reach Stage2Tracker before the access
// completes.
int main() {
    try {
        auto config = hv_vm_config_create();
        uint32_t max_bits;
        check(hv_vm_config_get_max_ipa_size(&max_bits) == HV_SUCCESS,
              "Query IPA size");
        check(hv_vm_config_set_ipa_size(config, max_bits) == HV_SUCCESS,
              "Set IPA size");
        check(hv_vm_create(config) == HV_SUCCESS, "Create VM");
        os_release(config);

        Memory code(APPLE_PAGE_SIZE);
        Memory data(APPLE_PAGE_SIZE);
        const u32 program[] = {
            0xf9400001u, // ldr x1, [x0]
            0xf9000002u, // str x2, [x0]
            0xd4000002u, // hvc #0
        };
        std::memcpy(reinterpret_cast<void*>(code.getPtr()), program,
                    sizeof(program));
        auto* data_words = reinterpret_cast<u64*>(data.getPtr());
        data_words[0] = 0xdeadull; // Stale guest memory

        auto& tracker = Stage2Tracker::instance();
        u32 flushes = 0;
        u32 invalidations = 0;
        tracker.setCallbacks({
            .flush_gpu_writes =
                [&](ztd::Range<uptr> range) {
                    check(range.getBegin() == data.getPtr(),
                          "Flush covers the faulting page");
                    data_words[0] = 0x1234ull; // GPU contents written back
                    flushes++;
                },
            .invalidate_gpu_caches =
                [&](ztd::Range<uptr> range) {
                    check(range.getBegin() == data.getPtr(),
                          "Invalidation covers the faulting page");
                    invalidations++;
                },
        });
        const auto data_range =
            ztd::Range<uptr>::fromSize(data.getPtr(), APPLE_PAGE_SIZE);
        tracker.setWriteTracked(data_range, true);
        tracker.protectGpuWritten(data_range);

        hv_vcpu_t vcpu;
        hv_vcpu_exit_t* exit;
        check(hv_vcpu_create(&vcpu, &exit, nullptr) == HV_SUCCESS,
              "Create vCPU");
        hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, 0x30d00800ull);
        hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5); // EL1h, interrupts masked
        hv_vcpu_set_reg(vcpu, HV_REG_PC, code.getPhysicalAddress());
        hv_vcpu_set_reg(vcpu, HV_REG_X0, data.getPhysicalAddress());
        hv_vcpu_set_reg(vcpu, HV_REG_X2, 0x5678ull);

        u32 faults = 0;
        while (true) {
            check(hv_vcpu_run(vcpu) == HV_SUCCESS, "Run vCPU");
            check(exit->reason == HV_EXIT_REASON_EXCEPTION,
                  "vCPU exits with an exception");
            const u64 syndrome = exit->exception.syndrome;
            const u32 ec = static_cast<u32>((syndrome >> 26) & 0x3f);
            if (ec == EC_HVC)
                break;
            check(ec == EC_DATA_ABORT_LOWER_EL, "Stage 2 data abort");
            check(++faults <= 4, "Faults are resolved");
            const bool is_write = (syndrome & (1ull << 6)) != 0;
            check(tracker.handleFault(exit->exception.physical_address,
                                      is_write),
                  "Tracker handles its own faults");
        }

        u64 loaded;
        hv_vcpu_get_reg(vcpu, HV_REG_X1, &loaded);
        check(flushes == 1, "GPU contents are written back once");
        check(loaded == 0x1234ull,
              "The CPU reads the written back GPU contents, not stale memory");
        check(invalidations == 1, "The CPU write invalidates the GPU caches");
        check(data_words[0] == 0x5678ull, "The retried write lands");
        check(faults == 2, "One fault for the read, one for the write");

        // Written pages are write protected again
        tracker.rearmWriteTracking();
        hv_vcpu_set_reg(vcpu, HV_REG_PC, code.getPhysicalAddress() + 4);
        while (true) {
            check(hv_vcpu_run(vcpu) == HV_SUCCESS, "Run vCPU again");
            const u64 syndrome = exit->exception.syndrome;
            const u32 ec = static_cast<u32>((syndrome >> 26) & 0x3f);
            if (ec == EC_HVC)
                break;
            check(tracker.handleFault(exit->exception.physical_address,
                                      (syndrome & (1ull << 6)) != 0),
                  "Rearmed fault is handled");
        }
        check(invalidations == 2, "Rearmed pages fault on the next write");
        check(flushes == 1, "Written back pages don't flush again");

        hv_vcpu_destroy(vcpu);
        std::cout << "PASS: stage 2 tracking writes GPU contents back before "
                     "CPU reads, invalidates on writes and rearms\n";
        return 0;
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
