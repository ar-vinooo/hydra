#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/analyzer/memory_analyzer.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::analyzer {

namespace {

SvAccess pushSv(std::unordered_set<SvSemantic>& svs,
                std::unordered_set<u8>& stage_in_outs, u64 addr) {
    const auto access = getSvAccessFromAddr(addr);
    if (access.sv.semantic == SvSemantic::UserInOut)
        stage_in_outs.insert(access.sv.index);
    else if (access.sv.semantic != SvSemantic::Invalid)
        svs.insert(access.sv.semantic);
    return access;
}

} // namespace

void MemoryAnalyzer::analyze(const ir::Module& modul,
                             const std::map<u32, TextureType>& type_hints) {
    // Textures that are only queried still need a declaration. Their type is
    // unknown, so other accesses decide it.
    std::vector<u32> queried_textures;
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
                case ir::Opcode::ImageLoad:
                    handleImageAccess(
                        instruction.getOperand(0).getRawValue<u32>(),
                        instruction.getOperand(1).getRawValue<TextureType>(),
                        false, true);
                    break;
                case ir::Opcode::ImageAtomic:
                    handleImageAccess(
                        instruction.getOperand(0).getRawValue<u32>(),
                        instruction.getOperand(1).getRawValue<TextureType>(),
                        true);
                    break;
                case ir::Opcode::LoadStorage:
                case ir::Opcode::StoreStorage:
                case ir::Opcode::AtomicStorage:
                case ir::Opcode::AtomicStorageCas: {
                    const auto binding =
                        instruction.getOperand(0).getRawValue<u32>();
                    auto& info = storage_buffers[binding];
                    info.written |=
                        instruction.getOpcode() != ir::Opcode::LoadStorage;
                    // The descriptor is read from the constant buffer
                    const_buffers.insert(binding >> 16);
                    break;
                }
                case ir::Opcode::LoadLocal:
                case ir::Opcode::StoreLocal:
                    uses_local_memory = true;
                    break;
                case ir::Opcode::PushReturn:
                case ir::Opcode::ReturnBranch:
                    uses_call_stack = true;
                    break;
                case ir::Opcode::ReadSystemRegister:
                    system_registers.insert(
                        instruction.getOperand(0).getRawValue<u32>());
                    break;
                case ir::Opcode::Intrinsic:
                    intrinsics.insert(
                        instruction.getOperand(0).getRawValue<ir::Intrinsic>());
                    break;
                case ir::Opcode::TextureQueryLod:
                    // Needs the sampler like a sample does
                    handleTextureAccess(
                        instruction.getOperand(0).getRawValue<u32>(),
                        TextureInfo{.type = instruction.getOperand(1)
                                                .getRawValue<TextureType>(),
                                    .is_depth = false});
                    break;
                case ir::Opcode::TextureQueryDimension:
                    queried_textures.push_back(
                        instruction.getOperand(0).getRawValue<u32>());
                    break;
                default:
                    break;
                }
            }
        }
    }

    for (const auto index : queried_textures) {
        const auto hint = type_hints.find(index);
        const bool hinted = hint != type_hints.end();
        textures.emplace(index,
                         TextureInfo{.type = hinted ? hint->second
                                                    : TextureType::_2D,
                                     .is_depth = false,
                                     .type_guessed = !hinted});
    }
}

void MemoryAnalyzer::handleAMemLoad(const AMem amem) {
    // TODO: support indexing with src
    // Not an assertion: release builds must not translate this silently wrong
    if (amem.reg != RZ)
        ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler,
                                 "Indexed attribute load (src: {})", amem.reg));
    pushSv(input_svs, stage_inputs, amem.imm);
}

void MemoryAnalyzer::handleCMemLoad(const CMem cmem) {
    const_buffers.insert(cmem.idx);
}

void MemoryAnalyzer::handleAMemStore(const AMem amem) {
    // TODO: support indexing with src
    if (amem.reg != RZ)
        ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler,
                                 "Indexed attribute store (src: {})", amem.reg));
    const auto access = pushSv(output_svs, stage_outputs, amem.imm);
    if (access.sv.semantic == SvSemantic::ClipDistance)
        clip_distance_outputs |=
            1u << (access.sv.index * 4 + access.component_index);
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
                                       bool is_atomic, bool is_read) {
    const auto res = images.emplace(index, ImageInfo{type, is_atomic, is_read});
    if (!res.second) {
        if (res.first->second.type != type)
            LOG_WARN(ShaderDecompiler, "Image type mismatch");
        res.first->second.is_atomic |= is_atomic;
        res.first->second.is_read |= is_read;
    }
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::analyzer
