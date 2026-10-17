#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/integer_misc.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

ir::Value getRegisterValue(DecoderContext& context, instruction_t inst,
                           u32 offset) {
    return context.builder.opCopy(ir::Value::createRegister(
        static_cast<reg_t>(extractBits(inst, offset, 8))));
}

ir::Value getConstBufferValue(DecoderContext& context, instruction_t inst) {
    return context.builder.opCopy(ir::Value::createConstMemory(
        CMem(static_cast<u32>(extractBits(inst, 34, 5)), RZ,
             extractBits(inst, 20, 14) * 4)));
}

void writeResult(DecoderContext& context, instruction_t inst,
                 const ir::Value& value) {
    context.builder.opCopy(
        ir::Value::createRegister(static_cast<reg_t>(extractBits(inst, 0, 8))),
        value);
}

} // namespace

ir::Value getAluSrcB(DecoderContext& context, instruction_t inst,
                     OperandForm form) {
    switch (form) {
    case OperandForm::Register:
        return getRegisterValue(context, inst, 20);
    case OperandForm::ConstBuffer:
        return getConstBufferValue(context, inst);
    case OperandForm::Immediate:
        return ir::Value::createConstantU(
            getIntImm20(static_cast<u32>(extractBits(inst, 20, 19)),
                        static_cast<u32>(extractBits(inst, 56, 1)), true));
    case OperandForm::RegisterConstBuffer:
        return getRegisterValue(context, inst, 39);
    }
    unreachable();
}

// PRMT Rd, Ra, Rb (selector), Rc: permutes the bytes of {Rc, Ra}
void emitPrmt(DecoderContext& context, instruction_t inst, OperandForm form) {
    auto& b = context.builder;
    const auto conditional =
        handlePredCond(b, static_cast<pred_t>(extractBits(inst, 16, 3)),
                       extractBits(inst, 19, 1) != 0);
    const auto a = getRegisterValue(context, inst, 8);
    const auto selector = getAluSrcB(context, inst, form);
    const auto high = form == OperandForm::RegisterConstBuffer
                          ? getConstBufferValue(context, inst)
                          : getRegisterValue(context, inst, 39);
    const auto mode = static_cast<u32>(extractBits(inst, 48, 4));
    writeResult(context, inst,
                b.opIntrinsic(ir::Intrinsic::Prmt, ir::ScalarType::U32,
                              {a, high, selector,
                               ir::Value::createConstantU(mode)}));
    if (conditional)
        b.opEndIf();
}

namespace {

void emitIntegerMultiply(DecoderContext& context, instruction_t inst,
                         const ir::Value& src_b, bool high, bool signed_a,
                         bool signed_b, bool write_cc) {
    auto& b = context.builder;
    const auto conditional =
        handlePredCond(b, static_cast<pred_t>(extractBits(inst, 16, 3)),
                       extractBits(inst, 19, 1) != 0);
    const auto a = getRegisterValue(context, inst, 8);
    const auto result =
        high ? b.opIntrinsic(ir::Intrinsic::IMulHi, ir::ScalarType::U32,
                             {a, src_b,
                              ir::Value::createConstantU(
                                  (signed_a ? 1u : 0u) | (signed_b ? 2u : 0u))})
             : b.opMultiply(a, b.opBitCast(src_b, ir::ScalarType::U32));
    if (write_cc) {
        // Zero and sign of the result; multiplication clears carry and
        // overflow
        const auto zero = ir::Value::createConstantU(0);
        b.opCopy(ir::Value::createConditionFlag(0),
                 b.opCompareEqual(result, zero));
        b.opCopy(ir::Value::createConditionFlag(1),
                 b.opCompareNotEqual(
                     b.opBitwiseAnd(result,
                                    ir::Value::createConstantU(0x80000000u)),
                     zero));
        b.opCopy(ir::Value::createConditionFlag(2),
                 ir::Value::createConstantB(false));
        b.opCopy(ir::Value::createConditionFlag(3),
                 ir::Value::createConstantB(false));
    }
    writeResult(context, inst, result);
    if (conditional)
        b.opEndIf();
}

} // namespace

void emitImul(DecoderContext& context, instruction_t inst, OperandForm form) {
    emitIntegerMultiply(context, inst, getAluSrcB(context, inst, form),
                        extractBits(inst, 39, 1) != 0,
                        extractBits(inst, 40, 1) != 0,
                        extractBits(inst, 41, 1) != 0,
                        extractBits(inst, 47, 1) != 0);
}

void emitImul32I(DecoderContext& context, instruction_t inst) {
    emitIntegerMultiply(
        context, inst,
        ir::Value::createConstantU(static_cast<u32>(extractBits(inst, 20, 32))),
        extractBits(inst, 53, 1) != 0, extractBits(inst, 54, 1) != 0,
        extractBits(inst, 55, 1) != 0, extractBits(inst, 52, 1) != 0);
}

// P2R Rd, Ra, mask: copies predicates (or condition flags) into a byte of Ra
void emitP2r(DecoderContext& context, instruction_t inst, OperandForm form) {
    auto& b = context.builder;
    const auto conditional =
        handlePredCond(b, static_cast<pred_t>(extractBits(inst, 16, 3)),
                       extractBits(inst, 19, 1) != 0);
    const bool cc_mode = extractBits(inst, 40, 1) != 0;
    const u32 shift = static_cast<u32>(extractBits(inst, 41, 2)) * 8;
    const u32 count = cc_mode ? 4 : 7;
    const auto mask = b.opBitwiseAnd(getAluSrcB(context, inst, form),
                                     ir::Value::createConstantU((1u << count) - 1));
    auto bits = ir::Value::createConstantU(0);
    for (u32 i = 0; i < count; i++) {
        const auto source = cc_mode
                                ? ir::Value::createConditionFlag(i)
                                : ir::Value::createPredicate(static_cast<pred_t>(i));
        bits = b.opBitwiseOr(
            bits, b.opSelect(source, ir::Value::createConstantU(1u << i),
                             ir::Value::createConstantU(0)));
    }
    const auto shifted_mask =
        b.opShiftLeft(mask, ir::Value::createConstantU(shift));
    const auto kept = b.opBitwiseAnd(getRegisterValue(context, inst, 8),
                                     b.opBitwiseNot(shifted_mask));
    const auto inserted = b.opBitwiseAnd(
        b.opShiftLeft(bits, ir::Value::createConstantU(shift)), shifted_mask);
    writeResult(context, inst, b.opBitwiseOr(kept, inserted));
    if (conditional)
        b.opEndIf();
}

// FCHK.DIVIDE Pd, Ra, Rb: whether a / b needs the precise division path.
// Compilers take that path through a subroutine call; the fast path result
// (a multiplied by the reciprocal of b, refined) is used instead.
void emitFchk(DecoderContext& context, instruction_t inst, OperandForm form) {
    (void)form;
    auto& b = context.builder;
    const auto conditional =
        handlePredCond(b, static_cast<pred_t>(extractBits(inst, 16, 3)),
                       extractBits(inst, 19, 1) != 0);
    if (extractBits(inst, 39, 6) != 0)
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "FCHK check {}",
                            extractBits(inst, 39, 6));
    const auto dst = static_cast<pred_t>(extractBits(inst, 3, 3));
    if (dst != PT)
        b.opCopy(ir::Value::createPredicate(dst),
                 ir::Value::createConstantB(false));
    if (conditional)
        b.opEndIf();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
