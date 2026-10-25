#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/ir/intrinsic.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/ir/value.hpp"
#include <span>

namespace hydra::hw::tegra_x1::gpu::renderer {
struct GuestShaderState;
}

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::ir {
struct Instruction;
class Function;
class Module;
} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::ir

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::analyzer {
class MemoryAnalyzer;
}

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen {

class Emitter {
  public:
    Emitter(const DecompilerContext& context_,
            const analyzer::MemoryAnalyzer& memory_analyzer_,
            const GuestShaderState& state_, std::vector<u8>& out_code_,
            ResourceMapping& out_resource_mapping_)
        : context{context_}, memory_analyzer{memory_analyzer_}, state{state_},
          out_code{out_code_}, out_resource_mapping{out_resource_mapping_} {}
    virtual ~Emitter() = default;

    void emit(const ir::Module& modul);

  protected:
    const DecompilerContext& context;
    const analyzer::MemoryAnalyzer& memory_analyzer;
    const GuestShaderState& state;

    std::vector<u8>& out_code;
    ResourceMapping& out_resource_mapping;

    virtual void start() = 0;
    virtual void finish() = 0;

    void emitInstruction(const ir::Instruction& inst);

    // Emit

    virtual void emitFunction(const ir::Function& func) = 0;

    // Data
    virtual void emitCopy(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitCast(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitBitCast(const ir::Value& dst, const ir::Value& src) = 0;

    // Arithmetic
    virtual void emitAbs(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitNeg(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitAdd(const ir::Value& dst, const ir::Value& srcA,
                         const ir::Value& srcB) = 0;
    virtual void emitMultiply(const ir::Value& dst, const ir::Value& srcA,
                              const ir::Value& srcB) = 0;
    virtual void emitFma(const ir::Value& dst, const ir::Value& srcA,
                         const ir::Value& srcB, const ir::Value& srcC) = 0;
    virtual void emitQuadShuffleXor(const ir::Value& dst, const ir::Value& src,
                                    const ir::Value& mask) = 0;
    virtual void emitQuadShuffle(const ir::Value& dst, const ir::Value& src,
                                 const ir::Value& index) = 0;
    virtual void emitSwizzleAdd(const ir::Value& dst, const ir::Value& src_a,
                                const ir::Value& src_b, u32 swizzle) = 0;
    virtual void emitMin(const ir::Value& dst, const ir::Value& srcA,
                         const ir::Value& srcB) = 0;
    virtual void emitMax(const ir::Value& dst, const ir::Value& srcA,
                         const ir::Value& srcB) = 0;
    virtual void emitClamp(const ir::Value& dst, const ir::Value& srcA,
                           const ir::Value& srcB, const ir::Value& srcC) = 0;

    // Math
    virtual void emitIsNan(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitRound(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitFloor(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitCeil(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitTrunc(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitReciprocal(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitSin(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitCos(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitExp2(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitLog2(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitSqrt(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitReciprocalSqrt(const ir::Value& dst,
                                    const ir::Value& src) = 0;

    // Logical & Bitwise
    virtual void emitNot(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitBitwiseNot(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitBitwiseAnd(const ir::Value& dst, const ir::Value& srcA,
                                const ir::Value& srcB) = 0;
    virtual void emitBitwiseOr(const ir::Value& dst, const ir::Value& srcA,
                               const ir::Value& srcB) = 0;
    virtual void emitBitwiseXor(const ir::Value& dst, const ir::Value& srcA,
                                const ir::Value& srcB) = 0;
    virtual void emitShiftLeft(const ir::Value& dst, const ir::Value& src_a,
                               const ir::Value& src_b) = 0;
    virtual void emitShiftRight(const ir::Value& dst, const ir::Value& src_a,
                                const ir::Value& src_b) = 0;
    virtual void emitBitfieldExtract(const ir::Value& dst,
                                     const ir::Value& src_a,
                                     const ir::Value& src_b,
                                     const ir::Value& src_c) = 0;

    // Comparison & Selection
    virtual void emitCompareLess(const ir::Value& dst, const ir::Value& srcA,
                                 const ir::Value& srcB) = 0;
    virtual void emitCompareLessOrEqual(const ir::Value& dst,
                                        const ir::Value& srcA,
                                        const ir::Value& srcB) = 0;
    virtual void emitCompareGreater(const ir::Value& dst, const ir::Value& srcA,
                                    const ir::Value& srcB) = 0;
    virtual void emitCompareGreaterOrEqual(const ir::Value& dst,
                                           const ir::Value& srcA,
                                           const ir::Value& srcB) = 0;
    virtual void emitCompareEqual(const ir::Value& dst, const ir::Value& srcA,
                                  const ir::Value& srcB) = 0;
    virtual void emitCompareNotEqual(const ir::Value& dst,
                                     const ir::Value& srcA,
                                     const ir::Value& srcB) = 0;
    virtual void emitSelect(const ir::Value& dst, const ir::Value& cond,
                            const ir::Value& src_true,
                            const ir::Value& src_false) = 0;

    // Control flow
    virtual void emitBeginIf(const ir::Value& cond) = 0;
    virtual void emitEndIf() = 0;
    virtual void emitBranch(label_t target) = 0;
    virtual void emitPushReturn(label_t target) = 0;
    virtual void emitReturnBranch() = 0;
    virtual void emitBranchConditional(const ir::Value& cond,
                                       label_t target_true,
                                       label_t target_false) = 0;

    // Vector
    virtual void emitVectorExtract(const ir::Value& dst, const ir::Value& src,
                                   u8 index) = 0;
    virtual void emitVectorInsert(const ir::Value& dst, const ir::Value& src,
                                  u8 index) = 0;
    virtual void
    emitVectorConstruct(const ir::Value& dst,
                        const std::vector<ir::Value>& elements) = 0;

    // Texture
    virtual void emitTextureSample(
        const ir::Value& dst, u32 const_buffer_index, TextureType type,
        TextureSampleFlags flags, const ir::Value& array_index,
        const ir::Value& coords, const ir::Value& cmp_value,
        const ir::Value& lod, const ir::Value& ddx, const ir::Value& ddy,
        const ir::Value& offset, const ir::Value& offsets, u8 component) = 0;
    virtual void emitTextureGather(const ir::Value& dst, u32 const_buffer_index,
                                   const ir::Value& coords, u8 component,
                                   const ir::Value& cmp_value) = 0;
    virtual void emitTextureQueryDimension(const ir::Value& dst,
                                           u32 const_buffer_index,
                                           u32 dimension,
                                           const ir::Value& lod) = 0;
    virtual void emitTextureQueryLod(const ir::Value& dst,
                                     u32 const_buffer_index, TextureType type,
                                     const ir::Value& coords,
                                     bool unclamped) = 0;

    // Exit
    virtual void emitExit() = 0;
    virtual void emitDiscard() = 0;

    // Bit counting
    virtual void emitPopCount(const ir::Value& dst, const ir::Value& src) = 0;
    virtual void emitFindMsb(const ir::Value& dst, const ir::Value& src,
                             bool is_signed) = 0;

    // Compute
    virtual void emitReadSystemRegister(const ir::Value& dst, u32 sr) = 0;
    virtual void emitLoadShared(const ir::Value& dst,
                                const ir::Value& addr) = 0;
    virtual void emitStoreShared(const ir::Value& addr,
                                 const ir::Value& value) = 0;
    virtual void emitLoadLocal(const ir::Value& dst,
                               const ir::Value& addr) = 0;
    virtual void emitStoreLocal(const ir::Value& addr,
                                const ir::Value& value) = 0;
    virtual void emitAtomicShared(const ir::Value& dst, const ir::Value& addr,
                                  const ir::Value& value, u32 op, bool is_signed) = 0;
    virtual void emitAtomicSharedCas(const ir::Value& dst, const ir::Value& addr,
                                     const ir::Value& compare,
                                     const ir::Value& value) = 0;
    virtual void emitLoadStorage(const ir::Value& dst, u32 binding,
                                 const ir::Value& offset,
                                 const ir::Value& size, u32 access) = 0;
    virtual void emitStoreStorage(u32 binding, const ir::Value& offset,
                                  const ir::Value& size,
                                  const ir::Value& value, u32 access) = 0;
    virtual void emitAtomicStorage(const ir::Value& dst, u32 binding,
                                   const ir::Value& offset,
                                   const ir::Value& size,
                                   const ir::Value& value, u32 op,
                                   bool is_signed) = 0;
    virtual void emitAtomicStorageCas(const ir::Value& dst, u32 binding,
                                      const ir::Value& offset,
                                      const ir::Value& size,
                                      const ir::Value& compare,
                                      const ir::Value& value) = 0;
    virtual void emitBarrier() = 0;
    virtual void emitImageStore(u32 index, TextureType type,
                                const ir::Value& coords,
                                const std::array<ir::Value, 4>& values,
                                u32 raw_size) = 0;
    virtual void emitImageLoad(const ir::Value& dst, u32 index,
                               TextureType type, const ir::Value& coords,
                               u32 raw_size) = 0;
    virtual void emitImageAtomic(const ir::Value& dst, u32 index,
                                 TextureType type, u32 op,
                                 const ir::Value& coords,
                                 const ir::Value& value) = 0;

    // Warp
    virtual void emitVote(const ir::Value& dst, const ir::Value& pred,
                          u32 mode) = 0;
    virtual void emitBallot(const ir::Value& dst, const ir::Value& pred) = 0;
    virtual void emitShuffle(const ir::Value& dst, const ir::Value& value,
                             const ir::Value& lane) = 0;

    // Intrinsic
    virtual void emitIntrinsic(const ir::Value& dst, ir::Intrinsic intrinsic,
                               std::span<const ir::Value> args) = 0;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen
