#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/ir/builder.hpp"

// TODO: ignore on release
#define COMMENT_IMPL(log_level, f_comment, f_log, ...)                         \
    {                                                                          \
        /* TODO: comments */                                                   \
        /*BUILDER.OpDebugComment(fmt::format(f_comment                         \
         * ZTD_PASS_VA_ARGS(__VA_ARGS__)));*/                                  \
        LOG_##log_level(ShaderDecompiler,                                      \
                        f_log ZTD_PASS_VA_ARGS(__VA_ARGS__));                  \
    }
#define COMMENT(f, ...) COMMENT_IMPL(DEBUG, f, f, __VA_ARGS__)
#define COMMENT_NOT_IMPLEMENTED(f, ...)                                        \
    COMMENT_IMPL(NOT_IMPLEMENTED, f " (NOT IMPLEMENTED)", f, __VA_ARGS__)

#define DEFINE_INST_VARIANTS_IMPL(inst_name, reg_name)                         \
    union Inst##inst_name##R {                                                 \
        Inst##inst_name##Base base;                                            \
        BitField64<reg_t, 20, 8> reg_name;                                     \
    };                                                                         \
    void emit##inst_name##R(DecoderContext& context, Inst##inst_name##R inst); \
    union Inst##inst_name##C {                                                 \
        Inst##inst_name##Base base;                                            \
        BitField64<u32, 20, 14> cbuf_offset;                                   \
        BitField64<u32, 34, 5> cbuf_slot;                                      \
    };                                                                         \
    void emit##inst_name##C(DecoderContext& context, Inst##inst_name##C inst); \
    union Inst##inst_name##I {                                                 \
        Inst##inst_name##Base base;                                            \
        BitField64<u32, 20, 19> imm20_0;                                       \
        BitField64<u32, 56, 1> imm20_19;                                       \
    };                                                                         \
    void emit##inst_name##I(DecoderContext& context, Inst##inst_name##I inst);

#define DEFINE_INST_SRC1_VARIANTS(inst_name)                                   \
    DEFINE_INST_VARIANTS_IMPL(inst_name, src)
#define DEFINE_INST_SRC2_VARIANTS(inst_name)                                   \
    DEFINE_INST_VARIANTS_IMPL(inst_name, src_b)

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

struct DecoderContext {
    const DecompilerContext& decomp_context;
    ztd::io::IStream* code_stream;
    ir::Builder& builder;
    u64 code_offset{0};
};

enum class RoundMode {
    Rn = 0,
    Rm = 1,
    Rp = 2,
    Rz = 3,
};

enum class FloatCmpOp {
    F = 0,
    Lt = 1,
    Eq = 2,
    Le = 3,
    Gt = 4,
    Ne = 5,
    Ge = 6,
    Num = 7,
    Nan = 8,
    Ltu = 9,
    Equ = 10,
    Leu = 11,
    Gtu = 12,
    Neu = 13,
    Geu = 14,
    T = 15,
};

enum class HalfSwizzle {
    F16 = 0,
    F32 = 1,
    H0H0 = 2,
    H1H1 = 3,
};

inline u32 getIntImm20(u32 imm20_0, u32 imm20_19, bool extend) {
    const auto imm20 = imm20_0 | (imm20_19 << 19);
    if (extend)
        return signExtend<u32, 20>(imm20);
    else
        return imm20;
}

inline f32 getFloatImm20(u32 imm20_0, u32 imm20_19) {
    return std::bit_cast<f32>((imm20_0 | (imm20_19 << 19)) << 12);
}

inline ir::Value getHalf2Const(ir::Builder& builder, u32 h0, u32 h1) {
    return builder.opVectorConstruct(
        ir::ScalarType::F16,
        {ir::Value::createConstant(h0, ir::ScalarType::F16),
         ir::Value::createConstant(h1, ir::ScalarType::F16)});
}

inline ir::Value getHalf2Const20(ir::Builder& builder, u32 h0_imm10,
                                 u32 h1_imm10) {
    return getHalf2Const(builder, h0_imm10 << 6, h1_imm10 << 6);
}

inline ir::Value getHalf2Const32(ir::Builder& builder, u32 imm32) {
    return getHalf2Const(builder, extractBits(imm32, 0, 16),
                         extractBits(imm32, 16, 16));
}

inline ir::Value negIf(ir::Builder& builder, const ir::Value& value, bool neg) {
    return neg ? builder.opNeg(value) : value;
}

inline ir::Value absIf(ir::Builder& builder, const ir::Value& value, bool abs) {
    return abs ? builder.opAbs(value) : value;
}

inline ir::Value absNegIf(ir::Builder& builder, const ir::Value& value,
                          bool abs, bool neg) {
    return negIf(builder, absIf(builder, value, abs), neg);
}

inline ir::Value notIf(ir::Builder& builder, const ir::Value& value,
                       bool not_) {
    return not_ ? builder.opNot(value) : value;
}

// Maxwell flow tests combine Z, S, C and O independently of predication.
inline ir::Value getFlowTest(ir::Builder& b, u32 test) {
    const auto z = ir::Value::createConditionFlag(0);
    const auto s = ir::Value::createConditionFlag(1);
    const auto c = ir::Value::createConditionFlag(2);
    const auto o = ir::Value::createConditionFlag(3);
    switch (test) {
    case 0: return ir::Value::createConstantB(false);
    case 1: return b.opBitwiseXor(b.opBitwiseAnd(s, b.opNot(z)), o);
    case 2: return b.opBitwiseAnd(b.opNot(s), z);
    case 3: return b.opBitwiseXor(s, b.opBitwiseOr(z, o));
    case 4: return b.opBitwiseAnd(b.opBitwiseXor(b.opNot(s), o), b.opNot(z));
    case 5: return b.opNot(z);
    case 6: return b.opNot(b.opBitwiseXor(s, o));
    case 7: return b.opBitwiseOr(b.opNot(s), b.opNot(z));
    case 8: return b.opBitwiseAnd(s, z);
    case 9: return b.opBitwiseXor(s, o);
    case 10: return z;
    case 11: return b.opBitwiseOr(b.opBitwiseXor(s, o), z);
    case 12: return b.opBitwiseXor(b.opNot(s), b.opBitwiseOr(z, o));
    case 13: return b.opBitwiseOr(s, b.opNot(z));
    case 14: return b.opBitwiseXor(b.opBitwiseOr(b.opNot(s), z), o);
    case 15: return ir::Value::createConstantB(true);
    case 16: return b.opNot(o);
    case 17: return b.opNot(c);
    case 18: return b.opNot(s);
    case 19: return b.opBitwiseOr(z, b.opNot(c));
    case 20: return b.opBitwiseAnd(c, b.opNot(z));
    case 21: return s;
    case 22: return c;
    case 23: return o;
    case 30: return b.opBitwiseOr(s, z);
    case 31: return b.opBitwiseAnd(b.opNot(s), b.opNot(z));
    default:
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Flow test {}", test);
        return ir::Value::createConstantB(false);
    }
}

inline ir::Value getFlowCondition(ir::Builder& b, pred_t pred, bool inv,
                                   u32 test) {
    const auto guard = notIf(b, ir::Value::createPredicate(pred), inv);
    return test == 15 ? guard : b.opBitwiseAnd(guard, getFlowTest(b, test));
}

inline void writeSetConditionCodes(ir::Builder& b, ir::Value result,
                                    bool b_float) {
    b.opCopy(ir::Value::createConditionFlag(0), b.opNot(result));
    b.opCopy(ir::Value::createConditionFlag(1),
             b_float ? ir::Value::createConstantB(false) : result);
    b.opCopy(ir::Value::createConditionFlag(2), ir::Value::createConstantB(false));
    b.opCopy(ir::Value::createConditionFlag(3), ir::Value::createConstantB(false));
}

inline ir::Value saturateIf(ir::Builder& builder, const ir::Value& value,
                            bool sat) {
    return sat ? builder.opClamp(value, ir::Value::createConstantF(0.0f),
                                 ir::Value::createConstantF(1.0f))
               : value;
}

inline ir::Value saturate2xF16If(ir::Builder& builder, const ir::Value& value,
                                 bool sat) {
    return sat ? builder.opClamp(value, getHalf2Const(builder, 0x0, 0x0),
                                 getHalf2Const(builder, 0x3c00, 0x3c00))
               : value;
}

inline bool handlePredCond(ir::Builder& builder, pred_t pred, bool pred_inv) {
    if (pred == PT) {
        if (pred_inv) { // Never
            // The instruction never executes; keep it as dead code
            builder.opBeginIf({ir::Value::createConstantB(false)});
            return true;
        }

        return false; // Always
    } else { // Conditional
        builder.opBeginIf(
            {notIf(builder, ir::Value::createPredicate(pred), pred_inv)});
        return true;
    }
}

enum class EnsureIntegerSignednessError {
    NotAnInteger,
};

template <bool signed_>
ir::Value ensureIntegerSignedness(ir::Builder& builder,
                                  const ir::Value& value) {
    const auto type = value.getType();
    if (signed_) {
        if (type.isUnsignedInteger())
            return builder.opCast(value, type.signedEquivalent());
    } else {
        if (type.isSignedInteger())
            return builder.opCast(value, type.unsignedEquivalent());
    }

    return value;
}

inline ir::Value getSwizzledHalf(ir::Builder& builder, HalfSwizzle swizzle,
                                 reg_t src) {
    if (swizzle == HalfSwizzle::F16)
        return ir::Value::createRegister(
            src, ir::VectorType(ir::ScalarType::F16, 2));

    ir::Value src_v = ir::Value::createUndefined();
    switch (swizzle) {
    case HalfSwizzle::F32: {
        src_v =
            builder.opCast(ir::Value::createRegister(src, ir::ScalarType::F32),
                           ir::ScalarType::F16);
        break;
    }
    case HalfSwizzle::H0H0: {
        src_v = ir::Value::createRegister(src, ir::ScalarType::F16);
        break;
    }
    case HalfSwizzle::H1H1: {
        src_v = builder.opBitfieldExtract(ir::Value::createRegister(src),
                                          ir::Value::createConstantU(16),
                                          ir::Value::createConstantU(16));
        src_v = builder.opCast(src_v, ir::ScalarType::U16);
        src_v = builder.opBitCast(src_v, ir::ScalarType::F16);
        break;
    }
    default:
        unreachable();
    }

    return builder.opVectorConstruct(ir::ScalarType::F16, {src_v, src_v});
}

// Half instructions read a constant buffer operand as one F32, used for both
// halves
inline ir::Value getHalfConstMemory(ir::Builder& builder, u32 slot,
                                    u32 offset) {
    const auto value = builder.opCast(
        ir::Value::createConstMemory(CMem(slot, RZ, static_cast<u64>(offset) * 4),
                                     ir::ScalarType::F32),
        ir::ScalarType::F16);
    return builder.opVectorConstruct(ir::ScalarType::F16, {value, value});
}

inline ir::Value getFloatCmp(DecoderContext& context, FloatCmpOp op,
                             ir::Value a, ir::Value b) {
    if (op == FloatCmpOp::F)
        return ir::Value::createConstantB(false);
    if (op == FloatCmpOp::T)
        return ir::Value::createConstantB(true);
    const auto nan = context.builder.opBitwiseOr(context.builder.opIsNan(a),
                                                 context.builder.opIsNan(b));
    if (op == FloatCmpOp::Num)
        return context.builder.opNot(nan);
    if (op == FloatCmpOp::Nan)
        return nan;

    auto comparison = ir::Value::createConstantB(false);
    switch (op) {
    case FloatCmpOp::Lt:
    case FloatCmpOp::Ltu:
        comparison = context.builder.opCompareLess(a, b);
        break;
    case FloatCmpOp::Le:
    case FloatCmpOp::Leu:
        comparison = context.builder.opCompareLessOrEqual(a, b);
        break;
    case FloatCmpOp::Gt:
    case FloatCmpOp::Gtu:
        comparison = context.builder.opCompareGreater(a, b);
        break;
    case FloatCmpOp::Ge:
    case FloatCmpOp::Geu:
        comparison = context.builder.opCompareGreaterOrEqual(a, b);
        break;
    case FloatCmpOp::Eq:
    case FloatCmpOp::Equ:
        comparison = context.builder.opCompareEqual(a, b);
        break;
    case FloatCmpOp::Ne:
    case FloatCmpOp::Neu:
        comparison = context.builder.opCompareNotEqual(a, b);
        break;
    default:
        unreachable();
    }
    // Ordered comparisons are false for NaN; unordered comparisons are true.
    if (op >= FloatCmpOp::Ltu)
        return context.builder.opBitwiseOr(comparison, nan);
    return context.builder.opBitwiseAnd(comparison, context.builder.opNot(nan));
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
