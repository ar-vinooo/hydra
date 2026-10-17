#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

enum class OperandForm {
    Register,
    ConstBuffer,
    Immediate,
    // Register in bits 39..46 for the second source, constant for the third
    RegisterConstBuffer,
};

// Second source operand of an ALU instruction in the given form, as a raw word
// (immediates are sign extended 20-bit values)
ir::Value getAluSrcB(DecoderContext& context, instruction_t inst,
                     OperandForm form);

void emitPrmt(DecoderContext& context, instruction_t inst, OperandForm form);
void emitImul(DecoderContext& context, instruction_t inst, OperandForm form);
void emitImul32I(DecoderContext& context, instruction_t inst);
void emitP2r(DecoderContext& context, instruction_t inst, OperandForm form);
void emitFchk(DecoderContext& context, instruction_t inst, OperandForm form);

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
