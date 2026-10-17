#pragma once

#include "core/hw/tegra_x1/gpu/macro/const.hpp"

namespace hydra::hw::tegra_x1::gpu::macro {

// The engine macros run on: they read its registers and call its methods
class MacroHost {
  public:
    virtual ~MacroHost() = default;

    virtual u32 readMacroRegister(u32 reg) = 0;
    virtual void callMacroMethod(u32 method, u32 arg) = 0;
};

struct result_t {
    ResultOperation op;
    u8 rD;
};

class DriverBase {
  public:
    explicit DriverBase(MacroHost& host_) : host{host_} {}
    virtual ~DriverBase() = default;

    // An argument written to a macro method (relative to the macro region).
    // The first argument starts a call to the macro of that (even) method,
    // every later argument is a parameter of the same call, whichever macro
    // method it is written to, until the call executes.
    void pushArgument(u32 macro_method, u32 arg);
    // Executes the pending call, if any
    void execute();

    void loadInstructionRamPointer(u32 ptr);
    void loadInstructionRam(u32 data);
    void loadStartAddressRamPointer(u32 ptr);
    void loadStartAddressRam(u32 data);

  protected:
    // Execution state of one call. Calls are reentrant: a macro can send to
    // a macro method, which executes the callee immediately.
    struct Call {
        u32 regs[REG_COUNT] = {0};
        u32 pc{0};
        bool carry{false};
        u32 method{0};
        u32 increment{0};
        std::vector<u32> params;
        usize next_param{1};
    };

    virtual void run(Call& call) = 0;

    u32 getInstruction(u32 pc) const {
        return instruction_ram[pc % INSTRUCTION_RAM_SIZE];
    }
    u32 fetchParam(Call& call);
    u32 get3DReg(u32 reg_3d);
    void setMethod(Call& call, u32 value);
    void send(Call& call, u32 arg);

  private:
    MacroHost& host;

    // Memory
    static constexpr u32 INSTRUCTION_RAM_SIZE = 0x1000;
    static constexpr u32 START_ADDRESS_RAM_SIZE = 0x100;
    u32 instruction_ram[INSTRUCTION_RAM_SIZE] = {0};
    u32 instruction_ram_ptr{0};
    u32 start_address_ram[START_ADDRESS_RAM_SIZE] = {0};
    u32 start_address_ram_ptr{0};

    // Call being collected
    std::optional<u32> pending_entry;
    std::vector<u32> pending_params;
};

} // namespace hydra::hw::tegra_x1::gpu::macro
