#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

enum class AvgMode {
    NoNeg = 0,
    NegB = 1,
    NegA = 2,
    PlusOne = 3,
};

union InstIaddBase {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> src_a;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<bool, 43, 1> x;
    BitField64<bool, 47, 1> write_cc;
    BitField64<AvgMode, 48, 2> avg_mode;
    BitField64<bool, 50, 1> sat;
};

DEFINE_INST_SRC2_VARIANTS(Iadd);

union InstIadd32I {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> src_a;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<u32, 20, 32> imm;
    BitField64<bool, 52, 1> write_cc;
    BitField64<bool, 53, 1> x;
    BitField64<bool, 54, 1> sat;
    BitField64<AvgMode, 55, 2> avg_mode;
};

void emitIadd32I(DecoderContext& context, InstIadd32I inst);

union InstIscaddBase {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> src_a;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<u32, 39, 5> shift;
    BitField64<bool, 47, 1> write_cc;
    BitField64<AvgMode, 48, 2> avg_mode;
};

DEFINE_INST_SRC2_VARIANTS(Iscadd);

union InstLeaLoBase {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> src_a;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<u32, 39, 5> shift;
    BitField64<bool, 45, 1> neg_a;
    BitField64<bool, 46, 1> x;
    BitField64<bool, 47, 1> write_cc;
    BitField64<pred_t, 48, 3> dst_pred;
};

DEFINE_INST_SRC2_VARIANTS(LeaLo);

union InstLeaHiR {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> src_a_lo;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<reg_t, 20, 8> src_b;
    BitField64<u32, 28, 5> shift;
    BitField64<bool, 37, 1> neg_a;
    BitField64<bool, 38, 1> x;
    BitField64<reg_t, 39, 8> src_a_hi;
    BitField64<bool, 47, 1> write_cc;
    BitField64<pred_t, 48, 3> dst_pred;
};

void emitLeaHiR(DecoderContext& context, InstLeaHiR inst);

union InstLeaHiC {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> src_a_lo;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<u32, 20, 14> cbuf_offset;
    BitField64<u32, 34, 5> cbuf_slot;
    BitField64<reg_t, 39, 8> src_a_hi;
    BitField64<bool, 47, 1> write_cc;
    BitField64<pred_t, 48, 3> dst_pred;
    BitField64<u32, 51, 5> shift;
    BitField64<bool, 56, 1> neg_a;
    BitField64<bool, 57, 1> x;
};

void emitLeaHiC(DecoderContext& context, InstLeaHiC inst);

union InstIscadd32I {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> src_a;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<u32, 20, 32> imm;
    BitField64<bool, 52, 1> write_cc;
    BitField64<u32, 53, 5> shift;
};

void emitIscadd32I(DecoderContext& context, InstIscadd32I inst);

enum class XmadCop {
    Cfull = 0,
    Clo = 1,
    Chi = 2,
    Csfu = 3,
    Cbcc = 4,
};

union InstXmadBase {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> src_a;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<bool, 38, 1> x; // R and I variants
    BitField64<bool, 47, 1> write_cc;
    BitField64<bool, 48, 1> a_signed;
    BitField64<bool, 49, 1> b_signed;
    BitField64<bool, 53, 1> hilo_a;
};

union InstXmadR {
    InstXmadBase base;
    BitField64<reg_t, 20, 8> src_b;
    BitField64<bool, 35, 1> hilo_b;
    BitField64<bool, 36, 1> psl;
    BitField64<bool, 37, 1> mrg;
    BitField64<reg_t, 39, 8> src_c;
    BitField64<XmadCop, 50, 3> cop;
};

void emitXmadR(DecoderContext& context, InstXmadR inst);

union InstXmadRC {
    InstXmadBase base;
    BitField64<u32, 20, 14> cbuf_offset;
    BitField64<u32, 34, 5> cbuf_slot;
    BitField64<reg_t, 39, 8> src_b;
    BitField64<XmadCop, 50, 2> cop;
    BitField64<bool, 52, 1> hilo_b;
    BitField64<bool, 54, 1> x;
};

void emitXmadRC(DecoderContext& context, InstXmadRC inst);

union InstXmadC {
    InstXmadBase base;
    BitField64<u32, 20, 14> cbuf_offset;
    BitField64<u32, 34, 5> cbuf_slot;
    BitField64<reg_t, 39, 8> src_c;
    BitField64<XmadCop, 50, 2> cop;
    BitField64<bool, 52, 1> hilo_b;
    BitField64<bool, 54, 1> x;
    BitField64<bool, 55, 1> psl;
    BitField64<bool, 56, 1> mrg;
};

void emitXmadC(DecoderContext& context, InstXmadC inst);

union InstXmadI {
    InstXmadBase base;
    BitField64<u32, 20, 16> imm16;
    BitField64<bool, 36, 1> psl;
    BitField64<bool, 37, 1> mrg;
    BitField64<reg_t, 39, 8> src_c;
    BitField64<XmadCop, 50, 3> cop;
};

void emitXmadI(DecoderContext& context, InstXmadI inst);

enum class Iadd3Shift {
    None = 0,
    Right = 1,
    Left = 2,
};

enum class Iadd3Half {
    Full = 0,
    Lower = 1,
    Upper = 2,
};

union InstIadd3Base {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> src_a;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<reg_t, 39, 8> src_c;
    BitField64<bool, 47, 1> write_cc;
    BitField64<bool, 48, 1> x;
    BitField64<bool, 49, 1> neg_c;
    BitField64<bool, 50, 1> neg_b;
    BitField64<bool, 51, 1> neg_a;
};

union InstIadd3R {
    InstIadd3Base base;
    BitField64<reg_t, 20, 8> src_b;
    BitField64<Iadd3Half, 31, 2> half_c;
    BitField64<Iadd3Half, 33, 2> half_b;
    BitField64<Iadd3Half, 35, 2> half_a;
    BitField64<Iadd3Shift, 37, 2> shift;
};
void emitIadd3R(DecoderContext& context, InstIadd3R inst);

union InstIadd3C {
    InstIadd3Base base;
    BitField64<u32, 20, 14> cbuf_offset;
    BitField64<u32, 34, 5> cbuf_slot;
};
void emitIadd3C(DecoderContext& context, InstIadd3C inst);

union InstIadd3I {
    InstIadd3Base base;
    BitField64<u32, 20, 19> imm20_0;
    BitField64<u32, 56, 1> imm20_19;
};
void emitIadd3I(DecoderContext& context, InstIadd3I inst);

union InstImnmxBase {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> src_a;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<pred_t, 39, 3> src_pred;
    BitField64<bool, 42, 1> src_pred_inv;
    BitField64<bool, 47, 1> write_cc;
    BitField64<bool, 48, 1> is_signed;
};

DEFINE_INST_SRC2_VARIANTS(Imnmx);

// Where IMAD takes its b and c operands from
enum class ImadForm {
    Register,            // b = R20, c = R39
    RegisterConstBuffer, // b = R39, c = c[slot][offset]
    ConstBuffer,         // b = c[slot][offset], c = R39
    Immediate,           // b = imm20, c = R39
};
void emitImad(DecoderContext& context, instruction_t inst, ImadForm form);

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
