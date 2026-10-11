#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/move.hpp"

#include <bit>

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

// TODO: mask
void emitMove(DecoderContext& context, pred_t pred, bool pred_inv, reg_t dst,
              ir::Value src) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    context.builder.opCopy(ir::Value::createRegister(dst), src);

    if (conditional)
        context.builder.opEndIf();
}

void emitSelect(DecoderContext& context, pred_t pred, bool pred_inv, reg_t dst,
                pred_t src_pred, bool src_pred_inv, reg_t src_a,
                ir::Value src_b) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    const auto src_pred_v = notIf(
        context.builder, ir::Value::createPredicate(src_pred), src_pred_inv);
    const auto res = context.builder.opSelect(
        src_pred_v, ir::Value::createRegister(src_a), src_b);
    context.builder.opCopy(ir::Value::createRegister(dst), res);

    if (conditional)
        context.builder.opEndIf();
}

} // namespace

void emitMovR(DecoderContext& context, InstMovR inst) {
    emitMove(context, inst.base.pred, inst.base.pred_inv, inst.base.dst,
             ir::Value::createRegister(inst.src));
}

void emitMovC(DecoderContext& context, InstMovC inst) {
    emitMove(context, inst.base.pred, inst.base.pred_inv, inst.base.dst,
             ir::Value::createConstMemory(CMem(
                 inst.cbuf_slot, RZ, static_cast<u64>(inst.cbuf_offset * 4))));
}

void emitMovI(DecoderContext& context, InstMovI inst) {
    emitMove(context, inst.base.pred, inst.base.pred_inv, inst.base.dst,
             ir::Value::createConstantU(
                 getIntImm20(inst.imm20_0, inst.imm20_19, false)));
}

void emitMov32I(DecoderContext& context, InstMov32I inst) {
    emitMove(context, inst.pred, inst.pred_inv, inst.dst,
             ir::Value::createConstantU(inst.imm));
}

void emitS2R(DecoderContext& context, InstS2R inst) {
    constexpr u32 SR_LANE_ID = 0x00;
    constexpr u32 SR_Y_DIRECTION = 0x12;

    // Thread and block IDs, lane masks
    const auto sr = static_cast<u32>(inst.sr);
    if (context.decomp_context.type == ShaderType::Compute &&
        (sr == SR_LANE_ID || (sr >= 0x20 && sr <= 0x23) ||
         (sr >= 0x25 && sr <= 0x27) || (sr >= 0x38 && sr <= 0x3c))) {
        emitMove(context, inst.pred, inst.pred_inv, inst.dst,
                 context.builder.opReadSystemRegister(sr));
        return;
    }

    u32 value = 0;
    switch (static_cast<u32>(inst.sr)) {
    case SR_Y_DIRECTION:
        // TODO: -1.0 when the window origin flips Y
        value = std::bit_cast<u32>(1.0f);
        break;
    case SR_LANE_ID:
        // TODO: lane ID
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Lane ID");
        break;
    default:
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Special register {:#x}",
                            static_cast<u32>(inst.sr));
        break;
    }

    emitMove(context, inst.pred, inst.pred_inv, inst.dst,
             ir::Value::createConstantU(value));
}

void emitSelR(DecoderContext& context, InstSelR inst) {
    emitSelect(context, inst.base.pred, inst.base.pred_inv, inst.base.dst,
               inst.base.src_pred, inst.base.src_pred_inv, inst.base.src_a,
               ir::Value::createRegister(inst.src_b));
}

void emitSelC(DecoderContext& context, InstSelC inst) {
    emitSelect(
        context, inst.base.pred, inst.base.pred_inv, inst.base.dst,
        inst.base.src_pred, inst.base.src_pred_inv, inst.base.src_a,
        ir::Value::createConstMemory(
            CMem(inst.cbuf_slot, RZ, static_cast<u64>(inst.cbuf_offset * 4))));
}

void emitSelI(DecoderContext& context, InstSelI inst) {
    emitSelect(context, inst.base.pred, inst.base.pred_inv, inst.base.dst,
               inst.base.src_pred, inst.base.src_pred_inv, inst.base.src_a,
               ir::Value::createConstantU(
                   getIntImm20(inst.imm20_0, inst.imm20_19, true)));
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
