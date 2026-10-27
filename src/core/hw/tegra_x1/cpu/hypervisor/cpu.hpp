#pragma once

#include <mutex>
#include <optional>

#include "core/hw/tegra_x1/cpu/cpu.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/memory.hpp"
#include "core/hw/tegra_x1/cpu/hypervisor/page_table.hpp"

namespace hydra::horizon {
class OS;
}

namespace hydra::hw::tegra_x1::cpu {
class IMmu;
}

namespace hydra::hw::tegra_x1::cpu::hypervisor {

class Mmu;

class VirtualMachine {
  public:
    VirtualMachine();
    ~VirtualMachine();
};

class Cpu : public ICpu {
  public:
    Cpu();
    ~Cpu() noexcept override = default;

    IMmu* createMmu(System& system) override;
    IThread* createThread(WallClock& wall_clock, IMmu* mmu,
                          const ThreadCallbacks& callbacks, IMemory* tls_mem,
                          vaddr_t tls_mem_base) override;
    IMemory* allocateMemory(u64 size) override;
    IMemory* wrapMemory(uptr ptr, u64 size) override;

    // Counter reads are emulated in the VM (see counter_emulation.hpp) once
    // the first vCPU found a vtimer offset with which its virtual counter
    // matches the host's; every vCPU then uses that offset
    void enableCounterEmulation(u64 vtimer_offset, u64 factor);
    std::once_flag& getCounterEmulationOnce() { return counter_emulation_once; }

  private:
    VirtualMachine vm;
    Memory kernel_mem;
    PageTable kernel_page_table;

    std::once_flag counter_emulation_once;
    std::optional<u64> counter_vtimer_offset;

  public:
    CONST_REF_GETTER(kernel_page_table, getKernelPageTable);
    CONST_REF_GETTER(counter_vtimer_offset, getCounterVtimerOffset);
};

} // namespace hydra::hw::tegra_x1::cpu::hypervisor
