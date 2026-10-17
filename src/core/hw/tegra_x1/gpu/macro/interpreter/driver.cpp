#include "core/hw/tegra_x1/gpu/macro/interpreter/driver.hpp"

namespace hydra::hw::tegra_x1::gpu::macro::interpreter {

namespace {

// Bounds runaway macros (a loop on a stale register would hang the GPU thread)
constexpr u32 MAX_STEPS = 1u << 22;

struct Opcode {
    u32 raw;

    Operation operation() const { return static_cast<Operation>(raw & 0x7); }
    bool isExit() const { return (raw & EXIT_BIT) != 0; }
    ResultOperation resultOperation() const {
        return static_cast<ResultOperation>(extractBits(raw, 4, 3));
    }
    bool branchNotZero() const { return extractBits(raw, 4, 1) != 0; }
    bool branchAnnul() const { return extractBits(raw, 5, 1) != 0; }
    u32 dst() const { return static_cast<u32>(extractBits(raw, 8, 3)); }
    u32 srcA() const { return static_cast<u32>(extractBits(raw, 11, 3)); }
    u32 srcB() const { return static_cast<u32>(extractBits(raw, 14, 3)); }
    AluOperation aluOperation() const {
        return static_cast<AluOperation>(extractBits(raw, 17, 5));
    }
    i32 immediate() const {
        return signExtend<i32, 18>(static_cast<i32>(extractBits(raw, 14, 18)));
    }
    u32 bitfieldSrcBit() const { return static_cast<u32>(extractBits(raw, 17, 5)); }
    u32 bitfieldSize() const { return static_cast<u32>(extractBits(raw, 22, 5)); }
    u32 bitfieldDstBit() const { return static_cast<u32>(extractBits(raw, 27, 5)); }
    u32 bitfieldMask() const { return (1u << bitfieldSize()) - 1; }
};

} // namespace

void Driver::run(Call& call) {
    std::optional<u32> delayed_pc;
    for (u32 steps = 0; steps < MAX_STEPS; steps++) {
        if (!step(call, false, delayed_pc))
            return;
    }
    LOG_ERROR(Macro, "Macro did not exit after {} steps (pc {:#x})", MAX_STEPS,
              call.pc);
}

bool Driver::step(Call& call, bool is_delay_slot,
                  std::optional<u32>& delayed_pc) {
    const u32 base = call.pc;
    const Opcode opcode{getInstruction(base)};
    call.pc = base + 1;
    // The delay slot of a taken branch continues at the branch target
    if (delayed_pc) {
        call.pc = *delayed_pc;
        delayed_pc.reset();
    }

    switch (opcode.operation()) {
    case Operation::Alu:
        result(call, opcode.resultOperation(), opcode.dst(),
               alu(call, opcode.aluOperation(), call.regs[opcode.srcA()],
                   call.regs[opcode.srcB()]));
        break;
    case Operation::AddImmediate:
        result(call, opcode.resultOperation(), opcode.dst(),
               call.regs[opcode.srcA()] +
                   static_cast<u32>(opcode.immediate()));
        break;
    case Operation::ExtractInsert: {
        // Replaces a bitfield of A with a bitfield of B
        u32 value = call.regs[opcode.srcA()];
        const u32 inserted =
            (call.regs[opcode.srcB()] >> opcode.bitfieldSrcBit()) &
            opcode.bitfieldMask();
        value &= ~(opcode.bitfieldMask() << opcode.bitfieldDstBit());
        value |= inserted << opcode.bitfieldDstBit();
        result(call, opcode.resultOperation(), opcode.dst(), value);
        break;
    }
    case Operation::ExtractShiftLeftImmediate: {
        // Bitfield of B at the position in A, shifted left by an immediate
        const u32 shift = call.regs[opcode.srcA()] & 0x1f;
        const u32 value =
            ((call.regs[opcode.srcB()] >> shift) & opcode.bitfieldMask())
            << opcode.bitfieldDstBit();
        result(call, opcode.resultOperation(), opcode.dst(), value);
        break;
    }
    case Operation::ExtractShiftLeftRegister: {
        // Bitfield of B at an immediate position, shifted left by A
        const u32 shift = call.regs[opcode.srcA()] & 0x1f;
        const u32 value =
            ((call.regs[opcode.srcB()] >> opcode.bitfieldSrcBit()) &
             opcode.bitfieldMask())
            << shift;
        result(call, opcode.resultOperation(), opcode.dst(), value);
        break;
    }
    case Operation::Read:
        result(call, opcode.resultOperation(), opcode.dst(),
               get3DReg(call.regs[opcode.srcA()] +
                        static_cast<u32>(opcode.immediate())));
        break;
    case Operation::Branch: {
        if (is_delay_slot) {
            LOG_ERROR(Macro, "Branch in a delay slot at {:#x}", base);
            break;
        }
        const bool taken =
            (call.regs[opcode.srcA()] == 0) != opcode.branchNotZero();
        if (!taken)
            break;
        const u32 target = base + static_cast<u32>(opcode.immediate());
        if (opcode.branchAnnul()) {
            call.pc = target;
            return true;
        }
        // The next instruction executes before the branch is taken
        delayed_pc = target;
        return step(call, true, delayed_pc);
    }
    default:
        LOG_ERROR(Macro, "Invalid macro operation {} at {:#x}",
                  static_cast<u32>(opcode.operation()), base);
        break;
    }

    // Exits after the next instruction, unless this one is a delay slot
    if (opcode.isExit() && !is_delay_slot) {
        step(call, true, delayed_pc);
        return false;
    }
    return true;
}

u32 Driver::alu(Call& call, AluOperation op, u32 a, u32 b) {
    switch (op) {
    case AluOperation::Add: {
        const u64 sum = u64(a) + b;
        call.carry = sum > 0xffffffffu;
        return static_cast<u32>(sum);
    }
    case AluOperation::AddWithCarry: {
        const u64 sum = u64(a) + b + (call.carry ? 1 : 0);
        call.carry = sum > 0xffffffffu;
        return static_cast<u32>(sum);
    }
    case AluOperation::Subtract: {
        // Carry is set when no borrow occurs
        const u64 difference = u64(a) - b;
        call.carry = difference < 0x100000000ull;
        return static_cast<u32>(difference);
    }
    case AluOperation::SubtractWithBorrow: {
        const u64 difference = u64(a) - b - (call.carry ? 0 : 1);
        call.carry = difference < 0x100000000ull;
        return static_cast<u32>(difference);
    }
    case AluOperation::Xor:
        return a ^ b;
    case AluOperation::Or:
        return a | b;
    case AluOperation::And:
        return a & b;
    case AluOperation::AndNot:
        return a & ~b;
    case AluOperation::Nand:
        return ~(a & b);
    default:
        LOG_ERROR(Macro, "Invalid macro ALU operation {}", static_cast<u32>(op));
        return 0;
    }
}

void Driver::result(Call& call, ResultOperation op, u32 reg, u32 value) {
    switch (op) {
    case ResultOperation::IgnoreAndFetch:
        setReg(call, reg, fetchParam(call));
        break;
    case ResultOperation::Move:
        setReg(call, reg, value);
        break;
    case ResultOperation::MoveAndSetMethod:
        setReg(call, reg, value);
        setMethod(call, value);
        break;
    case ResultOperation::FetchAndSend:
        setReg(call, reg, fetchParam(call));
        send(call, value);
        break;
    case ResultOperation::MoveAndSend:
        setReg(call, reg, value);
        send(call, value);
        break;
    case ResultOperation::FetchAndSetMethod:
        setReg(call, reg, fetchParam(call));
        setMethod(call, value);
        break;
    case ResultOperation::MoveAndSetMethodFetchAndSend:
        setReg(call, reg, value);
        setMethod(call, value);
        send(call, fetchParam(call));
        break;
    case ResultOperation::MoveAndSetMethodSend:
        setReg(call, reg, value);
        setMethod(call, value);
        send(call, (value >> 12) & 0x3f);
        break;
    }
}

} // namespace hydra::hw::tegra_x1::gpu::macro::interpreter
