#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/ir/module.hpp"
#include <set>

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::analyzer {

struct TextureInfo {
    TextureType type;
    bool is_depth;
};

struct ImageInfo {
    TextureType type;
    bool is_atomic;
    // Read by the shader (loads need a readable image)
    bool is_read{false};
};

class MemoryAnalyzer {
  public:
    void analyze(const ir::Module& modul);

  private:
    std::unordered_set<SvSemantic> input_svs;
    std::unordered_set<SvSemantic> output_svs;
    std::unordered_set<u8> stage_inputs;
    std::unordered_set<u8> stage_outputs;
    std::unordered_set<u32> const_buffers;
    std::map<u32, TextureInfo> textures;
    std::map<u32, ImageInfo> images;
    bool uses_local_memory{false};
    bool uses_call_stack{false};
    // Bit per clip distance the shader writes
    u32 clip_distance_outputs{0};
    std::unordered_set<u32> system_registers;
    std::set<ir::Intrinsic> intrinsics;

    // Helpers
    void handleAMemLoad(const AMem amem);
    void handleCMemLoad(const CMem cmem);
    void handleAMemStore(const AMem amem);
    void handleTextureAccess(u32 const_buffer_index, const TextureInfo& info);
    void handleImageAccess(u32 index, TextureType type, bool is_atomic,
                           bool is_read = false);

  public:
    CONST_REF_GETTER(input_svs, getInputSVs);
    CONST_REF_GETTER(output_svs, getOutputSVs);
    CONST_REF_GETTER(stage_inputs, getStageInputs);
    CONST_REF_GETTER(stage_outputs, getStageOutputs);
    CONST_REF_GETTER(const_buffers, getConstBuffers);
    CONST_REF_GETTER(textures, getTextures);
    CONST_REF_GETTER(images, getImages);
    CONST_REF_GETTER(uses_local_memory, getUsesLocalMemory);
    CONST_REF_GETTER(uses_call_stack, getUsesCallStack);
    GETTER(clip_distance_outputs, getClipDistanceOutputs);
    CONST_REF_GETTER(system_registers, getSystemRegisters);
    CONST_REF_GETTER(intrinsics, getIntrinsics);
    // The shader reads the lane ID or a lane mask
    bool usesLaneId() const {
        for (const auto sr : system_registers)
            if (sr == 0x00 || (sr >= 0x38 && sr <= 0x3c))
                return true;
        return false;
    }
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::analyzer
