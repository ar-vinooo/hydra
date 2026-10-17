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


// FP64 soft float. Metal has no doubles, so operations work on the bit
// patterns. All three helpers share this block (guarded against repeated
// inclusion). The fused multiply-add is exact: the product and the addend are
// aligned in 128 bits with a sticky bit before the single rounding.
constexpr std::string_view F64_SOURCE = R"(
#ifndef HYDRA_F64_HELPERS
#define HYDRA_F64_HELPERS
struct hydra_u128 { ulong hi; ulong lo; };

inline hydra_u128 hydra_u128_shl(hydra_u128 v, uint n) {
    if (n == 0u) return v;
    if (n >= 64u) return {v.lo << (n - 64u), 0ul};
    return {(v.hi << n) | (v.lo >> (64u - n)), v.lo << n};
}

// Shift right, folding the bits shifted out into bit 0
inline hydra_u128 hydra_u128_shr_jam(hydra_u128 v, uint n) {
    if (n == 0u) return v;
    if (n >= 128u) return {0ul, (v.hi | v.lo) != 0ul ? 1ul : 0ul};
    hydra_u128 out;
    bool lost;
    if (n < 64u) {
        out.hi = v.hi >> n;
        out.lo = (v.lo >> n) | (v.hi << (64u - n));
        lost = (v.lo << (64u - n)) != 0ul;
    } else if (n == 64u) {
        out.hi = 0ul;
        out.lo = v.hi;
        lost = v.lo != 0ul;
    } else {
        const uint s = n - 64u;
        out.hi = 0ul;
        out.lo = v.hi >> s;
        lost = v.lo != 0ul || (v.hi << (64u - s)) != 0ul;
    }
    if (lost) out.lo |= 1ul;
    return out;
}

inline hydra_u128 hydra_u128_add(hydra_u128 a, hydra_u128 b) {
    hydra_u128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo ? 1ul : 0ul);
    return r;
}

inline hydra_u128 hydra_u128_sub(hydra_u128 a, hydra_u128 b) {
    hydra_u128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo ? 1ul : 0ul);
    return r;
}

inline bool hydra_u128_ge(hydra_u128 a, hydra_u128 b) {
    return a.hi > b.hi || (a.hi == b.hi && a.lo >= b.lo);
}

inline uint hydra_u128_clz(hydra_u128 v) {
    return v.hi != 0ul ? uint(clz(v.hi)) : (v.lo != 0ul ? 64u + uint(clz(v.lo)) : 128u);
}

inline bool hydra_u128_bit(hydra_u128 v, uint n) {
    return n >= 64u ? ((v.hi >> (n - 64u)) & 1ul) != 0ul : ((v.lo >> n) & 1ul) != 0ul;
}

// Any of the n lowest bits set
inline bool hydra_u128_low_any(hydra_u128 v, uint n) {
    if (n == 0u) return false;
    if (n >= 128u) return (v.hi | v.lo) != 0ul;
    if (n >= 64u)
        return v.lo != 0ul || (n > 64u && (v.hi & ((1ul << (n - 64u)) - 1ul)) != 0ul);
    return (v.lo & ((1ul << n) - 1ul)) != 0ul;
}

constant ulong HYDRA_F64_SIGN = 0x8000000000000000ul;
constant ulong HYDRA_F64_FRAC = 0x000ffffffffffffful;
constant ulong HYDRA_F64_QNAN = 0x7ff8000000000000ul;
constant ulong HYDRA_F64_INF = 0x7ff0000000000000ul;

inline bool hydra_f64_is_nan(ulong x) { return (x & ~HYDRA_F64_SIGN) > HYDRA_F64_INF; }

// Unpacks a finite non-zero value into a 53 bit significand (bit 52 set) and
// the exponent of that bit
inline void hydra_f64_unpack(ulong x, thread int& e, thread ulong& m) {
    const uint ex = uint(x >> 52) & 0x7ffu;
    const ulong f = x & HYDRA_F64_FRAC;
    if (ex == 0u) {
        const uint shift = uint(clz(f)) - 11u;
        m = f << shift;
        e = -1022 - int(shift);
    } else {
        m = f | (1ul << 52);
        e = int(ex) - 1023;
    }
}

// Rounding: 0 nearest even, 1 toward -inf, 2 toward +inf, 3 toward zero
inline ulong hydra_f64_fma(ulong a, ulong b, ulong c, uint mode) {
    const uint sa = uint(a >> 63), sb = uint(b >> 63), sc = uint(c >> 63);
    const uint sp = sa ^ sb;
    if (hydra_f64_is_nan(a) || hydra_f64_is_nan(b) || hydra_f64_is_nan(c))
        return HYDRA_F64_QNAN;
    const bool a_inf = (a & ~HYDRA_F64_SIGN) == HYDRA_F64_INF;
    const bool b_inf = (b & ~HYDRA_F64_SIGN) == HYDRA_F64_INF;
    const bool c_inf = (c & ~HYDRA_F64_SIGN) == HYDRA_F64_INF;
    const bool a_zero = (a & ~HYDRA_F64_SIGN) == 0ul;
    const bool b_zero = (b & ~HYDRA_F64_SIGN) == 0ul;
    const bool c_zero = (c & ~HYDRA_F64_SIGN) == 0ul;
    if ((a_inf && b_zero) || (b_inf && a_zero)) return HYDRA_F64_QNAN;
    if (a_inf || b_inf) {
        if (c_inf && sc != sp) return HYDRA_F64_QNAN;
        return (ulong(sp) << 63) | HYDRA_F64_INF;
    }
    if (c_inf) return c;
    if (a_zero || b_zero) {
        if (!c_zero) return c;
        const uint s = sp == sc ? sp : (mode == 1u ? 1u : 0u);
        return ulong(s) << 63;
    }

    int ea, eb, ec = 0;
    ulong ma, mb, mc = 0ul;
    hydra_f64_unpack(a, ea, ma);
    hydra_f64_unpack(b, eb, mb);
    hydra_u128 p;
    p.lo = ma * mb;
    p.hi = mulhi(ma, mb);
    p = hydra_u128_shl(p, 21u);
    const int Ep = ea + eb - 125;

    int E = Ep;
    hydra_u128 X = p;
    hydra_u128 Y = {0ul, 0ul};
    uint sX = sp, sY = sp;
    if (!c_zero) {
        hydra_f64_unpack(c, ec, mc);
        const hydra_u128 cc = {mc << 10, 0ul};
        const int Ec = ec - 126;
        if (Ep >= Ec) {
            Y = hydra_u128_shr_jam(cc, uint(Ep - Ec));
            sY = sc;
        } else {
            E = Ec;
            X = cc;
            sX = sc;
            Y = hydra_u128_shr_jam(p, uint(Ec - Ep));
            sY = sp;
        }
    }

    hydra_u128 S;
    uint rs;
    if (sX == sY) {
        S = hydra_u128_add(X, Y);
        rs = sX;
    } else if (hydra_u128_ge(X, Y)) {
        S = hydra_u128_sub(X, Y);
        rs = sX;
    } else {
        S = hydra_u128_sub(Y, X);
        rs = sY;
    }
    if ((S.hi | S.lo) == 0ul)
        return mode == 1u ? HYDRA_F64_SIGN : 0ul;

    const int msb = 127 - int(hydra_u128_clz(S));
    const int e_res = E + msb;
    int q = max(e_res - 52, -1074);
    const int s = q - E;
    ulong r;
    bool guard = false, sticky = false;
    if (s <= 0) {
        r = S.lo << uint(-s);
    } else if (s >= 129) {
        r = 0ul;
        sticky = true;
    } else {
        if (s >= 128)
            r = 0ul;
        else if (s >= 64)
            r = S.hi >> uint(s - 64);
        else
            r = (S.lo >> uint(s)) | (S.hi << uint(64 - s));
        guard = hydra_u128_bit(S, uint(s - 1));
        sticky = hydra_u128_low_any(S, uint(s - 1));
    }
    const bool inexact = guard || sticky;
    bool up;
    if (mode == 0u) up = guard && (sticky || (r & 1ul) != 0ul);
    else if (mode == 1u) up = rs != 0u && inexact;
    else if (mode == 2u) up = rs == 0u && inexact;
    else up = false;
    if (up) r += 1ul;
    if (r == (1ul << 53)) {
        r = 1ul << 52;
        q += 1;
    }
    if (r < (1ul << 52))
        return (ulong(rs) << 63) | r;
    const int field = q + 52 + 1023;
    if (field >= 0x7ff) {
        const bool to_inf = mode == 0u || (mode == 1u && rs != 0u) || (mode == 2u && rs == 0u);
        return (ulong(rs) << 63) | (to_inf ? HYDRA_F64_INF : 0x7fefffffffffffffull);
    }
    return (ulong(rs) << 63) | (ulong(field) << 52) | (r & HYDRA_F64_FRAC);
}

// Rounds to an integral value. Modes: 0 none, 1 nearest even, 2 floor, 3 ceil,
// 4 truncate. NaN and infinity pass through
inline ulong hydra_f64_round_int(ulong x, uint mode) {
    if (mode == 0u || (x & ~HYDRA_F64_SIGN) >= HYDRA_F64_INF) return x;
    const ulong sign = x & HYDRA_F64_SIGN;
    const int e = int((x >> 52) & 0x7ffu) - 1023;
    if (e >= 52) return x;
    if (e < 0) {
        const bool nonzero = (x & ~HYDRA_F64_SIGN) != 0ul;
        bool one = false;
        if (mode == 1u) one = e == -1 && (x & HYDRA_F64_FRAC) != 0ul;
        else if (mode == 2u) one = sign != 0ul && nonzero;
        else if (mode == 3u) one = sign == 0ul && nonzero;
        return one ? (sign | 0x3ff0000000000000ul) : sign;
    }
    const ulong mask = (1ul << uint(52 - e)) - 1ul;
    const ulong frac = x & mask;
    if (frac == 0ul) return x;
    ulong result = x & ~mask;
    const ulong halfway = 1ul << uint(51 - e);
    bool up;
    if (mode == 1u) up = frac > halfway || (frac == halfway && ((x >> uint(52 - e)) & 1ul) != 0ul);
    else if (mode == 2u) up = sign != 0ul;
    else if (mode == 3u) up = sign == 0ul;
    else up = false;
    if (up) result += 1ul << uint(52 - e);
    return result;
}

// Order key: unsigned comparison of keys is the numeric order, with -0 below +0
inline ulong hydra_f64_key(ulong x) {
    return (x & HYDRA_F64_SIGN) != 0ul ? ~x : (x | HYDRA_F64_SIGN);
}

inline uint hydra_f64_word(ulong v, uint high) {
    return high != 0u ? uint(v >> 32) : uint(v);
}
#endif
)";

constexpr std::string_view F64_TO_INT_SOURCE = R"(
inline uint hydra_f64_to_int(uint lo, uint hi, uint control) {
    ulong x = ulong(lo) | (ulong(hi) << 32);
    if ((control & 0x40u) != 0u) x &= ~HYDRA_F64_SIGN;
    if ((control & 0x80u) != 0u) x ^= HYDRA_F64_SIGN;
    ulong result = 0ul;
    if (hydra_f64_is_nan(x)) {
        // 64-bit results of a NaN are the most negative integer
        if ((control & 0x400u) != 0u) result = 1ul << 63;
    } else {
        const ulong r = hydra_f64_round_int(x, (control & 3u) + 1u);
        const bool negative = (r & HYDRA_F64_SIGN) != 0ul;
        const int e = int((r >> 52) & 0x7ffu) - 1023;
        ulong magnitude;
        bool overflow = false;
        if ((r & ~HYDRA_F64_SIGN) == HYDRA_F64_INF || e >= 64) {
            overflow = true;
            magnitude = ~0ul;
        } else if (e < 0) {
            magnitude = 0ul;
        } else {
            const ulong mantissa = (r & HYDRA_F64_FRAC) | (1ul << 52);
            magnitude = e >= 52 ? mantissa << uint(e - 52) : mantissa >> uint(52 - e);
        }
        const bool is_signed = (control & 4u) != 0u;
        const uint bits = (control & 0x400u) != 0u ? 64u : ((control & 8u) != 0u ? 16u : 32u);
        if (is_signed) {
            const ulong limit = 1ul << (bits - 1u);
            if (negative)
                result = (overflow || magnitude >= limit) ? 0ul - limit : 0ul - magnitude;
            else
                result = (overflow || magnitude >= limit) ? limit - 1ul : magnitude;
        } else if (!negative) {
            const ulong maximum = bits == 64u ? ~0ul : ((1ul << bits) - 1ul);
            result = (overflow || magnitude > maximum) ? maximum : magnitude;
        }
    }
    if ((control & 0x400u) != 0u)
        return (control & 0x200u) != 0u ? uint(result >> 32) : uint(result);
    return uint(result);
}
)";

constexpr std::string_view F64_FROM_INT_SOURCE = R"(
inline uint hydra_f64_from_int(uint lo, uint hi, uint control) {
    const uint width = control & 3u;
    const bool is_signed = (control & 4u) != 0u;
    const bool absolute = (control & 0x80u) != 0u;
    ulong value;
    uint bits = 64u;
    if (width == 3u) {
        value = ulong(lo) | (ulong(hi) << 32);
    } else {
        bits = 8u << min(width, 2u);
        const uint offset = min(((control >> 5) & 3u) * 8u, 32u - bits);
        const uint field = is_signed
                               ? as_type<uint>(extract_bits(as_type<int>(lo), offset, bits))
                               : extract_bits(lo, offset, bits);
        value = is_signed ? ulong(long(as_type<int>(field))) : ulong(field);
    }
    long signed_value = as_type<long>(value);
    const long least = bits == 64u ? as_type<long>(0x8000000000000000ul) : -(1l << (bits - 1u));
    const bool is_least = is_signed && signed_value == least;
    bool negative = false;
    ulong magnitude = value;
    if (is_signed) {
        negative = signed_value < 0;
        magnitude = negative ? 0ul - value : value;
        if (absolute && !is_least) negative = false;
    }
    if ((control & 0x100u) != 0u && (absolute || !is_least))
        negative = !negative;
    ulong result = negative ? HYDRA_F64_SIGN : 0ul;
    if (magnitude != 0ul) {
        const int msb = 63 - int(clz(magnitude));
        ulong mantissa;
        int exponent = msb;
        if (msb <= 52) {
            mantissa = magnitude << uint(52 - msb);
        } else {
            const uint shift = uint(msb - 52);
            mantissa = magnitude >> shift;
            const ulong rest = magnitude & ((1ul << shift) - 1ul);
            const ulong halfway = 1ul << (shift - 1u);
            if (rest > halfway || (rest == halfway && (mantissa & 1ul) != 0ul)) mantissa += 1ul;
            if (mantissa == (1ul << 53)) {
                mantissa >>= 1;
                exponent += 1;
            }
        }
        result |= (ulong(exponent + 1023) << 52) | (mantissa & HYDRA_F64_FRAC);
    }
    return hydra_f64_word(result, (control >> 9) & 1u);
}
)";

constexpr std::string_view F64_ROUND_SOURCE = R"(
inline uint hydra_f64_round(uint lo, uint hi, uint control) {
    ulong x = ulong(lo) | (ulong(hi) << 32);
    if ((control & 8u) != 0u) x &= ~HYDRA_F64_SIGN;
    if ((control & 0x10u) != 0u) x ^= HYDRA_F64_SIGN;
    return hydra_f64_word(hydra_f64_round_int(x, control & 7u), (control >> 5) & 1u);
}
)";

constexpr std::string_view F64_FMA_SOURCE = R"(
inline uint hydra_f64_fma_word(uint alo, uint ahi, uint blo, uint bhi,
                               uint clo, uint chi, uint control) {
    const ulong r = hydra_f64_fma(ulong(alo) | (ulong(ahi) << 32),
                                  ulong(blo) | (ulong(bhi) << 32),
                                  ulong(clo) | (ulong(chi) << 32), control & 3u);
    return hydra_f64_word(r, (control >> 2) & 1u);
}
)";

// op is a FloatCmpOp: 0 F, 1 LT, 2 EQ, 3 LE, 4 GT, 5 NE, 6 GE, 7 NUM, 8 NAN,
// 9 LTU, 10 EQU, 11 LEU, 12 GTU, 13 NEU, 14 GEU, 15 T
constexpr std::string_view F64_COMPARE_SOURCE = R"(
inline uint hydra_f64_compare(uint alo, uint ahi, uint blo, uint bhi, uint op) {
    const ulong a = ulong(alo) | (ulong(ahi) << 32);
    const ulong b = ulong(blo) | (ulong(bhi) << 32);
    const bool unordered = hydra_f64_is_nan(a) || hydra_f64_is_nan(b);
    const ulong ka = hydra_f64_key(a), kb = hydra_f64_key(b);
    const bool zeros = ((a | b) & ~HYDRA_F64_SIGN) == 0ul;
    const bool eq = !unordered && (zeros || ka == kb);
    const bool lt = !unordered && !zeros && ka < kb;
    const bool gt = !unordered && !zeros && ka > kb;
    bool result;
    switch (op) {
    case 0u: result = false; break;
    case 1u: result = lt; break;
    case 2u: result = eq; break;
    case 3u: result = lt || eq; break;
    case 4u: result = gt; break;
    case 5u: result = !unordered && !eq; break;
    case 6u: result = gt || eq; break;
    case 7u: result = !unordered; break;
    case 8u: result = unordered; break;
    case 9u: result = lt || unordered; break;
    case 10u: result = eq || unordered; break;
    case 11u: result = lt || eq || unordered; break;
    case 12u: result = gt || unordered; break;
    case 13u: result = !eq; break;
    case 14u: result = gt || eq || unordered; break;
    default: result = true; break;
    }
    return result ? 1u : 0u;
}
)";

constexpr std::string_view F64_MINMAX_SOURCE = R"(
inline uint hydra_f64_minmax(uint alo, uint ahi, uint blo, uint bhi, uint control) {
    const ulong a = ulong(alo) | (ulong(ahi) << 32);
    const ulong b = ulong(blo) | (ulong(bhi) << 32);
    ulong r;
    if (hydra_f64_is_nan(a) && hydra_f64_is_nan(b)) r = HYDRA_F64_QNAN;
    else if (hydra_f64_is_nan(a)) r = b;
    else if (hydra_f64_is_nan(b)) r = a;
    else {
        const bool a_less = hydra_f64_key(a) < hydra_f64_key(b);
        r = ((control & 1u) != 0u) ? (a_less ? b : a) : (a_less ? a : b);
    }
    return hydra_f64_word(r, (control >> 1) & 1u);
}
)";

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
    case ir::Intrinsic::F64Fma: {
        static const std::string source =
            std::string(F64_SOURCE) + std::string(F64_FMA_SOURCE);
        return source;
    }
    case ir::Intrinsic::F64Compare: {
        static const std::string source =
            std::string(F64_SOURCE) + std::string(F64_COMPARE_SOURCE);
        return source;
    }
    case ir::Intrinsic::F64MinMax: {
        static const std::string source =
            std::string(F64_SOURCE) + std::string(F64_MINMAX_SOURCE);
        return source;
    }
    case ir::Intrinsic::F64ToInt: {
        static const std::string source =
            std::string(F64_SOURCE) + std::string(F64_TO_INT_SOURCE);
        return source;
    }
    case ir::Intrinsic::F64FromInt: {
        static const std::string source =
            std::string(F64_SOURCE) + std::string(F64_FROM_INT_SOURCE);
        return source;
    }
    case ir::Intrinsic::F64Round: {
        static const std::string source =
            std::string(F64_SOURCE) + std::string(F64_ROUND_SOURCE);
        return source;
    }
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
    case ir::Intrinsic::F64Fma:
        return "hydra_f64_fma_word";
    case ir::Intrinsic::F64Compare:
        return "hydra_f64_compare";
    case ir::Intrinsic::F64MinMax:
        return "hydra_f64_minmax";
    case ir::Intrinsic::F64ToInt:
        return "hydra_f64_to_int";
    case ir::Intrinsic::F64FromInt:
        return "hydra_f64_from_int";
    case ir::Intrinsic::F64Round:
        return "hydra_f64_round";
    }
    unreachable();
}

} // namespace
  // hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen::lang::msl
