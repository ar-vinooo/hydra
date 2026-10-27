#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/comparison_helper.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

enum class HalfOperandForm {
    Register,
    ConstBuffer,
    Immediate,
};

// HSET2: compares both halves and writes a mask (or 1.0 with .BF) per half
void emitHset2(DecoderContext& context, instruction_t inst,
               HalfOperandForm form);
// HSETP2: compares both halves into two predicates (or their AND with .H_AND)
void emitHsetp2(DecoderContext& context, instruction_t inst,
                HalfOperandForm form);

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
