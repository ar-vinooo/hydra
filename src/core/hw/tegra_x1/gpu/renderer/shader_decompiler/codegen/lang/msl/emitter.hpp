#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/emitter.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen::lang::
    msl {

class MslEmitter final : public LangEmitter {
  public:
    MslEmitter(const DecompilerContext& context,
               const analyzer::MemoryAnalyzer& memory_analyzer,
               const GuestShaderState& state, std::vector<u8>& out_code,
               ResourceMapping& out_resource_mapping);

  protected:
    // Emit

    void emitHeader() override;
    void emitTypeAliases() override;
    void emitDeclarations() override;
    void emitStateBindings() override;
    void emitStateBindingAssignments() override;
    void emitMainPrototype() override;
    void emitExitReturn() override;

    // Data
    void emitBitCast(const ir::Value& dst, const ir::Value& src) override;

    // Math
    void emitIsNan(const ir::Value& dst, const ir::Value& src) override;
    void emitReciprocal(const ir::Value& dst, const ir::Value& src) override;
    void emitSin(const ir::Value& dst, const ir::Value& src) override;
    void emitCos(const ir::Value& dst, const ir::Value& src) override;
    void emitExp2(const ir::Value& dst, const ir::Value& src) override;
    void emitLog2(const ir::Value& dst, const ir::Value& src) override;
    void emitSqrt(const ir::Value& dst, const ir::Value& src) override;
    void emitReciprocalSqrt(const ir::Value& dst,
                            const ir::Value& src) override;

    // Logical & Bitwise
    void emitBitfieldExtract(const ir::Value& dst, const ir::Value& src_a,
                             const ir::Value& src_b,
                             const ir::Value& src_c) override;

    // Texture
    void emitTextureSample(const ir::Value& dst, u32 const_buffer_index,
                           TextureType type, TextureSampleFlags flags,
                           const ir::Value& array_index,
                           const ir::Value& coords, const ir::Value& cmp_value,
                           const ir::Value& lod, const ir::Value& ddx,
                           const ir::Value& ddy, const ir::Value& offset,
                           const ir::Value& offsets, u8 component) override;
    void emitQuadShuffleXor(const ir::Value& dst, const ir::Value& src,
                            const ir::Value& mask) override;
    void emitQuadShuffle(const ir::Value& dst, const ir::Value& src,
                         const ir::Value& index) override;
    void emitSwizzleAdd(const ir::Value& dst, const ir::Value& src_a,
                        const ir::Value& src_b, u32 swizzle) override;
    void emitTextureGather(const ir::Value& dst, u32 const_buffer_index,
                           const ir::Value& coords, u8 component,
                           const ir::Value& cmp_value) override;
    void emitTextureQueryLod(const ir::Value& dst, u32 const_buffer_index,
                             TextureType type, const ir::Value& coords,
                             bool unclamped) override;
    void emitTextureQueryDimension(const ir::Value& dst, u32 const_buffer_index,
                                   u32 dimension,
                                   const ir::Value& lod) override;

    // Exit
    void emitDiscard() override;

    // Bit counting
    void emitPopCount(const ir::Value& dst, const ir::Value& src) override;
    void emitFindMsb(const ir::Value& dst, const ir::Value& src,
                     bool is_signed) override;

    // Compute
    void emitReadSystemRegister(const ir::Value& dst, u32 sr) override;
    void emitLoadShared(const ir::Value& dst, const ir::Value& addr) override;
    void emitStoreShared(const ir::Value& addr,
                         const ir::Value& value) override;
    void emitLoadLocal(const ir::Value& dst, const ir::Value& addr) override;
    void emitStoreLocal(const ir::Value& addr,
                        const ir::Value& value) override;
    void emitAtomicShared(const ir::Value& dst, const ir::Value& addr,
                          const ir::Value& value, u32 op, bool is_signed) override;
    void emitAtomicSharedCas(const ir::Value& dst, const ir::Value& addr,
                             const ir::Value& compare,
                             const ir::Value& value) override;
    void emitLoadStorage(const ir::Value& dst, u32 binding,
                         const ir::Value& offset, const ir::Value& size,
                         u32 access) override;
    void emitStoreStorage(u32 binding, const ir::Value& offset,
                          const ir::Value& size, const ir::Value& value,
                          u32 access) override;
    void emitAtomicStorage(const ir::Value& dst, u32 binding,
                           const ir::Value& offset, const ir::Value& size,
                           const ir::Value& value, u32 op,
                           bool is_signed) override;
    void emitAtomicStorageCas(const ir::Value& dst, u32 binding,
                              const ir::Value& offset, const ir::Value& size,
                              const ir::Value& compare,
                              const ir::Value& value) override;
    void emitBarrier() override;
    void emitImageStore(u32 index, TextureType type, const ir::Value& coords,
                        const std::array<ir::Value, 4>& values,
                        u32 raw_size) override;
    void emitImageLoad(const ir::Value& dst, u32 index, TextureType type,
                       const ir::Value& coords, u32 raw_size) override;
    void emitImageAtomic(const ir::Value& dst, u32 index, TextureType type,
                         u32 op, const ir::Value& coords,
                         const ir::Value& value) override;

    // Warp
    void emitVote(const ir::Value& dst, const ir::Value& pred,
                  u32 mode) override;
    void emitBallot(const ir::Value& dst, const ir::Value& pred) override;
    void emitShuffle(const ir::Value& dst, const ir::Value& value,
                     const ir::Value& lane) override;

    // Intrinsic
    void emitIntrinsic(const ir::Value& dst, ir::Intrinsic intrinsic,
                       std::span<const ir::Value> args) override;

    std::string getSvAccessQualifiedStr(const SvAccess& sv_access,
                                        bool output) override;

  private:
    // Number of shared memory words (compute)
    u32 shared_word_count{1};
    u32 local_word_count{1};

    // Helpers
    static std::string getSvStr(const Sv& sv);
    static std::string getStorageBufferName(u32 binding);
    // Bounds check and coordinate arguments for an image access
    std::string getImageBoundsStr(u32 index, TextureType type,
                                  const std::string& coords);
    static std::string getImageCoordsStr(TextureType type,
                                         const std::string& coords);
    std::string getSvQualifierStr(const Sv& sv, bool output);
    // Copies the fragment system values the shader reads to attribute memory
    void emitFragmentSystemValueInputs();
    u32 getClipDistanceCount() const;
};

} // namespace
  // hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen::lang::msl
