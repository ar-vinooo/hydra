#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

// Where the second source of an FP64 instruction comes from
enum class DoubleSource {
    Register,
    Constant,
    Immediate,
};

void emitDadd(DecoderContext& context, instruction_t inst, DoubleSource source);
void emitDmul(DecoderContext& context, instruction_t inst, DoubleSource source);
void emitDfma(DecoderContext& context, instruction_t inst, DoubleSource source);
// DFMA with the constant as the addend (0x5370): B comes from bits 39-46
void emitDfmaConstantAddend(DecoderContext& context, instruction_t inst);
void emitDmnmx(DecoderContext& context, instruction_t inst, DoubleSource source);
void emitDsetp(DecoderContext& context, instruction_t inst, DoubleSource source);
void emitDset(DecoderContext& context, instruction_t inst, DoubleSource source);

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
