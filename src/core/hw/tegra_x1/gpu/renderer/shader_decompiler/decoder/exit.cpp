#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/exit.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

void emitKil(DecoderContext& context, InstKil inst) {
    context.builder.opBeginIf(getFlowCondition(
        context.builder, inst.pred, inst.pred_inv, inst.ccc));

    context.builder.opDiscard();

    context.builder.opEndIf();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
