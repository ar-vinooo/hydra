#pragma once

#include <atomic>
#include <chrono>

#include "core/hw/tegra_x1/cpu/hypervisor/const.hpp"
#include "core/hw/tegra_x1/cpu/register_sync.hpp"
#include "core/hw/tegra_x1/cpu/thread.hpp"

namespace hydra::horizon {
class OS;
}

namespace hydra::hw::tegra_x1::cpu {
class IMemory;
}

namespace hydra::hw::tegra_x1::cpu::hypervisor {

class Cpu;
class Mmu;

constexpr u32 MAX_BREAKPOINTS = 16;

enum class ThreadMessageType {
    InsertBreakpoint,
    RemoveBreakpoint,
    SingleStep,
};

struct ThreadMessage {
    ThreadMessageType type;
    union {
        // Insert breakpoint
        struct {
            vaddr_t addr;
        } insert_breakpoint;

        // Remove breakpoint
        struct {
            vaddr_t addr;
        } remove_breakpoint;
    } payload{};
};

class Thread : public IThread {
  public:
    Thread(WallClock& wall_clock, Cpu& cpu_, IMmu* mmu,
           const ThreadCallbacks& callbacks, IMemory* tls_mem,
           vaddr_t tls_mem_base);
    ~Thread() override;

    void run() override;

    void setupVTimer();

    void updateVTimer();

    // Debug
    void insertBreakpoint(vaddr_t addr) override {
        sendMessage({.type = ThreadMessageType::InsertBreakpoint,
                     .payload = {.insert_breakpoint = {addr}}});
    }
    void removeBreakpoint(vaddr_t addr) override {
        sendMessage({.type = ThreadMessageType::RemoveBreakpoint,
                     .payload = {.remove_breakpoint = {addr}}});
    }
    void singleStep() override {
        sendMessage({.type = ThreadMessageType::SingleStep});
    }

    void completeState() override;
    u64 getTpidr() override {
        return has_vcpu ? getSysReg(HV_SYS_REG_TPIDR_EL0) : saved_tpidr;
    }
    // In an exception (the EL1 trampoline), EL0's PSTATE is in SPSR_EL1
    u32 getUserPstate() override {
        if (!exception)
            return state.pstate;
        return static_cast<u32>(has_vcpu ? getSysReg(HV_SYS_REG_SPSR_EL1)
                                         : saved_spsr);
    }
    void setUserPstate(u32 pstate) override {
        if (!exception)
            state.pstate = pstate;
        else if (has_vcpu)
            setSysReg(HV_SYS_REG_SPSR_EL1, pstate);
        else
            saved_spsr = pstate;
    }

  private:
    Cpu& cpu;
    vaddr_t tls_mem_base;

    // Hypervisor.framework allows only 64 vCPUs per VM, fewer than the threads
    // some games create. Threads that block in the kernel give their vCPU back
    // when vCPUs run low and create a new one when they run again; all guest
    // state lives in `state` between runs, plus the few registers below.
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t* exit;
    bool has_vcpu{false};
    u64 saved_pc{0};
    u64 saved_spsr{0};
    u64 saved_tpidr{0};

    void createVcpu();
    void releaseVcpu(bool save_state = true);
    // Enables Cpu::enableCounterEmulation if a vtimer offset makes the
    // virtual counter match the host's, by running a probe on this vCPU
    void probeCounterEmulation();
    // Exited from the exception vector (the guest's PC is in ELR_EL1)
    bool exception{false};

    u64 interrupt_time_delta_ticks;

    // Debug
    vaddr_t breakpoints[MAX_BREAKPOINTS] = {0x0};
    // DEBUG: HYDRA_HOOK_PCS slot to re-enable after stepping over its hit
    u32 hook_rearm_slot{MAX_BREAKPOINTS};

    // Messages
    std::mutex msg_mutex;
    std::queue<ThreadMessage> msg_queue;
    std::atomic<bool> has_messages{false};

    // State
    // Each exit reads only the registers its handler needs into `state`,
    // completeState reads the rest (see RegisterSync)
    struct VcpuRegisters {
        Thread* thread;

        static hv_reg_t generalReg(u8 index) {
            return index == 29   ? HV_REG_FP
                   : index == 30 ? HV_REG_LR
                                 : hv_reg_t(HV_REG_X0 + index);
        }
        u64 general(u8 index) const {
            return thread->getReg(generalReg(index));
        }
        void setGeneral(u8 index, u64 value) const {
            thread->setReg(generalReg(index), value);
        }
        u64 sp() const { return thread->getSysReg(HV_SYS_REG_SP_EL0); }
        void setSp(u64 value) const {
            thread->setSysReg(HV_SYS_REG_SP_EL0, value);
        }
        u64 pc() const {
            return thread->exception ? thread->getSysReg(HV_SYS_REG_ELR_EL1)
                                     : thread->getReg(HV_REG_PC);
        }
        void setPc(u64 value) const {
            if (thread->exception)
                thread->setSysReg(HV_SYS_REG_ELR_EL1, value);
            else
                thread->setReg(HV_REG_PC, value);
        }
        u32 pstate() const {
            return static_cast<u32>(thread->getReg(HV_REG_CPSR));
        }
        void setPstate(u32 value) const { thread->setReg(HV_REG_CPSR, value); }
        u32 fpcr() const {
            return static_cast<u32>(thread->getReg(HV_REG_FPCR));
        }
        void setFpcr(u32 value) const { thread->setReg(HV_REG_FPCR, value); }
        u32 fpsr() const {
            return static_cast<u32>(thread->getReg(HV_REG_FPSR));
        }
        void setFpsr(u32 value) const { thread->setReg(HV_REG_FPSR, value); }
        u128 simd(u8 index) const { return thread->getSimdFpReg(index); }
        void setSimd(u8 index, u128 value) const {
            thread->setSimdFpReg(index, value);
        }
    };
    RegisterSync<VcpuRegisters> registers{VcpuRegisters{this}};
    RegisterGroup getExitReads();
    void restoreRegisters();
    // Exits handled on the vCPU's registers alone, without `state`
    bool handleFastExit();
    bool handleFastGuestException();

    // Reads of the counter registers, which EL0 traps on (CNTKCTL_EL1)
    std::optional<u64> readTrappedSystemRegister(u32 esr) const;
    void instructionTrap(u32 esr);

    // TLB maintenance on returning to the guest, see selectTrampoline
    u64 tlb_generation_seen{0};
    bool local_tlb_flush{false};
    u64 selectTrampoline();
    // Horizon's user exception handling, unless a debugger takes it
    bool deliverUserException(u32 esr, u64 far);
    // HYDRA_PERF_STATS: guest run time and VM exits by kind
    void countExit(std::chrono::steady_clock::time_point run_start);

    // Helpers
    u64 getReg(hv_reg_t reg) const {
        u64 value;
        HV_ASSERT_SUCCESS(hv_vcpu_get_reg(vcpu, reg, &value));

        return value;
    }

    void setReg(hv_reg_t reg, u64 value) const {
        HV_ASSERT_SUCCESS(hv_vcpu_set_reg(vcpu, reg, value));
    }

    u128 getSimdFpReg(u8 reg) const {
        hv_simd_fp_uchar16_t value;
        HV_ASSERT_SUCCESS(hv_vcpu_get_simd_fp_reg(
            vcpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + reg),
            &value));

        // TODO: correct?
        return std::bit_cast<u128>(value);
    }

    void setSimdFpReg(u8 reg, u128 value) const {
        // TODO: correct?
        HV_ASSERT_SUCCESS(hv_vcpu_set_simd_fp_reg(
            vcpu, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + reg),
            std::bit_cast<hv_simd_fp_uchar16_t>(value)));
    }

    u64 getSysReg(hv_sys_reg_t reg) const {
        u64 value;
        HV_ASSERT_SUCCESS(hv_vcpu_get_sys_reg(vcpu, reg, &value));

        return value;
    }

    void setSysReg(hv_sys_reg_t reg, u64 value) const {
        HV_ASSERT_SUCCESS(hv_vcpu_set_sys_reg(vcpu, reg, value));
    }

    // Messages
    void sendMessage(const ThreadMessage& message) {
        std::scoped_lock lock(msg_mutex);
        msg_queue.push(message);
        has_messages.store(true, std::memory_order_release);
    }

    void processMessages();
};

} // namespace hydra::hw::tegra_x1::cpu::hypervisor
