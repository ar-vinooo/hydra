#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/memory.hpp"

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/texture.hpp"

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

// Offset into the storage buffer an address [Ra.64 + imm] points into. The
// buffer is found by following the low half of the address back to the
// constant buffer word with its NVN descriptor (u64 address, u32 size)
struct StorageAccess {
    u32 binding;
    ir::Value offset;
    ir::Value size;
};

std::optional<StorageAccess> makeStorageAccess(DecoderContext& context, bool e,
                                               reg_t addr, i32 imm) {
    // 32-bit and absolute addresses have no descriptor
    if (!e || addr == RZ)
        return std::nullopt;
    const auto word = trackStorageBuffer(context, addr);
    if (!word) {
        LOG_WARN(ShaderDecompiler,
                 "Failed to track the storage buffer of global address {}",
                 addr);
        return std::nullopt;
    }

    auto& b = context.builder;
    auto low = b.opCopy(ir::Value::createRegister(addr));
    if (imm != 0)
        low = b.opAdd(low, ir::Value::createConstantU(std::bit_cast<u32>(imm)));
    const auto descriptor = [&](u32 byte_offset) {
        return b.opCopy(ir::Value::createConstMemory(
            CMem(word->slot, RZ, u64(word->word) * 4 + byte_offset)));
    };
    // The descriptor holds the base address, so what is left of the guest
    // address is the offset into the buffer
    return StorageAccess{word->slot << 16 | word->word,
                         b.opAdd(low, b.opNeg(descriptor(0))), descriptor(8)};
}

u32 getAccessWordCount(LsSize3 size) {
    switch (size) {
    case LsSize3::B64:
        return 2;
    case LsSize3::B128:
    case LsSize3::UB128:
        return 4;
    default:
        return 1;
    }
}

// Word offset of a component of a multi-word access
ir::Value offsetWord(DecoderContext& context, const ir::Value& offset,
                     u32 index) {
    if (index == 0)
        return offset;
    return context.builder.opAdd(offset, ir::Value::createConstantU(index * 4));
}

// TODO: cache_op
void emitLoadGlobal(DecoderContext& context, pred_t pred, bool pred_inv,
                    LsSize3 size, reg_t dst, reg_t src, i32 offset, bool e) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);
    const u32 count = getAccessWordCount(size);

    const auto access = makeStorageAccess(context, e, src, offset);
    if (!access)
        LOG_NOT_IMPLEMENTED(ShaderDecompiler,
                            "Global load without a storage buffer (size: {}, "
                            "src: {}, offset: {})",
                            size, src, offset);

    std::vector<ir::Value> values;
    values.reserve(count);
    for (u32 i = 0; i < count; i++) {
        values.push_back(
            access ? context.builder.opLoadStorage(
                         access->binding, offsetWord(context, access->offset, i),
                         access->size, static_cast<u32>(size) <= 4
                                           ? static_cast<u32>(size)
                                           : 4)
                   : ir::Value::createConstantU(0));
    }

    if (dst != RZ)
        for (u32 i = 0; i < count; i++)
            context.builder.opCopy(
                ir::Value::createRegister(dst + static_cast<u8>(i)), values[i]);

    if (conditional)
        context.builder.opEndIf();
}

void emitStoreGlobal(DecoderContext& context, pred_t pred, bool pred_inv,
                     LsSize3 size, reg_t src, reg_t addr, i32 offset, bool e) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);
    const u32 count = getAccessWordCount(size);

    const auto access = makeStorageAccess(context, e, addr, offset);
    if (!access) {
        LOG_NOT_IMPLEMENTED(ShaderDecompiler,
                            "Global store without a storage buffer (size: {}, "
                            "addr: {}, offset: {})",
                            size, addr, offset);
    } else {
        for (u32 i = 0; i < count; i++)
            context.builder.opStoreStorage(
                access->binding, offsetWord(context, access->offset, i),
                access->size,
                context.builder.opCopy(
                    ir::Value::createRegister(src + static_cast<u8>(i))),
                static_cast<u32>(size) <= 4 ? static_cast<u32>(size) : 4);
    }

    if (conditional)
        context.builder.opEndIf();
}

// ATOM and RED: Eden's AtomOp/AtomSize encodings
void emitAtomicGlobal(DecoderContext& context, pred_t pred, bool pred_inv,
                      reg_t dst, reg_t addr, reg_t operand, i32 offset, bool e,
                      u32 size, u32 op) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);
    auto& b = context.builder;

    // U32, S32 and F32 additions
    const bool is_float = size == 3;
    const bool supported = (size <= 1 && op <= 8) || (is_float && op == 0);
    const auto access = supported ? makeStorageAccess(context, e, addr, offset)
                                  : std::nullopt;
    ir::Value result = ir::Value::createConstantU(0);
    if (access) {
        const auto value = b.opCopy(ir::Value::createRegister(operand));
        if ((op == 3 || op == 4) && !is_float) {
            // Eden: increment and decrement leave 32-bit memory unchanged
            result = b.opLoadStorage(access->binding, access->offset,
                                     access->size, 4);
        } else {
            result = b.opAtomicStorage(access->binding, access->offset,
                                       access->size, value,
                                       is_float ? 9 : op, size == 1);
        }
    } else {
        LOG_NOT_IMPLEMENTED(ShaderDecompiler,
                            "Global atomic (size: {}, op: {}, addr: {}, "
                            "offset: {})",
                            size, op, addr, offset);
    }
    if (dst != RZ)
        b.opCopy(ir::Value::createRegister(dst), result);

    if (conditional)
        b.opEndIf();
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
                   inst.src, signExtend<i32, 24>(static_cast<i32>(inst.imm24)),
                   inst.e);
}

void emitStg(DecoderContext& context, InstStg inst) {
    emitStoreGlobal(context, inst.pred, inst.pred_inv, inst.size, inst.dst,
                    inst.src, signExtend<i32, 24>(static_cast<i32>(inst.imm24)),
                    inst.e);
}

void emitAtom(DecoderContext& context, InstAtom inst) {
    emitAtomicGlobal(context, inst.pred, inst.pred_inv, inst.dst, inst.src,
                     inst.operand,
                     signExtend<i32, 20>(static_cast<i32>(inst.imm20)), inst.e,
                     inst.size, inst.op);
}

void emitAtomCas(DecoderContext& context, InstAtomCas inst) {
    const auto conditional =
        handlePredCond(context.builder, inst.pred, inst.pred_inv);
    auto& b = context.builder;
    const reg_t dst = inst.dst;
    const reg_t addr = inst.src;
    const reg_t operand = inst.operand;
    const bool is_64 = inst.is_64;
    const auto access =
        is_64 ? std::nullopt
              : makeStorageAccess(
                    context, inst.e, addr,
                    signExtend<i32, 20>(static_cast<i32>(inst.imm20)));
    ir::Value result = ir::Value::createConstantU(0);
    if (access) {
        const auto compare = b.opCopy(ir::Value::createRegister(operand));
        const auto value = b.opCopy(
            ir::Value::createRegister(operand + static_cast<u8>(1)));
        result = b.opAtomicStorageCas(access->binding, access->offset,
                                      access->size, compare, value);
    } else {
        LOG_NOT_IMPLEMENTED(ShaderDecompiler,
                            "Global ATOM.CAS (64-bit: {}, addr: {})", is_64,
                            addr);
    }
    if (dst != RZ)
        b.opCopy(ir::Value::createRegister(dst), result);

    if (conditional)
        b.opEndIf();
}

void emitRed(DecoderContext& context, InstRed inst) {
    emitAtomicGlobal(context, inst.pred, inst.pred_inv, RZ, inst.src,
                     inst.operand,
                     signExtend<i32, 20>(static_cast<i32>(inst.imm20)), inst.e,
                     inst.size, inst.op);
}

void emitIpa(DecoderContext& context, InstIpa inst) {
    emitInterpolateAttribute(context, inst.pred, inst.pred_inv, inst.op,
                             inst.sat, inst.dst, inst.src_a_r, inst.src_a_imm10,
                             inst.idx, inst.src_b, inst.src_c);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
