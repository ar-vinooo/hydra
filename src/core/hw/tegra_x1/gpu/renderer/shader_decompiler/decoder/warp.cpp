#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/warp.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

void emitShuffle(DecoderContext& context, pred_t pred, bool pred_inv,
                 ShuffleMode mode, reg_t dst, pred_t dst_pred, reg_t src_a,
                 ir::Value src_b, ir::Value src_c) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    const auto src_a_v = ir::Value::createRegister(src_a);

    ir::Value res = src_a_v;
    auto pred_res = ir::Value::createConstantB(true);
    // Segment mask (c[12:8]) 0x1c groups lanes in 2x2 quads
    constexpr u32 QUAD_SEGMENT_MASK = 0x1c;
    const bool quad_segment =
        src_c.getKind() == ir::ValueKind::Constant &&
        ((src_c.getConstant() >> 8) & 0x1f) == QUAD_SEGMENT_MASK;
    if (mode == ShuffleMode::Bfly &&
        context.decomp_context.type == ShaderType::Fragment) {
        // Butterfly shuffles within a quad are used to compute derivatives
        res = context.builder.opQuadShuffleXor(src_a_v, src_b);
    } else if (mode == ShuffleMode::Idx && quad_segment &&
               context.decomp_context.type == ShaderType::Fragment) {
        // Indexed shuffle within each quad. Fragment lanes are only known by
        // their quad position, so the lane math below is done per quad: the
        // source lane is (b & ~segment) | quad base, in range if it does not
        // pass the quad base | (clamp & 3)
        auto& builder = context.builder;
        const u32 clamp = src_c.getConstant() & 0x3;
        const auto in_range = builder.opCompareLessOrEqual(
            builder.opBitwiseAnd(src_b,
                                 ir::Value::createConstantU(~QUAD_SEGMENT_MASK)),
            ir::Value::createConstantU(clamp));
        const auto shuffled = builder.opQuadShuffle(
            src_a_v,
            builder.opBitwiseAnd(src_b, ir::Value::createConstantU(0x3)));
        res = builder.opSelect(in_range, shuffled, src_a_v);
        pred_res = in_range;
    } else if (context.decomp_context.type == ShaderType::Compute) {
        // Host SIMD groups have 32 lanes, like warps
        auto& builder = context.builder;
        const auto clamp = builder.opBitwiseAnd(
            src_c, ir::Value::createConstantU(0x1f));
        const auto seg_mask = builder.opBitwiseAnd(
            builder.opShiftRight(src_c, ir::Value::createConstantU(8)),
            ir::Value::createConstantU(0x1f));
        const auto not_seg_mask = builder.opBitwiseNot(seg_mask);
        const auto lane = builder.opReadSystemRegister(0x00);
        const auto min_lane = builder.opBitwiseAnd(lane, seg_mask);
        const auto max_lane = builder.opBitwiseOr(
            min_lane, builder.opBitwiseAnd(clamp, not_seg_mask));
        ir::Value src_lane = ir::Value::createUndefined();
        ir::Value in_range = ir::Value::createUndefined();
        switch (mode) {
        case ShuffleMode::Idx:
            src_lane = builder.opBitwiseOr(
                builder.opBitwiseAnd(src_b, not_seg_mask), min_lane);
            in_range = builder.opCompareLessOrEqual(src_lane, max_lane);
            break;
        case ShuffleMode::Up:
            src_lane = builder.opAdd(lane, builder.opNeg(src_b));
            in_range = builder.opCompareGreaterOrEqual(
                builder.opBitCast(src_lane, ir::ScalarType::I32),
                builder.opBitCast(max_lane, ir::ScalarType::I32));
            break;
        case ShuffleMode::Down:
            src_lane = builder.opAdd(lane, src_b);
            in_range = builder.opCompareLessOrEqual(src_lane, max_lane);
            break;
        case ShuffleMode::Bfly:
            src_lane = builder.opBitwiseXor(lane, src_b);
            in_range = builder.opCompareLessOrEqual(src_lane, max_lane);
            break;
        }
        const auto shuffled = builder.opShuffle(
            src_a_v,
            builder.opBitwiseAnd(src_lane, ir::Value::createConstantU(0x1f)));
        res = builder.opSelect(in_range, shuffled, src_a_v);
        pred_res = in_range;
    } else {
        // TODO: implement
        LOG_FUNC_WITH_ARGS_NOT_IMPLEMENTED(
            ShaderDecompiler, "mode: {}, src_a: {}, src_b: {}, src_c: {}",
            mode, src_a_v, src_b, src_c);
    }
    context.builder.opCopy(ir::Value::createRegister(dst), res);
    context.builder.opCopy(ir::Value::createPredicate(dst_pred), pred_res);

    if (conditional)
        context.builder.opEndIf();
}

} // namespace

void emitShfl(DecoderContext& context, InstShfl inst) {
    emitShuffle(context, inst.pred, inst.pred_inv, inst.mode, inst.dst,
                inst.dst_pred, inst.src_a,
                inst.b_is_imm ? ir::Value::createConstantU(inst.src_b_imm5)
                              : ir::Value::createRegister(inst.src_b_r),
                inst.c_is_imm ? ir::Value::createConstantU(inst.src_c_imm13)
                              : ir::Value::createRegister(inst.src_c_r));
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
