#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

union InstBfeBase {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> src_a;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<bool, 40, 1> brev;
    BitField64<bool, 47, 1> write_cc;
    BitField64<bool, 48, 1> is_signed;
};

DEFINE_INST_SRC2_VARIANTS(Bfe);

union InstBfiBase {
    BitField64<reg_t, 0, 8> dst;
    BitField64<reg_t, 8, 8> insert;
    BitField64<pred_t, 16, 3> pred;
    BitField64<bool, 19, 1> pred_inv;
    BitField64<reg_t, 39, 8> src_c;
    BitField64<bool, 47, 1> write_cc;
};
DEFINE_INST_SRC2_VARIANTS(Bfi);
void emitBfiRC(DecoderContext& context, InstBfiC inst);

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
