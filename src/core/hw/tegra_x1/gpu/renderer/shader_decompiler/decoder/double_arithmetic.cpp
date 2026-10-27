#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/double_arithmetic.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/comparison_helper.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

// FP64 operands are register pairs (low word first). Encodings follow
// envytools' gm107 tables: dadd 5c70/4c70/3870, dmul 5c80/4c80/3880, dfma
// 5b70/4b70/3670/5370, dmnmx 5c50/4c50/3850, dsetp 5b80/4b80/3680, dset
// 5900/4900/3200.
struct Double {
    ir::Value lo;
    ir::Value hi;
};

Double registerPair(instruction_t inst, u32 offset) {
    const auto reg = static_cast<reg_t>(extractBits(inst, offset, 8));
    return {ir::Value::createRegister(reg, ir::ScalarType::U32),
            ir::Value::createRegister(offsetRegister(reg, 1),
                                      ir::ScalarType::U32)};
}

Double sourceB(instruction_t inst, DoubleSource source) {
    switch (source) {
    case DoubleSource::Register:
        return registerPair(inst, 20);
    case DoubleSource::Constant: {
        const auto slot = static_cast<u32>(extractBits(inst, 34, 5));
        const auto offset = static_cast<u64>(extractBits(inst, 20, 14)) * 4;
        return {ir::Value::createConstMemory(CMem(slot, RZ, offset)),
                ir::Value::createConstMemory(CMem(slot, RZ, offset + 4))};
    }
    case DoubleSource::Immediate:
    default: {
        // The top 20 bits of the double
        const auto field = static_cast<u32>(extractBits(inst, 20, 19)) |
                           (static_cast<u32>(extractBits(inst, 56, 1)) << 19);
        return {ir::Value::createConstantU(0),
                ir::Value::createConstantU(field << 12)};
    }
    }
}

Double modify(ir::Builder& b, Double value, bool abs, bool neg) {
    // Copy first: an operand register may be written before it is used again
    value.lo = b.opCopy(value.lo);
    value.hi = b.opCopy(value.hi);
    if (abs)
        value.hi = b.opBitwiseAnd(value.hi,
                                  ir::Value::createConstantU(0x7fffffffu));
    if (neg)
        value.hi = b.opBitwiseXor(value.hi,
                                  ir::Value::createConstantU(0x80000000u));
    return value;
}

bool bit(instruction_t inst, u32 pos) { return extractBits(inst, pos, 1) != 0; }

void writePair(ir::Builder& b, reg_t dst, const ir::Value& lo,
               const ir::Value& hi) {
    if (dst == RZ)
        return;
    b.opCopy(ir::Value::createRegister(dst, ir::ScalarType::U32), lo);
    b.opCopy(ir::Value::createRegister(offsetRegister(dst, 1),
                                       ir::ScalarType::U32),
             hi);
}

bool begin(DecoderContext& context, instruction_t inst) {
    return handlePredCond(context.builder,
                          static_cast<pred_t>(extractBits(inst, 16, 3)),
                          bit(inst, 19));
}

// Fused multiply-add of the two result words
void emitFma(DecoderContext& context, instruction_t inst, const Double& a,
             const Double& b, const Double& c, u32 rounding) {
    auto& builder = context.builder;
    const auto word = [&](bool high) {
        return builder.opIntrinsic(
            ir::Intrinsic::F64Fma, ir::ScalarType::U32,
            {a.lo, a.hi, b.lo, b.hi, c.lo, c.hi,
             ir::Value::createConstantU(rounding | (high ? 4u : 0u))});
    };
    const auto lo = word(false);
    const auto hi = word(true);
    writePair(builder, static_cast<reg_t>(extractBits(inst, 0, 8)), lo, hi);
}

constexpr u32 DOUBLE_ONE_HI = 0x3ff00000u;

void warnConditionCodes(instruction_t inst, const char* name) {
    if (bit(inst, 47))
        SHADER_UNSUPPORTED_ONCE("{}.CC", name);
}

} // namespace

void emitDadd(DecoderContext& context, instruction_t inst, DoubleSource source) {
    warnConditionCodes(inst, "DADD");
    const bool conditional = begin(context, inst);
    auto& b = context.builder;
    const auto a = modify(b, registerPair(inst, 8), bit(inst, 46), bit(inst, 48));
    const auto addend =
        modify(b, sourceB(inst, source), bit(inst, 49), bit(inst, 45));
    // a * 1.0 + b, a single rounding like the hardware add
    const Double one{ir::Value::createConstantU(0),
                     ir::Value::createConstantU(DOUBLE_ONE_HI)};
    emitFma(context, inst, a, one, addend,
            static_cast<u32>(extractBits(inst, 39, 2)));
    if (conditional)
        b.opEndIf();
}

void emitDmul(DecoderContext& context, instruction_t inst, DoubleSource source) {
    warnConditionCodes(inst, "DMUL");
    const bool conditional = begin(context, inst);
    auto& b = context.builder;
    const auto a = modify(b, registerPair(inst, 8), false, false);
    const auto factor = modify(b, sourceB(inst, source), false, bit(inst, 48));
    // a * b + signed zero keeps the sign of an exact zero product
    const Double zero{
        ir::Value::createConstantU(0),
        b.opBitwiseAnd(b.opBitwiseXor(a.hi, factor.hi),
                       ir::Value::createConstantU(0x80000000u))};
    emitFma(context, inst, a, factor, zero,
            static_cast<u32>(extractBits(inst, 39, 2)));
    if (conditional)
        b.opEndIf();
}

void emitDfma(DecoderContext& context, instruction_t inst, DoubleSource source) {
    warnConditionCodes(inst, "DFMA");
    const bool conditional = begin(context, inst);
    auto& b = context.builder;
    const auto a = modify(b, registerPair(inst, 8), false, false);
    const auto factor = modify(b, sourceB(inst, source), false, bit(inst, 48));
    const auto addend = modify(b, registerPair(inst, 39), false, bit(inst, 49));
    emitFma(context, inst, a, factor, addend,
            static_cast<u32>(extractBits(inst, 50, 2)));
    if (conditional)
        b.opEndIf();
}

void emitDfmaConstantAddend(DecoderContext& context, instruction_t inst) {
    warnConditionCodes(inst, "DFMA");
    const bool conditional = begin(context, inst);
    auto& b = context.builder;
    const auto a = modify(b, registerPair(inst, 8), false, false);
    const auto factor = modify(b, registerPair(inst, 39), false, bit(inst, 48));
    const auto addend = modify(b, sourceB(inst, DoubleSource::Constant), false,
                               bit(inst, 49));
    emitFma(context, inst, a, factor, addend,
            static_cast<u32>(extractBits(inst, 50, 2)));
    if (conditional)
        b.opEndIf();
}

void emitDmnmx(DecoderContext& context, instruction_t inst, DoubleSource source) {
    warnConditionCodes(inst, "DMNMX");
    const bool conditional = begin(context, inst);
    auto& b = context.builder;
    const auto a = modify(b, registerPair(inst, 8), bit(inst, 46), bit(inst, 48));
    const auto other =
        modify(b, sourceB(inst, source), bit(inst, 49), bit(inst, 45));
    // The predicate selects the minimum when true, like FMNMX
    auto select = ir::Value::createPredicate(
        static_cast<pred_t>(extractBits(inst, 39, 3)));
    select = notIf(b, select, bit(inst, 42));
    const auto word = [&](bool maximum, bool high) {
        return b.opIntrinsic(
            ir::Intrinsic::F64MinMax, ir::ScalarType::U32,
            {a.lo, a.hi, other.lo, other.hi,
             ir::Value::createConstantU((maximum ? 1u : 0u) | (high ? 2u : 0u))});
    };
    const auto lo = b.opSelect(select, word(false, false), word(true, false));
    const auto hi = b.opSelect(select, word(false, true), word(true, true));
    writePair(b, static_cast<reg_t>(extractBits(inst, 0, 8)), lo, hi);
    if (conditional)
        b.opEndIf();
}

namespace {

// Comparison result before the boolean operation with the source predicate
ir::Value compare(DecoderContext& context, instruction_t inst, const Double& a,
                  const Double& other) {
    auto& b = context.builder;
    const auto op = static_cast<u32>(extractBits(inst, 48, 4));
    const auto result = b.opIntrinsic(
        ir::Intrinsic::F64Compare, ir::ScalarType::U32,
        {a.lo, a.hi, other.lo, other.hi, ir::Value::createConstantU(op)});
    return b.opCompareNotEqual(result, ir::Value::createConstantU(0));
}

ir::Value combine(DecoderContext& context, instruction_t inst,
                  const ir::Value& flag) {
    auto& b = context.builder;
    const auto source = notIf(
        b,
        ir::Value::createPredicate(static_cast<pred_t>(extractBits(inst, 39, 3))),
        bit(inst, 42));
    return getLogical(context, static_cast<BoolOp>(extractBits(inst, 45, 2)),
                      flag, source);
}

} // namespace

void emitDsetp(DecoderContext& context, instruction_t inst, DoubleSource source) {
    const bool conditional = begin(context, inst);
    auto& b = context.builder;
    const auto a = modify(b, registerPair(inst, 8), bit(inst, 7), bit(inst, 43));
    const auto other =
        modify(b, sourceB(inst, source), bit(inst, 44), bit(inst, 6));
    const auto flag = compare(context, inst, a, other);
    // The second destination holds the negated comparison under the same
    // boolean operation
    const auto result = combine(context, inst, flag);
    const auto negated = combine(context, inst, b.opNot(flag));
    b.opCopy(ir::Value::createPredicate(
                 static_cast<pred_t>(extractBits(inst, 3, 3))),
             result);
    b.opCopy(ir::Value::createPredicate(
                 static_cast<pred_t>(extractBits(inst, 0, 3))),
             negated);
    if (conditional)
        b.opEndIf();
}

void emitDset(DecoderContext& context, instruction_t inst, DoubleSource source) {
    warnConditionCodes(inst, "DSET");
    const bool conditional = begin(context, inst);
    auto& b = context.builder;
    const auto a = modify(b, registerPair(inst, 8), bit(inst, 54), bit(inst, 43));
    const auto other =
        modify(b, sourceB(inst, source), bit(inst, 44), bit(inst, 53));
    const auto result =
        combine(context, inst, compare(context, inst, a, other));
    const auto dst = static_cast<reg_t>(extractBits(inst, 0, 8));
    // Like FSET: 1.0f with .BF, otherwise an all ones mask
    const auto value =
        bit(inst, 52)
            ? b.opSelect(result, ir::Value::createConstantU(0x3f800000u),
                         ir::Value::createConstantU(0u))
            : b.opSelect(result, ir::Value::createConstantU(0xffffffffu),
                         ir::Value::createConstantU(0u));
    if (dst != RZ)
        b.opCopy(ir::Value::createRegister(dst, ir::ScalarType::U32), value);
    if (conditional)
        b.opEndIf();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
