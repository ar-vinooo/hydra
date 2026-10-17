#pragma once

#include "core/hw/tegra_x1/gpu/macro/driver_base.hpp"

namespace hydra::hw::tegra_x1::gpu::macro::interpreter {

class Driver : public DriverBase {
  public:
    explicit Driver(MacroHost& host) : DriverBase(host) {}

  protected:
    void run(Call& call) override;

  private:
    // Executes the instruction at the program counter. Returns false once the
    // macro exits.
    bool step(Call& call, bool is_delay_slot, std::optional<u32>& delayed_pc);
    u32 alu(Call& call, AluOperation op, u32 a, u32 b);
    void result(Call& call, ResultOperation op, u32 reg, u32 value);

    static void setReg(Call& call, u32 reg, u32 value) {
        // r0 always reads as zero
        if (reg != 0)
            call.regs[reg] = value;
    }
};

} // namespace hydra::hw::tegra_x1::gpu::macro::interpreter
