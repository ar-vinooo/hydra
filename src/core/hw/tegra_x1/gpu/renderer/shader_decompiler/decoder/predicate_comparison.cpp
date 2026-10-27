#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/predicate_comparison.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

void emitPredicateSet(DecoderContext& context, pred_t pred, bool pred_inv,
                      BoolOp b_op_ab, BoolOp b_op_c, bool b_float, bool write_cc, reg_t dst,
                      pred_t src_a_pred, bool src_a_pred_inv, pred_t src_b_pred,
                      bool src_b_pred_inv, pred_t src_c_pred,
                      bool src_c_pred_inv) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    auto src_a_v =
        notIf(context.builder, ir::Value::createPredicate(src_a_pred),
              src_a_pred_inv);
    auto src_b_v =
        notIf(context.builder, ir::Value::createPredicate(src_b_pred),
              src_b_pred_inv);
    auto src_c_v =
        notIf(context.builder, ir::Value::createPredicate(src_c_pred),
              src_c_pred_inv);

    auto res = getLogical(context, b_op_ab, src_a_v, src_b_v);
    res = getLogical(context, b_op_c, res, src_c_v);

    if (write_cc)
        writeSetConditionCodes(context.builder, res, b_float);

    if (b_float) {
        res = context.builder.opSelect(res, ir::Value::createConstantF(1.0f),
                                       ir::Value::createConstantF(0.0f));
        context.builder.opCopy(
            ir::Value::createRegister(dst, ir::ScalarType::F32), res);
    } else {
        // Without .BF, true is written as 0xffffffff (usable as a bit mask
        // or as -1), not as 1
        res = context.builder.opSelect(res, ir::Value::createConstantU(0xffffffffu),
                                       ir::Value::createConstantU(0u));
        context.builder.opCopy(ir::Value::createRegister(dst), res);
    }

    if (conditional)
        context.builder.opEndIf();
}

void emitPredicateSetPredicate(DecoderContext& context, pred_t pred,
                               bool pred_inv, BoolOp b_op_ab, BoolOp b_op_c,
                               pred_t dst_pred, pred_t dst_inv_pred,
                               pred_t src_a_pred, bool src_a_pred_inv,
                               pred_t src_b_pred, bool src_b_pred_inv,
                               pred_t src_c_pred, bool src_c_pred_inv) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    auto src_a_v =
        notIf(context.builder, ir::Value::createPredicate(src_a_pred),
              src_a_pred_inv);
    auto src_b_v =
        notIf(context.builder, ir::Value::createPredicate(src_b_pred),
              src_b_pred_inv);
    auto src_c_v =
        notIf(context.builder, ir::Value::createPredicate(src_c_pred),
              src_c_pred_inv);

    auto res0 = getLogical(context, b_op_ab, src_a_v, src_b_v);
    auto res1 = getLogical(context, b_op_ab, context.builder.opNot(src_a_v), src_b_v);

    res0 = getLogical(context, b_op_c, res0, src_c_v);
    res1 = getLogical(context, b_op_c, res1, src_c_v);

    context.builder.opCopy(ir::Value::createPredicate(dst_pred), res0);
    context.builder.opCopy(ir::Value::createPredicate(dst_inv_pred), res1);

    if (conditional)
        context.builder.opEndIf();
}

} // namespace

void emitPset(DecoderContext& context, InstPset inst) {
    emitPredicateSet(context, inst.pred, inst.pred_inv, inst.b_op_ab,
                     inst.b_op_c, inst.b_float, inst.write_cc, inst.dst, inst.src_a_pred,
                     inst.src_a_pred_inv, inst.src_b_pred, inst.src_b_pred_inv,
                     inst.src_c_pred, inst.src_c_pred_inv);
}

void emitPsetp(DecoderContext& context, InstPsetp inst) {
    emitPredicateSetPredicate(context, inst.pred, inst.pred_inv, inst.b_op_ab,
                              inst.b_op_c, inst.dst_pred, inst.dst_inv_pred,
                              inst.src_a_pred, inst.src_a_pred_inv,
                              inst.src_b_pred, inst.src_b_pred_inv,
                              inst.src_c_pred, inst.src_c_pred_inv);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
