#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/const.hpp"

// Instructions mostly used by compute shaders. Field layouts follow
// src/shader_recompiler/frontend/maxwell/translate/impl in
// https://github.com/eden-emulator/mirror
namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

// Shared memory
void emitLds(DecoderContext& context, instruction_t inst);
void emitSts(DecoderContext& context, instruction_t inst);

// Synchronization
void emitBar(DecoderContext& context, instruction_t inst);

// Surfaces
void emitSust(DecoderContext& context, instruction_t inst);
void emitSuatom(DecoderContext& context, instruction_t inst);

// Integer
void emitLop3R(DecoderContext& context, instruction_t inst);
void emitLop3C(DecoderContext& context, instruction_t inst);
void emitLop3I(DecoderContext& context, instruction_t inst);
void emitPopcR(DecoderContext& context, instruction_t inst);
void emitPopcC(DecoderContext& context, instruction_t inst);
void emitPopcI(DecoderContext& context, instruction_t inst);
void emitFloR(DecoderContext& context, instruction_t inst);
void emitFloC(DecoderContext& context, instruction_t inst);
void emitFloI(DecoderContext& context, instruction_t inst);

// Warp
void emitVote(DecoderContext& context, instruction_t inst);

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
