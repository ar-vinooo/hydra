#include <array>
#include <cstdlib>

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
    // The handle is a shader constant instead
    std::optional<u32> constant{};
};

// Whether execution never continues after this instruction (EXIT, KIL, RET or
// an always taken BRA/BRX/JMP/JMX)
bool isUnconditionalTerminator(instruction_t inst) {
    const auto op = inst >> 52;
    if (op != 0xe30 && op != 0xe33 && op != 0xe32 && op != 0xe24 &&
        op != 0xe25 && op != 0xe21 && op != 0xe20)
        return false;
    // Guard predicate must be PT, and the condition code (if any) always true
    if (extractBits(inst, 16, 4) != PT)
        return false;
    return extractBits(inst, 0, 5) == 0xf;
}

// Finds the branch that jumps into (after_pc, end_pc], if there is exactly
// one
std::optional<u64> findBranchInto(ztd::io::IStream& stream, u64 after_pc,
                                  u64 end_pc) {
    std::optional<u64> found;
    for (u64 pc = 1; pc <= after_pc; pc++) {
        if ((pc % 4) == 0) // Sched
            continue;

        stream.seekTo(pc * sizeof(instruction_t));
        const auto inst = stream.read<instruction_t>();
        if ((inst >> 52) != 0xe24) // BRA
            continue;

        const auto relative =
            signExtend<i32, 24>(static_cast<i32>(extractBits(inst, 20, 24)));
        const i64 target = (static_cast<i64>(pc + 1) * 8 + relative) / 8;
        if (target <= static_cast<i64>(after_pc) ||
            target > static_cast<i64>(end_pc))
            continue;
        if (found)
            return std::nullopt;
        found = pc;
    }

    return found;
}

// Finds the closest instruction before pc that writes reg. Code that ends in
// an unconditional terminator cannot fall through, so the search continues
// before the branch that jumps past it instead.
std::optional<std::pair<u64, instruction_t>>
findDefinition(ztd::io::IStream& stream, reg_t reg, u64 pc) {
    u64 def_pc = pc;
    for (u32 steps = 0; steps < MAX_HANDLE_TRACK_DISTANCE && def_pc > 0;
         steps++) {
        def_pc--;
        if ((def_pc % 4) == 0) // Sched
            continue;

        stream.seekTo(def_pc * sizeof(instruction_t));
        const auto inst = stream.read<instruction_t>();
        if (isUnconditionalTerminator(inst)) {
            const auto branch = findBranchInto(stream, def_pc, pc);
            if (branch) {
                // Continue with the instructions before the branch
                def_pc = *branch;
                continue;
            }
        }

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

    // MOV Rd, RZ
    if ((inst & 0xfff8000000000000) == 0x5c98000000000000 &&
        extractBits(inst, 20, 8) == RZ)
        return TrackedHandle{{}, std::nullopt, 0u};

    // MOV32I Rd, imm
    if ((inst & 0xfff0000000000000) == 0x0100000000000000)
        return TrackedHandle{
            {}, std::nullopt, static_cast<u32>(extractBits(inst, 20, 32))};

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
        if (!texture || texture->sampler || texture->constant)
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
        if (!a || !b || a->sampler || b->sampler || a->constant ||
            b->constant)
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

    if (handle && handle->constant &&
        *handle->constant <= CONSTANT_TEXTURE_HANDLE_MASK)
        return makeConstantTextureIndex(*handle->constant);
    if (handle && !handle->constant &&
        (!handle->sampler || handle->sampler->slot == handle->texture.slot))
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

// Texture array indices are integers in the low 16 bits of the register
ir::Value arrayIndex(ir::Builder& builder, reg_t reg) {
    return builder.opBitwiseAnd(
        ir::Value::createRegister(reg, ir::ScalarType::U32),
        ir::Value::createConstantU(0xffff));
}

// TODO: nodep
void emitTextureQuery(DecoderContext& context, pred_t pred, bool pred_inv,
                      TextureQuery query, reg_t dst, u8 write_mask, reg_t src,
                      u32 cbuf_index) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    // NOLINTNEXTLINE(readability-trivial-switch)
    switch (query) {
    case TextureQuery::Dimensions: {
        // Width, height, depth (or layers) at the LOD in src, then the level
        // count; enabled components go to consecutive registers
        const auto lod = context.builder.opCopy(ir::Value::createRegister(src));
        std::vector<ir::Value> results;
        for (u32 i = 0; i < 4; i++)
            if ((write_mask & (1u << i)) != 0)
                results.push_back(
                    context.builder.opTextureQueryDimension(cbuf_index, i, lod));
        for (u32 i = 0; i < results.size(); i++)
            context.builder.opCopy(
                ir::Value::createRegister(dst + static_cast<u8>(i)), results[i]);
        break;
    }
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

// Packs the enabled components as F16 pairs: the first two into dst_a, the
// others into dst_b (missing upper halves are zero)
void copyTextureResultF16(ir::Builder& builder, reg_t dst_a, reg_t dst_b,
                          ir::Value res, u8 component_mask) {
    std::vector<ir::Value> halves;
    for (u8 i = 0; i < 4; i++) {
        if ((component_mask & (1 << i)) == 0x0)
            continue;
        const auto half = builder.opCast(builder.opVectorExtract(res, i),
                                         ir::ScalarType::F16);
        halves.push_back(builder.opCast(
            builder.opBitCast(half, ir::ScalarType::U16), ir::ScalarType::U32));
    }
    const auto pack = [&](usize first) {
        auto value = halves[first];
        if (first + 1 < halves.size())
            value = builder.opBitwiseOr(
                value, builder.opShiftLeft(halves[first + 1],
                                           ir::Value::createConstantU(16)));
        return value;
    };
    if (halves.empty())
        return;
    const auto low = pack(0);
    const auto high = halves.size() > 2 ? pack(2) : ir::Value::createUndefined();
    builder.opCopy(ir::Value::createRegister(dst_a), low);
    if (halves.size() > 2)
        builder.opCopy(ir::Value::createRegister(dst_b), high);
}

constexpr std::array<std::array<u8, 8>, 2> COMPONENT_MASK_LUT = {{
    {0b0001, 0b0010, 0b0100, 0b1000, 0b0011, 0b1001, 0b1010, 0b1100},
    {0b0111, 0b1011, 0b1101, 0b1110, 0b1111, 0b0000, 0b0000, 0b0000},
}};

// TODO: nodep
void emitTextureSample(DecoderContext& context, pred_t pred, bool pred_inv,
                       bool int_coords, TextureSampleTarget target, reg_t dst0,
                       reg_t dst1, u8 write_mask, reg_t src_a, reg_t src_b,
                       u32 cbuf_index, bool f16 = false) {
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
        flags |= TextureSampleFlags::LodZero;
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
        flags |= TextureSampleFlags::LodZero;
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
        // The LOD comes before the depth reference
        type = TextureType::_2D;
        flags |= TextureSampleFlags::DepthCompare | TextureSampleFlags::Lod;
        coords.push_back(RA());
        coords.push_back(RA());
        lod = RB();
        cmp_value = RB();
        break;
    case TextureSampleTarget::_2DLodZeroDepthCompare:
        type = TextureType::_2D;
        flags |= TextureSampleFlags::DepthCompare | TextureSampleFlags::Lod;
        coords.push_back(RA());
        coords.push_back(RA());
        cmp_value = RB();
        flags |= TextureSampleFlags::LodZero;
        lod = ir::Value::createConstantF(0.0f);
        break;
    case TextureSampleTarget::_2DArray:
        type = TextureType::_2DArray;
        array_index = arrayIndex(context.builder, src_a++);
        coords.push_back(RA());
        coords.push_back(RB());
        break;
    case TextureSampleTarget::_2DArrayLodZero:
        type = TextureType::_2DArray;
        flags |= TextureSampleFlags::Lod;
        array_index = arrayIndex(context.builder, src_a++);
        coords.push_back(RA());
        coords.push_back(RB());
        flags |= TextureSampleFlags::LodZero;
        lod = ir::Value::createConstantF(0.0f);
        break;
    case TextureSampleTarget::_2DArrayLodZeroDepthCompare:
        type = TextureType::_2DArray;
        flags |= TextureSampleFlags::DepthCompare | TextureSampleFlags::Lod;
        array_index = arrayIndex(context.builder, src_a++);
        coords.push_back(RA());
        coords.push_back(RB());
        cmp_value = RB();
        flags |= TextureSampleFlags::LodZero;
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
        flags |= TextureSampleFlags::LodZero;
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
    if (f16)
        copyTextureResultF16(context.builder, dst0, dst1, res, component_mask);
    else
        copyTextureResult(context.builder,
                          {dst0 + 0, dst0 + 1, dst1 + 0, dst1 + 1}, res,
                          component_mask);

    if (conditional)
        context.builder.opEndIf();
}

// TLDS fetches texels with its own encoding table
void emitTexelFetchSwizzled(DecoderContext& context, InstTlds inst) {
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, inst.pred, inst.pred_inv);
    const reg_t src_a = inst.src_a.get();
    const reg_t src_b = inst.src_b.get();
    const auto reg = [](reg_t r) {
        return ir::Value::createRegister(r, ir::ScalarType::F32);
    };
    // Signed 4-bit texel offsets in the low byte
    const auto offset = [&](reg_t r) {
        const auto packed = ir::Value::createRegister(r, ir::ScalarType::I32);
        return b.opVectorConstruct(
            ir::ScalarType::I32,
            {b.opBitfieldExtract(packed, ir::Value::createConstantU(0),
                                 ir::Value::createConstantU(4)),
             b.opBitfieldExtract(packed, ir::Value::createConstantU(4),
                                 ir::Value::createConstantU(4))});
    };
    TextureType type = TextureType::_2D;
    auto flags = TextureSampleFlags::IntCoords | TextureSampleFlags::Lod;
    auto array_index = ir::Value::createUndefined();
    std::vector<ir::Value> coords;
    auto lod = ir::Value::createConstantF(0.0f);
    auto texel_offset = ir::Value::createUndefined();
    bool lod_zero = true;
    switch (static_cast<u32>(inst.target.get())) {
    case 0: // 1D.LZ
        type = TextureType::_1D;
        coords = {reg(src_a)};
        break;
    case 1: // 1D.LL
        type = TextureType::_1D;
        coords = {reg(src_a)};
        lod = reg(src_b);
        lod_zero = false;
        break;
    case 2: // 2D.LZ
        coords = {reg(src_a), reg(src_b)};
        break;
    case 4: // 2D.LZ.AOFFI
        coords = {reg(src_a), reg(src_a + 1)};
        texel_offset = offset(src_b);
        break;
    case 5: // 2D.LL
        coords = {reg(src_a), reg(src_a + 1)};
        lod = reg(src_b);
        lod_zero = false;
        break;
    case 6: // 2D.LZ.MS
        ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler, "TLDS multisample fetch"));
        coords = {reg(src_a), reg(src_a + 1)};
        break;
    case 7: // 3D.LZ
        type = TextureType::_3D;
        coords = {reg(src_a), reg(src_a + 1), reg(src_b)};
        break;
    case 8: // 2D array.LZ
        type = TextureType::_2DArray;
        array_index = arrayIndex(b, src_a);
        coords = {reg(src_b), reg(src_b + 1)};
        break;
    case 12: // 2D.LL.AOFFI
        coords = {reg(src_a), reg(src_a + 1)};
        lod = reg(src_b);
        lod_zero = false;
        texel_offset = offset(src_b + 1);
        break;
    default:
        LOG_ERROR(ShaderDecompiler, "Invalid TLDS encoding {}",
                  static_cast<u32>(inst.target.get()));
        if (conditional)
            b.opEndIf();
        return;
    }
    if (lod_zero)
        flags |= TextureSampleFlags::LodZero;
    if (texel_offset.getKind() != ir::ValueKind::Undefined)
        flags |= TextureSampleFlags::Offset;
    const auto res = b.opTextureSample(
        inst.cbuf_index, type, flags, array_index,
        b.opVectorConstruct(ir::ScalarType::F32, coords),
        ir::Value::createUndefined(), lod, ir::Value::createUndefined(),
        ir::Value::createUndefined(), texel_offset);
    const reg_t dst0 = inst.dst0.get();
    const reg_t dst1 = inst.dst1.get();
    const auto component_mask =
        COMPONENT_MASK_LUT[dst1 == RZ ? 0 : 1][inst.write_mask.get()];
    if (!inst.f32)
        copyTextureResultF16(b, dst0, dst1, res, component_mask);
    else
        copyTextureResult(b, {dst0 + 0, dst0 + 1, dst1 + 0, dst1 + 1}, res,
                          component_mask);
    if (conditional)
        b.opEndIf();
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
        array_index = arrayIndex(context.builder, src_a++);
        coords.push_back(RA());
        break;
    case TextureDimension::_2D:
        type = TextureType::_2D;
        coords.push_back(RA());
        coords.push_back(RA());
        break;
    case TextureDimension::_2DArray:
        type = TextureType::_2DArray;
        array_index = arrayIndex(context.builder, src_a++);
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
        array_index = arrayIndex(context.builder, src_a++);
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
        array_index = arrayIndex(context.builder, src_a++);
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
        flags |= TextureSampleFlags::LodZero;
        lod = ir::Value::createConstantF(0.0f);
        break;
    case Lod::Ll:
    case Lod::Lla:
        flags |= TextureSampleFlags::Lod;
        lod = RB();
        break;
    case Lod::Lb:
    case Lod::Lba:
        flags |= TextureSampleFlags::Bias;
        lod = RB();
        break;
    default:
        break;
    }
    ir::Value texel_offset = ir::Value::createUndefined();
    if (aoffi) {
        auto& b = context.builder;
        const auto packed =
            ir::Value::createRegister(src_b++, ir::ScalarType::I32);
        std::vector<ir::Value> values;
        for (u32 axis = 0; axis < coords.size(); ++axis)
            values.push_back(b.opBitfieldExtract(
                packed, ir::Value::createConstantU(axis * 4),
                ir::Value::createConstantU(4)));
        texel_offset = b.opVectorConstruct(ir::ScalarType::I32, values);
        flags |= TextureSampleFlags::Offset;
    }
    ir::Value cmp_value = ir::Value::createUndefined();
    if (dc) {
        flags |= TextureSampleFlags::DepthCompare;
        cmp_value = RB();
    }

    const auto coords_v =
        context.builder.opVectorConstruct(ir::ScalarType::F32, coords);
    ir::Value res = context.builder.opTextureSample(
        cbuf_index, type, flags, array_index, coords_v, cmp_value, lod,
        ir::Value::createUndefined(), ir::Value::createUndefined(),
        texel_offset);
    copyTextureResult(context.builder, dst, res, component_mask);

    if (conditional)
        context.builder.opEndIf();
}

// TODO: ndv, nodep, lc, dst_pred
void emitTextureGather(DecoderContext& context, pred_t pred, bool pred_inv,
                       TextureComponent component, reg_t dst, u8 component_mask,
                       reg_t src_a, reg_t src_b, u32 cbuf_index,
                       TextureOffset offset = TextureOffset::None,
                       bool dc = false,
                       TextureDimension dimension = TextureDimension::_2D) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    auto& b = context.builder;
    const bool array = dimension == TextureDimension::_2DArray ||
                       dimension == TextureDimension::CubeArray;
    const bool cube = dimension == TextureDimension::Cube ||
                      dimension == TextureDimension::CubeArray;
    const auto layer =
        array ? arrayIndex(b, src_a++) : ir::Value::createUndefined();
    std::vector<ir::Value> coordinates;
    for (u32 axis = 0; axis < (cube ? 3u : 2u); ++axis)
        coordinates.push_back(
            ir::Value::createRegister(src_a++, ir::ScalarType::F32));
    const auto coords_v = b.opVectorConstruct(ir::ScalarType::F32, coordinates);
    const auto type = cube
                          ? (array ? TextureType::CubeArray : TextureType::Cube)
                          : (array ? TextureType::_2DArray : TextureType::_2D);
    auto flags = TextureSampleFlags::Gather;
    auto single_offset = ir::Value::createUndefined();
    auto per_texel_offsets = ir::Value::createUndefined();
    if (offset == TextureOffset::Aoffi) {
        const auto packed =
            ir::Value::createRegister(src_b++, ir::ScalarType::I32);
        single_offset = b.opVectorConstruct(
            ir::ScalarType::I32,
            {b.opBitfieldExtract(packed, ir::Value::createConstantU(0),
                                 ir::Value::createConstantU(6)),
             b.opBitfieldExtract(packed, ir::Value::createConstantU(8),
                                 ir::Value::createConstantU(6))});
        flags |= TextureSampleFlags::Offset;
    } else if (offset == TextureOffset::Ptp) {
        const auto low =
            ir::Value::createRegister(src_b++, ir::ScalarType::U32);
        const auto high =
            ir::Value::createRegister(src_b++, ir::ScalarType::U32);
        per_texel_offsets =
            b.opVectorConstruct(ir::ScalarType::U32, {low, high});
        flags |= TextureSampleFlags::Offsets;
    }
    const auto cmp_value =
        dc ? ir::Value::createRegister(src_b, ir::ScalarType::F32)
           : ir::Value::createUndefined();
    const auto res = b.opTextureGather(
        cbuf_index, coords_v, toTexComponent(component), cmp_value, type, flags,
        layer, single_offset, per_texel_offsets);
    copyTextureResult(context.builder, dst, res, component_mask);

    if (conditional)
        context.builder.opEndIf();
}

// TODO: nodep, offsets
void emitTextureGather2(DecoderContext& context, pred_t pred, bool pred_inv,
                        TextureComponent component, reg_t dst0, reg_t dst1,
                        reg_t src_a, reg_t src_b, u32 cbuf_index,
                        bool aoffi = false, bool dc = false, bool f16 = false) {
    const auto conditional = handlePredCond(context.builder, pred, pred_inv);

    // With extra operands, both coordinates are in register A and register B
    // holds the offset and depth reference
    const bool extra_operands = aoffi || dc;
    std::array<ir::Value, 2> coords{
        ir::Value::createRegister(src_a, ir::ScalarType::F32),
        ir::Value::createRegister(extra_operands ? reg_t(src_a + 1) : src_b,
                                  ir::ScalarType::F32)};
    auto flags = TextureSampleFlags::Gather;
    auto single_offset = ir::Value::createUndefined();
    if (aoffi) {
        const auto packed =
            ir::Value::createRegister(src_b++, ir::ScalarType::I32);
        auto& b = context.builder;
        single_offset = b.opVectorConstruct(
            ir::ScalarType::I32,
            {b.opBitfieldExtract(packed, ir::Value::createConstantU(0),
                                 ir::Value::createConstantU(6)),
             b.opBitfieldExtract(packed, ir::Value::createConstantU(8),
                                 ir::Value::createConstantU(6))});
        flags |= TextureSampleFlags::Offset;
    }
    const auto coords_v = context.builder.opVectorConstruct(
        ir::ScalarType::F32, {coords[0], coords[1]});
    const auto cmp_value =
        dc ? ir::Value::createRegister(src_b, ir::ScalarType::F32)
           : ir::Value::createUndefined();

    const auto res = context.builder.opTextureGather(
        cbuf_index, coords_v, toTexComponent(component), cmp_value,
        TextureType::_2D, flags, ir::Value::createUndefined(), single_offset);
    if (f16)
        copyTextureResultF16(context.builder, dst0, dst1, res, 0b1111);
    else
        copyTextureResult(context.builder,
                          {dst0 + 0, dst0 + 1, dst1 + 0, dst1 + 1}, res, 0b1111);

    if (conditional)
        context.builder.opEndIf();
}

} // namespace

namespace {

// TMML returns the clamped and unclamped levels in 24.8 fixed point, then
// two floats
void emitMipLevelQuery(DecoderContext& context, const InstTmml& inst,
                       u32 binding) {
    auto& b = context.builder;
    const auto conditional = handlePredCond(b, inst.pred.get(), inst.pred_inv.get());

    reg_t coordinate = inst.src.get();
    const auto next = [&] {
        return ir::Value::createRegister(coordinate++, ir::ScalarType::F32);
    };
    TextureType type;
    u32 axes;
    bool array = false;
    switch (static_cast<TextureDimension>(inst.dim.get())) {
    case TextureDimension::_1D:
        type = TextureType::_1D;
        axes = 1;
        break;
    case TextureDimension::_1DArray:
        type = TextureType::_1DArray;
        axes = 1;
        array = true;
        break;
    case TextureDimension::_2D:
        type = TextureType::_2D;
        axes = 2;
        break;
    case TextureDimension::_2DArray:
        type = TextureType::_2DArray;
        axes = 2;
        array = true;
        break;
    case TextureDimension::_3D:
        type = TextureType::_3D;
        axes = 3;
        break;
    case TextureDimension::Cube:
        type = TextureType::Cube;
        axes = 3;
        break;
    case TextureDimension::CubeArray:
        type = TextureType::CubeArray;
        axes = 3;
        array = true;
        break;
    default:
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "TMML texture dimension {}",
                            inst.dim.get());
        if (conditional)
            b.opEndIf();
        return;
    }
    // The array index is not needed on the host
    if (array)
        coordinate++;
    std::vector<ir::Value> values;
    for (u32 axis = 0; axis < axes; axis++)
        values.push_back(next());
    const auto coords = b.opVectorConstruct(ir::ScalarType::F32, values);

    reg_t dst = inst.dst.get();
    for (u32 element = 0; element < 4; element++) {
        if ((inst.write_mask.get() & (1u << element)) == 0)
            continue;
        auto value = ir::Value::createUndefined();
        if (element < 2) {
            const auto lod = b.opTextureQueryLod(binding, type, coords, element == 1);
            // Levels below 0 do not exist
            const auto fixed = b.opCast(
                b.opMax(b.opMultiply(lod, ir::Value::createConstantF(256.0f)),
                        ir::Value::createConstantF(0.0f)),
                ir::ScalarType::U32);
            value = fixed;
        } else {
            value = ir::Value::createConstantF(0.0f);
        }
        b.opCopy(ir::Value::createRegister(dst++, element < 2
                                                      ? ir::ScalarType::U32
                                                      : ir::ScalarType::F32),
                 value);
    }

    if (conditional)
        b.opEndIf();
}

} // namespace

void emitTmml(DecoderContext& context, InstTmml inst) {
    emitMipLevelQuery(context, inst, inst.cbuf_index.get());
}

void emitTmmlB(DecoderContext& context, InstTmml inst) {
    const auto index = trackBindlessHandle(context, inst.meta.get());
    if (!index)
        return;
    emitMipLevelQuery(context, inst, *index);
}

void emitTxq(DecoderContext& context, InstTxq inst) {
    emitTextureQuery(context, inst.pred, inst.pred_inv, inst.query, inst.dst,
                     inst.write_mask, inst.src, inst.cbuf_index);
}

void emitTexs(DecoderContext& context, InstTexs inst) {
    emitTextureSample(context, inst.pred, inst.pred_inv, false, inst.target,
                      inst.dst0, inst.dst1, inst.write_mask, inst.src_a,
                      inst.src_b, inst.cbuf_index, !inst.f32);
}

void emitTlds(DecoderContext& context, InstTlds inst) {
    emitTexelFetchSwizzled(context, inst);
}

void emitTex(DecoderContext& context, InstTex inst) {
    emitTextureSample2(context, inst.pred, inst.pred_inv, false, inst.dim,
                       inst.dst, inst.write_mask, inst.src_a, inst.src_b,
                       inst.cbuf_index, inst.lod, inst.aoffi, inst.dc);
}

namespace {
void emitTextureGradient(DecoderContext& context, InstTxd inst, bool bindless) {
    const auto conditional =
        handlePredCond(context.builder, inst.pred, inst.pred_inv);
    auto& b = context.builder;
    reg_t coordinate = inst.src_a;
    u32 binding = inst.cbuf_index;
    if (bindless) {
        const auto tracked = trackBindlessHandle(context, coordinate++);
        if (!tracked) {
            if (conditional)
                b.opEndIf();
            return;
        }
        binding = *tracked;
    }
    const bool array = inst.dim == TextureDimension::_2DArray ||
                       inst.dim == TextureDimension::_1DArray;
    const bool is_1d = inst.dim == TextureDimension::_1D ||
                       inst.dim == TextureDimension::_1DArray;
    if (!is_1d && inst.dim != TextureDimension::_2D &&
        inst.dim != TextureDimension::_2DArray) {
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "TXD texture dimension {}",
                            static_cast<u32>(inst.dim.get()));
        if (conditional)
            b.opEndIf();
        return;
    }
    const u32 axes = is_1d ? 1 : 2;
    std::vector<ir::Value> coordinate_values;
    for (u32 axis = 0; axis < axes; axis++)
        coordinate_values.push_back(
            ir::Value::createRegister(coordinate++, ir::ScalarType::F32));
    const auto coords =
        b.opVectorConstruct(ir::ScalarType::F32, coordinate_values);
    const auto packed =
        ir::Value::createRegister(coordinate, ir::ScalarType::I32);
    auto layer = ir::Value::createUndefined();
    if (array)
        layer = b.opBitwiseAnd(
            ir::Value::createRegister(coordinate, ir::ScalarType::U32),
            ir::Value::createConstantU(inst.lc ? 0xfffu : 0xffffu));
    // Derivatives are interleaved per axis: dx.x, dy.x, dx.y, dy.y
    std::vector<ir::Value> dx_values, dy_values;
    for (u32 axis = 0; axis < axes; axis++) {
        dx_values.push_back(ir::Value::createRegister(
            static_cast<reg_t>(inst.src_b.get() + axis * 2),
            ir::ScalarType::F32));
        dy_values.push_back(ir::Value::createRegister(
            static_cast<reg_t>(inst.src_b.get() + axis * 2 + 1),
            ir::ScalarType::F32));
    }
    const auto dx = b.opVectorConstruct(ir::ScalarType::F32, dx_values);
    const auto dy = b.opVectorConstruct(ir::ScalarType::F32, dy_values);
    auto flags = TextureSampleFlags::Derivatives;
    auto offset = ir::Value::createUndefined();
    if (inst.aoffi) {
        const u32 bit = inst.lc ? 12 : 16;
        std::vector<ir::Value> offsets;
        for (u32 axis = 0; axis < axes; axis++)
            offsets.push_back(b.opBitfieldExtract(
                packed, ir::Value::createConstantU(bit + axis * 4),
                ir::Value::createConstantU(4)));
        offset = b.opVectorConstruct(ir::ScalarType::I32, offsets);
        flags |= TextureSampleFlags::Offset;
    }
    auto clamp = ir::Value::createUndefined();
    if (inst.lc) {
        const auto raw =
            ir::Value::createRegister(coordinate, ir::ScalarType::U32);
        clamp = b.opMultiply(
            b.opCast(b.opBitfieldExtract(raw, ir::Value::createConstantU(20),
                                         ir::Value::createConstantU(12)),
                     ir::ScalarType::F32),
            ir::Value::createConstantF(1.0f / 256.0f));
        flags |= TextureSampleFlags::Lod;
    }
    const auto type = is_1d ? (array ? TextureType::_1DArray : TextureType::_1D)
                            : (array ? TextureType::_2DArray : TextureType::_2D);
    const auto result =
        b.opTextureSample(binding, type, flags, layer, coords,
                          ir::Value::createUndefined(), clamp, dx, dy, offset);
    copyTextureResult(b, inst.dst, result, inst.write_mask);
    if (conditional)
        b.opEndIf();
}
} // namespace

void emitTxd(DecoderContext& context, InstTxd inst) {
    emitTextureGradient(context, inst, false);
}
void emitTxdB(DecoderContext& context, InstTxdB inst) {
    emitTextureGradient(context, inst, true);
}

void emitTld4(DecoderContext& context, InstTld4 inst) {
    // TODO: why does this mess up SMO title screen?
    emitTextureGather(context, inst.pred, inst.pred_inv, inst.component,
                      inst.dst, inst.write_mask, inst.src_a, inst.src_b,
                      inst.cbuf_index, inst.offset, inst.dc, inst.dim);
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
                       static_cast<reg_t>(inst.src_b.get() + 1), *index,
                       inst.lod, inst.aoffi, inst.dc);
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
                      inst.offset, inst.dc, inst.dim);
}

void emitTld4s(DecoderContext& context, InstTld4s inst) {
    emitTextureGather2(context, inst.pred, inst.pred_inv, inst.component,
                       inst.dst0, inst.dst1, inst.src_a, inst.src_b,
                       inst.cbuf_index, inst.aoffi, inst.dc, inst.f16);
}

std::optional<u32> trackBindlessResource(DecoderContext& context, reg_t reg) {
    return trackBindlessHandle(context, reg);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
