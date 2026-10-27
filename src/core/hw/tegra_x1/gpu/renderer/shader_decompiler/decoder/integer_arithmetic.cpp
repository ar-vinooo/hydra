#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/integer_arithmetic.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

void emitIadd(DecoderContext& context, pred_t pred, bool pred_inv,
              AvgMode avg_mode, reg_t dst, reg_t src_a, ir::Value src_b,
              bool write_cc, bool extended, bool saturate) {
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, pred, pred_inv);
    const auto one = ir::Value::createConstantU(1);
    const auto zero = ir::Value::createConstantU(0);
    auto a = b.opCopy(ir::Value::createRegister(src_a));
    auto other = b.opBitCast(src_b, ir::ScalarType::U32);
    // A negated operand is inverted and the missing +1 enters as the carry:
    // 1 for a plain add, the carry flag for .X. This is what makes a 64-bit
    // subtraction (IADD.CC, IADD.X) borrow correctly and gives a negated
    // operand the carry of the 33-bit sum.
    const bool negated = avg_mode == AvgMode::NegA || avg_mode == AvgMode::NegB;
    if (avg_mode == AvgMode::NegA) a = b.opBitwiseNot(a);
    if (avg_mode == AvgMode::NegB) other = b.opBitwiseNot(other);
    const auto sum = b.opAdd(a, other);
    auto carry_in = zero;
    if (extended)
        carry_in = b.opSelect(ir::Value::createConditionFlag(2), one, zero);
    else if (negated)
        carry_in = one;
    if (avg_mode == AvgMode::PlusOne) carry_in = b.opAdd(carry_in, one);
    auto result = b.opAdd(sum, carry_in);
    const auto sign = [&](ir::Value v) { return b.opCompareNotEqual(b.opBitwiseAnd(v, ir::Value::createConstantU(0x80000000u)), zero); };
    // Signed overflow of the unsaturated sum, shared by SAT and the CC flags.
    std::optional<ir::Value> overflow;
    if (saturate || write_cc)
        overflow = sign(b.opBitwiseAnd(b.opBitwiseNot(b.opBitwiseXor(a, other)), b.opBitwiseXor(a, result)));
    if (saturate) {
        // IADD.SAT clamps the signed sum; overflow has the sign of the operands.
        const auto clamp = b.opSelect(sign(a), ir::Value::createConstantU(0x80000000u),
                                      ir::Value::createConstantU(0x7fffffffu));
        result = b.opSelect(*overflow, clamp, result);
    }
    if (write_cc) {
        const auto carry = b.opBitwiseOr(b.opCompareLess(sum, a), b.opCompareLess(b.opAdd(sum, carry_in), sum));
        b.opCopy(ir::Value::createConditionFlag(0), b.opCompareEqual(result, zero));
        b.opCopy(ir::Value::createConditionFlag(1), sign(result));
        b.opCopy(ir::Value::createConditionFlag(2), carry);
        b.opCopy(ir::Value::createConditionFlag(3), *overflow);
    }
    b.opCopy(ir::Value::createRegister(dst), result);
    if (conditional) b.opEndIf();
}

// lhs + rhs (+ the carry flag when extended). With write_cc the flags of the
// sum go to CC like IADD.CC; the result is still copied by the caller.
ir::Value addWithCarry(DecoderContext& context, ir::Value lhs, ir::Value rhs,
                       bool extended, bool write_cc) {
    auto& b = context.builder;
    const auto zero = ir::Value::createConstantU(0);
    const auto sign = [&](ir::Value v) {
        return b.opCompareNotEqual(
            b.opBitwiseAnd(v, ir::Value::createConstantU(0x80000000u)), zero);
    };
    const auto sum = b.opAdd(lhs, rhs);
    auto result = sum;
    if (extended)
        result = b.opAdd(sum, b.opSelect(ir::Value::createConditionFlag(2),
                                         ir::Value::createConstantU(1), zero));
    if (write_cc) {
        const auto carry = b.opBitwiseOr(b.opCompareLess(sum, lhs),
                                         b.opCompareLess(result, sum));
        const auto overflow = sign(b.opBitwiseAnd(
            b.opBitwiseNot(b.opBitwiseXor(lhs, rhs)), b.opBitwiseXor(lhs, result)));
        b.opCopy(ir::Value::createConditionFlag(0), b.opCompareEqual(result, zero));
        b.opCopy(ir::Value::createConditionFlag(1), sign(result));
        b.opCopy(ir::Value::createConditionFlag(2), carry);
        b.opCopy(ir::Value::createConditionFlag(3), overflow);
    }
    return result;
}

} // namespace

// IMAD Rd, Ra, b, c: the low (or with .HI the high) word of the 32x32-bit
// product plus c, with the carry (.X) and flags (.CC) of IADD. .SAT clamps the
// signed sum, which PTX defines for mad.hi.sat.s32.
void emitImad(DecoderContext& context, instruction_t inst, ImadForm form) {
    auto& b = context.builder;
    const bool write_cc = extractBits(inst, 47, 1) != 0;
    const bool a_signed = extractBits(inst, 48, 1) != 0;
    const bool extended = extractBits(inst, 49, 1) != 0;
    const bool saturate = extractBits(inst, 50, 1) != 0;
    const bool b_signed = extractBits(inst, 53, 1) != 0;
    const bool high = extractBits(inst, 54, 1) != 0;
    if (saturate && (!high || !a_signed || !b_signed || write_cc || extended)) {
        SHADER_UNSUPPORTED("IMAD.SAT without .HI.S32.S32 or with .X/.CC");
        return;
    }
    const auto reg = [&](u32 offset) {
        return b.opCopy(ir::Value::createRegister(
            static_cast<reg_t>(extractBits(inst, offset, 8)), ir::ScalarType::U32));
    };
    const auto cbuf = [&] {
        return b.opBitCast(
            b.opCopy(ir::Value::createConstMemory(
                CMem(static_cast<u32>(extractBits(inst, 34, 5)), RZ,
                     extractBits(inst, 20, 14) * 4))),
            ir::ScalarType::U32);
    };
    const auto conditional =
        handlePredCond(b, static_cast<pred_t>(extractBits(inst, 16, 3)),
                       extractBits(inst, 19, 1) != 0);
    const auto a = reg(8);
    ir::Value src_b = ir::Value::createConstantU(0);
    ir::Value src_c = ir::Value::createConstantU(0);
    switch (form) {
    case ImadForm::Register:
        src_b = reg(20);
        src_c = reg(39);
        break;
    case ImadForm::RegisterConstBuffer:
        src_b = reg(39);
        src_c = cbuf();
        break;
    case ImadForm::ConstBuffer:
        src_b = cbuf();
        src_c = reg(39);
        break;
    case ImadForm::Immediate:
        src_b = ir::Value::createConstantU(
            getIntImm20(static_cast<u32>(extractBits(inst, 20, 19)),
                        static_cast<u32>(extractBits(inst, 56, 1)), true));
        src_c = reg(39);
        break;
    }
    const auto product =
        high ? b.opIntrinsic(ir::Intrinsic::IMulHi, ir::ScalarType::U32,
                             {a, src_b,
                              ir::Value::createConstantU(
                                  (a_signed ? 1u : 0u) | (b_signed ? 2u : 0u))})
             : b.opMultiply(a, src_b);
    auto result = addWithCarry(context, product, src_c, extended, write_cc);
    if (saturate) {
        const auto zero = ir::Value::createConstantU(0);
        const auto sign = [&](ir::Value v) {
            return b.opCompareNotEqual(
                b.opBitwiseAnd(v, ir::Value::createConstantU(0x80000000u)), zero);
        };
        const auto overflow = sign(b.opBitwiseAnd(
            b.opBitwiseNot(b.opBitwiseXor(product, src_c)),
            b.opBitwiseXor(product, result)));
        const auto clamp = b.opSelect(sign(product),
                                      ir::Value::createConstantU(0x80000000u),
                                      ir::Value::createConstantU(0x7fffffffu));
        result = b.opSelect(overflow, clamp, result);
    }
    b.opCopy(ir::Value::createRegister(static_cast<reg_t>(extractBits(inst, 0, 8)),
                                       ir::ScalarType::U32),
             result);
    if (conditional)
        b.opEndIf();
}

namespace {

void emitIscadd(DecoderContext& context, pred_t pred, bool pred_inv,
                AvgMode avg_mode, reg_t dst, reg_t src_a, u32 shift,
                ir::Value src_b, bool write_cc = false) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    auto src_a_v = negIf(context.builder,
                         ir::Value::createRegister(src_a, ir::ScalarType::U32),
                         avg_mode == AvgMode::NegA);
    src_a_v =
        context.builder.opShiftLeft(src_a_v, ir::Value::createConstantU(shift));
    auto src_b_v = negIf(context.builder, context.builder.opBitCast(src_b, ir::ScalarType::U32), avg_mode == AvgMode::NegB);

    if (write_cc && avg_mode == AvgMode::PlusOne)
        SHADER_UNSUPPORTED("ISCADD.PO condition code");
    auto res = addWithCarry(context, src_a_v, src_b_v, false,
                            write_cc && avg_mode != AvgMode::PlusOne);
    if (avg_mode == AvgMode::PlusOne)
        res = context.builder.opAdd(res, ir::Value::createConstantU(1));
    context.builder.opCopy(ir::Value::createRegister(dst, ir::ScalarType::U32),
                           res);

    if (conditional)
        context.builder.opEndIf();
}

// TODO: check this
ir::Value extend16To32(DecoderContext& context, ir::Value value, bool high) {
    if (high)
        return context.builder.opShiftRight(value,
                                            ir::Value::createConstantU(16));
    else
        return context.builder.opBitfieldExtract(
            value, ir::Value::createConstantU(0),
            ir::Value::createConstantU(16));
}

// TODO: verify
void emitXmad(DecoderContext& context, pred_t pred, bool pred_inv, XmadCop mode,
              bool product_shift_left, bool merge, reg_t dst, reg_t src_a,
              bool a_signed, bool high_a, ir::Value src_b, bool high_b,
              ir::Value src_c, bool extended, bool write_cc) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    auto src_a_v = ir::Value::createRegister(
        src_a, a_signed ? ir::ScalarType::I32 : ir::ScalarType::U32);
    src_a_v = extend16To32(context, src_a_v, high_a);
    auto src_b_v = extend16To32(context, src_b, high_b);

    auto res = context.builder.opMultiply(
        ensureIntegerSignedness<false>(context.builder, src_a_v),
        ensureIntegerSignedness<false>(context.builder, src_b_v));
    if (product_shift_left)
        res = context.builder.opShiftLeft(res, ir::Value::createConstantU(16));

    auto src_c_v = src_c;
    switch (mode) {
    case XmadCop::Cfull:
        break;
    case XmadCop::Clo:
        src_c_v = extend16To32(context, src_c, false);
        break;
    case XmadCop::Chi:
        src_c_v = extend16To32(context, src_c, true);
        break;
    case XmadCop::Cbcc:
        src_c_v = context.builder.opAdd(
            src_c, context.builder.opShiftLeft(
                       ensureIntegerSignedness<false>(context.builder, src_b),
                       ir::Value::createConstantU(16)));
        break;
    case XmadCop::Csfu: {
        const auto src_a_adjusted = context.builder.opShiftLeft(
            context.builder.opShiftRight(
                ensureIntegerSignedness<false>(context.builder, src_a_v),
                ir::Value::createConstantU(31)),
            ir::Value::createConstantU(16));
        const auto src_b_adjusted = context.builder.opShiftLeft(
            context.builder.opShiftRight(
                ensureIntegerSignedness<false>(context.builder, src_b_v),
                ir::Value::createConstantU(31)),
            ir::Value::createConstantU(16));
        src_c_v = context.builder.opAdd(
            src_c, context.builder.opNeg(
                       context.builder.opAdd(src_a_adjusted, src_b_adjusted)));
        break;
    }
    }

    // The flags come from the addition, like IADD
    auto& b = context.builder;
    const auto product = b.opBitCast(res, ir::ScalarType::U32);
    const auto addend = b.opBitCast(src_c_v, ir::ScalarType::U32);
    const auto sum = b.opAdd(product, addend);
    res = sum;
    if (extended)
        res = b.opAdd(sum, b.opSelect(ir::Value::createConditionFlag(2),
                                      ir::Value::createConstantU(1),
                                      ir::Value::createConstantU(0)));
    if (write_cc) {
        const auto zero = ir::Value::createConstantU(0);
        const auto sign = [&](ir::Value v) {
            return b.opCompareNotEqual(
                b.opBitwiseAnd(v, ir::Value::createConstantU(0x80000000u)),
                zero);
        };
        const auto overflow =
            sign(b.opBitwiseAnd(b.opBitwiseNot(b.opBitwiseXor(product, addend)),
                                b.opBitwiseXor(product, res)));
        const auto carry = b.opBitwiseOr(b.opCompareLess(sum, product),
                                         b.opCompareLess(res, sum));
        b.opCopy(ir::Value::createConditionFlag(0), b.opCompareEqual(res, zero));
        b.opCopy(ir::Value::createConditionFlag(1), sign(res));
        b.opCopy(ir::Value::createConditionFlag(2), carry);
        b.opCopy(ir::Value::createConditionFlag(3), overflow);
    }

    if (merge) {
        res = context.builder.opBitwiseAnd(res,
                                           ir::Value::createConstantU(0xffff));
        res = context.builder.opBitwiseOr(
            res,
            context.builder.opShiftLeft(src_b, ir::Value::createConstantU(16)));
    }

    context.builder.opCopy(ir::Value::createRegister(dst, ir::ScalarType::U32),
                           res);

    if (conditional)
        context.builder.opEndIf();
}

// TODO: x, write_cc
void emitIntegerMinMax(DecoderContext& context, pred_t pred, bool pred_inv,
                       reg_t dst, pred_t src_pred, bool src_pred_inv,
                       bool is_signed, reg_t src_a, ir::Value src_b) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    const auto type = is_signed ? ir::ScalarType::I32 : ir::ScalarType::U32;
    const auto src_a_v = ir::Value::createRegister(src_a, type);
    const auto src_b_v = is_signed
                             ? ensureIntegerSignedness<true>(context.builder, src_b)
                             : ensureIntegerSignedness<false>(context.builder, src_b);
    const auto min_v = context.builder.opMin(src_a_v, src_b_v);
    const auto max_v = context.builder.opMax(src_a_v, src_b_v);
    const auto res = context.builder.opSelect(
        notIf(context.builder, ir::Value::createPredicate(src_pred),
              src_pred_inv),
        min_v, max_v);
    context.builder.opCopy(ir::Value::createRegister(dst, type), res);

    if (conditional)
        context.builder.opEndIf();
}

ir::Value extractIadd3Half(DecoderContext& context, ir::Value value,
                           Iadd3Half half) {
    switch (half) {
    case Iadd3Half::Lower:
        return context.builder.opBitfieldExtract(
            ensureIntegerSignedness<false>(context.builder, value),
            ir::Value::createConstantU(0), ir::Value::createConstantU(16));
    case Iadd3Half::Upper:
        return context.builder.opShiftRight(
            ensureIntegerSignedness<false>(context.builder, value),
            ir::Value::createConstantU(16));
    default:
        return value;
    }
}

void emitIadd3(DecoderContext& context, InstIadd3Base base,
               ir::Value src_b, Iadd3Half half_a = Iadd3Half::Full,
               Iadd3Half half_b = Iadd3Half::Full,
               Iadd3Half half_c = Iadd3Half::Full,
               Iadd3Shift shift = Iadd3Shift::None) {
    // A three-input carry can be 2, which one carry flag cannot hold
    if (base.x || base.write_cc)
        SHADER_UNSUPPORTED("IADD3 X/CC");
    const auto conditional =
        handlePredCond(context.builder, base.pred, base.pred_inv);

    auto a = extractIadd3Half(
        context, ir::Value::createRegister(base.src_a, ir::ScalarType::U32),
        half_a);
    auto b = extractIadd3Half(
        context, ensureIntegerSignedness<false>(context.builder, src_b), half_b);
    auto c = extractIadd3Half(
        context, ir::Value::createRegister(base.src_c, ir::ScalarType::U32),
        half_c);
    a = ensureIntegerSignedness<false>(context.builder, a);
    b = ensureIntegerSignedness<false>(context.builder, b);
    c = ensureIntegerSignedness<false>(context.builder, c);
    a = negIf(context.builder, a, base.neg_a);
    b = negIf(context.builder, b, base.neg_b);
    c = negIf(context.builder, c, base.neg_c);

    auto res = context.builder.opAdd(a, b);
    switch (shift) {
    case Iadd3Shift::Right:
        res = context.builder.opShiftRight(
            ensureIntegerSignedness<false>(context.builder, res),
            ir::Value::createConstantU(16));
        break;
    case Iadd3Shift::Left:
        res = context.builder.opShiftLeft(res, ir::Value::createConstantU(16));
        break;
    default:
        break;
    }
    res = context.builder.opAdd(ensureIntegerSignedness<false>(context.builder, res),
                                c);
    context.builder.opCopy(
        ir::Value::createRegister(base.dst, ir::ScalarType::U32), res);

    if (conditional)
        context.builder.opEndIf();
}

} // namespace

void emitImnmxR(DecoderContext& context, InstImnmxR inst) {
    emitIntegerMinMax(context, inst.base.pred, inst.base.pred_inv, inst.base.dst,
                      inst.base.src_pred, inst.base.src_pred_inv,
                      inst.base.is_signed, inst.base.src_a,
                      ir::Value::createRegister(inst.src_b, ir::ScalarType::I32));
}

void emitImnmxC(DecoderContext& context, InstImnmxC inst) {
    emitIntegerMinMax(
        context, inst.base.pred, inst.base.pred_inv, inst.base.dst,
        inst.base.src_pred, inst.base.src_pred_inv, inst.base.is_signed,
        inst.base.src_a,
        ir::Value::createConstMemory(
            CMem(inst.cbuf_slot, RZ, static_cast<u64>(inst.cbuf_offset * 4)),
            ir::ScalarType::I32));
}

void emitImnmxI(DecoderContext& context, InstImnmxI inst) {
    emitIntegerMinMax(context, inst.base.pred, inst.base.pred_inv, inst.base.dst,
                      inst.base.src_pred, inst.base.src_pred_inv,
                      inst.base.is_signed, inst.base.src_a,
                      ir::Value::createConstant(
                          getIntImm20(inst.imm20_0, inst.imm20_19, true),
                          ir::ScalarType::I32));
}

void emitIadd3R(DecoderContext& context, InstIadd3R inst) {
    emitIadd3(context, inst.base,
              ir::Value::createRegister(inst.src_b, ir::ScalarType::I32),
              inst.half_a, inst.half_b, inst.half_c, inst.shift);
}

void emitIadd3C(DecoderContext& context, InstIadd3C inst) {
    emitIadd3(context, inst.base,
              ir::Value::createConstMemory(
                  CMem(inst.cbuf_slot, RZ,
                       static_cast<u64>(inst.cbuf_offset * 4)),
                  ir::ScalarType::I32));
}

void emitIadd3I(DecoderContext& context, InstIadd3I inst) {
    emitIadd3(context, inst.base,
              ir::Value::createConstant(
                  getIntImm20(inst.imm20_0, inst.imm20_19, true),
                  ir::ScalarType::I32));
}

void emitIaddR(DecoderContext& context, InstIaddR inst) {
    emitIadd(context, inst.base.pred, inst.base.pred_inv, inst.base.avg_mode,
             inst.base.dst, inst.base.src_a,
             ir::Value::createRegister(inst.src_b, ir::ScalarType::I32), inst.base.write_cc, inst.base.x, inst.base.sat);
}

void emitIaddC(DecoderContext& context, InstIaddC inst) {
    emitIadd(
        context, inst.base.pred, inst.base.pred_inv, inst.base.avg_mode,
        inst.base.dst, inst.base.src_a,
        ir::Value::createConstMemory(
            CMem(inst.cbuf_slot, RZ, static_cast<u64>(inst.cbuf_offset * 4)),
            ir::ScalarType::I32), inst.base.write_cc, inst.base.x, inst.base.sat);
}

void emitIaddI(DecoderContext& context, InstIaddI inst) {
    emitIadd(context, inst.base.pred, inst.base.pred_inv, inst.base.avg_mode,
             inst.base.dst, inst.base.src_a,
             ir::Value::createConstant(
                 getIntImm20(inst.imm20_0, inst.imm20_19, true),
                 ir::ScalarType::I32), inst.base.write_cc, inst.base.x, inst.base.sat);
}

void emitIadd32I(DecoderContext& context, InstIadd32I inst) {
    emitIadd(context, inst.pred, inst.pred_inv, inst.avg_mode, inst.dst,
             inst.src_a,
             ir::Value::createConstant(inst.imm, ir::ScalarType::I32), inst.write_cc, inst.x, inst.sat);
}

void emitIscaddR(DecoderContext& context, InstIscaddR inst) {
    emitIscadd(context, inst.base.pred, inst.base.pred_inv, inst.base.avg_mode,
               inst.base.dst, inst.base.src_a, inst.base.shift,
               ir::Value::createRegister(inst.src_b, ir::ScalarType::I32),
               inst.base.write_cc);
}

void emitIscaddC(DecoderContext& context, InstIscaddC inst) {
    emitIscadd(
        context, inst.base.pred, inst.base.pred_inv, inst.base.avg_mode,
        inst.base.dst, inst.base.src_a, inst.base.shift,
        ir::Value::createConstMemory(
            CMem(inst.cbuf_slot, RZ, static_cast<u64>(inst.cbuf_offset * 4)),
            ir::ScalarType::I32),
        inst.base.write_cc);
}

void emitIscaddI(DecoderContext& context, InstIscaddI inst) {
    emitIscadd(context, inst.base.pred, inst.base.pred_inv, inst.base.avg_mode,
               inst.base.dst, inst.base.src_a, inst.base.shift,
               ir::Value::createConstant(
                   getIntImm20(inst.imm20_0, inst.imm20_19, true),
                   ir::ScalarType::I32),
               inst.base.write_cc);
}

// LEA: dst = (neg ? -a : a) << shift + b
void emitLeaLo(DecoderContext& context, InstLeaLoBase inst,
               ir::Value src_b) {
    if (static_cast<pred_t>(inst.dst_pred) != PT)
        SHADER_UNSUPPORTED("LEA predicate output");

    const auto conditional =
        handlePredCond(context.builder, inst.pred, inst.pred_inv);

    auto src_a = negIf(context.builder,
                       ir::Value::createRegister(inst.src_a, ir::ScalarType::U32),
                       inst.neg_a);
    src_a = context.builder.opShiftLeft(src_a,
                                        ir::Value::createConstantU(inst.shift));
    context.builder.opCopy(
        ir::Value::createRegister(inst.dst, ir::ScalarType::U32),
        addWithCarry(context, src_a,
                     context.builder.opBitCast(src_b, ir::ScalarType::U32),
                     inst.x, inst.write_cc));

    if (conditional)
        context.builder.opEndIf();
}

void emitLeaLoR(DecoderContext& context, InstLeaLoR inst) {
    emitLeaLo(context, inst.base,
              ir::Value::createRegister(inst.src_b, ir::ScalarType::I32));
}

void emitLeaLoC(DecoderContext& context, InstLeaLoC inst) {
    emitLeaLo(context, inst.base,
              ir::Value::createConstMemory(
                  CMem(inst.cbuf_slot, RZ,
                       static_cast<u64>(inst.cbuf_offset * 4)),
                  ir::ScalarType::I32));
}

void emitLeaLoI(DecoderContext& context, InstLeaLoI inst) {
    emitLeaLo(context, inst.base,
              ir::Value::createConstant(
                  getIntImm20(inst.imm20_0, inst.imm20_19, true),
                  ir::ScalarType::I32));
}

namespace {

// LEA.HI: dst = low word of ((neg ? -a : a) >> (32 - shift)) + b, where a is
// the 64-bit value a_hi:a_lo
void emitLeaHi(DecoderContext& context, pred_t pred, bool pred_inv, reg_t dst,
               reg_t src_a_lo, reg_t src_a_hi, u32 shift, bool neg_a, bool x,
               bool write_cc, pred_t dst_pred, ir::Value src_b) {
    if (dst_pred != PT)
        SHADER_UNSUPPORTED("LEA.HI predicate output");

    auto& builder = context.builder;
    const auto conditional = handlePredCond(builder, pred, pred_inv);

    auto lo = ir::Value::createRegister(src_a_lo, ir::ScalarType::U32);
    auto hi = ir::Value::createRegister(src_a_hi, ir::ScalarType::U32);
    if (neg_a) {
        // 64-bit two's complement negation
        const auto lo_zero =
            builder.opCompareEqual(lo, ir::Value::createConstantU(0));
        lo = builder.opNeg(lo);
        lo = builder.opBitCast(lo, ir::ScalarType::U32);
        hi = builder.opBitwiseNot(hi);
        hi = builder.opAdd(hi, builder.opSelect(lo_zero,
                                                ir::Value::createConstantU(1),
                                                ir::Value::createConstantU(0)));
    }

    const auto word =
        (shift == 0
             ? hi
             : builder.opBitwiseOr(
                   builder.opShiftRight(lo,
                                        ir::Value::createConstantU(32 - shift)),
                   builder.opShiftLeft(hi, ir::Value::createConstantU(shift))));
    builder.opCopy(ir::Value::createRegister(dst, ir::ScalarType::U32),
                   addWithCarry(context, word,
                                builder.opBitCast(src_b, ir::ScalarType::U32),
                                x, write_cc));

    if (conditional)
        builder.opEndIf();
}

} // namespace

void emitLeaHiR(DecoderContext& context, InstLeaHiR inst) {
    emitLeaHi(context, inst.pred, inst.pred_inv, inst.dst, inst.src_a_lo,
              inst.src_a_hi, inst.shift, inst.neg_a, inst.x, inst.write_cc,
              inst.dst_pred,
              ir::Value::createRegister(inst.src_b, ir::ScalarType::U32));
}

void emitLeaHiC(DecoderContext& context, InstLeaHiC inst) {
    emitLeaHi(context, inst.pred, inst.pred_inv, inst.dst, inst.src_a_lo,
              inst.src_a_hi, inst.shift, inst.neg_a, inst.x, inst.write_cc,
              inst.dst_pred,
              ir::Value::createConstMemory(
                  CMem(inst.cbuf_slot, RZ,
                       static_cast<u64>(inst.cbuf_offset * 4)),
                  ir::ScalarType::U32));
}

void emitIscadd32I(DecoderContext& context, InstIscadd32I inst) {
    emitIscadd(context, inst.pred, inst.pred_inv, AvgMode::NoNeg, inst.dst,
               inst.src_a, inst.shift,
               ir::Value::createConstant(inst.imm, ir::ScalarType::I32),
               inst.write_cc);
}

void emitXmadR(DecoderContext& context, InstXmadR inst) {
    emitXmad(context, inst.base.pred, inst.base.pred_inv, inst.cop, inst.psl,
             inst.mrg, inst.base.dst, inst.base.src_a, inst.base.a_signed,
             inst.base.hilo_a,
             ir::Value::createRegister(inst.src_b, inst.base.b_signed
                                                       ? ir::ScalarType::I32
                                                       : ir::ScalarType::U32),
             inst.hilo_b,
             ir::Value::createRegister(inst.src_c, ir::ScalarType::U32),
             inst.base.x, inst.base.write_cc);
}

void emitXmadRC(DecoderContext& context, InstXmadRC inst) {
    emitXmad(
        context, inst.base.pred, inst.base.pred_inv, inst.cop, false, false,
        inst.base.dst, inst.base.src_a, inst.base.a_signed, inst.base.hilo_a,
        ir::Value::createRegister(inst.src_b, inst.base.b_signed
                                                  ? ir::ScalarType::I32
                                                  : ir::ScalarType::U32),
        inst.hilo_b,
        ir::Value::createConstMemory(
            CMem(inst.cbuf_slot, RZ, static_cast<u64>(inst.cbuf_offset * 4)),
            ir::ScalarType::U32),
        inst.x, inst.base.write_cc);
}

void emitXmadC(DecoderContext& context, InstXmadC inst) {
    emitXmad(
        context, inst.base.pred, inst.base.pred_inv, inst.cop, inst.psl,
        inst.mrg,
        inst.base.dst, inst.base.src_a, inst.base.a_signed, inst.base.hilo_a,
        ir::Value::createConstMemory(
            CMem(inst.cbuf_slot, RZ, static_cast<u64>(inst.cbuf_offset * 4)),
            inst.base.b_signed ? ir::ScalarType::I32 : ir::ScalarType::U32),
        inst.hilo_b,
        ir::Value::createRegister(inst.src_c, ir::ScalarType::U32), inst.x,
        inst.base.write_cc);
}

void emitXmadI(DecoderContext& context, InstXmadI inst) {
    emitXmad(context, inst.base.pred, inst.base.pred_inv, inst.cop, inst.psl,
             inst.mrg, inst.base.dst, inst.base.src_a, inst.base.a_signed,
             inst.base.hilo_a,
             ir::Value::createConstant(inst.imm16, inst.base.b_signed
                                                       ? ir::ScalarType::I32
                                                       : ir::ScalarType::U32),
             false, ir::Value::createRegister(inst.src_c, ir::ScalarType::U32),
             inst.base.x, inst.base.write_cc);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
