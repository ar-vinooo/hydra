#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/conversion.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

// The conversions are backend helpers taking a control word, the bits are
// documented with the helpers

// Words of a source operand: 64-bit operands use the next register or
// constant buffer word
struct Source {
    ir::Value lo;
    ir::Value hi;
};

Source getRegisterSource(reg_t reg) {
    return {ir::Value::createRegister(reg, ir::ScalarType::U32),
            ir::Value::createRegister(offsetRegister(reg, 1),
                                      ir::ScalarType::U32)};
}

Source getConstSource(u32 slot, u32 offset) {
    return {ir::Value::createConstMemory(
                CMem(slot, RZ, static_cast<u64>(offset) * 4)),
            ir::Value::createConstMemory(
                CMem(slot, RZ, static_cast<u64>(offset) * 4 + 4))};
}

// Float immediates are the top bits of the value. An F16 immediate is the
// low half of the field, in both halves.
Source getFloatImmSource(u32 imm20_0, u32 imm20_19, FloatFormat format) {
    const u32 field = imm20_0 | (imm20_19 << 19);
    switch (format) {
    case FloatFormat::F16: {
        const u32 half = imm20_0 & 0xffff;
        return {ir::Value::createConstantU(half | (half << 16)),
                ir::Value::createConstantU(0)};
    }
    case FloatFormat::F64:
        return {ir::Value::createConstantU(0),
                ir::Value::createConstantU(field << 12)};
    default:
        return {ir::Value::createConstantU(field << 12),
                ir::Value::createConstantU(0)};
    }
}

// Integer immediates are sign extended, to 64 bits as well
Source getIntImmSource(u32 imm20_0, u32 imm20_19) {
    const u32 value = getIntImm20(imm20_0, imm20_19, true);
    return {ir::Value::createConstantU(value),
            ir::Value::createConstantU((value & 0x80000000u) != 0 ? 0xffffffffu
                                                                  : 0u)};
}

ir::Value callIntrinsic(ir::Builder& builder, ir::Intrinsic intrinsic,
                        const std::vector<ir::Value>& args) {
    return builder.opIntrinsic(intrinsic, ir::ScalarType::U32, args);
}

void writeWord(ir::Builder& builder, reg_t dst, const ir::Value& value) {
    builder.opCopy(ir::Value::createRegister(dst, ir::ScalarType::U32), value);
}

void writePair(ir::Builder& builder, reg_t dst, const ir::Value& lo,
               const ir::Value& hi) {
    writeWord(builder, dst, lo);
    if (dst != RZ)
        writeWord(builder, offsetRegister(dst, 1), hi);
}

// Format code of the F16/F32 helpers (1 F16, 2 F32)
u32 getFloatCode(FloatFormat format) {
    return format == FloatFormat::F16 ? 1u : 2u;
}

// TODO: write_cc
void emitFloatToFloat(DecoderContext& context, InstF2fBase inst,
                      const Source& src) {
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, inst.pred, inst.pred_inv);
    if (inst.write_cc)
        ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler, "F2F CC"));

    const auto src_fmt = inst.src_fmt.get();
    const auto dst_fmt = inst.dst_fmt.get();
    u32 rounding = 0;
    switch (inst.getRoundMode()) {
    case RoundMode2::Round:
        rounding = 1;
        break;
    case RoundMode2::Floor:
        rounding = 2;
        break;
    case RoundMode2::Ceil:
        rounding = 3;
        break;
    case RoundMode2::Trunc:
        rounding = 4;
        break;
    default:
        break;
    }
    // Integer rounding applies to conversions between equal sizes
    if (src_fmt != dst_fmt)
        rounding = 0;

    if (src_fmt == FloatFormat::F64 && dst_fmt == FloatFormat::F64) {
        const auto word = [&](bool high) {
            return callIntrinsic(
                b, ir::Intrinsic::F64Round,
                {src.lo, src.hi,
                 ir::Value::createConstantU(rounding | (inst.abs ? 8u : 0u) |
                                            (inst.neg ? 0x10u : 0u) |
                                            (inst.sat ? 0x40u : 0u) |
                                            (high ? 0x20u : 0u))});
        };
        const auto lo = word(false);
        const auto hi = word(true);
        writePair(b, inst.dst, lo, hi);
    } else if (src_fmt == FloatFormat::F64) {
        const u32 control = (inst.abs ? 1u : 0u) | (inst.neg ? 2u : 0u) |
                            (inst.sat ? 4u : 0u) | (getFloatCode(dst_fmt) << 3) |
                            (inst.round_mode_0.get() << 5) | (inst.ftz ? 0x80u : 0u);
        writeWord(b, inst.dst,
                  callIntrinsic(b, ir::Intrinsic::F64ToF32,
                                {src.lo, src.hi,
                                 ir::Value::createConstantU(control)}));
    } else if (dst_fmt == FloatFormat::F64) {
        const u32 control = getFloatCode(src_fmt) | (inst.sh ? 4u : 0u) |
                            (inst.abs ? 8u : 0u) | (inst.neg ? 0x10u : 0u) |
                            (inst.ftz ? 0x40u : 0u) | (inst.sat ? 0x80u : 0u);
        const auto convert = [&](bool high) {
            return callIntrinsic(
                b, ir::Intrinsic::F32ToF64,
                {src.lo,
                 ir::Value::createConstantU(control | (high ? 0x20u : 0u))});
        };
        const auto lo = convert(false);
        const auto hi = convert(true);
        writePair(b, inst.dst, lo, hi);
    } else {
        const u32 control = getFloatCode(src_fmt) |
                            (getFloatCode(dst_fmt) << 2) |
                            (inst.sh ? 0x10u : 0u) | (rounding << 5) |
                            (inst.abs ? 0x100u : 0u) |
                            (inst.neg ? 0x200u : 0u) |
                            (inst.sat ? 0x400u : 0u) | (inst.ftz ? 0x800u : 0u) |
                            (inst.round_mode_0.get() << 12);
        writeWord(b, inst.dst,
                  callIntrinsic(b, ir::Intrinsic::FloatToFloat,
                                {src.lo, ir::Value::createConstantU(control)}));
    }

    if (conditional)
        b.opEndIf();
}

// TODO: write_cc
void emitFloatToInt(DecoderContext& context, InstF2iBase inst,
                    const Source& src) {
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, inst.pred, inst.pred_inv);
    if (inst.write_cc)
        ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler, "F2I CC"));

    auto value = src.lo;
    bool is_f16 = inst.src_fmt == FloatFormat::F16;
    const bool is_f64 = inst.src_fmt == FloatFormat::F64;

    auto width = inst.dst_width.get();
    if (width == IntegerWidth::Invalid) {
        ONCE(LOG_WARN(ShaderDecompiler, "Invalid F2I destination format"));
        width = IntegerWidth::_32;
    }
    const u32 control =
        static_cast<u32>(inst.round_mode.get()) |
        (inst.dst_signed ? 4u : 0u) |
        (width == IntegerWidth::_16 ? 8u : 0u) | (is_f16 ? 0x10u : 0u) |
        (inst.sh ? 0x20u : 0u) | (inst.abs ? 0x40u : 0u) |
        (inst.neg ? 0x80u : 0u) | (inst.ftz ? 0x100u : 0u);
    if (is_f64) {
        // Exact conversion of the double, results as in the 32-bit helper
        const u32 f64_control =
            static_cast<u32>(inst.round_mode.get()) |
            (inst.dst_signed ? 4u : 0u) |
            (width == IntegerWidth::_16 ? 8u : 0u) |
            (inst.abs ? 0x40u : 0u) | (inst.neg ? 0x80u : 0u) |
            (width == IntegerWidth::_64 ? 0x400u : 0u);
        const auto word = [&](bool high) {
            return callIntrinsic(
                b, ir::Intrinsic::F64ToInt,
                {src.lo, src.hi,
                 ir::Value::createConstantU(f64_control | (high ? 0x200u : 0u))});
        };
        if (width == IntegerWidth::_64)
            writePair(b, inst.dst, word(false), word(true));
        else
            writeWord(b, inst.dst, word(false));
    } else if (width == IntegerWidth::_64) {
        const auto lo = callIntrinsic(b, ir::Intrinsic::FloatToInt64,
                                      {value, ir::Value::createConstantU(control)});
        const auto hi = callIntrinsic(
            b, ir::Intrinsic::FloatToInt64,
            {value, ir::Value::createConstantU(control | 0x200u)});
        writePair(b, inst.dst, lo, hi);
    } else {
        writeWord(b, inst.dst,
                  callIntrinsic(b, ir::Intrinsic::FloatToInt,
                                {value, ir::Value::createConstantU(control)}));
    }

    if (conditional)
        b.opEndIf();
}

void emitIntToInt(DecoderContext& context, InstI2iBase inst,
                  const Source& src) {
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, inst.pred, inst.pred_inv);

    const auto to_width = [](SmallIntegerWidth width) {
        if (width == SmallIntegerWidth::_64) {
            ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler, "64-bit I2I"));
            return 2u;
        }
        return static_cast<u32>(width);
    };
    const u32 control = to_width(inst.src_width) |
                        (inst.src_signed ? 4u : 0u) |
                        (to_width(inst.dst_width) << 3) |
                        (inst.dst_signed ? 0x20u : 0u) |
                        (inst.byte_sel.get() << 6) | (inst.sat ? 0x200u : 0u) |
                        (inst.abs ? 0x400u : 0u) | (inst.neg ? 0x800u : 0u);
    const auto res = callIntrinsic(b, ir::Intrinsic::IntToInt,
                                   {src.lo, ir::Value::createConstantU(control)});
    writeWord(b, inst.dst, res);

    if (inst.write_cc) {
        // Zero and sign of the result, carry and overflow are cleared
        const auto zero = ir::Value::createConstantU(0);
        b.opCopy(ir::Value::createConditionFlag(0), b.opCompareEqual(res, zero));
        b.opCopy(ir::Value::createConditionFlag(1),
                 b.opCompareNotEqual(
                     b.opBitwiseAnd(res, ir::Value::createConstantU(0x80000000u)),
                     zero));
        b.opCopy(ir::Value::createConditionFlag(2),
                 ir::Value::createConstantB(false));
        b.opCopy(ir::Value::createConditionFlag(3),
                 ir::Value::createConstantB(false));
    }

    if (conditional)
        b.opEndIf();
}

// TODO: write_cc
void emitIntToFloat(DecoderContext& context, InstI2fBase inst,
                    const Source& src) {
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, inst.pred, inst.pred_inv);
    if (inst.write_cc)
        ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler, "I2F CC"));

    const auto dst_fmt = inst.dst_fmt.get();
    const bool is_64 = inst.src_width == SmallIntegerWidth::_64;
    const u32 control = static_cast<u32>(inst.src_width.get()) |
                        (inst.src_signed ? 4u : 0u) |
                        (getFloatCode(dst_fmt == FloatFormat::F64
                                          ? FloatFormat::F32
                                          : dst_fmt)
                         << 3) |
                        (is_64 ? 0u : inst.byte_sel.get() << 5) |
                        (inst.abs ? 0x80u : 0u) | (inst.neg ? 0x100u : 0u) |
                        (static_cast<u32>(inst.round_mode.get()) << 10);
    if (dst_fmt == FloatFormat::F64) {
        const auto word = [&](bool high) {
            return callIntrinsic(
                b, ir::Intrinsic::F64FromInt,
                {src.lo, src.hi,
                 ir::Value::createConstantU(control | (high ? 0x200u : 0u))});
        };
        writePair(b, inst.dst, word(false), word(true));
    } else {
        const auto res =
            is_64 ? callIntrinsic(
                        b, ir::Intrinsic::Int64ToFloat,
                        {src.lo, src.hi, ir::Value::createConstantU(control)})
                  : callIntrinsic(b, ir::Intrinsic::IntToFloat,
                                  {src.lo, ir::Value::createConstantU(control)});
        writeWord(b, inst.dst, res);
    }

    if (conditional)
        b.opEndIf();
}

} // namespace

void emitF2fR(DecoderContext& context, InstF2fR inst) {
    emitFloatToFloat(context, inst.base, getRegisterSource(inst.src));
}

void emitF2fC(DecoderContext& context, InstF2fC inst) {
    emitFloatToFloat(context, inst.base,
                     getConstSource(inst.cbuf_slot, inst.cbuf_offset));
}

void emitF2fI(DecoderContext& context, InstF2fI inst) {
    emitFloatToFloat(context, inst.base,
                     getFloatImmSource(inst.imm20_0, inst.imm20_19,
                                       inst.base.src_fmt));
}

void emitF2iR(DecoderContext& context, InstF2iR inst) {
    emitFloatToInt(context, inst.base, getRegisterSource(inst.src));
}

void emitF2iC(DecoderContext& context, InstF2iC inst) {
    emitFloatToInt(context, inst.base,
                   getConstSource(inst.cbuf_slot, inst.cbuf_offset));
}

void emitF2iI(DecoderContext& context, InstF2iI inst) {
    emitFloatToInt(context, inst.base,
                   getFloatImmSource(inst.imm20_0, inst.imm20_19,
                                     inst.base.src_fmt));
}

void emitI2iR(DecoderContext& context, InstI2iR inst) {
    emitIntToInt(context, inst.base, getRegisterSource(inst.src));
}

void emitI2iC(DecoderContext& context, InstI2iC inst) {
    emitIntToInt(context, inst.base,
                 getConstSource(inst.cbuf_slot, inst.cbuf_offset));
}

void emitI2iI(DecoderContext& context, InstI2iI inst) {
    emitIntToInt(context, inst.base,
                 getIntImmSource(inst.imm20_0, inst.imm20_19));
}

void emitI2fR(DecoderContext& context, InstI2fR inst) {
    emitIntToFloat(context, inst.base, getRegisterSource(inst.src));
}

void emitI2fC(DecoderContext& context, InstI2fC inst) {
    emitIntToFloat(context, inst.base,
                   getConstSource(inst.cbuf_slot, inst.cbuf_offset));
}

void emitI2fI(DecoderContext& context, InstI2fI inst) {
    emitIntToFloat(context, inst.base,
                   getIntImmSource(inst.imm20_0, inst.imm20_19));
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
