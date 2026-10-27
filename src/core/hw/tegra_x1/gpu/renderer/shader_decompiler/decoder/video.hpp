#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

void emitVmad(DecoderContext& context, instruction_t inst);
void emitVmnmx(DecoderContext& context, instruction_t inst);
void emitVsetp(DecoderContext& context, instruction_t inst);

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
