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
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::ir

ENABLE_ENUM_FORMATTING(
    hydra::hw::tegra_x1::gpu::renderer::shader_decomp::ir::Intrinsic, Prmt,
    "prmt", IMulHi, "imul_hi", FloatToInt, "f2i", FloatToInt64, "f2i64",
    FloatToFloat, "f2f", F64ToF32, "f64_to_f32", F32ToF64, "f32_to_f64",
    IntToInt, "i2i", IntToFloat, "i2f", Int64ToFloat, "i2f64", Rcp64H,
    "rcp64h", Rsq64H, "rsq64h", BitReverse, "bit_reverse")
