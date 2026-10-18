#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/video.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/comparison_helper.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {
namespace {

ir::Value reg(DecoderContext& context, instruction_t inst, u32 offset) {
    return context.builder.opCopy(ir::Value::createRegister(
        static_cast<reg_t>(extractBits(inst, offset, 8))));
}

ir::Value extract(DecoderContext& context, ir::Value value, u32 width,
                  u32 selector, bool is_signed) {
    // Byte selectors share bit 1 with the width field. For halfwords that
    // bit is zero, leaving h0/h1; the full-word form ignores the selector.
    const u32 bits = width < 2 ? 8 : width == 2 ? 16 : 32;
    if (bits == 32)
        return value;
    auto& b = context.builder;
    value = b.opShiftRight(value, ir::Value::createConstantU(selector * bits));
    value = b.opBitwiseAnd(value, ir::Value::createConstantU((1u << bits) - 1));
    if (is_signed) {
        value = b.opBitCast(
            b.opShiftRight(
                b.opBitCast(
                    b.opShiftLeft(value, ir::Value::createConstantU(32 - bits)),
                    ir::ScalarType::I32),
                ir::Value::createConstantU(32 - bits)),
            ir::ScalarType::U32);
    }
    return value;
}

std::pair<ir::Value, ir::Value> operands(DecoderContext& context,
                                         instruction_t inst) {
    const bool immediate = extractBits(inst, 50, 1) == 0;
    const auto a = extract(context, reg(context, inst, 8),
                           static_cast<u32>(extractBits(inst, 37, 2)),
                           static_cast<u32>(extractBits(inst, 36, 2)),
                           extractBits(inst, 48, 1) != 0);
    const auto raw_b = immediate ? ir::Value::createConstantU(static_cast<u32>(
                                       extractBits(inst, 20, 16)))
                                 : reg(context, inst, 20);
    const auto b =
        extract(context, raw_b,
                immediate ? 2 : static_cast<u32>(extractBits(inst, 29, 2)),
                immediate ? 0 : static_cast<u32>(extractBits(inst, 28, 2)),
                extractBits(inst, 49, 1) != 0);
    return {a, b};
}

ir::Value minmax(DecoderContext& context, ir::Value a, ir::Value b,
                 bool maximum, bool is_signed) {
    auto& builder = context.builder;
    const auto type = is_signed ? ir::ScalarType::I32 : ir::ScalarType::U32;
    a = builder.opBitCast(a, type);
    b = builder.opBitCast(b, type);
    return builder.opBitCast(maximum ? builder.opMax(a, b)
                                     : builder.opMin(a, b),
                             ir::ScalarType::U32);
}

bool begin(DecoderContext& context, instruction_t inst) {
    return handlePredCond(context.builder,
                          static_cast<pred_t>(extractBits(inst, 16, 3)),
                          extractBits(inst, 19, 1) != 0);
}

void write(DecoderContext& context, instruction_t inst, ir::Value result) {
    context.builder.opCopy(
        ir::Value::createRegister(static_cast<reg_t>(extractBits(inst, 0, 8))),
        result);
}

} // namespace

void emitVmad(DecoderContext& context, instruction_t inst) {
    // The base form is host-tested. Do not silently discard modifiers whose
    // carry/saturation/scaling semantics lack NVIDIA hardware evidence.
    if (extractBits(inst, 47, 1) || extractBits(inst, 51, 2) ||
        extractBits(inst, 55, 1)) {
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "VMAD CC/scale/saturation");
        return;
    }
    const bool conditional = begin(context, inst);
    const auto [a, b] = operands(context, inst);
    auto& builder = context.builder;
    auto product = builder.opMultiply(a, b);
    auto third = reg(context, inst, 39);
    const u32 mode = static_cast<u32>(extractBits(inst, 53, 2));
    if (mode == 1)
        third = builder.opNeg(third);
    else if (mode == 2)
        product = builder.opNeg(product);
    auto result = builder.opAdd(product, third);
    if (mode == 3)
        result = builder.opAdd(result, ir::Value::createConstantU(1));
    write(context, inst, result);
    if (conditional)
        context.builder.opEndIf();
}

void emitVmnmx(DecoderContext& context, instruction_t inst) {
    const u32 operation = static_cast<u32>(extractBits(inst, 51, 3));
    if (extractBits(inst, 47, 1) ||
        extractBits(inst, 48, 1) != extractBits(inst, 49, 1)) {
        LOG_NOT_IMPLEMENTED(
            ShaderDecompiler,
            "VMNMX CC/mixed signedness (secondary operation {})", operation);
        return;
    }
    const bool conditional = begin(context, inst);
    const auto [a, b] = operands(context, inst);
    const bool signed_source = extractBits(inst, 49, 1) != 0;
    const bool signed_destination = extractBits(inst, 54, 1) != 0;
    auto result =
        minmax(context, a, b, extractBits(inst, 56, 1) != 0, signed_source);
    if (extractBits(inst, 55, 1)) {
        // Clamp the primary result to the destination's integer domain before
        // the secondary merge/accumulate/min/max operation.
        if (signed_destination && !signed_source)
            result =
                minmax(context, result, ir::Value::createConstantU(0x7fffffffu),
                       false, false);
        else if (!signed_destination && signed_source)
            result = minmax(context, result, ir::Value::createConstantU(0),
                            true, true);
    }
    const auto third = reg(context, inst, 39);
    if (operation < 4) {
        const u32 bits = operation < 2 ? 16 : 8;
        const u32 shift = operation == 0 || operation == 3 ? 16 : 0;
        const u32 mask = ((1u << bits) - 1) << shift;
        result = context.builder.opBitwiseOr(
            context.builder.opBitwiseAnd(third,
                                         ir::Value::createConstantU(~mask)),
            context.builder.opBitwiseAnd(
                context.builder.opShiftLeft(result,
                                            ir::Value::createConstantU(shift)),
                ir::Value::createConstantU(mask)));
    } else if (operation == 4) {
        result = context.builder.opAdd(result, third);
    } else if (operation == 5 || operation == 6) {
        result =
            minmax(context, result, third, operation == 6, signed_destination);
    }
    write(context, inst, result);
    if (conditional)
        context.builder.opEndIf();
}

void emitVsetp(DecoderContext& context, instruction_t inst) {
    const auto boolean_op = static_cast<BoolOp>(extractBits(inst, 45, 2));
    if (static_cast<u32>(boolean_op) == 3 ||
        extractBits(inst, 48, 1) != extractBits(inst, 49, 1)) {
        LOG_NOT_IMPLEMENTED(ShaderDecompiler,
                            "VSETP boolean operation 3/mixed signedness");
        return;
    }
    const bool conditional = begin(context, inst);
    auto [a, b] = operands(context, inst);
    auto& builder = context.builder;
    if (extractBits(inst, 49, 1)) {
        a = builder.opBitCast(a, ir::ScalarType::I32);
        b = builder.opBitCast(b, ir::ScalarType::I32);
    }
    // The compare field is split around the boolean operation (45..46).
    // See envytools gm107.c tab50f0_0 mask 0x0000980000000000.
    const u32 compare = static_cast<u32>(extractBits(inst, 43, 2)) |
                        (static_cast<u32>(extractBits(inst, 47, 1)) << 2);
    auto result = ir::Value::createConstantB(false);
    switch (compare) {
    case 0:
        break;
    case 1:
        result = builder.opCompareLess(a, b);
        break;
    case 2:
        result = builder.opCompareEqual(a, b);
        break;
    case 3:
        result = builder.opCompareLessOrEqual(a, b);
        break;
    case 4:
        result = builder.opCompareGreater(a, b);
        break;
    case 5:
        result = builder.opCompareNotEqual(a, b);
        break;
    case 6:
        result = builder.opCompareGreaterOrEqual(a, b);
        break;
    case 7:
        result = ir::Value::createConstantB(true);
        break;
    }
    // Snapshot before either destination is written: source and destinations
    // may alias, including both destinations referring to the same predicate.
    const auto input_pred =
        builder.opCopy(notIf(builder,
                             ir::Value::createPredicate(
                                 static_cast<pred_t>(extractBits(inst, 39, 3))),
                             extractBits(inst, 42, 1) != 0));
    const auto result_a = getLogical(context, boolean_op, result, input_pred);
    const auto result_b =
        getLogical(context, boolean_op, builder.opNot(result), input_pred);
    const auto dst_a = static_cast<pred_t>(extractBits(inst, 3, 3));
    const auto dst_b = static_cast<pred_t>(extractBits(inst, 0, 3));
    if (dst_a != PT)
        builder.opCopy(ir::Value::createPredicate(dst_a), result_a);
    if (dst_b != PT)
        builder.opCopy(ir::Value::createPredicate(dst_b), result_b);
    if (conditional)
        builder.opEndIf();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
