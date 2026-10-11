#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/analyzer/memory_analyzer.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::analyzer {

namespace {

void pushSv(std::unordered_set<SvSemantic>& svs,
            std::unordered_set<u8>& stage_in_outs, u64 addr) {
    const auto sv = getSvAccessFromAddr(addr).sv;
    if (sv.semantic == SvSemantic::UserInOut)
        stage_in_outs.insert(sv.index);
    else
        svs.insert(sv.semantic);
}

} // namespace

void MemoryAnalyzer::analyze(const ir::Module& modul) {
    for (const auto& [name, function] : modul.getFunctions()) {
        for (const auto& [label, block] : function.getBlocks()) {
            for (const auto& instruction : block.getInstructions()) {
                // Dst
                if (instruction.hasDst()) {
                    const auto& dst = instruction.getDst();
                    // NOLINTNEXTLINE(readability-trivial-switch)
                    switch (dst.getKind()) {
                    case ir::ValueKind::AttrMemory:
                        handleAMemStore(dst.getAttrMemory());
                        break;
                    default:
                        break;
                    }
                }

                // Operands
                for (const auto& operand : instruction.getOperands()) {
                    switch (operand.getKind()) {
                    case ir::ValueKind::AttrMemory:
                        handleAMemLoad(operand.getAttrMemory());
                        break;
                    case ir::ValueKind::ConstMemory:
                        handleCMemLoad(operand.getConstMemory());
                        break;
                    default:
                        break;
                    }
                }

                // Texture
                switch (instruction.getOpcode()) {
                case ir::Opcode::TextureSample: {
                    const auto const_buffer_index =
                        instruction.getOperand(0).getRawValue<u32>();
                    const auto type =
                        instruction.getOperand(1).getRawValue<TextureType>();
                    const auto flags = instruction.getOperand(2)
                                           .getRawValue<TextureSampleFlags>();
                    bool is_depth =
                        any(flags & TextureSampleFlags::DepthCompare);
                    handleTextureAccess(
                        const_buffer_index,
                        TextureInfo{.type = type, .is_depth = is_depth});
                    break;
                }
                case ir::Opcode::TextureGather: {
                    const auto const_buffer_index =
                        instruction.getOperand(0).getRawValue<u32>();
                    const bool is_depth =
                        instruction.getOperand(3).getKind() !=
                        ir::ValueKind::Undefined;
                    handleTextureAccess(const_buffer_index,
                                        TextureInfo{.type = TextureType::_2D,
                                                    .is_depth = is_depth});
                    break;
                }
                case ir::Opcode::ImageStore:
                    handleImageAccess(
                        instruction.getOperand(0).getRawValue<u32>(),
                        instruction.getOperand(1).getRawValue<TextureType>(),
                        false);
                    break;
                case ir::Opcode::ImageAtomic:
                    handleImageAccess(
                        instruction.getOperand(0).getRawValue<u32>(),
                        instruction.getOperand(1).getRawValue<TextureType>(),
                        true);
                    break;
                case ir::Opcode::LoadLocal:
                case ir::Opcode::StoreLocal:
                    uses_local_memory = true;
                    break;
                // TODO: TextureQueryDimension?
                default:
                    break;
                }
            }
        }
    }
}

void MemoryAnalyzer::handleAMemLoad(const AMem amem) {
    // TODO: support indexing with src
    ASSERT_DEBUG(amem.reg == RZ, ShaderDecompiler,
                 "Indexing not implemented (src: {})", amem.reg);
    pushSv(input_svs, stage_inputs, amem.imm);
}

void MemoryAnalyzer::handleCMemLoad(const CMem cmem) {
    const_buffers.insert(cmem.idx);
}

void MemoryAnalyzer::handleAMemStore(const AMem amem) {
    // TODO: support indexing with src
    ASSERT_DEBUG(amem.reg == RZ, ShaderDecompiler,
                 "Indexing not implemented (src: {})", amem.reg);
    pushSv(output_svs, stage_outputs, amem.imm);
}

void MemoryAnalyzer::handleTextureAccess(u32 const_buffer_index,
                                         const TextureInfo& info) {
    const auto res = textures.emplace(const_buffer_index, info);
    if (!res.second && (res.first->second.type != info.type ||
                        res.first->second.is_depth != info.is_depth)) {
        // TODO: handle this
        LOG_WARN(ShaderDecompiler, "Texture type mismatch");
    }
}

void MemoryAnalyzer::handleImageAccess(u32 index, TextureType type,
                                       bool is_atomic) {
    const auto res = images.emplace(index, ImageInfo{type, is_atomic});
    if (!res.second) {
        if (res.first->second.type != type)
            LOG_WARN(ShaderDecompiler, "Image type mismatch");
        res.first->second.is_atomic |= is_atomic;
    }
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::analyzer
