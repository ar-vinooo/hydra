#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/msl/intrinsics.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen::lang::
    msl {

namespace {

// PRMT: result byte i comes from byte nibble[i] & 7 of {b, a}. The default
// mode takes the nibbles from the selector and replicates the sign of the
// byte when bit 3 is set; the other modes derive them from selector bits 0-1.
constexpr std::string_view PRMT_SOURCE = R"(
inline uint hydra_prmt(uint a, uint b, uint sel, uint mode) {
    const ulong bytes = ulong(a) | (ulong(b) << 32);
    const uint n = sel & 3u;
    uint nibbles;
    switch (mode) {
    case 1u: // Forward 4 extract
        nibbles = n | ((n + 1u) << 4) | ((n + 2u) << 8) | ((n + 3u) << 12);
        break;
    case 2u: // Backward 4 extract
        nibbles = n | (((n - 1u) & 7u) << 4) | (((n - 2u) & 7u) << 8) |
                  (((n - 3u) & 7u) << 12);
        break;
    case 3u: // Replicate 8
        nibbles = n * 0x1111u;
        break;
    case 4u: // Edge clamp left
        nibbles = n | (max(n, 1u) << 4) | (max(n, 2u) << 8) | (3u << 12);
        break;
    case 5u: // Edge clamp right
        nibbles = (min(n, 1u) << 4) | (min(n, 2u) << 8) | (n << 12);
        break;
    case 6u: { // Replicate 16
        const uint low = (n & 1u) * 2u;
        nibbles = low | ((low + 1u) << 4) | (low << 8) | ((low + 1u) << 12);
        break;
    }
    default:
        nibbles = sel & 0xffffu;
        break;
    }
    uint result = 0u;
    for (uint i = 0u; i < 4u; i++) {
        const uint nibble = (nibbles >> (i * 4u)) & 0xfu;
        uint byte = uint(bytes >> ((nibble & 7u) * 8u)) & 0xffu;
        if (mode == 0u && (nibble & 8u) != 0u)
            byte = (byte & 0x80u) != 0u ? 0xffu : 0u;
        result |= byte << (i * 8u);
    }
    return result;
}
)";

// Bits 0 and 1 of signs select signed operands
constexpr std::string_view IMUL_HI_SOURCE = R"(
inline uint hydra_imul_hi(uint a, uint b, uint signs) {
    const ulong wide_a = (signs & 1u) != 0u ? ulong(long(int(a))) : ulong(a);
    const ulong wide_b = (signs & 2u) != 0u ? ulong(long(int(b))) : ulong(b);
    return uint((wide_a * wide_b) >> 32);
}
)";

// F2I. Control: bits 0-1 rounding (even, floor, ceil, trunc), 2 signed,
// 3 16-bit result, 4 F16 source, 5 its upper half, 6 abs, 7 neg, 8 FTZ.
// Results saturate to the destination range, NaN converts to 0.
constexpr std::string_view FLOAT_TO_INT_SOURCE = R"(
inline uint hydra_f2i(uint src, uint control) {
    float x = (control & 0x10u) != 0u
                  ? float(as_type<half2>(src)[(control >> 5) & 1u])
                  : as_type<float>(src);
    if ((control & 0x100u) != 0u && fabs(x) < 1.17549435e-38f)
        x = copysign(0.0f, x);
    if ((control & 0x40u) != 0u)
        x = fabs(x);
    if ((control & 0x80u) != 0u)
        x = -x;
    switch (control & 3u) {
    case 0u: x = rint(x); break;
    case 1u: x = floor(x); break;
    case 2u: x = ceil(x); break;
    default: x = trunc(x); break;
    }
    if (isnan(x))
        return 0u;
    const bool is_signed = (control & 4u) != 0u;
    if ((control & 8u) != 0u)
        return is_signed ? as_type<uint>(int(clamp(x, -32768.0f, 32767.0f)))
                         : uint(clamp(x, 0.0f, 65535.0f));
    if (is_signed) {
        if (x >= 2147483648.0f)
            return 0x7fffffffu;
        if (x <= -2147483648.0f)
            return 0x80000000u;
        return as_type<uint>(int(x));
    }
    if (x >= 4294967296.0f)
        return 0xffffffffu;
    return x <= 0.0f ? 0u : uint(x);
}
)";

// F2I to a 64-bit integer, control as hydra_f2i with bit 9 selecting the high
// word. NaN converts to 0x8000000000000000.
constexpr std::string_view FLOAT_TO_INT64_SOURCE = R"(
inline uint hydra_f2i64(uint src, uint control) {
    float x = (control & 0x10u) != 0u
                  ? float(as_type<half2>(src)[(control >> 5) & 1u])
                  : as_type<float>(src);
    if ((control & 0x100u) != 0u && fabs(x) < 1.17549435e-38f)
        x = copysign(0.0f, x);
    if ((control & 0x40u) != 0u)
        x = fabs(x);
    if ((control & 0x80u) != 0u)
        x = -x;
    switch (control & 3u) {
    case 0u: x = rint(x); break;
    case 1u: x = floor(x); break;
    case 2u: x = ceil(x); break;
    default: x = trunc(x); break;
    }
    ulong result;
    if (isnan(x))
        result = 0x8000000000000000ul;
    else if ((control & 4u) != 0u)
        result = x >= 9223372036854775808.0f    ? 0x7ffffffffffffffful
                 : x <= -9223372036854775808.0f ? 0x8000000000000000ul
                                                : as_type<ulong>(long(x));
    else
        result = x >= 18446744073709551616.0f ? 0xfffffffffffffffful
                 : x <= 0.0f                  ? 0ul
                                              : ulong(x);
    return (control & 0x200u) != 0u ? uint(result >> 32) : uint(result);
}
)";

// F2F between F16 and F32. Control: bits 0-1 source and 2-3 destination
// format (1 F16, 2 F32), 4 upper half of an F16 source, 5-7 integer rounding
// (1 even, 2 floor, 3 ceil, 4 trunc), 8 abs, 9 neg, 10 saturate, 11 FTZ.
// F16 results are in the low half, the high half is 0.
constexpr std::string_view FLOAT_TO_FLOAT_SOURCE = R"(
inline uint hydra_f2f(uint src, uint control) {
    float x = (control & 3u) == 1u
                  ? float(as_type<half2>(src)[(control >> 4) & 1u])
                  : as_type<float>(src);
    if ((control & 0x800u) != 0u && fabs(x) < 1.17549435e-38f)
        x = copysign(0.0f, x);
    if ((control & 0x100u) != 0u)
        x = fabs(x);
    if ((control & 0x200u) != 0u)
        x = -x;
    switch ((control >> 5) & 7u) {
    case 1u: x = rint(x); break;
    case 2u: x = floor(x); break;
    case 3u: x = ceil(x); break;
    case 4u: x = trunc(x); break;
    default: break;
    }
    if ((control & 0x400u) != 0u)
        x = clamp(x, 0.0f, 1.0f);
    if (((control >> 2) & 3u) == 1u)
        return uint(as_type<ushort>(half(x)));
    return as_type<uint>(x);
}
)";

// Double (lo, hi words) to F32 or F16, rounding to nearest even. Control:
// bit 0 abs, 1 neg, 2 saturate, 3-4 destination format (1 F16, 2 F32).
constexpr std::string_view F64_TO_F32_SOURCE = R"(
inline uint hydra_f64_to_f32(uint lo, uint hi, uint control) {
    if ((control & 1u) != 0u)
        hi &= 0x7fffffffu;
    if ((control & 2u) != 0u)
        hi ^= 0x80000000u;
    const uint sign = hi & 0x80000000u;
    const int exponent = int((hi >> 20) & 0x7ffu);
    const ulong mantissa = (ulong(hi & 0xfffffu) << 32) | ulong(lo);
    uint bits;
    if (exponent == 0x7ff) {
        bits = sign | 0x7f800000u | (mantissa != 0ul ? 0x400000u : 0u);
    } else if (exponent == 0) {
        bits = sign;
    } else {
        const int e = exponent - 1023 + 127;
        // Subnormal results lose the bits below the smallest exponent
        const uint shift = e > 0 ? 29u : 29u + uint(min(1 - e, 64));
        if (e >= 255) {
            bits = sign | 0x7f800000u;
        } else if (shift >= 64u) {
            bits = sign;
        } else {
            const ulong m = mantissa | (1ul << 52);
            uint result = uint(m >> shift);
            const ulong remainder = m & ((1ul << shift) - 1ul);
            const ulong halfway = 1ul << (shift - 1u);
            if (remainder > halfway ||
                (remainder == halfway && (result & 1u) != 0u))
                result++;
            // The implicit bit of a normal result (or a rounding carry out of
            // it) lands in the exponent
            if (e > 0)
                result += (uint(e) << 23) - 0x800000u;
            bits = sign | min(result, 0x7f800000u);
        }
    }
    float x = as_type<float>(bits);
    if ((control & 4u) != 0u)
        x = clamp(x, 0.0f, 1.0f);
    if (((control >> 3) & 3u) == 1u)
        return uint(as_type<ushort>(half(x)));
    return as_type<uint>(x);
}
)";

// F32 or F16 to a double (exact). Control: bits 0-1 source format (1 F16,
// 2 F32), 2 upper half of an F16 source, 3 abs, 4 neg, 5 high word.
constexpr std::string_view F32_TO_F64_SOURCE = R"(
inline uint hydra_f32_to_f64(uint src, uint control) {
    float x = (control & 3u) == 1u
                  ? float(as_type<half2>(src)[(control >> 2) & 1u])
                  : as_type<float>(src);
    uint bits = as_type<uint>(x);
    if ((control & 8u) != 0u)
        bits &= 0x7fffffffu;
    if ((control & 0x10u) != 0u)
        bits ^= 0x80000000u;
    const uint sign = bits & 0x80000000u;
    const uint exponent = (bits >> 23) & 0xffu;
    uint mantissa = bits & 0x7fffffu;
    uint hi;
    uint lo;
    if (exponent == 0xffu) {
        hi = sign | 0x7ff00000u | (mantissa >> 3) | (mantissa != 0u ? 0x80000u : 0u);
        lo = mantissa << 29;
    } else if (exponent == 0u && mantissa == 0u) {
        hi = sign;
        lo = 0u;
    } else {
        int e = int(exponent) - 127 + 1023;
        if (exponent == 0u) {
            // Normalize a subnormal input
            const uint leading = 31u - clz(mantissa);
            e = int(leading) - 149 + 1023;
            mantissa = (mantissa << (23u - leading)) & 0x7fffffu;
        }
        hi = sign | (uint(e) << 20) | (mantissa >> 3);
        lo = mantissa << 29;
    }
    return (control & 0x20u) != 0u ? hi : lo;
}
)";

// I2I. Control: bits 0-1 source and 3-4 destination width (0 8-bit,
// 1 16-bit, 2 32-bit), 2 signed source, 5 signed destination, 6-8 source
// byte, 9 saturate, 10 abs, 11 neg. Without saturation the result is
// truncated (and zero extended) to the destination width.
constexpr std::string_view INT_TO_INT_SOURCE = R"(
inline uint hydra_i2i(uint src, uint control) {
    const uint src_bits = 8u << min(control & 3u, 2u);
    const uint offset = min(((control >> 6) & 7u) * 8u, 32u - src_bits);
    const bool src_signed = (control & 4u) != 0u;
    const bool dst_signed = (control & 0x20u) != 0u;
    uint value = src_signed
                     ? as_type<uint>(extract_bits(as_type<int>(src), offset, src_bits))
                     : extract_bits(src, offset, src_bits);
    if ((control & 0x400u) != 0u)
        value = as_type<uint>(abs(as_type<int>(value)));
    if ((control & 0x800u) != 0u)
        value = 0u - value;
    const uint dst_bits = 8u << min((control >> 3) & 3u, 2u);
    if ((control & 0x200u) == 0u)
        return dst_bits == 32u ? value : value & ((1u << dst_bits) - 1u);
    if (dst_signed && src_signed) {
        const int high = int((1u << (dst_bits - 1u)) - 1u);
        return as_type<uint>(clamp(as_type<int>(value), -high - 1, high));
    }
    if (src_signed && as_type<int>(value) < 0)
        return 0u;
    const uint high = dst_signed ? (1u << (dst_bits - 1u)) - 1u
                      : dst_bits == 32u ? 0xffffffffu
                                        : (1u << dst_bits) - 1u;
    return min(value, high);
}
)";

// I2F. Control: bits 0-1 source width (0 8-bit, 1 16-bit, 2 32-bit),
// 2 signed, 3-4 destination format (1 F16, 2 F32), 5-6 source byte, 7 abs,
// 8 neg. The most negative value keeps its sign under abs and neg.
constexpr std::string_view INT_TO_FLOAT_SOURCE = R"(
inline uint hydra_i2f(uint src, uint control) {
    const uint bits = 8u << min(control & 3u, 2u);
    const uint offset = min(((control >> 5) & 3u) * 8u, 32u - bits);
    const bool is_signed = (control & 4u) != 0u;
    uint value = is_signed
                     ? as_type<uint>(extract_bits(as_type<int>(src), offset, bits))
                     : extract_bits(src, offset, bits);
    const bool is_least = is_signed && value == 0u - (1u << (bits - 1u));
    const bool absolute = (control & 0x80u) != 0u;
    if (absolute && is_signed && !is_least)
        value = as_type<uint>(abs(as_type<int>(value)));
    float x = is_signed ? float(as_type<int>(value)) : float(value);
    if ((control & 0x100u) != 0u && (absolute || !is_least))
        x = -x;
    if (((control >> 3) & 3u) == 1u)
        return uint(as_type<ushort>(half(x)));
    return as_type<uint>(x);
}
)";

// I2F of a 64-bit integer (lo, hi words), control as hydra_i2f
constexpr std::string_view INT64_TO_FLOAT_SOURCE = R"(
inline uint hydra_i2f64(uint lo, uint hi, uint control) {
    const ulong value = ulong(lo) | (ulong(hi) << 32);
    const bool is_signed = (control & 4u) != 0u;
    const bool is_least = is_signed && value == 0x8000000000000000ul;
    const bool absolute = (control & 0x80u) != 0u;
    long signed_value = as_type<long>(value);
    if (absolute && is_signed && !is_least && signed_value < 0)
        signed_value = -signed_value;
    float x = is_signed ? float(signed_value) : float(value);
    if ((control & 0x100u) != 0u && (absolute || !is_least))
        x = -x;
    if (((control >> 3) & 3u) == 1u)
        return uint(as_type<ushort>(half(x)));
    return as_type<uint>(x);
}
)";

// MUFU.RCP64H: the high word of an approximate reciprocal of the double whose
// high word is given (the low word is taken as 0)
constexpr std::string_view RCP64H_SOURCE = R"(
inline uint hydra_rcp64h(uint hi) {
    const uint sign = hi & 0x80000000u;
    const uint exponent = (hi >> 20) & 0x7ffu;
    const uint mantissa = hi & 0xfffffu;
    if (exponent == 0x7ffu)
        return mantissa != 0u ? hi | 0x80000u : sign;
    if (exponent == 0u)
        return sign | 0x7ff00000u;
    // 1 / (m * 2^e) = (2 / m) * 2^(-e - 1), with 2 / m in (1, 2]
    float r = 2.0f / (1.0f + float(mantissa) / 1048576.0f);
    int result_exponent = 2045 - int(exponent);
    uint result_mantissa = uint(rint((r - 1.0f) * 1048576.0f));
    if (result_mantissa >= 0x100000u) {
        result_mantissa = 0u;
        result_exponent++;
    }
    if (result_exponent <= 0)
        return sign;
    return sign | (uint(result_exponent) << 20) | result_mantissa;
}
)";

// MUFU.RSQ64H: the high word of an approximate reciprocal square root of the
// double whose high word is given (the low word is taken as 0)
constexpr std::string_view RSQ64H_SOURCE = R"(
inline uint hydra_rsq64h(uint hi) {
    const uint sign = hi & 0x80000000u;
    const uint exponent = (hi >> 20) & 0x7ffu;
    const uint mantissa = hi & 0xfffffu;
    if (exponent == 0x7ffu && mantissa != 0u)
        return hi | 0x80000u;
    if (exponent == 0u)
        return sign | 0x7ff00000u;
    if (sign != 0u)
        return 0xfff80000u;
    if (exponent == 0x7ffu)
        return 0u;
    // Make the exponent even, then 1 / sqrt(m * 2^e) = (2 / sqrt(m)) *
    // 2^(-e / 2 - 1), with 2 / sqrt(m) in (1, 2]
    int e = int(exponent) - 1023;
    float m = 1.0f + float(mantissa) / 1048576.0f;
    if ((e & 1) != 0) {
        m *= 2.0f;
        e -= 1;
    }
    const float r = 2.0f * rsqrt(m);
    int result_exponent = -e / 2 - 1 + 1023;
    uint result_mantissa = uint(rint((r - 1.0f) * 1048576.0f));
    if (result_mantissa >= 0x100000u) {
        result_mantissa = 0u;
        result_exponent++;
    }
    return (uint(result_exponent) << 20) | result_mantissa;
}
)";

constexpr std::string_view BIT_REVERSE_SOURCE = R"(
inline uint hydra_bit_reverse(uint a) { return reverse_bits(a); }
)";

} // namespace

std::string_view getIntrinsicSource(ir::Intrinsic intrinsic) {
    switch (intrinsic) {
    case ir::Intrinsic::Prmt:
        return PRMT_SOURCE;
    case ir::Intrinsic::IMulHi:
        return IMUL_HI_SOURCE;
    case ir::Intrinsic::FloatToInt:
        return FLOAT_TO_INT_SOURCE;
    case ir::Intrinsic::FloatToInt64:
        return FLOAT_TO_INT64_SOURCE;
    case ir::Intrinsic::FloatToFloat:
        return FLOAT_TO_FLOAT_SOURCE;
    case ir::Intrinsic::F64ToF32:
        return F64_TO_F32_SOURCE;
    case ir::Intrinsic::F32ToF64:
        return F32_TO_F64_SOURCE;
    case ir::Intrinsic::IntToInt:
        return INT_TO_INT_SOURCE;
    case ir::Intrinsic::IntToFloat:
        return INT_TO_FLOAT_SOURCE;
    case ir::Intrinsic::Int64ToFloat:
        return INT64_TO_FLOAT_SOURCE;
    case ir::Intrinsic::Rcp64H:
        return RCP64H_SOURCE;
    case ir::Intrinsic::Rsq64H:
        return RSQ64H_SOURCE;
    case ir::Intrinsic::BitReverse:
        return BIT_REVERSE_SOURCE;
    }
    unreachable();
}

std::string_view getIntrinsicFunctionName(ir::Intrinsic intrinsic) {
    switch (intrinsic) {
    case ir::Intrinsic::Prmt:
        return "hydra_prmt";
    case ir::Intrinsic::IMulHi:
        return "hydra_imul_hi";
    case ir::Intrinsic::FloatToInt:
        return "hydra_f2i";
    case ir::Intrinsic::FloatToInt64:
        return "hydra_f2i64";
    case ir::Intrinsic::FloatToFloat:
        return "hydra_f2f";
    case ir::Intrinsic::F64ToF32:
        return "hydra_f64_to_f32";
    case ir::Intrinsic::F32ToF64:
        return "hydra_f32_to_f64";
    case ir::Intrinsic::IntToInt:
        return "hydra_i2i";
    case ir::Intrinsic::IntToFloat:
        return "hydra_i2f";
    case ir::Intrinsic::Int64ToFloat:
        return "hydra_i2f64";
    case ir::Intrinsic::Rcp64H:
        return "hydra_rcp64h";
    case ir::Intrinsic::Rsq64H:
        return "hydra_rsq64h";
    case ir::Intrinsic::BitReverse:
        return "hydra_bit_reverse";
    }
    unreachable();
}

} // namespace
  // hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen::lang::msl
