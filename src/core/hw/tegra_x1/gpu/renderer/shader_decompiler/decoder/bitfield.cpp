#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/bitfield.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

void emitBitfieldExtract(DecoderContext& context, pred_t pred, bool pred_inv,
                         bool is_signed, bool brev, bool write_cc, reg_t dst, reg_t src_a,
                         ir::Value src_b) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    auto src_a_v = ir::Value::createRegister(
        src_a, (is_signed ? ir::ScalarType::I32 : ir::ScalarType::U32));
    // .BREV reverses the source bits before the extraction
    if (brev) {
        src_a_v = context.builder.opIntrinsic(
            ir::Intrinsic::BitReverse, ir::ScalarType::U32,
            {ir::Value::createRegister(src_a, ir::ScalarType::U32)});
        if (is_signed)
            src_a_v = context.builder.opBitCast(src_a_v, ir::ScalarType::I32);
    }
    const auto position =
        context.builder.opBitwiseAnd(src_b, ir::Value::createConstantU(0xff));
    const auto size = context.builder.opBitfieldExtract(
        src_b, ir::Value::createConstantU(8), ir::Value::createConstantU(8));

    auto res = context.builder.opBitfieldExtract(src_a_v, position, size);
    if (is_signed) {
        const auto negative = context.builder.opCompareLess(
            src_a_v, ir::Value::createConstantI(0));
        const auto replicated = context.builder.opSelect(
            negative, ir::Value::createConstantI(-1),
            ir::Value::createConstantI(0));
        res = context.builder.opSelect(
            context.builder.opCompareGreaterOrEqual(
                position, ir::Value::createConstantU(32)),
            replicated, res);
        res = context.builder.opSelect(
            context.builder.opCompareEqual(size, ir::Value::createConstantU(0)),
            ir::Value::createConstantI(0), res);
    }
    if (write_cc) {
        auto& b = context.builder;
        const auto bits = b.opBitCast(res, ir::ScalarType::U32);
        b.opCopy(ir::Value::createConditionFlag(0),
                 b.opCompareEqual(bits, ir::Value::createConstantU(0)));
        b.opCopy(ir::Value::createConditionFlag(1),
                 b.opCompareLess(b.opBitCast(bits, ir::ScalarType::I32),
                                 ir::Value::createConstantI(0)));
        b.opCopy(ir::Value::createConditionFlag(2), ir::Value::createConstantB(false));
        b.opCopy(ir::Value::createConditionFlag(3), ir::Value::createConstantB(false));
    }
    context.builder.opCopy(ir::Value::createRegister(dst), res);

    if (conditional)
        context.builder.opEndIf();
}

} // namespace

void emitBfeR(DecoderContext& context, InstBfeR inst) {
    emitBitfieldExtract(context, inst.base.pred, inst.base.pred_inv,
                        inst.base.is_signed, inst.base.brev, inst.base.write_cc, inst.base.dst, inst.base.src_a,
                        ir::Value::createRegister(inst.src_b));
}

void emitBfeC(DecoderContext& context, InstBfeC inst) {
    emitBitfieldExtract(
        context, inst.base.pred, inst.base.pred_inv, inst.base.is_signed, inst.base.brev, inst.base.write_cc,
        inst.base.dst, inst.base.src_a,
        ir::Value::createConstMemory(
            CMem(inst.cbuf_slot, RZ, static_cast<u64>(inst.cbuf_offset * 4))));
}

void emitBfeI(DecoderContext& context, InstBfeI inst) {
    emitBitfieldExtract(
        context, inst.base.pred, inst.base.pred_inv, inst.base.is_signed, inst.base.brev, inst.base.write_cc,
        inst.base.dst, inst.base.src_a,
        ir::Value::createConstantU(inst.imm20_0 | (inst.imm20_19 << 19)));
}

namespace {
void emitInsert(DecoderContext& context, InstBfiBase inst, ir::Value control,
                ir::Value base) {
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, inst.pred, inst.pred_inv);
    const auto bits = ir::Value::createConstantU(32);
    const auto offset =
        b.opBitwiseAnd(control, ir::Value::createConstantU(255));
    const auto raw_count = b.opBitfieldExtract(
        control, ir::Value::createConstantU(8), ir::Value::createConstantU(8));
    const auto valid = b.opCompareLess(offset, bits);
    const auto safe_offset =
        b.opBitwiseAnd(offset, ir::Value::createConstantU(31));
    const auto remaining = b.opAdd(bits, b.opNeg(safe_offset));
    const auto count =
        b.opSelect(b.opCompareLess(raw_count, remaining), raw_count, remaining);
    // Mask shift counts before evaluating: Metal/C++ shifts by 32 are
    // undefined.
    const auto n = b.opBitwiseAnd(count, ir::Value::createConstantU(31));
    const auto low_mask = b.opSelect(
        b.opCompareEqual(count, bits), ir::Value::createConstantU(0xffffffffu),
        b.opAdd(b.opShiftLeft(ir::Value::createConstantU(1), n),
                ir::Value::createConstantU(0xffffffffu)));
    const auto mask = b.opShiftLeft(low_mask, safe_offset);
    const auto inserted = b.opBitwiseAnd(
        b.opShiftLeft(ir::Value::createRegister(inst.insert), safe_offset),
        mask);
    const auto result =
        b.opBitwiseOr(b.opBitwiseAnd(base, b.opBitwiseNot(mask)), inserted);
    b.opCopy(ir::Value::createRegister(inst.dst),
             b.opSelect(valid, result, base));
    if (inst.write_cc)
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "BFI condition code output");
    if (conditional)
        b.opEndIf();
}
} // namespace
void emitBfiR(DecoderContext& c, InstBfiR i) {
    emitInsert(c, i.base, ir::Value::createRegister(i.src_b),
               ir::Value::createRegister(i.base.src_c));
}
void emitBfiC(DecoderContext& c, InstBfiC i) {
    emitInsert(
        c, i.base,
        ir::Value::createConstMemory(CMem(i.cbuf_slot, RZ, i.cbuf_offset * 4)),
        ir::Value::createRegister(i.base.src_c));
}
void emitBfiRC(DecoderContext& c, InstBfiC i) {
    emitInsert(
        c, i.base, ir::Value::createRegister(i.base.src_c),
        ir::Value::createConstMemory(CMem(i.cbuf_slot, RZ, i.cbuf_offset * 4)));
}
void emitBfiI(DecoderContext& c, InstBfiI i) {
    emitInsert(
        c, i.base,
        ir::Value::createConstantU(getIntImm20(i.imm20_0, i.imm20_19, true)),
        ir::Value::createRegister(i.base.src_c));
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
