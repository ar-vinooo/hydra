#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/ir/module.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::ir {

class Builder {
  public:
    explicit Builder(Module& modul_) : modul{modul_} {
        setInsertFunction("main");
        setInsertBlock(0x0);
    }

    // Operations

    // Data
    void opCopy(const Value& dst, const Value& src) {
        addInstructionWithDst(Opcode::Copy, dst, {src});
    }
    Value opCopy(const Value& src) {
        return addInstruction(Opcode::Copy, src.getType(), {src});
    }
    Value opCast(const Value& src, Type dst_type) {
        return addInstruction(Opcode::Cast, dst_type, {src});
    }
    Value opBitCast(const Value& src, Type dst_type) {
        return addInstruction(Opcode::BitCast, dst_type, {src});
    }

    // Arithmetic
    Value opAbs(const Value& src) {
        return addInstruction(Opcode::Abs, src.getType(), {src});
    }
    Value opNeg(const Value& src) {
        return addInstruction(Opcode::Neg, src.getType(), {src});
    }
    Value opAdd(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for add ({} != {})", src_a.getType(),
                     src_b.getType());
        return addInstruction(Opcode::Add, src_a.getType(), {src_a, src_b});
    }
    Value opMultiply(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for multiply ({} != {})", src_a.getType(),
                     src_b.getType());
        return addInstruction(Opcode::Multiply, src_a.getType(),
                              {src_a, src_b});
    }
    // Value of the neighbouring pixel in the 2x2 quad (lane ^ mask)
    Value opQuadShuffleXor(const Value& src, const Value& mask) {
        return addInstruction(Opcode::QuadShuffleXor, ScalarType::U32,
                              {src, mask});
    }
    // Value of the pixel at quad lane `index` (x | (y << 1)) of the 2x2 quad
    Value opQuadShuffle(const Value& src, const Value& index) {
        return addInstruction(Opcode::QuadShuffle, ScalarType::U32,
                              {src, index});
    }
    // a * LUT_A[mode] + b * LUT_B[mode], mode = swizzle >> (quad lane * 2)
    Value opSwizzleAdd(const Value& src_a, const Value& src_b, u32 swizzle) {
        return addInstruction(Opcode::SwizzleAdd, ScalarType::F32,
                              {src_a, src_b, Value::createRawValue(swizzle)});
    }
    Value opFma(const Value& src_a, const Value& src_b, const Value& src_c) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for fma ({} != {})", src_a.getType(),
                     src_b.getType());
        ASSERT_DEBUG(src_a.getType() == src_c.getType(), ShaderDecompiler,
                     "Type mismatch for fma ({} != {})", src_a.getType(),
                     src_c.getType());
        return addInstruction(Opcode::Fma, src_a.getType(),
                              {src_a, src_b, src_c});
    }
    Value opMin(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for min ({} != {})", src_a.getType(),
                     src_b.getType());
        return addInstruction(Opcode::Min, src_a.getType(), {src_a, src_b});
    }
    Value opMax(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for max ({} != {})", src_a.getType(),
                     src_b.getType());
        return addInstruction(Opcode::Max, src_a.getType(), {src_a, src_b});
    }
    Value opClamp(const Value& src_a, const Value& src_b, const Value& src_c) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for clamp ({} != {})", src_a.getType(),
                     src_b.getType());
        ASSERT_DEBUG(src_a.getType() == src_c.getType(), ShaderDecompiler,
                     "Type mismatch for clamp ({} != {})", src_a.getType(),
                     src_c.getType());
        return addInstruction(Opcode::Clamp, src_a.getType(),
                              {src_a, src_b, src_c});
    }

    // Math
    Value opIsNan(const Value& src) {
        ASSERT_DEBUG(src.getType().isFloatingPoint(), ShaderDecompiler,
                     "Cannot check for NaN with type {}", src.getType());
        const auto type =
            src.getType().isScalar()
                ? Type(ScalarType::Bool)
                : Type(VectorType(ScalarType::Bool,
                                  src.getType().getVectorType().getSize()));
        return addInstruction(Opcode::IsNan, type, {src});
    }
    Value opRound(const Value& src) {
        ASSERT_DEBUG(src.getType().isFloatingPoint(), ShaderDecompiler,
                     "Cannot round with type {}", src.getType());
        return addInstruction(Opcode::Round, src.getType(), {src});
    }
    Value opFloor(const Value& src) {
        ASSERT_DEBUG(src.getType().isFloatingPoint(), ShaderDecompiler,
                     "Cannot floor with type {}", src.getType());
        return addInstruction(Opcode::Floor, src.getType(), {src});
    }
    Value opCeil(const Value& src) {
        ASSERT_DEBUG(src.getType().isFloatingPoint(), ShaderDecompiler,
                     "Cannot ceil with type {}", src.getType());
        return addInstruction(Opcode::Ceil, src.getType(), {src});
    }
    Value opTrunc(const Value& src) {
        ASSERT_DEBUG(src.getType().isFloatingPoint(), ShaderDecompiler,
                     "Cannot truncate with type {}", src.getType());
        return addInstruction(Opcode::Trunc, src.getType(), {src});
    }
    Value opReciprocal(const Value& src) {
        ASSERT_DEBUG(src.getType().isFloatingPoint(), ShaderDecompiler,
                     "Cannot reciprocal with type {}", src.getType());
        return addInstruction(Opcode::Reciprocal, src.getType(), {src});
    }
    Value opSin(const Value& src) {
        ASSERT_DEBUG(src.getType().isFloatingPoint(), ShaderDecompiler,
                     "Cannot perform sin with type {}", src.getType());
        return addInstruction(Opcode::Sin, src.getType(), {src});
    }
    Value opCos(const Value& src) {
        ASSERT_DEBUG(src.getType().isFloatingPoint(), ShaderDecompiler,
                     "Cannot perform cos with type {}", src.getType());
        return addInstruction(Opcode::Cos, src.getType(), {src});
    }
    Value opExp2(const Value& src) {
        return addInstruction(Opcode::Exp2, src.getType(), {src});
    }
    Value opLog2(const Value& src) {
        return addInstruction(Opcode::Log2, src.getType(), {src});
    }
    Value opSqrt(const Value& src) {
        ASSERT_DEBUG(src.getType().isFloatingPoint(), ShaderDecompiler,
                     "Cannot perform sqrt with type {}", src.getType());
        return addInstruction(Opcode::Sqrt, src.getType(), {src});
    }
    Value opReciprocalSqrt(const Value& src) {
        ASSERT_DEBUG(src.getType().isFloatingPoint(), ShaderDecompiler,
                     "Cannot perform reciprocal sqrt with type {}",
                     src.getType());
        return addInstruction(Opcode::ReciprocalSqrt, src.getType(), {src});
    }

    // Logical & Bitwise
    Value opNot(const Value& src) {
        ASSERT_DEBUG(src.getType() == ScalarType::Bool, ShaderDecompiler,
                     "Cannot perform not with non-boolean type {}",
                     src.getType());
        return addInstruction(Opcode::Not, ScalarType::Bool, {src});
    }
    Value opBitwiseNot(const Value& src) {
        ASSERT_DEBUG(src.getType().isScalar() &&
                         (src.getType().isInteger() ||
                          src.getType() == ScalarType::Bool),
                     ShaderDecompiler,
                     "Cannot perform bitwise not with non-integer type {}",
                     src.getType());
        return addInstruction(Opcode::BitwiseNot, src.getType(), {src});
    }
    Value opBitwiseAnd(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for bitwise and ({} != {})",
                     src_a.getType(), src_b.getType());
        ASSERT_DEBUG(src_a.getType().isScalar() &&
                         (src_a.getType().isInteger() ||
                          src_a.getType() == ScalarType::Bool),
                     ShaderDecompiler,
                     "Cannot perform bitwise and with non-integer type {}",
                     src_a.getType());
        return addInstruction(Opcode::BitwiseAnd, src_a.getType(),
                              {src_a, src_b});
    }
    Value opBitwiseOr(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for bitwise or ({} != {})", src_a.getType(),
                     src_b.getType());
        ASSERT_DEBUG(src_a.getType().isScalar() &&
                         (src_a.getType().isInteger() ||
                          src_a.getType() == ScalarType::Bool),
                     ShaderDecompiler,
                     "Cannot perform bitwise or with non-integer type {}",
                     src_a.getType());
        return addInstruction(Opcode::BitwiseOr, src_a.getType(),
                              {src_a, src_b});
    }
    Value opBitwiseXor(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for bitwise xor ({} != {})",
                     src_a.getType(), src_b.getType());
        ASSERT_DEBUG(src_a.getType().isScalar() &&
                         (src_a.getType().isInteger() ||
                          src_a.getType() == ScalarType::Bool),
                     ShaderDecompiler,
                     "Cannot perform bitwise xor with non-integer type {}",
                     src_a.getType());
        return addInstruction(Opcode::BitwiseXor, src_a.getType(),
                              {src_a, src_b});
    }
    Value opShiftLeft(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType().isScalar() && src_a.getType().isInteger(),
                     ShaderDecompiler,
                     "Cannot perform shift left with non-integer type {}",
                     src_a.getType());
        ASSERT_DEBUG(src_b.getType().isScalar() && src_b.getType().isInteger(),
                     ShaderDecompiler,
                     "Cannot perform shift left with non-integer type {}",
                     src_b.getType());
        return addInstruction(Opcode::ShiftLeft, src_a.getType(),
                              {src_a, src_b});
    }
    Value opShiftRight(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType().isScalar() && src_a.getType().isInteger(),
                     ShaderDecompiler,
                     "Cannot perform shift left with non-integer type {}",
                     src_a.getType());
        ASSERT_DEBUG(src_b.getType().isScalar() && src_b.getType().isInteger(),
                     ShaderDecompiler,
                     "Cannot perform shift left with non-integer type {}",
                     src_b.getType());
        return addInstruction(Opcode::ShiftRight, src_a.getType(),
                              {src_a, src_b});
    }
    Value opBitfieldExtract(const Value& src_a, const Value& src_b,
                            const Value& src_c) {
        ASSERT_DEBUG(src_a.getType().isScalar() && src_a.getType().isInteger(),
                     ShaderDecompiler,
                     "Cannot perform bitfield extract with non-integer type {}",
                     src_a.getType());
        ASSERT_DEBUG(src_b.getType().isScalar() && src_b.getType().isInteger(),
                     ShaderDecompiler,
                     "Cannot perform bitfield extract with non-integer type {}",
                     src_b.getType());
        ASSERT_DEBUG(src_c.getType().isScalar() && src_c.getType().isInteger(),
                     ShaderDecompiler,
                     "Cannot perform bitfield extract with non-integer type {}",
                     src_c.getType());
        return addInstruction(Opcode::BitfieldExtract, src_a.getType(),
                              {src_a, src_b, src_c});
    }

    // Comparison & Selection
    Value opCompareLess(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for compare less ({} != {})",
                     src_a.getType(), src_b.getType());
        return addInstruction(Opcode::CompareLess, ScalarType::Bool,
                              {src_a, src_b});
    }
    Value opCompareLessOrEqual(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for compare less or equal ({} != {})",
                     src_a.getType(), src_b.getType());
        return addInstruction(Opcode::CompareLessOrEqual, ScalarType::Bool,
                              {src_a, src_b});
    }
    Value opCompareGreater(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for compare greater ({} != {})",
                     src_a.getType(), src_b.getType());
        return addInstruction(Opcode::CompareGreater, ScalarType::Bool,
                              {src_a, src_b});
    }
    Value opCompareGreaterOrEqual(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for compare greater or equal ({} != {})",
                     src_a.getType(), src_b.getType());
        return addInstruction(Opcode::CompareGreaterOrEqual, ScalarType::Bool,
                              {src_a, src_b});
    }
    Value opCompareEqual(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for compare equal ({} != {})",
                     src_a.getType(), src_b.getType());
        return addInstruction(Opcode::CompareEqual, ScalarType::Bool,
                              {src_a, src_b});
    }
    Value opCompareNotEqual(const Value& src_a, const Value& src_b) {
        ASSERT_DEBUG(src_a.getType() == src_b.getType(), ShaderDecompiler,
                     "Type mismatch for compare not equal ({} != {})",
                     src_a.getType(), src_b.getType());
        return addInstruction(Opcode::CompareNotEqual, ScalarType::Bool,
                              {src_a, src_b});
    }
    Value opSelect(const Value& cond, const Value& src_true,
                   const Value& src_false) {
        ASSERT_DEBUG(cond.getType() == ScalarType::Bool, ShaderDecompiler,
                     "Cannot perform select with non-boolean type {}",
                     cond.getType());
        ASSERT_DEBUG(src_true.getType() == src_false.getType(),
                     ShaderDecompiler, "Type mismatch for select ({} != {})",
                     src_true.getType(), src_false.getType());
        return addInstruction(Opcode::Select, src_true.getType(),
                              {cond, src_true, src_false});
    }

    // Control flow
    void opBeginIf(const Value& cond) {
        ASSERT_DEBUG(cond.getType() == ScalarType::Bool, ShaderDecompiler,
                     "Cannot perform begin if with non-boolean type {}",
                     cond.getType());
        addInstructionWithDst(Opcode::BeginIf, std::nullopt, {cond});
    }
    void opEndIf() { addInstructionWithDst(Opcode::EndIf); }
    void opBranch(label_t target) {
        addInstructionWithDst(Opcode::Branch, std::nullopt,
                              {Value::createLabel(target)});
    }
    void opPushReturn(label_t target) {
        addInstructionWithDst(Opcode::PushReturn, std::nullopt,
                              {Value::createLabel(target)});
    }
    void opReturnBranch() {
        addInstructionWithDst(Opcode::ReturnBranch);
    }
    void opBranchConditional(const Value& cond, label_t target_true,
                             label_t target_false) {
        ASSERT_DEBUG(
            cond.getType() == ScalarType::Bool, ShaderDecompiler,
            "Cannot perform branch conditional with non-boolean type {}",
            cond.getType());
        addInstructionWithDst(Opcode::BranchConditional, std::nullopt,
                              {cond, Value::createLabel(target_true),
                               Value::createLabel(target_false)});
    }

    // Vector
    Value opVectorExtract(const Value& src, u8 index) {
        ASSERT_DEBUG(src.getType().isVector(), ShaderDecompiler,
                     "Cannot perform vector extract with non-vector type {}",
                     src.getType());
        return addInstruction(Opcode::VectorExtract,
                              src.getType().getVectorType().getElementType(),
                              {src, Value::createRawValue(index)});
    }
    void opVectorInsert(const Value& dst, const Value& src, u8 index) {
        ASSERT_DEBUG(dst.getType().isVector(), ShaderDecompiler,
                     "Cannot perform vector insert with non-vector type {}",
                     dst.getType());
        ASSERT_DEBUG(dst.getType().getVectorType().getElementType() ==
                         src.getType().getScalarType(),
                     ShaderDecompiler,
                     "Element type mismatch for vector insert ({} != {})",
                     src.getType(),
                     dst.getType().getVectorType().getElementType());
        addInstructionWithDst(Opcode::VectorInsert, dst,
                              {src, Value::createRawValue(index)});
    }
    Value opVectorConstruct(ScalarType element_type,
                            const std::vector<Value>& elements) {
        std::vector<Value> operands;
        operands.reserve(elements.size());
        for (const auto& element : elements)
            operands.push_back(element);
        return addInstruction(
            Opcode::VectorConstruct,
            Type::createVector(element_type, static_cast<u8>(elements.size())),
            operands);
    }

    // Texture
    Value opTextureSample(u32 const_buffer_index, TextureType type,
                          TextureSampleFlags flags, const Value& array_index,
                          const Value& coords, const Value& cmp_value,
                          const Value& lod,
                          const Value& ddx = Value::createUndefined(),
                          const Value& ddy = Value::createUndefined(),
                          const Value& offset = Value::createUndefined(),
                          const Value& offsets = Value::createUndefined(),
                          u8 component = 0) {
        ASSERT(!(any(flags & TextureSampleFlags::Offset) &&
                 any(flags & TextureSampleFlags::Offsets)),
               ShaderDecompiler,
               "Texture Offset and Offsets are mutually exclusive");
        ASSERT(!any(flags & TextureSampleFlags::Offsets) ||
                   any(flags & TextureSampleFlags::Gather),
               ShaderDecompiler, "Per-texel offsets require a gather");
        ASSERT(!any(flags & TextureSampleFlags::Derivatives) ||
                   (!any(flags & (TextureSampleFlags::IntCoords |
                                  TextureSampleFlags::Bias |
                                  TextureSampleFlags::LodZero |
                                  TextureSampleFlags::Gather)) &&
                    ddx.getKind() != ValueKind::Undefined &&
                    ddy.getKind() != ValueKind::Undefined),
               ShaderDecompiler,
               "Explicit texture derivatives require gradients and a filtered "
               "sample");
        ASSERT(!any(flags & TextureSampleFlags::Bias) ||
                   !any(flags & (TextureSampleFlags::Lod |
                                 TextureSampleFlags::LodZero)),
               ShaderDecompiler,
               "Texture bias and explicit LOD are mutually exclusive");
        ASSERT(component < 4, ShaderDecompiler,
               "Invalid texture gather component");
        ASSERT(
            !any(flags & TextureSampleFlags::Gather) ||
                !any(flags &
                     (TextureSampleFlags::IntCoords | TextureSampleFlags::Bias |
                      TextureSampleFlags::Lod | TextureSampleFlags::LodZero)),
            ShaderDecompiler,
            "Gather does not accept texel-fetch, bias or explicit LOD modes");
        ASSERT(!any(flags & TextureSampleFlags::Offset) ||
                   offset.getKind() != ValueKind::Undefined,
               ShaderDecompiler, "Texture Offset requires an offset operand");
        ASSERT(!any(flags & TextureSampleFlags::Offsets) ||
                   offsets.getKind() != ValueKind::Undefined,
               ShaderDecompiler,
               "Texture Offsets requires four packed offset pairs");
        ASSERT_DEBUG(coords.getType().isVector() &&
                         coords.getType().isFloatingPoint(),
                     ShaderDecompiler,
                     "Cannot perform texture sample with non-floating point "
                     "vector type {}",
                     coords.getType());
        if (renderer::isTrackedTextureIndex(const_buffer_index))
            flags |= TextureSampleFlags::Bindless;
        return addInstruction(
            Opcode::TextureSample, Type::createVector(ScalarType::F32, 4),
            {Value::createRawValue(const_buffer_index),
             Value::createRawValue(type), Value::createRawValue(flags),
             array_index, coords, cmp_value, lod, ddx, ddy, offset, offsets,
             Value::createRawValue(component)});
    }
    // TODO: more args
    Value opTextureGather(u32 const_buffer_index, const Value& coords,
                          u8 component,
                          const Value& cmp_value = Value::createUndefined(),
                          TextureType type = TextureType::_2D,
                          TextureSampleFlags flags = TextureSampleFlags::None,
                          const Value& array_index = Value::createUndefined(),
                          const Value& offset = Value::createUndefined(),
                          const Value& offsets = Value::createUndefined()) {
        ASSERT_DEBUG(coords.getType().isVector() &&
                         coords.getType().isFloatingPoint(),
                     ShaderDecompiler,
                     "Cannot perform texture gather with non-floating point "
                     "vector type {}",
                     coords.getType());
        flags |= TextureSampleFlags::Gather;
        if (cmp_value.getKind() != ValueKind::Undefined)
            flags |= TextureSampleFlags::DepthCompare;
        return opTextureSample(
            const_buffer_index, type, flags, array_index, coords, cmp_value,
            Value::createUndefined(), Value::createUndefined(),
            Value::createUndefined(), offset, offsets, component);
    }
    // Dimension 0-2 at the LOD (size, height, depth or array size), 3 is the
    // number of mip levels
    Value opTextureQueryDimension(u32 const_buffer_index, u32 dimension,
                                  const Value& lod = Value::createConstantU(0)) {
        return addInstruction(Opcode::TextureQueryDimension, ScalarType::U32,
                              {Value::createRawValue(const_buffer_index),
                               Value::createRawValue(dimension), lod});
    }

    // Mip level the texture is sampled at with the coordinates (float2 clamped
    // to the texture's levels, unclamped), 0 where the host has none
    Value opTextureQueryLod(u32 const_buffer_index, TextureType type,
                            const Value& coords, bool unclamped) {
        return addInstruction(
            Opcode::TextureQueryLod, ScalarType::F32,
            {Value::createRawValue(const_buffer_index),
             Value::createRawValue(type), coords,
             Value::createRawValue(static_cast<u32>(unclamped))});
    }

    // Bit counting
    Value opPopCount(const Value& src) {
        return addInstruction(Opcode::PopCount, ScalarType::U32, {src});
    }
    // Index of the most significant bit (sign bit excluded when signed), ~0
    // when not found
    Value opFindMsb(const Value& src, bool is_signed) {
        return addInstruction(Opcode::FindMsb, ScalarType::U32,
                              {src, Value::createRawValue(is_signed)});
    }

    // Compute
    Value opReadSystemRegister(u32 sr) {
        return addInstruction(Opcode::ReadSystemRegister, ScalarType::U32,
                              {Value::createRawValue(sr)});
    }
    // Shared memory is addressed in bytes and accessed in words
    Value opLoadShared(const Value& addr) {
        return addInstruction(Opcode::LoadShared, ScalarType::U32, {addr});
    }
    void opStoreShared(const Value& addr, const Value& value) {
        addInstructionWithDst(Opcode::StoreShared, std::nullopt,
                              {addr, value});
    }
    Value opAtomicShared(const Value& addr, const Value& value, u32 op, bool is_signed) {
        return addInstruction(Opcode::AtomicShared, ScalarType::U32,
                              {addr, value, Value::createRawValue(op), Value::createRawValue(is_signed)});
    }
    // Compare and swap, returns the previous value
    Value opAtomicSharedCas(const Value& addr, const Value& compare,
                            const Value& value) {
        return addInstruction(Opcode::AtomicSharedCas, ScalarType::U32,
                              {addr, compare, value});
    }
    // Storage buffers (global memory). `binding` is the constant buffer word
    // that holds the NVN descriptor (slot << 16 | word), offsets are in bytes
    // from the buffer start and `size` is the descriptor's byte size. Access
    // sizes: 0 U8, 1 S8, 2 U16, 3 S16, 4 B32
    Value opLoadStorage(u32 binding, const Value& offset, const Value& size,
                        u32 access) {
        return addInstruction(Opcode::LoadStorage, ScalarType::U32,
                              {Value::createRawValue(binding), offset, size,
                               Value::createRawValue(access)});
    }
    void opStoreStorage(u32 binding, const Value& offset, const Value& size,
                        const Value& value, u32 access) {
        addInstructionWithDst(Opcode::StoreStorage, std::nullopt,
                              {Value::createRawValue(binding), offset, size,
                               value, Value::createRawValue(access)});
    }
    // Returns the previous value. Operations as in ATOM (0 add ... 8 exchange)
    // plus 9 for a floating point add
    Value opAtomicStorage(u32 binding, const Value& offset, const Value& size,
                          const Value& value, u32 op, bool is_signed) {
        return addInstruction(Opcode::AtomicStorage, ScalarType::U32,
                              {Value::createRawValue(binding), offset, size,
                               value, Value::createRawValue(op),
                               Value::createRawValue(is_signed)});
    }
    // Compare and swap, returns the previous value
    Value opAtomicStorageCas(u32 binding, const Value& offset,
                             const Value& size, const Value& compare,
                             const Value& value) {
        return addInstruction(Opcode::AtomicStorageCas, ScalarType::U32,
                              {Value::createRawValue(binding), offset, size,
                               compare, value});
    }
    // Per-thread local memory, addressed in bytes and accessed in words
    Value opLoadLocal(const Value& addr) {
        return addInstruction(Opcode::LoadLocal, ScalarType::U32, {addr});
    }
    void opStoreLocal(const Value& addr, const Value& value) {
        addInstructionWithDst(Opcode::StoreLocal, std::nullopt,
                              {addr, value});
    }
    void opBarrier() { addInstructionWithDst(Opcode::Barrier); }
    // Geometry shader output (OUT)
    void opEmitVertex() { addInstructionWithDst(Opcode::EmitVertex); }
    void opEndPrimitive() { addInstructionWithDst(Opcode::EndPrimitive); }
    // Raw sizes of images accessed as raw words (SULD.D, SUST.D); formatted
    // accesses convert the texel to or from channel values
    // Store of 4 raw register values, formatted or as raw words of the given
    // size (U8, S8, U16, S16, B32, B64, B128 as 0-6)
    void opImageStore(u32 index, TextureType type, const Value& coords,
                      const std::array<Value, 4>& values,
                      u32 raw_size = IMAGE_FORMATTED) {
        addInstructionWithDst(Opcode::ImageStore, std::nullopt,
                              {Value::createRawValue(index),
                               Value::createRawValue(type), coords, values[0],
                               values[1], values[2], values[3],
                               Value::createRawValue(raw_size)});
    }
    // Load of 4 raw words (channel bits, or the texel's words when raw)
    Value opImageLoad(u32 index, TextureType type, const Value& coords,
                      u32 raw_size = IMAGE_FORMATTED) {
        return addInstruction(Opcode::ImageLoad,
                              Type::createVector(ScalarType::U32, 4),
                              {Value::createRawValue(index),
                               Value::createRawValue(type), coords,
                               Value::createRawValue(raw_size)});
    }
    // Returns the previous value
    Value opImageAtomic(u32 index, TextureType type, u32 op,
                        const Value& coords, const Value& value) {
        return addInstruction(Opcode::ImageAtomic, ScalarType::U32,
                              {Value::createRawValue(index),
                               Value::createRawValue(type),
                               Value::createRawValue(op), coords, value});
    }

    // Warp
    // mode: 0 = all, 1 = any, 2 = equal
    Value opVote(const Value& pred, u32 mode) {
        return addInstruction(Opcode::Vote, ScalarType::Bool,
                              {pred, Value::createRawValue(mode)});
    }
    Value opBallot(const Value& pred) {
        return addInstruction(Opcode::Ballot, ScalarType::U32, {pred});
    }
    Value opShuffle(const Value& value, const Value& lane) {
        return addInstruction(Opcode::Shuffle, ScalarType::U32, {value, lane});
    }

    // Intrinsic
    Value opIntrinsic(Intrinsic intrinsic, Type dst_type,
                      const std::vector<Value>& args) {
        std::vector<Value> operands;
        operands.reserve(args.size() + 1);
        operands.push_back(Value::createRawValue(intrinsic));
        operands.insert(operands.end(), args.begin(), args.end());
        return addInstruction(Opcode::Intrinsic, dst_type, operands);
    }

    // Exit
    void opExit() { addInstructionWithDst(Opcode::Exit); }
    void opDiscard() { addInstructionWithDst(Opcode::Discard); }

  protected:
    Module& modul;

    Function* insert_func;
    Block* insert_block;

    void addInstructionWithDst(Opcode opcode,
                               const std::optional<Value> dst = std::nullopt,
                               const std::vector<Value>& operands = {}) {
        ASSERT_DEBUG(insert_block, ShaderDecompiler, "No insert block");
        insert_block->addInstruction(opcode, dst, operands);
    }

    Value addInstruction(Opcode opcode, Type dst_type,
                         const std::vector<Value>& operands = {}) {
        ASSERT_DEBUG(insert_block, ShaderDecompiler, "No insert block");
        const auto dst = insert_block->createLocal(dst_type);
        insert_block->addInstruction(opcode, dst, operands);
        return dst;
    }

  public:
    void setInsertFunction(const std::string& name) {
        insert_func = &modul.getFunction(name);
    }

    void setInsertBlock(const label_t label) {
        ASSERT_DEBUG(insert_func, ShaderDecompiler, "No insert function");
        insert_block = &insert_func->getBlock(label);
    }
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::ir
