#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/shift.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

void emitShift(DecoderContext& context, reg_t dst, reg_t src_a, ir::Value raw,
               pred_t pred, bool pred_inv, bool right, bool wrap,
               bool is_signed = false, bool brev = false) {
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, pred, pred_inv);
    const auto u = [](u32 x) { return ir::Value::createConstantU(x); };
    auto value = ir::Value::createRegister(src_a);
    if (brev) {
        // Reverse bits without depending on a backend-specific intrinsic.
        for (const auto& [n, mask] :
             std::array<std::pair<u32, u32>, 5>{{{1, 0x55555555},
                                                 {2, 0x33333333},
                                                 {4, 0x0f0f0f0f},
                                                 {8, 0x00ff00ff},
                                                 {16, 0x0000ffff}}}) {
            value = b.opBitwiseOr(
                b.opShiftLeft(b.opBitwiseAnd(value, u(mask)), u(n)),
                b.opBitwiseAnd(b.opShiftRight(value, u(n)), u(mask)));
        }
    }
    const auto n = b.opBitwiseAnd(raw, u(31));
    auto result =
        right
            ? (is_signed ? b.opBitCast(
                               b.opShiftRight(
                                   b.opBitCast(value, ir::ScalarType::I32), n),
                               ir::ScalarType::U32)
                         : b.opShiftRight(value, n))
            : b.opShiftLeft(value, n);
    if (!wrap) {
        const auto fill =
            right && is_signed
                ? b.opBitCast(
                      b.opShiftRight(b.opBitCast(value, ir::ScalarType::I32),
                                     u(31)),
                      ir::ScalarType::U32)
                : u(0);
        result =
            b.opSelect(b.opCompareGreaterOrEqual(raw, u(32)), fill, result);
    }
    b.opCopy(ir::Value::createRegister(dst), result);
    if (conditional)
        b.opEndIf();
}

void emitShiftLeft(DecoderContext& context, InstShlBase inst, ir::Value shift) {
    emitShift(context, inst.dst, inst.src_a, shift, inst.pred, inst.pred_inv,
              false, inst.m);
    if (inst.x || inst.write_cc)
        SHADER_UNSUPPORTED("SHL extended mode/condition code");
}

void emitShiftRight(DecoderContext& context, InstShrBase inst,
                    ir::Value shift) {
    emitShift(context, inst.dst, inst.src_a, shift, inst.pred, inst.pred_inv,
              true, inst.m, inst.is_signed, inst.brev);
    if (static_cast<u32>(inst.x_mode.get()) || inst.write_cc)
        SHADER_UNSUPPORTED("SHR extended mode/condition code");
}

} // namespace

void emitShlR(DecoderContext& context, InstShlR inst) {
    emitShiftLeft(context, inst.base, ir::Value::createRegister(inst.src_b));
}

void emitShlC(DecoderContext& context, InstShlC inst) {
    emitShiftLeft(
        context, inst.base,
        ir::Value::createConstMemory(
            CMem(inst.cbuf_slot, RZ, static_cast<u64>(inst.cbuf_offset * 4))));
}

void emitShlI(DecoderContext& context, InstShlI inst) {
    emitShiftLeft(context, inst.base,
                  ir::Value::createConstantU(
                      getIntImm20(inst.imm20_0, inst.imm20_19, true)));
}

void emitShrR(DecoderContext& context, InstShrR inst) {
    emitShiftRight(context, inst.base, ir::Value::createRegister(inst.src_b));
}

void emitShrC(DecoderContext& context, InstShrC inst) {
    emitShiftRight(
        context, inst.base,
        ir::Value::createConstMemory(
            CMem(inst.cbuf_slot, RZ, static_cast<u64>(inst.cbuf_offset * 4))));
}

void emitShrI(DecoderContext& context, InstShrI inst) {
    emitShiftRight(context, inst.base,
                   ir::Value::createConstantU(
                       getIntImm20(inst.imm20_0, inst.imm20_19, true)));
}

void emitShf(DecoderContext& context, instruction_t inst, bool right,
             bool immediate) {
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, pred_t(extractBits(inst, 16, 3)),
                                            extractBits(inst, 19, 1));
    const auto u = [](u32 x) { return ir::Value::createConstantU(x); };
    const auto lo = ir::Value::createRegister(reg_t(extractBits(inst, 8, 8)));
    const auto hi = ir::Value::createRegister(reg_t(extractBits(inst, 39, 8)));
    const u32 mode = static_cast<u32>(extractBits(inst, 37, 2));
    const bool long_shift = mode >= 2;
    const bool signed_shift = mode == 3;
    const auto raw =
        immediate ? u(static_cast<u32>(extractBits(inst, 20, 6)))
                  : ir::Value::createRegister(reg_t(extractBits(inst, 20, 8)));
    const u32 limit = long_shift ? 64 : 32;
    const auto n = extractBits(inst, 50, 1) ? b.opBitwiseAnd(raw, u(limit - 1))
                                            : b.opMin(raw, u(limit));
    const auto small = b.opBitwiseAnd(n, u(31));
    const auto opposite = b.opBitwiseAnd(b.opNeg(small), u(31));
    auto part = right ? b.opBitwiseOr(b.opShiftRight(lo, small),
                                      b.opShiftLeft(hi, opposite))
                      : b.opBitwiseOr(b.opShiftLeft(hi, small),
                                      b.opShiftRight(lo, opposite));
    part = b.opSelect(b.opCompareEqual(n, u(0)), right ? lo : hi, part);
    auto high_part = ir::Value::createUndefined();
    if (right && signed_shift)
        high_part = b.opBitCast(
            b.opShiftRight(b.opBitCast(hi, ir::ScalarType::I32), small),
            ir::ScalarType::U32);
    else
        high_part =
            right ? b.opShiftRight(hi, small) : b.opShiftLeft(lo, small);
    auto res = b.opSelect(b.opCompareGreaterOrEqual(n, u(32)), high_part, part);
    if (long_shift) {
        const auto fill =
            right && signed_shift
                ? b.opBitCast(b.opShiftRight(
                                  b.opBitCast(hi, ir::ScalarType::I32), u(31)),
                              ir::ScalarType::U32)
                : u(0);
        res = b.opSelect(b.opCompareEqual(n, u(64)), fill, res);
    }
    b.opCopy(ir::Value::createRegister(reg_t(extractBits(inst, 0, 8))), res);
    if (extractBits(inst, 47, 1) || extractBits(inst, 48, 2))
        SHADER_UNSUPPORTED("SHF condition code/extended mode");
    if (conditional)
        b.opEndIf();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
