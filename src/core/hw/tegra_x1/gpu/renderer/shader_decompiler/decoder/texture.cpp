#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/texture.hpp"

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/memory.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

// How far back to look for the instruction that defines a register
constexpr u32 MAX_HANDLE_TRACK_DISTANCE = 1024;
constexpr u32 MAX_HANDLE_TRACK_DEPTH = 4;

bool isControlFlow(instruction_t inst) {
    // BRA, BRX, JMP, JMX, SSY, PBK, PEXIT, CAL, JCAL, RET, BRK, CONT, SYNC,
    // EXIT, KIL, ...
    return (inst >> 56) == 0xe2 || (inst >> 56) == 0xe3;
}

bool isStore(instruction_t inst) {
    // ST, STG, STL, STS, RED, ATOM(S), SUST: bits 0-7 are a source
    return (inst & 0xe000000000000000) == 0xa000000000000000 ||
           (inst & 0xfff8000000000000) == 0xeed8000000000000 ||
           (inst & 0xfff8000000000000) == 0xef50000000000000 ||
           (inst & 0xfff8000000000000) == 0xef58000000000000 ||
           (inst & 0xfff0000000000000) == 0xebf0000000000000 ||
           (inst & 0xfe00000000000000) == 0xec00000000000000 ||
           (inst & 0xfff0000000000000) == 0xeb20000000000000;
}

// Predicate-only instructions encode predicate destinations in bits 0..7.
// They must not shadow a texture handle held in a general-purpose register.
bool isPredicateOnly(instruction_t inst) {
    const auto op = inst & 0xfff0000000000000ull;
    if (op == 0x5b60000000000000ull || op == 0x4b60000000000000ull ||
        (inst & 0xfef0000000000000ull) == 0x3660000000000000ull ||
        op == 0x5bb0000000000000ull || op == 0x4bb0000000000000ull ||
        (inst & 0xfef0000000000000ull) == 0x36b0000000000000ull ||
        op == 0x5b80000000000000ull || op == 0x4b80000000000000ull ||
        (inst & 0xfef0000000000000ull) == 0x3680000000000000ull)
        return true;
    const auto op8 = inst & 0xfff8000000000000ull;
    return op8 == 0x5d20000000000000ull ||
           (inst & 0xfe00000000000000ull) == 0x7e00000000000000ull ||
           op8 == 0x50a0000000000000ull || op8 == 0x5090000000000000ull ||
           op8 == 0x50b0000000000000ull || op8 == 0x4cf0000000000000ull ||
           op8 == 0x5cf0000000000000ull ||
           (inst & 0xfef8000000000000ull) == 0x38f0000000000000ull;
}

bool isLdc(instruction_t inst) {
    return (inst & 0xfff8000000000000) == 0xef90000000000000;
}

struct CbufWord {
    u32 slot;
    u32 word;
};

struct TrackedHandle {
    CbufWord texture;
    std::optional<CbufWord> sampler;
};

// Finds the closest instruction before pc that writes reg
std::optional<std::pair<u64, instruction_t>>
findDefinition(ztd::io::IStream& stream, reg_t reg, u64 pc) {
    for (u64 distance = 1; distance <= MAX_HANDLE_TRACK_DISTANCE && distance <= pc;
         distance++) {
        const u64 def_pc = pc - distance;
        if ((def_pc % 4) == 0) // Sched
            continue;

        stream.seekTo(def_pc * sizeof(instruction_t));
        const auto inst = stream.read<instruction_t>();
        const auto dst = static_cast<reg_t>(inst & 0xff);
        if (isLdc(inst)) {
            const auto ldc = std::bit_cast<InstLdc>(inst);
            const u32 count = ldc.size.get() == LsSize2::B128  ? 4u
                              : ldc.size.get() == LsSize2::B64 ? 2u
                                                               : 1u;
            if (static_cast<u32>(reg) >= static_cast<u32>(dst) &&
                static_cast<u32>(reg) - static_cast<u32>(dst) < count)
                return std::make_pair(def_pc, inst);
            continue;
        }
        if (dst == reg && !isControlFlow(inst) && !isStore(inst) &&
            !isPredicateOnly(inst))
            return std::make_pair(def_pc, inst);
    }

    return std::nullopt;
}

std::optional<TrackedHandle> traceHandle(ztd::io::IStream& stream, reg_t reg,
                                         u64 pc, u32 depth,
                                         instruction_t& out_failed_def) {
    if (reg == RZ || depth > MAX_HANDLE_TRACK_DEPTH)
        return std::nullopt;

    const auto def = findDefinition(stream, reg, pc);
    if (!def)
        return std::nullopt;
    const auto [def_pc, inst] = *def;

    // LDC Rd, c[slot][RZ + offset]
    if (isLdc(inst)) {
        const auto ldc = std::bit_cast<InstLdc>(inst);
        if (ldc.src.get() != RZ || ldc.address_mode.get() != AddressMode{}) {
            out_failed_def = inst;
            return std::nullopt;
        }
        const auto dst = static_cast<reg_t>(inst & 0xff);
        const u32 offset = ldc.cbuf_offset.get() +
                           (static_cast<u32>(reg) - static_cast<u32>(dst)) * 4u;
        return TrackedHandle{{ldc.cbuf_slot.get(), offset / 4}, std::nullopt};
    }

    // MOV Rd, c[slot][offset]
    if ((inst & 0xfff8000000000000) == 0x4c98000000000000)
        return TrackedHandle{{static_cast<u32>(extractBits(inst, 34, 5)),
                              static_cast<u32>(extractBits(inst, 20, 14))},
                             std::nullopt};

    // MOV Rd, Rb
    if ((inst & 0xfff8000000000000) == 0x5c98000000000000)
        return traceHandle(stream, static_cast<reg_t>(extractBits(inst, 20, 8)),
                           def_pc, depth + 1, out_failed_def);

    // LOP.OR Rd, Ra, c[slot][offset]: texture handle combined with a separate
    // sampler handle
    if ((inst & 0xffc0000000000000) == 0x4c40000000000000 &&
        extractBits(inst, 41, 2) == 1 && extractBits(inst, 39, 2) == 0) {
        auto texture =
            traceHandle(stream, static_cast<reg_t>(extractBits(inst, 8, 8)),
                        def_pc, depth + 1, out_failed_def);
        if (!texture || texture->sampler)
            return std::nullopt;
        texture->sampler = CbufWord{static_cast<u32>(extractBits(inst, 34, 5)),
                                    static_cast<u32>(extractBits(inst, 20, 14))};
        return texture;
    }

    // LOP.OR Rd, Ra, Rb
    if ((inst & 0xfff8000000000000) == 0x5c40000000000000 &&
        extractBits(inst, 41, 2) == 1 && extractBits(inst, 39, 2) == 0) {
        auto a = traceHandle(stream, static_cast<reg_t>(extractBits(inst, 8, 8)),
                             def_pc, depth + 1, out_failed_def);
        auto b =
            traceHandle(stream, static_cast<reg_t>(extractBits(inst, 20, 8)),
                        def_pc, depth + 1, out_failed_def);
        if (!a || !b || a->sampler || b->sampler)
            return std::nullopt;
        return TrackedHandle{a->texture, b->texture};
    }

    out_failed_def = inst;
    return std::nullopt;
}

// Bindless instructions take the texture handle from a register. Shaders load
// it from a constant buffer just before use, so trace the register back to
// those loads and bind the constant buffer words like a regular texture.
std::optional<u32> trackBindlessHandle(DecoderContext& context, reg_t reg) {
    auto& stream = *context.code_stream;
    const u64 saved_seek = stream.getSeek();
    const u64 crnt_pc = saved_seek / sizeof(instruction_t) - 1;

    instruction_t failed_def = 0x0;
    const auto handle = traceHandle(stream, reg, crnt_pc, 0, failed_def);
    stream.seekTo(saved_seek);

    if (handle && (!handle->sampler || handle->sampler->slot == handle->texture.slot))
        return makeTrackedTextureIndex(
            handle->texture.slot, handle->texture.word,
            handle->sampler ? handle->sampler->word
                            : TRACKED_TEXTURE_NO_SAMPLER);

    LOG_WARN(ShaderDecompiler,
             "Failed to trace bindless texture handle in {} (pc: {:#x}, "
             "defined by: {:#018x})",
             reg, crnt_pc, failed_def);
    return std::nullopt;
}

// TODO: nodep
void emitTextureQuery(DecoderContext& context, pred_t pred, bool pred_inv,
                      TextureQuery query, reg_t dst, u8 write_mask, reg_t src,
                      u32 cbuf_index) {
    // TODO: src
    (void)src;

    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    // NOLINTNEXTLINE(readability-trivial-switch)
    switch (query) {
    case TextureQuery::Dimensions:
        for (u32 i = 0, mask = write_mask; mask != 0x0; i++, mask >>= 1) {
            if ((mask & 1) != 0u) {
                const auto res =
                    context.builder.opTextureQueryDimension(cbuf_index, i);
                context.builder.opCopy(
                    ir::Value::createRegister(dst + static_cast<u8>(i)), res);
            }
        }
        break;
    default:
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Texture query {}", query);
        break;
    }

    if (conditional)
        context.builder.opEndIf();
}

u8 toTexComponent(TextureComponent component) {
    switch (component) {
    case TextureComponent::R:
        return 0;
    case TextureComponent::G:
        return 1;
    case TextureComponent::B:
        return 2;
    case TextureComponent::A:
        return 3;
    }
}

void copyTextureResult(ir::Builder& builder, std::array<reg_t, 4> dsts,
                       ir::Value res, u8 component_mask) {
    u32 output_index = 0;
    for (u8 i = 0; i < 4; i++) {
        if ((component_mask & (1 << i)) == 0x0)
            continue;

        builder.opCopy(ir::Value::createRegister(dsts[output_index++],
                                                 ir::ScalarType::F32),
                       builder.opVectorExtract(res, i));
    }
}

void copyTextureResult(ir::Builder& builder, reg_t dst, ir::Value res,
                       u8 component_mask) {
    copyTextureResult(builder, {dst + 0, dst + 1, dst + 2, dst + 3}, res,
                      component_mask);
}

constexpr std::array<std::array<u8, 8>, 2> COMPONENT_MASK_LUT = {{
    {0b0001, 0b0010, 0b0100, 0b1000, 0b0011, 0b1001, 0b1010, 0b1100},
    {0b0111, 0b1011, 0b1101, 0b1110, 0b1111, 0b0000, 0b0000, 0b0000},
}};

// TODO: nodep
void emitTextureSample(DecoderContext& context, pred_t pred, bool pred_inv,
                       bool int_coords, TextureSampleTarget target, reg_t dst0,
                       reg_t dst1, u8 write_mask, reg_t src_a, reg_t src_b,
                       u32 cbuf_index) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

#define RA() ir::Value::createRegister(src_a++, ir::ScalarType::F32)
#define RB() ir::Value::createRegister(src_b++, ir::ScalarType::F32)

    TextureType type;
    TextureSampleFlags flags =
        (int_coords ? TextureSampleFlags::IntCoords : TextureSampleFlags::None);
    ir::Value array_index = ir::Value::createUndefined();
    std::vector<ir::Value> coords;
    ir::Value cmp_value = ir::Value::createUndefined();
    ir::Value lod = ir::Value::createUndefined();
    switch (target) {
    case TextureSampleTarget::_1DLodZero:
        type = TextureType::_1D;
        flags |= TextureSampleFlags::Lod;
        coords.push_back(RA());
        lod = ir::Value::createConstantF(0.0f);
        break;
    case TextureSampleTarget::_2D:
        type = TextureType::_2D;
        coords.push_back(RA());
        coords.push_back(RB());
        break;
    case TextureSampleTarget::_2DLodZero:
        type = TextureType::_2D;
        flags |= TextureSampleFlags::Lod;
        coords.push_back(RA());
        coords.push_back(RB());
        lod = ir::Value::createConstantF(0.0f);
        break;
    case TextureSampleTarget::_2DLodLevel:
        type = TextureType::_2D;
        flags |= TextureSampleFlags::Lod;
        coords.push_back(RA());
        coords.push_back(RA());
        lod = RB();
        break;
    case TextureSampleTarget::_2DDepthCompare:
        type = TextureType::_2D;
        flags |= TextureSampleFlags::DepthCompare;
        coords.push_back(RA());
        coords.push_back(RA());
        cmp_value = RB();
        break;
    case TextureSampleTarget::_2DLodLevelDepthCompare:
        type = TextureType::_2D;
        flags |= TextureSampleFlags::DepthCompare | TextureSampleFlags::Lod;
        coords.push_back(RA());
        coords.push_back(RA());
        cmp_value = RB();
        lod = RB();
        break;
    case TextureSampleTarget::_2DLodZeroDepthCompare:
        type = TextureType::_2D;
        flags |= TextureSampleFlags::DepthCompare | TextureSampleFlags::Lod;
        coords.push_back(RA());
        coords.push_back(RA());
        cmp_value = RB();
        lod = ir::Value::createConstantF(0.0f);
        break;
    case TextureSampleTarget::_2DArray:
        type = TextureType::_2DArray;
        array_index = RA();
        coords.push_back(RA());
        coords.push_back(RB());
        break;
    case TextureSampleTarget::_2DArrayLodZero:
        type = TextureType::_2DArray;
        flags |= TextureSampleFlags::Lod;
        array_index = RA();
        coords.push_back(RA());
        coords.push_back(RB());
        lod = ir::Value::createConstantF(0.0f);
        break;
    case TextureSampleTarget::_2DArrayLodZeroDepthCompare:
        type = TextureType::_2DArray;
        flags |= TextureSampleFlags::DepthCompare | TextureSampleFlags::Lod;
        array_index = RA();
        coords.push_back(RA());
        coords.push_back(RB());
        cmp_value = RB();
        lod = ir::Value::createConstantF(0.0f);
        break;
    case TextureSampleTarget::_3D:
        type = TextureType::_3D;
        coords.push_back(RA());
        coords.push_back(RA());
        coords.push_back(RB());
        break;
    case TextureSampleTarget::_3DLodZero:
        type = TextureType::_3D;
        flags |= TextureSampleFlags::Lod;
        coords.push_back(RA());
        coords.push_back(RA());
        coords.push_back(RB());
        lod = ir::Value::createConstantF(0.0f);
        break;
    case TextureSampleTarget::Cube:
        type = TextureType::Cube;
        coords.push_back(RA());
        coords.push_back(RA());
        coords.push_back(RB());
        break;
    case TextureSampleTarget::CubeLodLevel:
        type = TextureType::Cube;
        flags |= TextureSampleFlags::Lod;
        coords.push_back(RA());
        coords.push_back(RA());
        coords.push_back(RB());
        lod = RB();
        break;
    }

    const auto coords_v =
        context.builder.opVectorConstruct(ir::ScalarType::F32, coords);
    ir::Value res = context.builder.opTextureSample(
        cbuf_index, type, flags, array_index, coords_v, cmp_value, lod);

    const auto component_mask =
        COMPONENT_MASK_LUT[dst1 == RZ ? 0 : 1][write_mask];
    ASSERT_DEBUG(component_mask != 0, ShaderDecompiler,
                 "Invalid component mask");
    copyTextureResult(context.builder, {dst0 + 0, dst0 + 1, dst1 + 0, dst1 + 1},
                      res, component_mask);

    if (conditional)
        context.builder.opEndIf();
}

// TODO: ndv, nodep, dst_pred, aoffi, lc, LOD bias
void emitTextureSample2(DecoderContext& context, pred_t pred, bool pred_inv,
                        bool int_coords, TextureDimension dim, reg_t dst,
                        u8 component_mask, reg_t src_a, reg_t src_b,
                        u32 cbuf_index, Lod lod_mode = Lod{},
                        bool aoffi = false, bool dc = false) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

#define RA() ir::Value::createRegister(src_a++, ir::ScalarType::F32)
#define RB() ir::Value::createRegister(src_b++, ir::ScalarType::F32)

    TextureType type;
    TextureSampleFlags flags =
        (int_coords ? TextureSampleFlags::IntCoords : TextureSampleFlags::None);
    ir::Value array_index = ir::Value::createUndefined();
    std::vector<ir::Value> coords;
    switch (dim) {
    case TextureDimension::_1D:
        type = TextureType::_1D;
        coords.push_back(RA());
        break;
    case TextureDimension::_1DArray:
        type = TextureType::_1DArray;
        array_index = RA();
        coords.push_back(RA());
        break;
    case TextureDimension::_2D:
        type = TextureType::_2D;
        coords.push_back(RA());
        coords.push_back(RA());
        break;
    case TextureDimension::_2DArray:
        type = TextureType::_2DArray;
        array_index = RA();
        coords.push_back(RA());
        coords.push_back(RA());
        break;
    case TextureDimension::_3D:
        type = TextureType::_3D;
        coords.push_back(RA());
        coords.push_back(RA());
        coords.push_back(RA());
        break;
    case TextureDimension::_3DArray:
        type = TextureType::_3DArray;
        array_index = RA();
        coords.push_back(RA());
        coords.push_back(RA());
        coords.push_back(RA());
        break;
    case TextureDimension::Cube:
        type = TextureType::Cube;
        coords.push_back(RA());
        coords.push_back(RA());
        coords.push_back(RA());
        break;
    case TextureDimension::CubeArray:
        type = TextureType::CubeArray;
        array_index = RA();
        coords.push_back(RA());
        coords.push_back(RA());
        coords.push_back(RA());
        break;
    }

    // Extra operands in register B: LOD/bias, offset, depth reference
    ir::Value lod = ir::Value::createUndefined();
    switch (lod_mode) {
    case Lod::Lz:
        flags |= TextureSampleFlags::Lod;
        lod = ir::Value::createConstantF(0.0f);
        break;
    case Lod::Ll:
    case Lod::Lla:
        flags |= TextureSampleFlags::Lod;
        lod = RB();
        break;
    case Lod::Lb:
    case Lod::Lba:
        // TODO: LOD bias
        src_b++;
        break;
    default:
        break;
    }
    if (aoffi) {
        // TODO: offsets
        src_b++;
    }
    ir::Value cmp_value = ir::Value::createUndefined();
    if (dc) {
        flags |= TextureSampleFlags::DepthCompare;
        cmp_value = RB();
    }

    const auto coords_v =
        context.builder.opVectorConstruct(ir::ScalarType::F32, coords);
    ir::Value res = context.builder.opTextureSample(
        cbuf_index, type, flags, array_index, coords_v, cmp_value, lod);
    copyTextureResult(context.builder, dst, res, component_mask);

    if (conditional)
        context.builder.opEndIf();
}

// TODO: dim, ndv, nodep, offset, lc, dst_pred
void emitTextureGather(DecoderContext& context, pred_t pred, bool pred_inv,
                       TextureComponent component, reg_t dst, u8 component_mask,
                       reg_t src_a, reg_t src_b, u32 cbuf_index,
                       bool has_offset = false, bool dc = false) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    const auto coords_v = context.builder.opVectorConstruct(
        ir::ScalarType::F32,
        {ir::Value::createRegister(src_a + 0, ir::ScalarType::F32),
         ir::Value::createRegister(src_a + 1, ir::ScalarType::F32)});

    // Register B: offset, depth reference
    if (has_offset)
        src_b++; // TODO: offsets
    const auto cmp_value =
        dc ? ir::Value::createRegister(src_b, ir::ScalarType::F32)
           : ir::Value::createUndefined();

    const auto res = context.builder.opTextureGather(
        cbuf_index, coords_v, toTexComponent(component), cmp_value);
    copyTextureResult(context.builder, dst, res, component_mask);

    if (conditional)
        context.builder.opEndIf();
}

// TODO: nodep, offsets
void emitTextureGather2(DecoderContext& context, pred_t pred, bool pred_inv,
                        TextureComponent component, reg_t dst0, reg_t dst1,
                        reg_t src_a, reg_t src_b, u32 cbuf_index,
                        bool aoffi = false, bool dc = false) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    // With extra operands, both coordinates are in register A and register B
    // holds the offset and depth reference
    const bool extra_operands = aoffi || dc;
    const auto coords_v = context.builder.opVectorConstruct(
        ir::ScalarType::F32,
        {ir::Value::createRegister(src_a, ir::ScalarType::F32),
         ir::Value::createRegister(extra_operands ? reg_t(src_a + 1) : src_b,
                                   ir::ScalarType::F32)});
    if (aoffi)
        src_b++; // TODO: offsets
    const auto cmp_value =
        dc ? ir::Value::createRegister(src_b, ir::ScalarType::F32)
           : ir::Value::createUndefined();

    const auto res = context.builder.opTextureGather(
        cbuf_index, coords_v, toTexComponent(component), cmp_value);
    copyTextureResult(context.builder, {dst0 + 0, dst0 + 1, dst1 + 0, dst1 + 1},
                      res, 0b1111);

    if (conditional)
        context.builder.opEndIf();
}

} // namespace

void emitTxq(DecoderContext& context, InstTxq inst) {
    emitTextureQuery(context, inst.pred, inst.pred_inv, inst.query, inst.dst,
                     inst.write_mask, inst.src, inst.cbuf_index);
}

void emitTexs(DecoderContext& context, InstTexs inst) {
    emitTextureSample(context, inst.pred, inst.pred_inv, false, inst.target,
                      inst.dst0, inst.dst1, inst.write_mask, inst.src_a,
                      inst.src_b, inst.cbuf_index);
}

void emitTlds(DecoderContext& context, InstTlds inst) {
    emitTextureSample(context, inst.pred, inst.pred_inv, true, inst.target,
                      inst.dst0, inst.dst1, inst.write_mask, inst.src_a,
                      inst.src_b, inst.cbuf_index);
}

void emitTex(DecoderContext& context, InstTex inst) {
    emitTextureSample2(context, inst.pred, inst.pred_inv, false, inst.dim,
                       inst.dst, inst.write_mask, inst.src_a, inst.src_b,
                       inst.cbuf_index, inst.lod, inst.aoffi, inst.dc);
}

void emitTld4(DecoderContext& context, InstTld4 inst) {
    // TODO: why does this mess up SMO title screen?
    emitTextureGather(context, inst.pred, inst.pred_inv, inst.component,
                      inst.dst, inst.write_mask, inst.src_a, inst.src_b,
                      inst.cbuf_index, inst.offset != TextureOffset{},
                      inst.dc);
}

void emitTxqB(DecoderContext& context, InstTxqB inst) {
    const auto index = trackBindlessHandle(context, inst.src);
    if (!index)
        return;
    emitTextureQuery(context, inst.pred, inst.pred_inv, inst.query, inst.dst,
                     inst.write_mask, static_cast<reg_t>(inst.src.get() + 1),
                     *index);
}

void emitTexB(DecoderContext& context, InstTexB inst) {
    const auto index = trackBindlessHandle(context, inst.src_b);
    if (!index)
        return;
    emitTextureSample2(context, inst.pred, inst.pred_inv, false, inst.dim,
                       inst.dst, inst.write_mask, inst.src_a,
                       static_cast<reg_t>(inst.src_b.get() + 1), *index, inst.lod,
                       inst.aoffi, inst.dc);
}

// TODO: multisample index
void emitTld(DecoderContext& context, InstTld inst) {
    emitTextureSample2(context, inst.pred, inst.pred_inv, true, inst.dim,
                       inst.dst, inst.write_mask, inst.src_a, inst.src_b,
                       inst.cbuf_index, inst.lod ? Lod::Ll : Lod::Lz,
                       inst.aoffi);
}

void emitTldB(DecoderContext& context, InstTldB inst) {
    const auto index = trackBindlessHandle(context, inst.src_b);
    if (!index)
        return;
    emitTextureSample2(context, inst.pred, inst.pred_inv, true, inst.dim,
                       inst.dst, inst.write_mask, inst.src_a,
                       static_cast<reg_t>(inst.src_b.get() + 1), *index,
                       inst.lod ? Lod::Ll : Lod::Lz, inst.aoffi);
}

void emitTld4B(DecoderContext& context, InstTld4B inst) {
    const auto index = trackBindlessHandle(context, inst.src_b);
    if (!index)
        return;
    emitTextureGather(context, inst.pred, inst.pred_inv, inst.component,
                      inst.dst, inst.write_mask, inst.src_a,
                      static_cast<reg_t>(inst.src_b.get() + 1), *index,
                      inst.offset != TextureOffset{}, inst.dc);
}

void emitTld4s(DecoderContext& context, InstTld4s inst) {
    emitTextureGather2(context, inst.pred, inst.pred_inv, inst.component,
                       inst.dst0, inst.dst1, inst.src_a, inst.src_b,
                       inst.cbuf_index, inst.aoffi, inst.dc);
}

std::optional<u32> trackBindlessResource(DecoderContext& context, reg_t reg) {
    return trackBindlessHandle(context, reg);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
