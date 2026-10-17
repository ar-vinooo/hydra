#include "core/hw/tegra_x1/gpu/macro/driver_base.hpp"
#include "core/hw/tegra_x1/gpu/macro/const.hpp"

namespace hydra::hw::tegra_x1::gpu::macro {

void DriverBase::pushArgument(u32 macro_method, u32 arg) {
    if (!pending_entry) {
        // A call starts with the macro's call method, not its parameter
        if ((macro_method & 0x1) != 0)
            LOG_WARN(Macro, "Macro call started by its parameter method {:#x}",
                     macro_method);
        pending_entry = (macro_method >> 1) % START_ADDRESS_RAM_SIZE;
    }
    pending_params.push_back(arg);
}

void DriverBase::execute() {
    if (!pending_entry)
        return;

    Call call;
    call.pc = start_address_ram[*pending_entry];
    call.params = std::move(pending_params);
    call.regs[1] = call.params[0];
    pending_entry.reset();
    pending_params.clear();

    run(call);

    if (call.next_param != call.params.size())
        LOG_WARN(Macro, "Macro used {} of {} parameters", call.next_param,
                 call.params.size());
}

void DriverBase::loadInstructionRamPointer(u32 ptr) {
    instruction_ram_ptr = ptr;
}

void DriverBase::loadInstructionRam(u32 data) {
    instruction_ram[instruction_ram_ptr++ % INSTRUCTION_RAM_SIZE] = data;
}

void DriverBase::loadStartAddressRamPointer(u32 ptr) {
    start_address_ram_ptr = ptr;
}

void DriverBase::loadStartAddressRam(u32 data) {
    start_address_ram[start_address_ram_ptr++ % START_ADDRESS_RAM_SIZE] = data;
}

u32 DriverBase::fetchParam(Call& call) {
    if (call.next_param >= call.params.size()) {
        LOG_WARN(Macro, "Macro fetched more parameters than the {} it got",
                 call.params.size());
        return 0;
    }
    return call.params[call.next_param++];
}

u32 DriverBase::get3DReg(u32 reg_3d) { return host.readMacroRegister(reg_3d); }

void DriverBase::setMethod(Call& call, u32 value) {
    call.method = value & 0xfff;
    call.increment = (value >> 12) & 0x3f;
}

void DriverBase::send(Call& call, u32 arg) {
    const u32 method = call.method;
    call.method = (call.method + call.increment) & 0xfff;
    host.callMacroMethod(method, arg);
    // A macro calling a macro executes it right away
    if (method >= MACRO_METHODS_REGION)
        execute();
}

} // namespace hydra::hw::tegra_x1::gpu::macro
