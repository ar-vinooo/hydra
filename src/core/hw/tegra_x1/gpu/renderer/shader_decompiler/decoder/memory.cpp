#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/memory.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

// TODO: p, todo
void emitLoadAttribute(DecoderContext& context, pred_t pred, bool pred_inv,
                       bool is_input, LsSize size, reg_t dst, reg_t src,
                       u32 src_offset) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    u32 count;
    switch (size) {
    case LsSize::_32:
        count = 1;
        break;
    case LsSize::_64:
        count = 2;
        break;
    case LsSize::_96:
        count = 3;
        break;
    case LsSize::_128:
        count = 4;
        break;
    default:
        unreachable();
    }

    std::vector<ir::Value> values;
    values.reserve(count);
    for (u32 i = 0; i < count; i++) {
        values.push_back(context.builder.opCopy(ir::Value::createAttrMemory(
            AMem(src, src_offset + i * sizeof(u32), is_input))));
    }

    for (u32 i = 0; i < count; i++) {
        context.builder.opCopy(
            ir::Value::createRegister(dst + static_cast<u8>(i)), values[i]);
    }

    if (conditional)
        context.builder.opEndIf();
}

// TODO: p, todo
void emitStoreAttribute(DecoderContext& context, pred_t pred, bool pred_inv,
                        LsSize size, reg_t dst, u32 dst_offset, reg_t src) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    u32 count;
    switch (size) {
    case LsSize::_32:
        count = 1;
        break;
    case LsSize::_64:
        count = 2;
        break;
    case LsSize::_96:
        count = 3;
        break;
    case LsSize::_128:
        count = 4;
        break;
    default:
        unreachable();
    }

    for (u32 i = 0; i < count; i++) {
        context.builder.opCopy(
            ir::Value::createAttrMemory(
                AMem(dst, dst_offset + i * sizeof(u32), false)),
            ir::Value::createRegister(src + static_cast<u8>(i)));
    }

    if (conditional)
        context.builder.opEndIf();
}

void emitLoadConstant(DecoderContext& context, pred_t pred, bool pred_inv,
                      LsSize2 size, AddressMode address_mode, reg_t dst,
                      u32 cbuf_slot, reg_t src, u32 cbuf_offset) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    u32 count;
    switch (size) {
    case LsSize2::U8:
    case LsSize2::S8:
    case LsSize2::U16:
    case LsSize2::S16:
    case LsSize2::B32:
        count = 1;
        break;
    case LsSize2::B64:
        count = 2;
        break;
    case LsSize2::B128:
        count = 4;
        break;
    default:
        unreachable();
    }

    // TODO: slot addressing (mode 0 is the default and already supported)
    if (static_cast<u32>(address_mode) != 0)
        LOG_NOT_IMPLEMENTED(ShaderDecompiler,
                            "LDC address mode {} (slot: {}, reg: r{}, offset: "
                            "{:#x})",
                            static_cast<u32>(address_mode), cbuf_slot, src,
                            cbuf_offset);

    // Register indexed offsets are signed
    if (src != RZ)
        cbuf_offset = static_cast<u32>(signExtend<i32, 16>(
            static_cast<i32>(cbuf_offset & 0xffff)));

    // Small integers are extracted from the word containing them
    if (size == LsSize2::U8 || size == LsSize2::S8 || size == LsSize2::U16 ||
        size == LsSize2::S16) {
        auto& b = context.builder;
        const bool is_byte = size == LsSize2::U8 || size == LsSize2::S8;
        const bool is_signed = size == LsSize2::S8 || size == LsSize2::S16;
        const auto word = b.opCopy(
            ir::Value::createConstMemory(CMem(cbuf_slot, src, cbuf_offset)));
        const auto address =
            src == RZ ? ir::Value::createConstantU(cbuf_offset)
                      : b.opAdd(b.opCopy(ir::Value::createRegister(src)),
                                ir::Value::createConstantU(cbuf_offset));
        const auto shift = b.opShiftLeft(
            b.opBitwiseAnd(address, ir::Value::createConstantU(is_byte ? 3 : 2)),
            ir::Value::createConstantU(3));
        const u32 bits = is_byte ? 8 : 16;
        // Move the value to the top, then shift it down logically or
        // arithmetically
        const auto top = b.opShiftLeft(
            b.opShiftRight(word, shift), ir::Value::createConstantU(32 - bits));
        const auto value =
            is_signed
                ? b.opBitCast(b.opShiftRight(b.opBitCast(top, ir::ScalarType::I32),
                                             ir::Value::createConstantU(32 - bits)),
                              ir::ScalarType::U32)
                : b.opShiftRight(top, ir::Value::createConstantU(32 - bits));
        b.opCopy(ir::Value::createRegister(dst), value);
        if (conditional)
            b.opEndIf();
        return;
    }

    std::vector<ir::Value> values;
    values.reserve(count);
    for (u32 i = 0; i < count; i++) {
        values.push_back(context.builder.opCopy(ir::Value::createConstMemory(
            CMem(cbuf_slot, src, cbuf_offset + i * sizeof(u32)))));
    }

    for (u32 i = 0; i < count; i++) {
        context.builder.opCopy(
            ir::Value::createRegister(dst + static_cast<u8>(i)), values[i]);
    }

    if (conditional)
        context.builder.opEndIf();
}

// TODO: cache_op, extended
void emitLoadGlobal(DecoderContext& context, pred_t pred, bool pred_inv,
                    LsSize3 size, reg_t dst, reg_t src, i32 offset) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    u32 count;
    switch (size) {
    case LsSize3::U8:
    case LsSize3::S8:
    case LsSize3::U16:
    case LsSize3::S16:
    case LsSize3::B32:
        count = 1;
        break;
    case LsSize3::B64:
        count = 2;
        break;
    case LsSize3::B128:
    case LsSize3::UB128:
        count = 4;
        break;
    default:
        unreachable();
    }

    // TODO: handle small ints
    if (size == LsSize3::U8 || size == LsSize3::S8 || size == LsSize3::U16 ||
        size == LsSize3::S16)
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Small integer loading");

    LOG_FUNC_WITH_ARGS_NOT_IMPLEMENTED(
        ShaderDecompiler, "size: {}, src: {}, offset: {}", size, src, offset);

    std::vector<ir::Value> values;
    values.reserve(count);
    for (u32 i = 0; i < count; i++) {
        // TODO: global memory
        values.push_back(ir::Value::createConstantU(0));
    }

    for (u32 i = 0; i < count; i++) {
        context.builder.opCopy(
            ir::Value::createRegister(dst + static_cast<u8>(i)), values[i]);
    }

    if (conditional)
        context.builder.opEndIf();
}

// TODO: msi
// TODO: what is src_c for?
void emitInterpolateAttribute(DecoderContext& context, pred_t pred,
                              bool pred_inv, IpaOp op, bool saturate, reg_t dst,
                              reg_t src_a_r, u32 src_a_imm, bool indexed,
                              reg_t src_b, reg_t src_c) {
    (void)src_c;

    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    // Integer system values (front facing, primitive ID, layer) are copied
    // as raw bits
    const bool is_generic =
        src_a_imm >= SV_USER_IN_OUT_BASE && src_a_imm < SV_USER_IN_OUT_END;
    const bool is_integer =
        !indexed && (src_a_imm == SV_FRONT_FACING_BASE ||
                     src_a_imm == SV_PRIMITIVE_ID_BASE ||
                     src_a_imm == SV_LAYER_BASE ||
                     src_a_imm == SV_VIEWPORT_INDEX_BASE);
    if (is_integer) {
        context.builder.opCopy(
            ir::Value::createRegister(dst),
            ir::Value::createAttrMemory(AMem(RZ, src_a_imm, true)));
        if (conditional)
            context.builder.opEndIf();
        return;
    }

    auto res = ir::Value::createAttrMemory(
        AMem(indexed ? src_a_r : RZ, !indexed ? src_a_imm : 0, true),
        ir::ScalarType::F32);

    // Perspective multiply
    // TODO: why force perspective multiply when indexing?
    if (indexed ||
        (is_generic &&
         context.decomp_context.frag.pixel_imaps[(src_a_imm - 0x80) >> 0x4]
                 .getFirstUsedType() == PixelImapType::Perspective)) {
        res = context.builder.opMultiply(
            res, ir::Value::createAttrMemory(AMem(RZ, 0x7c, true),
                                             ir::ScalarType::F32));
    }

    // Op
    // TODO: what about other?
    if (op == IpaOp::Multiply) {
        const auto src_b_v =
            ir::Value::createRegister(src_b, ir::ScalarType::F32);
        res = context.builder.opMultiply(res, src_b_v);
    }

    res = saturateIf(context.builder, res, saturate);
    context.builder.opCopy(ir::Value::createRegister(dst, ir::ScalarType::F32),
                           res);

    if (conditional)
        context.builder.opEndIf();
}

} // namespace

void emitLda(DecoderContext& context, InstLda inst) {
    emitLoadAttribute(context, inst.pred, inst.pred_inv, !inst.o, inst.size,
                      inst.dst, inst.src, inst.src_offset);
}

void emitSta(DecoderContext& context, InstSta inst) {
    emitStoreAttribute(context, inst.pred, inst.pred_inv, inst.size, inst.dst,
                       inst.dst_offset, inst.src);
}

void emitLdc(DecoderContext& context, InstLdc inst) {
    // TODO: sign extend cbuf_offset?
    emitLoadConstant(context, inst.pred, inst.pred_inv, inst.size,
                     inst.address_mode, inst.dst, inst.cbuf_slot, inst.src,
                     inst.cbuf_offset);
}

void emitLdg(DecoderContext& context, InstLdg inst) {
    emitLoadGlobal(context, inst.pred, inst.pred_inv, inst.size, inst.dst,
                   inst.src, signExtend<i32, 24>(static_cast<i32>(inst.imm24)));
}

void emitIpa(DecoderContext& context, InstIpa inst) {
    emitInterpolateAttribute(context, inst.pred, inst.pred_inv, inst.op,
                             inst.sat, inst.dst, inst.src_a_r, inst.src_a_imm10,
                             inst.idx, inst.src_b, inst.src_c);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
