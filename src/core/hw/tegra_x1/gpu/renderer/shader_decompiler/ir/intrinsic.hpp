#pragma once

#include "common/common.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::ir {

// Operations with exact guest semantics that the backend implements as helper
// functions. Operands and results are raw 32-bit words unless noted.
enum class Intrinsic : u32 {
    // (a, b, selector, mode): byte permute of {b, a}
    Prmt,
    // (a, b, signedness bits): high word of the 64-bit product
    IMulHi,
    // Conversions (source word(s), control word), see the backend helpers.
    // Results are whole register words.
    FloatToInt,
    FloatToInt64,
    FloatToFloat,
    F64ToF32,
    F32ToF64,
    IntToInt,
    IntToFloat,
    Int64ToFloat,
    // (high word of a double): high word of its approximate reciprocal
    // (square root)
    Rcp64H,
    Rsq64H,
    // (a): a with its bits reversed
    BitReverse,
    // FP64 on register words (low, high) of each operand. F64Fma: (a, b, c,
    // control) fused multiply-add, control bits 0-1 rounding (nearest even,
    // down, up, zero) and bit 2 the word to return. F64Compare: (a, b, op)
    // 1 or 0 for a FloatCmpOp. F64MinMax: (a, b, control) bit 0 selects the
    // maximum and bit 1 the word to return
    F64Fma,
    F64Compare,
    F64MinMax,
    // Exact conversions of doubles. F64ToInt: (double, control) bits 0-1
    // rounding (even, floor, ceil, trunc), 2 signed, 3 16-bit, 6 abs, 7 neg,
    // 9 high word, 10 64-bit result. F64FromInt: (integer words, control) as
    // Int64ToFloat plus bit 9 for the high word. F64Round: (double, control)
    // bits 0-2 mode (none, even, floor, ceil, trunc), 3 abs, 4 neg, 5 high word,
    // 6 saturate
    F64ToInt,
    F64FromInt,
    F64Round,
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::ir

ENABLE_ENUM_FORMATTING(
    hydra::hw::tegra_x1::gpu::renderer::shader_decomp::ir::Intrinsic, Prmt,
    "prmt", IMulHi, "imul_hi", FloatToInt, "f2i", FloatToInt64, "f2i64",
    FloatToFloat, "f2f", F64ToF32, "f64_to_f32", F32ToF64, "f32_to_f64",
    IntToInt, "i2i", IntToFloat, "i2f", Int64ToFloat, "i2f64", Rcp64H,
    "rcp64h", Rsq64H, "rsq64h", BitReverse, "bit_reverse", F64Fma, "f64_fma", F64Compare,
    "f64_compare", F64MinMax, "f64_minmax", F64ToInt, "f64_to_int",
    F64FromInt, "f64_from_int", F64Round, "f64_round")
