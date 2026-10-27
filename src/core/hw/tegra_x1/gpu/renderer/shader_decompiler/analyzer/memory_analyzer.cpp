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

// Conservative unsigned address bounds within one straight-line block.
class AddressBounds {
  public:
    AddressBounds() { clear(); }
    void clear() {
        registers.fill(0xffffffffu);
        locals.clear();
    }
    u64 get(const ir::Value& value) const {
        if (value.getKind() == ir::ValueKind::Constant &&
            value.getType() == ir::ScalarType::I32 &&
            value.getConstant() <= 0x7fffffffu)
            return value.getConstant();
        if (value.getType() != ir::ScalarType::U32)
            return 0xffffffffu;
        switch (value.getKind()) {
        case ir::ValueKind::Constant: return value.getConstant();
        case ir::ValueKind::Register:
            return value.getRegister() == RZ ? 0u
                : registers[static_cast<u8>(value.getRegister())];
        case ir::ValueKind::Local: {
            const auto it = locals.find(value.getLocal().id);
            return it == locals.end() ? 0xffffffffu : it->second;
        }
        default: return 0xffffffffu;
        }
    }
    void update(const ir::Instruction& instruction) {
        using ir::Opcode;
        if (instruction.getOpcode() == Opcode::BeginIf ||
            instruction.getOpcode() == Opcode::EndIf)
            clear();
        if (!instruction.hasDst()) return;
        const auto& dst = instruction.getDst();
        u64 upper = 0xffffffffu;
        if (dst.getType() == ir::ScalarType::U32) {
            const auto bound = [&](usize index) { return get(instruction.getOperand(index)); };
            switch (instruction.getOpcode()) {
            case Opcode::Copy:
            case Opcode::Cast: upper = bound(0); break;
            case Opcode::Min:
            case Opcode::BitwiseAnd: upper = std::min(bound(0), bound(1)); break;
            case Opcode::Max: upper = std::max(bound(0), bound(1)); break;
            case Opcode::Add: upper = bound(0) + bound(1); break;
            case Opcode::ShiftLeft:
                if (bound(1) < 32) upper = bound(0) << bound(1);
                break;
            case Opcode::Select: {
                const auto& condition = instruction.getOperand(0);
                if (condition.getKind() == ir::ValueKind::Predicate &&
                    condition.getPredicate() == PT)
                    upper = bound(1);
                else if (condition.getKind() == ir::ValueKind::Constant)
                    upper = bound(condition.getConstant() ? 1 : 2);
                else
                    upper = std::max(bound(1), bound(2));
                break;
            }
            default: break;
            }
        }
        if (upper > 0xffffffffu) upper = 0xffffffffu;
        if (dst.getKind() == ir::ValueKind::Register)
            registers[static_cast<u8>(dst.getRegister())] = upper;
        else if (dst.getKind() == ir::ValueKind::Local) {
            if (upper != 0xffffffffu) locals[dst.getLocal().id] = upper;
            else locals.erase(dst.getLocal().id);
        }
    }
  private:
    std::array<u64, 256> registers;
    std::map<u32, u64> locals;
};

} // namespace

void MemoryAnalyzer::analyze(const ir::Module& modul,
                             const std::map<u32, TextureType>& type_hints) {
    // Textures that are only queried still need a declaration. Their type is
    // unknown, so other accesses decide it.
    std::vector<u32> queried_textures;
    for (const auto& [name, function] : modul.getFunctions()) {
        for (const auto& [label, block] : function.getBlocks()) {
            AddressBounds address_bounds;
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
                    {
                        auto cmem = operand.getConstMemory();
                        const auto upper = address_bounds.get(ir::Value::createRegister(cmem.reg));
                        if (cmem.reg != RZ && upper <= 0x1fffcu &&
                            cmem.imm <= 0x1fffcu - upper) {
                            cmem.imm += upper;
                            cmem.reg = RZ;
                        }
                        handleCMemLoad(cmem);
                        break;
                    }
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
                    handleCMemLoad(CMem(binding >> 16, RZ,
                                         u64(binding & 0xffff) * 4 + 8));
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
                address_bounds.update(instruction);
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
    // Per patch attributes are not stage inputs
    if (amem.patch)
        return;
    // TODO: support indexing with src
    // Not an assertion: release builds must not translate this silently wrong
    if (amem.reg != RZ && !geometry)
        SHADER_UNSUPPORTED_ONCE("Indexed attribute load (src: {})", amem.reg);
    pushSv(input_svs, stage_inputs, amem.imm);
}

void MemoryAnalyzer::handleCMemLoad(const CMem cmem) {
    const_buffers.insert(cmem.idx);
    if (cmem.reg != RZ || cmem.imm > 0x1fffcu) {
        indexed_const_buffers.insert(cmem.idx);
        const_buffer_sizes[cmem.idx] = 0;
    } else if (!indexed_const_buffers.contains(cmem.idx)) {
        auto& size = const_buffer_sizes[cmem.idx];
        size = std::max(size, (cmem.imm & ~u64{3}) + 4);
    }
}

void MemoryAnalyzer::handleAMemStore(const AMem amem) {
    // Per patch attributes are not stage outputs
    if (amem.patch)
        return;
    // TODO: support indexing with src
    if (amem.reg != RZ && !geometry)
        SHADER_UNSUPPORTED_ONCE("Indexed attribute store (src: {})", amem.reg);
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
