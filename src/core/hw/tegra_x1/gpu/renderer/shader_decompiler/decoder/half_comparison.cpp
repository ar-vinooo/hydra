#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/half_comparison.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

// Half operands are compared as F32: the conversion is exact, so the results
// match an F16 comparison, and F32 swizzles keep their full precision
struct HalfPair {
    ir::Value lhs;
    ir::Value rhs;
};

ir::Value halfToF32(ir::Builder& b, const ir::Value& word, bool high,
                    bool ftz) {
    auto bits = high ? b.opShiftRight(word, ir::Value::createConstantU(16))
                     : b.opBitwiseAnd(word, ir::Value::createConstantU(0xffff));
    // Denormal halves (exponent zero) flush to a signed zero
    if (ftz) {
        const auto denormal = b.opCompareEqual(
            b.opBitwiseAnd(bits, ir::Value::createConstantU(0x7c00)),
            ir::Value::createConstantU(0));
        bits = b.opSelect(denormal,
                          b.opBitwiseAnd(bits, ir::Value::createConstantU(0x8000)),
                          bits);
    }
    const auto half = b.opBitCast(b.opCast(bits, ir::ScalarType::U16),
                                  ir::ScalarType::F16);
    return b.opCast(half, ir::ScalarType::F32);
}

HalfPair extractHalves(ir::Builder& b, const ir::Value& word,
                       HalfSwizzle swizzle, bool ftz) {
    switch (swizzle) {
    case HalfSwizzle::F16:
        return {halfToF32(b, word, false, ftz), halfToF32(b, word, true, ftz)};
    case HalfSwizzle::F32: {
        auto value = b.opBitCast(word, ir::ScalarType::F32);
        if (ftz) {
            const auto denormal = b.opCompareEqual(
                b.opBitwiseAnd(word, ir::Value::createConstantU(0x7f800000)),
                ir::Value::createConstantU(0));
            value = b.opSelect(
                denormal,
                b.opBitCast(b.opBitwiseAnd(word,
                                           ir::Value::createConstantU(0x80000000)),
                            ir::ScalarType::F32),
                value);
        }
        return {value, value};
    }
    case HalfSwizzle::H0H0: {
        const auto value = halfToF32(b, word, false, ftz);
        return {value, value};
    }
    case HalfSwizzle::H1H1: {
        const auto value = halfToF32(b, word, true, ftz);
        return {value, value};
    }
    }
    unreachable();
}

struct HalfCompare {
    ir::Value lhs;
    ir::Value rhs;
};

HalfCompare compareHalves(DecoderContext& context, instruction_t inst,
                          HalfOperandForm form, bool ftz) {
    auto& b = context.builder;
    const auto src_a = static_cast<reg_t>(extractBits(inst, 8, 8));
    const auto swizzle_a = static_cast<HalfSwizzle>(extractBits(inst, 47, 2));
    const bool neg_a = extractBits(inst, 43, 1) != 0;
    const bool abs_a = extractBits(inst, 44, 1) != 0;

    ir::Value src_b = ir::Value::createUndefined();
    HalfSwizzle swizzle_b = HalfSwizzle::F16;
    bool neg_b = false;
    bool abs_b = false;
    FloatCmpOp op;
    switch (form) {
    case HalfOperandForm::Register:
        src_b = b.opCopy(ir::Value::createRegister(
            static_cast<reg_t>(extractBits(inst, 20, 8))));
        swizzle_b = static_cast<HalfSwizzle>(extractBits(inst, 28, 2));
        abs_b = extractBits(inst, 30, 1) != 0;
        neg_b = extractBits(inst, 31, 1) != 0;
        op = static_cast<FloatCmpOp>(extractBits(inst, 35, 4));
        break;
    case HalfOperandForm::ConstBuffer:
        // The constant is a single F32 compared with both halves
        src_b = b.opCopy(ir::Value::createConstMemory(
            CMem(static_cast<u32>(extractBits(inst, 34, 5)), RZ,
                 extractBits(inst, 20, 14) * 4)));
        swizzle_b = HalfSwizzle::F32;
        neg_b = extractBits(inst, 56, 1) != 0;
        op = static_cast<FloatCmpOp>(extractBits(inst, 49, 4));
        break;
    case HalfOperandForm::Immediate: {
        const auto low = static_cast<u32>(extractBits(inst, 20, 9));
        const auto neg_low = static_cast<u32>(extractBits(inst, 29, 1));
        const auto high = static_cast<u32>(extractBits(inst, 30, 9));
        const auto neg_high = static_cast<u32>(extractBits(inst, 56, 1));
        src_b = ir::Value::createConstantU((low << 6) | (neg_low << 15) |
                                           (high << 22) | (neg_high << 31));
        op = static_cast<FloatCmpOp>(extractBits(inst, 49, 4));
        break;
    }
    }

    auto a = extractHalves(b, b.opCopy(ir::Value::createRegister(src_a)),
                           swizzle_a, ftz);
    auto bb = extractHalves(b, src_b, swizzle_b, ftz);
    a.lhs = absNegIf(b, a.lhs, abs_a, neg_a);
    a.rhs = absNegIf(b, a.rhs, abs_a, neg_a);
    bb.lhs = absNegIf(b, bb.lhs, abs_b, neg_b);
    bb.rhs = absNegIf(b, bb.rhs, abs_b, neg_b);

    const auto pred = notIf(
        b, ir::Value::createPredicate(static_cast<pred_t>(extractBits(inst, 39, 3))),
        extractBits(inst, 42, 1) != 0);
    const auto bop = static_cast<BoolOp>(extractBits(inst, 45, 2));
    return {getLogical(context, bop, getFloatCmp(context, op, a.lhs, bb.lhs), pred),
            getLogical(context, bop, getFloatCmp(context, op, a.rhs, bb.rhs), pred)};
}

} // namespace

void emitHset2(DecoderContext& context, instruction_t inst,
               HalfOperandForm form) {
    auto& b = context.builder;
    const auto conditional =
        handlePredCond(b, static_cast<pred_t>(extractBits(inst, 16, 3)),
                       extractBits(inst, 19, 1) != 0);
    const bool bf = extractBits(inst, form == HalfOperandForm::Register ? 49 : 53,
                                1) != 0;
    const bool ftz = extractBits(inst, form == HalfOperandForm::Register ? 50 : 54,
                                 1) != 0;
    const auto result = compareHalves(context, inst, form, ftz);
    const u32 true_value = bf ? 0x3c00 : 0xffff;
    const auto zero = ir::Value::createConstantU(0);
    const auto value = b.opBitwiseOr(
        b.opSelect(result.lhs, ir::Value::createConstantU(true_value), zero),
        b.opSelect(result.rhs, ir::Value::createConstantU(true_value << 16), zero));
    b.opCopy(ir::Value::createRegister(static_cast<reg_t>(extractBits(inst, 0, 8))),
             value);
    if (conditional)
        b.opEndIf();
}

void emitHsetp2(DecoderContext& context, instruction_t inst,
                HalfOperandForm form) {
    auto& b = context.builder;
    const auto conditional =
        handlePredCond(b, static_cast<pred_t>(extractBits(inst, 16, 3)),
                       extractBits(inst, 19, 1) != 0);
    const bool ftz = extractBits(inst, 6, 1) != 0;
    const bool h_and =
        extractBits(inst, form == HalfOperandForm::Register ? 49 : 53, 1) != 0;
    if (form == HalfOperandForm::ConstBuffer && extractBits(inst, 54, 1) != 0)
        ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler, "HSETP2 cbuf |b|"));
    auto result = compareHalves(context, inst, form, ftz);
    // The AND of both halves and its inverse
    if (h_and) {
        const auto both = b.opBitwiseAnd(result.lhs, result.rhs);
        result = {both, b.opNot(both)};
    }
    const auto dst_a = static_cast<pred_t>(extractBits(inst, 3, 3));
    const auto dst_b = static_cast<pred_t>(extractBits(inst, 0, 3));
    if (dst_a != PT)
        b.opCopy(ir::Value::createPredicate(dst_a), result.lhs);
    if (dst_b != PT)
        b.opCopy(ir::Value::createPredicate(dst_b), result.rhs);
    if (conditional)
        b.opEndIf();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
