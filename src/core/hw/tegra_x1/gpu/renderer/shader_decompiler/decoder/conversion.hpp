#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

enum class FloatFormat {
    F16 = 1,
    F32 = 2,
    F64 = 3,
};

// Integer rounding of same-size F2F, from bits 39-40 and 42 (bit 41 selects
// the half of an F16 source)
enum class RoundMode2 {
    Pass = 1,
    Round = 4,
    Floor = 5,
    Ceil = 6,
    Trunc = 7,
};

union InstF2fBase {
    BitField64<reg_t, 0, 8> dst;
    BitField64<FloatFormat, 8, 2> dst_fmt;
    BitField64<FloatFormat, 10, 2> src_fmt;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<u32, 39, 2> round_mode_0;
    BitField64<bool, 41, 1> sh;
    BitField64<u32, 42, 1> round_mode_2;
    BitField64<bool, 44, 1> ftz;
    BitField64<bool, 45, 1> neg;
    BitField64<bool, 47, 1> write_cc;
    BitField64<bool, 49, 1> abs;
    BitField64<bool, 50, 1> sat;

    RoundMode2 getRoundMode() const {
        return static_cast<RoundMode2>(round_mode_0.get() |
                                       (round_mode_2.get() << 2));
    }
};

DEFINE_INST_SRC1_VARIANTS(F2f);

enum class IntegerWidth {
    Invalid = 0,
    _16 = 1,
    _32 = 2,
    _64 = 3,
};

enum class RoundMode3 {
    Round = 0,
    Floor = 1,
    Ceil = 2,
    Trunc = 3,
};

union InstF2iBase {
    BitField64<reg_t, 0, 8> dst;
    BitField64<IntegerWidth, 8, 2> dst_width;
    BitField64<FloatFormat, 10, 2> src_fmt;
    BitField64<bool, 12, 1> dst_signed;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<RoundMode3, 39, 2> round_mode;
    BitField64<bool, 41, 1> sh;
    BitField64<bool, 44, 1> ftz;
    BitField64<bool, 45, 1> neg;
    BitField64<bool, 47, 1> write_cc;
    BitField64<bool, 49, 1> abs;
};

DEFINE_INST_SRC1_VARIANTS(F2i);

// 8, 16 and 32-bit integers (3 is 64-bit for I2F)
enum class SmallIntegerWidth {
    _8 = 0,
    _16 = 1,
    _32 = 2,
    _64 = 3,
};

union InstI2iBase {
    BitField64<reg_t, 0, 8> dst;
    BitField64<SmallIntegerWidth, 8, 2> dst_width;
    BitField64<SmallIntegerWidth, 10, 2> src_width;
    BitField64<bool, 12, 1> dst_signed;
    BitField64<bool, 13, 1> src_signed;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<u32, 41, 3> byte_sel;
    BitField64<bool, 45, 1> neg;
    BitField64<bool, 47, 1> write_cc;
    BitField64<bool, 49, 1> abs;
    BitField64<bool, 50, 1> sat;
};

DEFINE_INST_SRC1_VARIANTS(I2i);

union InstI2fBase {
    BitField64<reg_t, 0, 8> dst;
    BitField64<FloatFormat, 8, 2> dst_fmt;
    BitField64<SmallIntegerWidth, 10, 2> src_width;
    BitField64<bool, 13, 1> src_signed;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<RoundMode, 39, 2> round_mode;
    BitField64<u32, 41, 2> byte_sel;
    BitField64<bool, 45, 1> neg;
    BitField64<bool, 47, 1> write_cc;
    BitField64<bool, 49, 1> abs;
};

DEFINE_INST_SRC1_VARIANTS(I2f);

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
