#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/compute.hpp"

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/texture.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

pred_t getPred(instruction_t inst) {
    return static_cast<pred_t>(extractBits(inst, 16, 3));
}

bool getPredInv(instruction_t inst) { return extractBits(inst, 19, 1) != 0; }

reg_t getReg(instruction_t inst, u32 shift) {
    return static_cast<reg_t>(extractBits(inst, shift, 8));
}

// Register value at an offset from a base register, RZ reads as zero
ir::Value getRegOffset(reg_t base, u32 offset) {
    if (base == RZ)
        return ir::Value::createConstantU(0);
    return ir::Value::createRegister(static_cast<reg_t>(base + offset));
}

ir::Value getSrcBReg(instruction_t inst) {
    return ir::Value::createRegister(getReg(inst, 20));
}

ir::Value getSrcBCbuf(instruction_t inst) {
    return ir::Value::createConstMemory(
        CMem(static_cast<u32>(extractBits(inst, 34, 5)), RZ,
             static_cast<u64>(extractBits(inst, 20, 14)) * 4));
}

ir::Value getSrcBImm(instruction_t inst) {
    return ir::Value::createConstantU(
        getIntImm20(static_cast<u32>(extractBits(inst, 20, 19)),
                    static_cast<u32>(extractBits(inst, 56, 1)), true));
}

ir::Value addOffset(ir::Builder& builder, const ir::Value& addr, u32 offset) {
    if (offset == 0)
        return addr;
    return builder.opAdd(addr, ir::Value::createConstantU(offset));
}

// Shared memory

void emitSharedAccess(DecoderContext& context, instruction_t inst, bool store,
                      bool local = false) {
    auto& builder = context.builder;
    const auto conditional =
        handlePredCond(builder, getPred(inst), getPredInv(inst));

    const auto loadMem = [&](const ir::Value& a) {
        return local ? builder.opLoadLocal(a) : builder.opLoadShared(a);
    };
    const auto storeMem = [&](const ir::Value& a, const ir::Value& v) {
        if (local)
            builder.opStoreLocal(a, v);
        else
            builder.opStoreShared(a, v);
    };

    const auto data = getReg(inst, 0);
    const auto addr_reg = getReg(inst, 8);
    const auto offset =
        signExtend<i32, 24>(static_cast<i32>(extractBits(inst, 20, 24)));
    const auto size = static_cast<u32>(extractBits(inst, 48, 3));

    auto addr = ir::Value::createConstantU(std::bit_cast<u32>(offset));
    if (addr_reg != RZ)
        addr = builder.opAdd(ir::Value::createRegister(addr_reg), addr);

    if (size < 4) {
        // 8 and 16 bit accesses
        const u32 bits = (size < 2 ? 8 : 16);
        const bool is_signed = (size & 1) != 0;
        const auto word = loadMem(addr);
        const auto shift = builder.opMultiply(
            builder.opBitwiseAnd(addr, ir::Value::createConstantU(3)),
            ir::Value::createConstantU(8));
        if (store) {
            const auto mask =
                builder.opShiftLeft(ir::Value::createConstantU((1u << bits) - 1),
                                    shift);
            const auto value = builder.opBitwiseAnd(
                builder.opShiftLeft(getRegOffset(data, 0), shift), mask);
            const auto kept =
                builder.opBitwiseAnd(word, builder.opBitwiseNot(mask));
            storeMem(addr, builder.opBitwiseOr(kept, value));
        } else {
            auto res = ir::Value::createUndefined();
            if (is_signed)
                res = builder.opBitCast(
                    builder.opBitfieldExtract(
                        builder.opBitCast(word, ir::ScalarType::I32), shift,
                        ir::Value::createConstantU(bits)),
                    ir::ScalarType::U32);
            else
                res = builder.opBitfieldExtract(
                    word, shift, ir::Value::createConstantU(bits));
            builder.opCopy(ir::Value::createRegister(data), res);
        }
    } else {
        const u32 count = (size == 4 ? 1 : size == 5 ? 2 : 4);
        if (store) {
            for (u32 i = 0; i < count; i++)
                storeMem(addOffset(builder, addr, i * 4),
                                      getRegOffset(data, i));
        } else {
            std::vector<ir::Value> values;
            for (u32 i = 0; i < count; i++)
                values.push_back(
                    loadMem(addOffset(builder, addr, i * 4)));
            for (u32 i = 0; i < count; i++)
                builder.opCopy(ir::Value::createRegister(
                                   static_cast<reg_t>(data + i)),
                               values[i]);
        }
    }

    if (conditional)
        builder.opEndIf();
}

// Surfaces

TextureType getSurfaceType(u32 type) {
    switch (type) {
    case 0:
        return TextureType::_1D;
    case 1:
        return TextureType::Buffer;
    case 2:
        return TextureType::_1DArray;
    case 3:
        return TextureType::_2D;
    case 4:
        return TextureType::_2DArray;
    case 5:
        return TextureType::_3D;
    default:
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Surface type {}", type);
        return TextureType::_2D;
    }
}

ir::Value getSurfaceCoords(ir::Builder& builder, reg_t reg, TextureType type) {
    u32 count = 1;
    switch (type) {
    case TextureType::_1DArray:
    case TextureType::_2D:
        count = 2;
        break;
    case TextureType::_2DArray:
    case TextureType::_3D:
        count = 3;
        break;
    default:
        break;
    }

    if (count == 1)
        return getRegOffset(reg, 0);

    std::vector<ir::Value> coords;
    for (u32 i = 0; i < count; i++)
        coords.push_back(getRegOffset(reg, i));
    return builder.opVectorConstruct(ir::ScalarType::U32, coords);
}

ir::Value applyLut(ir::Builder& builder, const ir::Value& a, const ir::Value& b,
                   const ir::Value& c, u32 lut) {
    lut &= 0xff;
    if (lut == 0x00)
        return ir::Value::createConstantU(0);
    if (lut == 0xff)
        return ir::Value::createConstantU(0xffffffff);
    if (lut == 0xf0)
        return builder.opCopy(a);
    if (lut == 0xcc)
        return builder.opCopy(b);
    if (lut == 0xaa)
        return builder.opCopy(c);

    // Sum of minterms, bit i of the LUT is set for a = i & 4, b = i & 2,
    // c = i & 1
    const std::array<ir::Value, 2> as = {builder.opBitwiseNot(a), a};
    const std::array<ir::Value, 2> bs = {builder.opBitwiseNot(b), b};
    const std::array<ir::Value, 2> cs = {builder.opBitwiseNot(c), c};
    std::optional<ir::Value> res;
    for (u32 i = 0; i < 8; i++) {
        if ((lut & (1u << i)) == 0)
            continue;
        const auto term = builder.opBitwiseAnd(
            builder.opBitwiseAnd(as[(i >> 2) & 1], bs[(i >> 1) & 1]),
            cs[i & 1]);
        res = res ? builder.opBitwiseOr(*res, term) : term;
    }
    return *res;
}

void emitLop3(DecoderContext& context, instruction_t inst, ir::Value src_b,
              u32 lut, bool has_pred) {
    auto& builder = context.builder;
    const auto conditional =
        handlePredCond(builder, getPred(inst), getPredInv(inst));

    const auto dst = getReg(inst, 0);
    const auto res =
        applyLut(builder, ir::Value::createRegister(getReg(inst, 8)), src_b,
                 ir::Value::createRegister(getReg(inst, 39)), lut);
    builder.opCopy(ir::Value::createRegister(dst), res);

    if (has_pred) {
        const auto dst_pred = static_cast<pred_t>(extractBits(inst, 48, 3));
        auto pred_res = ir::Value::createUndefined();
        switch (extractBits(inst, 36, 2)) {
        case 0:
            pred_res = ir::Value::createConstantB(false);
            break;
        case 1:
            pred_res = ir::Value::createConstantB(true);
            break;
        case 2:
            pred_res =
                builder.opCompareEqual(res, ir::Value::createConstantU(0));
            break;
        default:
            pred_res =
                builder.opCompareNotEqual(res, ir::Value::createConstantU(0));
            break;
        }
        if (dst_pred != PT)
            builder.opCopy(ir::Value::createPredicate(dst_pred), pred_res);
    }

    if (conditional)
        builder.opEndIf();
}

void emitPopc(DecoderContext& context, instruction_t inst, ir::Value src) {
    auto& builder = context.builder;
    const auto conditional =
        handlePredCond(builder, getPred(inst), getPredInv(inst));

    if (extractBits(inst, 40, 1) != 0)
        src = builder.opBitwiseNot(src);
    builder.opCopy(ir::Value::createRegister(getReg(inst, 0)),
                   builder.opPopCount(src));

    if (conditional)
        builder.opEndIf();
}

void emitFlo(DecoderContext& context, instruction_t inst, ir::Value src) {
    auto& builder = context.builder;
    const auto conditional =
        handlePredCond(builder, getPred(inst), getPredInv(inst));

    if (extractBits(inst, 40, 1) != 0)
        src = builder.opBitwiseNot(src);
    auto res = builder.opFindMsb(src, extractBits(inst, 48, 1) != 0);
    // Position relative to the most significant bit
    if (extractBits(inst, 41, 1) != 0) {
        const auto not_found = ir::Value::createConstantU(0xffffffff);
        res = builder.opSelect(
            builder.opCompareEqual(res, not_found), not_found,
            builder.opBitwiseXor(res, ir::Value::createConstantU(31)));
    }
    builder.opCopy(ir::Value::createRegister(getReg(inst, 0)), res);

    if (conditional)
        builder.opEndIf();
}

} // namespace

void emitCsetp(DecoderContext& context, instruction_t inst) {
    const auto test = extractBits(inst, 8, 5);
    if (test != 13 && test != 0 && test != 15) {
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "CSETP flow test {}", test);
        return;
    }
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, getPred(inst), getPredInv(inst));
    const auto condition = test == 13
        ? b.opBitwiseOr(ir::Value::createConditionFlag(1), b.opNot(ir::Value::createConditionFlag(0)))
        : ir::Value::createConstantB(test == 15);
    const auto other = notIf(b, ir::Value::createPredicate(static_cast<pred_t>(extractBits(inst, 39, 3))), extractBits(inst, 42, 1));
    const u32 bop = static_cast<u32>(extractBits(inst, 45, 2));
    const auto combine = [&](ir::Value x) {
        if (bop == 0) return b.opBitwiseAnd(x, other);
        if (bop == 1) return b.opBitwiseOr(x, other);
        return b.opBitwiseXor(x, other);
    };
    const auto result_a = b.opCopy(combine(condition));
    const auto result_b = b.opCopy(combine(b.opNot(condition)));
    const auto pa = static_cast<pred_t>(extractBits(inst, 3, 3));
    const auto pb = static_cast<pred_t>(extractBits(inst, 0, 3));
    if (pa != PT) b.opCopy(ir::Value::createPredicate(pa), result_a);
    if (pb != PT) b.opCopy(ir::Value::createPredicate(pb), result_b);
    if (conditional) b.opEndIf();
}

void emitR2p(DecoderContext& context, instruction_t inst, u32 variant) {
    const bool cc_mode = extractBits(inst, 40, 1);
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, getPred(inst), getPredInv(inst));
    const auto mask = variant == 0 ? getSrcBReg(inst) : variant == 1 ? getSrcBCbuf(inst) : getSrcBImm(inst);
    const auto source = ir::Value::createRegister(getReg(inst, 8));
    const u32 byte = static_cast<u32>(extractBits(inst, 41, 2)) * 8;
    // Snapshot source/mask before writing predicates, including the predicate
    // that guards this instruction itself.
    const auto bits = b.opCopy(source);
    const auto enabled = b.opCopy(mask);
    for (u32 i = 0; i < (cc_mode ? 4u : 7u); ++i) {
        const auto selected = b.opCompareNotEqual(
            b.opBitwiseAnd(enabled, ir::Value::createConstantU(1u << i)), ir::Value::createConstantU(0));
        const auto bit = b.opCompareNotEqual(
            b.opBitwiseAnd(bits, ir::Value::createConstantU(1u << (byte + i))), ir::Value::createConstantU(0));
        const auto pred = cc_mode ? ir::Value::createConditionFlag(i) : ir::Value::createPredicate(static_cast<pred_t>(i));
        b.opCopy(pred, b.opSelect(selected, bit, pred));
    }
    if (conditional) b.opEndIf();
}

void emitAtoms(DecoderContext& context, instruction_t inst) {
    const u32 size = static_cast<u32>(extractBits(inst, 28, 2));
    const u32 op = static_cast<u32>(extractBits(inst, 52, 4));
    if (size > 1 || op > 8) {
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "ATOMS size {} operation {}", size, op);
        return;
    }
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, getPred(inst), getPredInv(inst));
    const auto addr_reg = getReg(inst, 8);
    const u32 offset = static_cast<u32>(extractBits(inst, 30, 22)) << 2;
    auto addr = ir::Value::createConstantU(offset);
    if (addr_reg != RZ) {
        const auto signed_offset = signExtend<i32, 24>(static_cast<i32>(offset));
        addr = b.opAdd(ir::Value::createRegister(addr_reg),
                       ir::Value::createConstantU(std::bit_cast<u32>(signed_offset)));
    }
    const auto result = b.opAtomicShared(addr, ir::Value::createRegister(getReg(inst, 20)), op, size == 1);
    const auto dst = getReg(inst, 0);
    if (dst != RZ) b.opCopy(ir::Value::createRegister(dst), result);
    if (conditional) b.opEndIf();
}

void emitLds(DecoderContext& context, instruction_t inst) {
    emitSharedAccess(context, inst, false);
}

void emitSts(DecoderContext& context, instruction_t inst) {
    emitSharedAccess(context, inst, true);
}

void emitLdl(DecoderContext& context, instruction_t inst) {
    emitSharedAccess(context, inst, false, true);
}

void emitStl(DecoderContext& context, instruction_t inst) {
    emitSharedAccess(context, inst, true, true);
}

void emitBar(DecoderContext& context, instruction_t inst) {
    // TODO: modes other than BAR.SYNC 0
    const auto conditional =
        handlePredCond(context.builder, getPred(inst), getPredInv(inst));
    context.builder.opBarrier();
    if (conditional)
        context.builder.opEndIf();
}

void emitSust(DecoderContext& context, instruction_t inst) {
    auto& builder = context.builder;

    const bool is_bound = extractBits(inst, 51, 1) != 0;
    std::optional<u32> index;
    if (is_bound)
        index = static_cast<u32>(extractBits(inst, 36, 13));
    else
        index = trackBindlessResource(context, getReg(inst, 39));
    if (!index)
        return;

    const auto conditional =
        handlePredCond(builder, getPred(inst), getPredInv(inst));

    const auto type = getSurfaceType(static_cast<u32>(extractBits(inst, 33, 3)));
    const auto data = getReg(inst, 0);
    const auto coords = getSurfaceCoords(builder, getReg(inst, 8), type);

    std::array<ir::Value, 4> values = {
        ir::Value::createConstantU(0), ir::Value::createConstantU(0),
        ir::Value::createConstantU(0), ir::Value::createConstantU(0)};
    if (extractBits(inst, 52, 1) == 0) {
        // Formatted, the mask selects the components
        const auto mask = static_cast<u32>(extractBits(inst, 20, 4));
        u32 reg_offset = 0;
        for (u32 i = 0; i < 4; i++) {
            if ((mask & (1u << i)) != 0)
                values[i] = getRegOffset(data, reg_offset++);
        }
    } else {
        // TODO: convert raw data to the image format
        ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Sized surface store"));
        const auto size = static_cast<u32>(extractBits(inst, 20, 3));
        const u32 count = (size <= 4 ? 1 : size == 5 ? 2 : 4);
        for (u32 i = 0; i < count; i++)
            values[i] = getRegOffset(data, i);
    }

    builder.opImageStore(*index, type, coords, values);

    if (conditional)
        builder.opEndIf();
}

void emitSuatom(DecoderContext& context, instruction_t inst) {
    auto& builder = context.builder;

    const bool is_bindless = extractBits(inst, 54, 1) != 0;
    std::optional<u32> index;
    if (is_bindless)
        index = trackBindlessResource(context, getReg(inst, 39));
    else
        index = static_cast<u32>(extractBits(inst, 36, 13));
    if (!index)
        return;

    const auto conditional =
        handlePredCond(builder, getPred(inst), getPredInv(inst));

    // TODO: size (assumed to be 32 bit)
    const auto type = getSurfaceType(static_cast<u32>(extractBits(inst, 33, 3)));
    const auto op = static_cast<u32>(extractBits(inst, 29, 4));
    const auto coords = getSurfaceCoords(builder, getReg(inst, 8), type);
    const auto res = builder.opImageAtomic(*index, type, op, coords,
                                           getRegOffset(getReg(inst, 20), 0));
    builder.opCopy(ir::Value::createRegister(getReg(inst, 0)), res);

    if (conditional)
        builder.opEndIf();
}

void emitLop3R(DecoderContext& context, instruction_t inst) {
    emitLop3(context, inst, getSrcBReg(inst),
             static_cast<u32>(extractBits(inst, 28, 8)), true);
}

void emitLop3C(DecoderContext& context, instruction_t inst) {
    emitLop3(context, inst, getSrcBCbuf(inst),
             static_cast<u32>(extractBits(inst, 48, 8)), false);
}

void emitLop3I(DecoderContext& context, instruction_t inst) {
    emitLop3(context, inst, getSrcBImm(inst),
             static_cast<u32>(extractBits(inst, 48, 8)), false);
}

void emitPopcR(DecoderContext& context, instruction_t inst) {
    emitPopc(context, inst, getSrcBReg(inst));
}

void emitPopcC(DecoderContext& context, instruction_t inst) {
    emitPopc(context, inst, getSrcBCbuf(inst));
}

void emitPopcI(DecoderContext& context, instruction_t inst) {
    emitPopc(context, inst, getSrcBImm(inst));
}

void emitFloR(DecoderContext& context, instruction_t inst) {
    emitFlo(context, inst, getSrcBReg(inst));
}

void emitFloC(DecoderContext& context, instruction_t inst) {
    emitFlo(context, inst, getSrcBCbuf(inst));
}

void emitFloI(DecoderContext& context, instruction_t inst) {
    emitFlo(context, inst, getSrcBImm(inst));
}

void emitVote(DecoderContext& context, instruction_t inst) {
    auto& builder = context.builder;
    const auto conditional =
        handlePredCond(builder, getPred(inst), getPredInv(inst));

    const auto pred_a = static_cast<pred_t>(extractBits(inst, 39, 3));
    const auto pred_b = static_cast<pred_t>(extractBits(inst, 45, 3));
    const auto mode = static_cast<u32>(extractBits(inst, 48, 2));
    const auto value = notIf(builder, ir::Value::createPredicate(pred_a),
                             extractBits(inst, 42, 1) != 0);

    if (pred_b != PT)
        builder.opCopy(ir::Value::createPredicate(pred_b),
                       builder.opVote(value, mode));
    const auto dst = getReg(inst, 0);
    if (dst != RZ)
        builder.opCopy(ir::Value::createRegister(dst),
                       builder.opBallot(value));

    if (conditional)
        builder.opEndIf();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
