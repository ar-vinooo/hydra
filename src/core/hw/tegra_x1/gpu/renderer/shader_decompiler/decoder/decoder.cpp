#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/decoder.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/bitfield.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/compute.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/conversion.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/exit.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/float_arithmetic.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/float_comparison.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/float_min_max.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/half_arithmetic.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/half_comparison.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/integer_arithmetic.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/integer_comparison.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/integer_logical.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/integer_misc.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/memory.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/move.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/multifunction.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/predicate_comparison.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/shift.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/warp.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/double_arithmetic.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/video.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder {

namespace {

// OUT: emits the vertex built so far and/or ends the strip. The result
// register (the next vertex handle) is not written: attribute stores and OUT
// address the vertex being built regardless of handles.
void emitOut(ir::Builder& b, u64 inst, bool rasterized_stream) {
    const auto conditional =
        handlePredCond(b, static_cast<pred_t>(extractBits(inst, 16, 3)),
                       extractBits(inst, 19, 1) != 0);
    const auto type = extractBits(inst, 39, 2);
    if (rasterized_stream) {
        if (type == 0)
            SHADER_UNSUPPORTED("OUT type 0");
        if ((type & 0x1) != 0)
            b.opEmitVertex();
        if ((type & 0x2) != 0)
            b.opEndPrimitive();
    }
    if (conditional)
        b.opEndIf();
}

} // namespace

void Decoder::decode() {
    crnt_block = &blocks[0x0];
    while (crnt_block != nullptr) {
        parseNextInstruction();
    }
}

// Compiler-generated switches use a clamped index, optionally a SHL, then
// either branch directly or load the offset from a constant-buffer table.
// Enumerate only the proven bounded targets; arbitrary indirect addresses are
// never treated as shader code.
bool Decoder::emitIndirectBranch(instruction_t inst, u32 pc) {
    // Target validation also depends on the available code window.
    if (context.decomp_context.code_read_size)
        *context.decomp_context.code_read_size = invalid<u64>();
    if ((inst & 0x60u) != 0)
        return false; // cbuf mode / LMT
    const auto saved = context.code_stream->getSeek();
    const reg_t branch_reg = static_cast<u8>(extractBits(inst, 8, 8));
    reg_t index_reg = branch_reg;
    std::optional<InstLdc> table;
    u32 shift = 0;
    std::optional<u32> bound;
    for (u32 distance = 1; distance <= 32 && distance <= pc; ++distance) {
        const u32 at = pc - distance;
        if (at % 4 == 0)
            continue;
        context.code_stream->seekTo(at * sizeof(instruction_t));
        const auto x = context.code_stream->read<instruction_t>();
        if (static_cast<u8>(x & 255) != static_cast<u8>(index_reg))
            continue;
        const auto op = x & 0xfff0000000000000ull;
        if (op == 0x5b60000000000000ull || op == 0x4b60000000000000ull ||
            op == 0x5bb0000000000000ull || op == 0x4bb0000000000000ull)
            continue;
        if (extractBits(x, 16, 4) != 7)
            break;
        if (!table && (x & 0xfff8000000000000ull) == 0xef90000000000000ull) {
            const auto load = std::bit_cast<InstLdc>(x);
            if (load.size.get() != LsSize2::B32 ||
                static_cast<u32>(load.address_mode.get()) != 0 ||
                load.src.get() == RZ)
                break;
            table = load;
            index_reg = load.src.get();
            continue;
        }
        if ((x & 0xfef8000000000000ull) == 0x3848000000000000ull) {
            if (shift != 0 || extractBits(x, 39, 1) != 0 ||
                extractBits(x, 56, 1) != 0)
                break;
            shift = static_cast<u32>(extractBits(x, 20, 19));
            if (shift > 8)
                break;
            index_reg = static_cast<u8>(extractBits(x, 8, 8));
            continue;
        }
        if ((x & 0xfef8000000000000ull) == 0x3820000000000000ull &&
            extractBits(x, 39, 4) == 7 && extractBits(x, 56, 1) == 0 &&
            extractBits(x, 48, 1) == 0) {
            const auto n = static_cast<u32>(extractBits(x, 20, 19));
            if (n <= 255)
                bound = n;
        }
        break;
    }
    context.code_stream->seekTo(saved);
    if (!bound || (table && !context.decomp_context.read_const_word))
        return false;
    std::map<u32, label_t> targets;
    std::map<std::pair<u32, u32>, u32> constants;
    const bool absolute = (inst & 0xfff0000000000000ull) == 0xe200000000000000ull;
    const i64 relative = absolute
        ? std::bit_cast<i32>(static_cast<u32>(extractBits(inst, 20, 32)))
        : signExtend<i32, 24>(static_cast<i32>(extractBits(inst, 20, 24)));
    for (u32 index = 0; index <= *bound; ++index) {
        u32 offset = index << shift;
        if (table) {
            const u32 byte_offset = table->cbuf_offset.get() + offset;
            if (byte_offset % 4 != 0)
                return false;
            const auto value = context.decomp_context.read_const_word(
                table->cbuf_slot.get(), byte_offset / 4);
            if (!value)
                return false;
            offset = *value;
            constants[{table->cbuf_slot.get(), byte_offset / 4}] = offset;
        }
        const i64 address = (absolute ? -static_cast<i64>(context.code_offset)
                                      : static_cast<i64>(pc + 1) * 8) +
                            relative + std::bit_cast<i32>(offset);
        if (address < 0 || address % 8 != 0 ||
            static_cast<u64>(address) >= context.code_stream->getSize())
            return false;
        targets.emplace(offset, static_cast<u32>(address / 8));
    }
    if (context.decomp_context.branch_constants)
        context.decomp_context.branch_constants->insert(constants.begin(),
                                                        constants.end());
    auto& b = context.builder;
    const auto pred = pred_t(extractBits(inst, 16, 3));
    const bool pred_inv = extractBits(inst, 19, 1) != 0;
    const auto flow_test = static_cast<u32>(extractBits(inst, 0, 5));
    const bool conditional = pred != PT || pred_inv || flow_test != 15;
    if (conditional)
        b.opBeginIf(getFlowCondition(b, pred, pred_inv, flow_test));
    // Capture the input before a target block can change its register.
    const auto offset_value = b.opCopy(ir::Value::createRegister(branch_reg));
    for (const auto& [offset, target] : targets) {
        inheritSyncPoints(target);
        b.opBeginIf(
            b.opCompareEqual(offset_value, ir::Value::createConstantU(offset)));
        b.opBranch(target);
        b.opEndIf();
    }
    b.opExit(); // malformed runtime table offset
    if (conditional) {
        b.opEndIf();
        inheritSyncPoints(pc + 1);
        b.opBranch(pc + 1);
    }
    endBlock();
    return true;
}

void Decoder::parseNextInstruction() {
    const u32 pc = getPc();
    // Control flow that leaves the shader (unsupported instructions, garbage
    // branch targets) must not read arbitrary memory
    const auto seek = context.code_stream->getSeek();
    const auto size = context.code_stream->getSize();
    if (seek > size || size - seek < sizeof(instruction_t)) {
        if (context.decomp_context.code_read_size)
            *context.decomp_context.code_read_size = invalid<u64>();
        LOG_ERROR(ShaderDecompiler, "Shader control flow left the code at {:#x}",
                  pc);
        context.builder.opExit();
        endBlock();
        return;
    }
    const auto inst = context.code_stream->read<instruction_t>();
    if (context.decomp_context.code_read_size)
        *context.decomp_context.code_read_size = std::max(
            *context.decomp_context.code_read_size,
            static_cast<u64>(seek + sizeof(instruction_t)));
    if ((pc % 4) == 0) // Sched
        return;

    LOG_DEBUG(ShaderDecompiler, "Instruction 0x{:016x}", inst);
    auto& visual = VisualDiagnostics::instance();
    visual.decoded(pc, inst, "instruction");

#define INST0(value, mask) if ((inst & mask##ull) == value##ull)
#define INST(value, mask) else INST0(value, mask)

#define EMIT(op) do { visual.decoded(pc, inst, #op); emit##op(context, std::bit_cast<Inst##op>(inst)); } while (false)

    // OUT (envytools gm107.c): bits 39-40 emit (1), cut (2) or both (3) for
    // the vertex stream in bits 20+. Only stream 0 is rasterized.
    INST0(0xfbe0000000000000, 0xfff8000000000000) {
        if (static_cast<reg_t>(extractBits(inst, 20, 8)) != RZ)
            SHADER_UNSUPPORTED("OUT with a register stream");
        emitOut(context.builder, inst, true);
    }
    INST(0xf6e0000000000000, 0xfef8000000000000) {
        emitOut(context.builder, inst, (extractBits(inst, 20, 20) & 0x3) == 0);
    }
    INST(0xebe0000000000000, 0xfff8000000000000) {
        SHADER_UNSUPPORTED("OUT with a constant buffer stream");
        emitOut(context.builder, inst, true);
    }
    INST(0xf0f8000000000000, 0xfff8000000000000) { // sync
                const auto pred = static_cast<pred_t>(extractBits(inst, 16, 3));
        const auto pred_inv = static_cast<bool>(extractBits(inst, 19, 1));

        if (crnt_block->sync_point_stack.empty()) {
            // Reading the top of an empty stack is undefined behavior
            ONCE(LOG_ERROR(ShaderDecompiler, "No sync point in stack at {:#x}",
                           pc));
            context.builder.opExit();
            endBlock();
            return;
        }

        const auto target = crnt_block->sync_point_stack.top();
        if (pred == PT && extractBits(inst, 0, 5) == 15) {
            if (!pred_inv) {
                // Pop and then inherit
                crnt_block->sync_point_stack.pop();
                inheritSyncPoints(target);

                context.builder.opBranch(target);
                endBlock();
            } else {
                // Never taken: execution falls through to the next
                // instruction and the sync point stays on the stack
            }
        } else { // Conditional
            // Inherit for the continuation block, then pop and inherit for sync
            // block
            inheritSyncPoints(pc + 1);
            // TODO: should this pop? (probably not)
            crnt_block->sync_point_stack.pop();
            inheritSyncPoints(target);

            context.builder.opBranchConditional(
                getFlowCondition(context.builder, pred, pred_inv, static_cast<u32>(extractBits(inst, 0, 5))),
                target, pc + 1);
            endBlock();
        }
    }
    INST(0xf0f0000000000000, 0xfff8000000000000) {
        // Waits for scoreboard dependencies, instructions execute in order
        COMMENT("depbar");
    }
    INST(0xf0c8000000000000, 0xfff8000000000000) { EMIT(S2R); }
    INST(0xf0c0000000000000, 0xfff8000000000000) {
        COMMENT_NOT_IMPLEMENTED("r2b");
    }
    INST(0xf0b8000000000000, 0xfff8000000000000) {
        COMMENT_NOT_IMPLEMENTED("b2r");
    }
    INST(0xf0a8000000000000, 0xfff8000000000000) { emitBar(context, inst); }
    INST(0xeff0000000000000, 0xfff8000000000000) { EMIT(Sta); }
    INST(0xefe8000000000000, 0xfff8000000000000) { // pixld
        // Values of the pixel: its coverage mask or the sample index
        const auto mode = static_cast<u32>(extractBits(inst, 31, 3));
        u32 sr = invalid<u32>();
        if (mode == 1)
            sr = SR_PIXEL_COVERAGE_MASK;
        else if (mode == 5)
            sr = SR_PIXEL_SAMPLE_INDEX;
        if (sr == invalid<u32>()) {
            COMMENT_NOT_IMPLEMENTED("pixld mode {}", mode);
        } else {
            auto& b = context.builder;
            const auto conditional = handlePredCond(
                b, static_cast<pred_t>(extractBits(inst, 16, 3)),
                extractBits(inst, 19, 1) != 0);
            b.opCopy(ir::Value::createRegister(
                         static_cast<reg_t>(extractBits(inst, 0, 8)),
                         ir::ScalarType::U32),
                     b.opReadSystemRegister(sr));
            // What the predicate output holds is not documented
            if (static_cast<pred_t>(extractBits(inst, 45, 3)) != PT)
                SHADER_UNSUPPORTED("PIXLD predicate output");
            if (conditional)
                b.opEndIf();
        }
    }
    INST(0xefd8000000000000, 0xfff8000000000000) { EMIT(Lda); }
    INST(0xefd0000000000000, 0xfff8000000000000) {
        // ISBERD: the plain form gives the vertex index for ALD as is
        // (yuzu, Ryujinx); skew, o, the patch/prim/attr modes and sizes are
        // not known
        if (extractBits(inst, 31, 4) != 0 || extractBits(inst, 47, 2) != 0)
            SHADER_UNSUPPORTED("ISBERD mode {:#x}", extractBits(inst, 31, 18));
        auto& b = context.builder;
        const auto conditional = handlePredCond(
            b, static_cast<pred_t>(extractBits(inst, 16, 3)),
            extractBits(inst, 19, 1) != 0);
        const auto dst = static_cast<reg_t>(extractBits(inst, 0, 8));
        if (dst != RZ)
            b.opCopy(ir::Value::createRegister(dst),
                     ir::Value::createRegister(
                         static_cast<reg_t>(extractBits(inst, 8, 8))));
        if (conditional)
            b.opEndIf();
    }
    INST(0xefa0000000000000, 0xfff8000000000000) {
        COMMENT_NOT_IMPLEMENTED("al2p");
    }
    INST(0xef98000000000000, 0xfff8000000000000) {
        // Accesses are ordered by barriers
        COMMENT("membar");
    }
    INST(0xef90000000000000, 0xfff8000000000000) { EMIT(Ldc); }
    INST(0xef80000000000000, 0xffe0000000000000) {
        // Cache control of local memory: memory is coherent on Metal
        COMMENT("cctll");
    }
    INST(0xef60000000000000, 0xffe0000000000000) {
        // Cache control (invalidate, write back): memory is coherent on
        // Metal, ordering comes from MEMBAR
        COMMENT("cctl");
    }
    INST(0xef58000000000000, 0xfff8000000000000) { emitSts(context, inst); }
    INST(0xef50000000000000, 0xfff8000000000000) { emitStl(context, inst); }
    INST(0xef48000000000000, 0xfff8000000000000) { emitLds(context, inst); }
    INST(0xef40000000000000, 0xfff8000000000000) { emitLdl(context, inst); }
    INST(0xef10000000000000, 0xfff8000000000000) { EMIT(Shfl); }
    INST(0xeef0000000000000, 0xfff0000000000000) { EMIT(AtomCas); }
    INST(0xeed8000000000000, 0xfff8000000000000) { EMIT(Stg); }
    INST(0xeed0000000000000, 0xfff8000000000000) { EMIT(Ldg); }
    INST(0xeec8000000000000, 0xfff8000000000000) {
        COMMENT_NOT_IMPLEMENTED("ldg");
    }
    INST(0xeea0000000000000, 0xfff8000000000000) {
        COMMENT_NOT_IMPLEMENTED("stp");
    }
    INST(0xee00000000000000, 0xff80000000000000) {
        emitAtomsCas(context, inst);
    }
    INST(0xed00000000000000, 0xff00000000000000) { EMIT(Atom); }
    INST(0xec00000000000000, 0xff00000000000000) {
        emitAtoms(context, inst);
    }
    INST(0xebf8000000000000, 0xfff8000000000000) { EMIT(Red); }
    INST(0xebf0000000000000, 0xfff9000000000000) {
        // Texture cache control: texture memory is coherent on Metal
        COMMENT("cctlt");
    }
    INST(0xebe0000000000000, 0xfff8000000000000) {
        COMMENT_NOT_IMPLEMENTED("out");
    }
    INST(0xeb40000000000000, 0xffe0000000000000) { emitSured(context, inst); }
    INST(0xeb20000000000000, 0xffe0000000000000) { emitSust(context, inst); }
    INST(0xeb00000000000000, 0xffe0000000000000) { emitSuld(context, inst); }
    INST(0xeac0000000000000, 0xffe0000000000000) {
        COMMENT_NOT_IMPLEMENTED("suatom");
    }
    INST(0xea80000000000000, 0xffc0000000000000) {
        COMMENT_NOT_IMPLEMENTED("suatom");
    }
    INST(0xea60000000000000, 0xffe0000000000000) { emitSuatom(context, inst); }
    INST(0xea00000000000000, 0xffc0000000000000) { emitSuatom(context, inst); }
    INST(0xe3a0000000000000, 0xfff0000000000000) {
        COMMENT_NOT_IMPLEMENTED("bpt");
    }
    INST(0xe390000000000000, 0xfff0000000000000) {
        COMMENT_NOT_IMPLEMENTED("ide");
    }
    INST(0xe380000000000000, 0xfff0000000000000) {
        // Restores the warp's active mask. Control flow is structured per
        // thread here (SSY/SYNC, PBK/BRK), there is no mask to restore.
        COMMENT("ram");
    }
    INST(0xe370000000000000, 0xfff0000000000000) {
        // Saves the warp's active mask, see RAM
        COMMENT("sam");
    }
    INST(0xe360000000000000, 0xfff0000000000000) {
        COMMENT_NOT_IMPLEMENTED("rtt");
    }
    INST(0xe350000000000000, 0xfff0000000000000) { // cont
        const auto pred = static_cast<pred_t>(extractBits(inst, 16, 3));
        const auto pred_inv = static_cast<bool>(extractBits(inst, 19, 1));

        if (crnt_block->continue_point_stack.empty()) {
            LOG_ERROR(ShaderDecompiler, "No continue point in stack");
            return;
        }

        const auto target = crnt_block->continue_point_stack.top();
        if (pred == PT && extractBits(inst, 0, 5) == 15) {
            if (!pred_inv) {
                crnt_block->continue_point_stack.pop();
                inheritSyncPoints(target);

                context.builder.opBranch(target);
                endBlock();
            }
        } else {
            inheritSyncPoints(pc + 1);
            crnt_block->continue_point_stack.pop();
            inheritSyncPoints(target);

            context.builder.opBranchConditional(
                getFlowCondition(context.builder, pred, pred_inv,
                                 static_cast<u32>(extractBits(inst, 0, 5))),
                target, pc + 1);
            endBlock();
        }
    }
    INST(0xe340000000000000, 0xfff0000000000000) { // brk
                const auto pred = static_cast<pred_t>(extractBits(inst, 16, 3));
        const auto pred_inv = static_cast<bool>(extractBits(inst, 19, 1));

        if (crnt_block->break_point_stack.empty()) {
            LOG_ERROR(ShaderDecompiler, "No break point in stack");
            return;
        }

        const auto target = crnt_block->break_point_stack.top();
        if (pred == PT && extractBits(inst, 0, 5) == 15) {
            if (!pred_inv) {
                // Pop and then inherit
                crnt_block->break_point_stack.pop();
                inheritSyncPoints(target);

                context.builder.opBranch(target);
                endBlock();
            }
        } else { // Conditional
            // Inherit for the continuation block, then pop and inherit for
            // break block
            inheritSyncPoints(pc + 1);
            crnt_block->break_point_stack.pop();
            inheritSyncPoints(target);

            context.builder.opBranchConditional(
                getFlowCondition(context.builder, pred, pred_inv, static_cast<u32>(extractBits(inst, 0, 5))),
                target, pc + 1);
            endBlock();
        }
    }
    INST(0xe330000000000000, 0xfff0000000000000) { EMIT(Kil); }
    INST(0xe320000000000000, 0xfff0000000000000) { // ret
        // Returns to the address CAL or PRET pushed (the shader ends if there
        // is none)
        const auto pred = static_cast<pred_t>(extractBits(inst, 16, 3));
        const auto pred_inv = static_cast<bool>(extractBits(inst, 19, 1));
        if (pred == PT && extractBits(inst, 0, 5) == 15) {
            if (!pred_inv) {
                context.builder.opReturnBranch();
                endBlock();
            }
        } else {
            context.builder.opBeginIf(getFlowCondition(
                context.builder, pred, pred_inv,
                static_cast<u32>(extractBits(inst, 0, 5))));
            context.builder.opReturnBranch();
            context.builder.opEndIf();
            inheritSyncPoints(pc + 1);
            context.builder.opBranch(pc + 1);
            endBlock();
        }
    }
    INST(0xe310000000000000, 0xfff0000000000000) {
        COMMENT_NOT_IMPLEMENTED("longjmp");
    }
    INST(0xe300000000000000, 0xfff0000000000000) { // exit
        const auto pred = static_cast<pred_t>(extractBits(inst, 16, 3));
        const auto pred_inv = static_cast<bool>(extractBits(inst, 19, 1));
        const bool unconditional = pred == PT && extractBits(inst, 0, 5) == 15;
        if (unconditional && pred_inv)
            return;
        auto& b = context.builder;
        if (!crnt_block->exit_point_stack.empty()) {
            const auto target = crnt_block->exit_point_stack.top();
            if (!unconditional)
                inheritSyncPoints(pc + 1);
            crnt_block->exit_point_stack.pop();
            inheritSyncPoints(target);
            if (unconditional)
                b.opBranch(target);
            else
                b.opBranchConditional(
                    getFlowCondition(b, pred, pred_inv,
                                     static_cast<u32>(extractBits(inst, 0, 5))),
                    target, pc + 1);
            endBlock();
        } else if (unconditional) {
            b.opExit();
            endBlock();
        } else {
            b.opBeginIf(getFlowCondition(
                b, pred, pred_inv, static_cast<u32>(extractBits(inst, 0, 5))));
            b.opExit();
            b.opEndIf();
        }
    }
    INST(0xe2f0000000000000, 0xfff0000000000000) {
        COMMENT_NOT_IMPLEMENTED("setlmembase");
    }
    INST(0xe2e0000000000000, 0xfff0000000000000) {
        COMMENT_NOT_IMPLEMENTED("setcrsptr");
    }
    INST(0xe2d0000000000000, 0xfff0000000000000) {
        COMMENT_NOT_IMPLEMENTED("getlmembase");
    }
    INST(0xe2c0000000000000, 0xfff0000000000000) {
        COMMENT_NOT_IMPLEMENTED("getcrsptr");
    }
    INST(0xe2b0000000000020, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("pcnt");
    }
    INST(0xe2b0000000000000, 0xfff0000000000020) { // pcnt
        const auto target = static_cast<u32>(
            static_cast<i32>(pc) +
            signExtend<i32, 24>(static_cast<i32>(extractBits(inst, 20, 24))) /
                static_cast<i32>(sizeof(instruction_t)) +
            1);

        pushContinuePoint(target);
    }
    INST(0xe2a0000000000020, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("pbk");
    }
    INST(0xe2a0000000000000, 0xfff0000000000020) { // pbk
        const auto target = static_cast<u32>(
            static_cast<i32>(pc) +
            signExtend<i32, 24>(static_cast<i32>(extractBits(inst, 20, 24))) /
                static_cast<i32>(sizeof(instruction_t)) +
            1);

        pushBreakPoint(target);
    }
    INST(0xe290000000000020, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("ssy");
    }
    INST(0xe290000000000000, 0xfff0000000000020) { // ssy
        const auto target = static_cast<u32>(
            static_cast<i32>(pc) +
            signExtend<i32, 24>(static_cast<i32>(extractBits(inst, 20, 24))) /
                static_cast<i32>(sizeof(instruction_t)) +
            1);

        pushSyncPoint(target);
    }
    INST(0xe280000000000020, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("plongjmp");
    }
    INST(0xe280000000000000, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("plongjmp");
    }
    INST(0xe270000000000020, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("pret");
    }
    INST(0xe270000000000000, 0xfff0000000000020) { // pret
        // Pushes the address RET returns to; execution continues here
        const auto target = static_cast<u32>(
            static_cast<i32>(pc) +
            signExtend<i32, 24>(static_cast<i32>(extractBits(inst, 20, 24))) /
                static_cast<i32>(sizeof(instruction_t)) +
            1);

        inheritSyncPoints(target);
        context.builder.opPushReturn(target);
    }
    INST(0xe260000000000020, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("cal");
    }
    INST(0xe260000000000000, 0xfff0000000000020) { // cal
        const auto pred = static_cast<pred_t>(extractBits(inst, 16, 3));
        const auto pred_inv = static_cast<bool>(extractBits(inst, 19, 1));
        const auto target = static_cast<u32>(
            static_cast<i32>(pc) +
            signExtend<i32, 24>(static_cast<i32>(extractBits(inst, 20, 24))) /
                static_cast<i32>(sizeof(instruction_t)) +
            1);

        inheritSyncPoints(target);
        inheritSyncPoints(pc + 1);
        if (pred == PT && extractBits(inst, 0, 5) == 15) {
            if (!pred_inv) {
                context.builder.opPushReturn(pc + 1);
                context.builder.opBranch(target);
                endBlock();
            }
        } else {
            context.builder.opBeginIf(getFlowCondition(
                context.builder, pred, pred_inv,
                static_cast<u32>(extractBits(inst, 0, 5))));
            context.builder.opPushReturn(pc + 1);
            context.builder.opBranch(target);
            context.builder.opEndIf();
            context.builder.opBranch(pc + 1);
            endBlock();
        }
    }
    INST(0xe250000000000020, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("brx");
    }
    INST(0xe250000000000000, 0xfff0000000000000) {
        if (!emitIndirectBranch(inst, pc)) {
            if (context.decomp_context.read_const_word)
                SHADER_UNSUPPORTED("BRX unbounded/unsupported target pattern at {:#x}", pc);
            else
                // Offline tools have no constant buffers, so a jump table
                // cannot be resolved there
                LOG_WARN(ShaderDecompiler,
                         "BRX at {:#x} needs a constant buffer reader", pc);
        }
    }
    INST(0xe240000000000020, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("bra");
    }
    INST(0xe240000000000000, 0xfff0000000000020) { // bra
        // TODO: ca, lmt, u
        const auto pred = static_cast<pred_t>(extractBits(inst, 16, 3));
        const auto pred_inv = static_cast<bool>(extractBits(inst, 19, 1));
        const auto target = static_cast<u32>(
            static_cast<i32>(pc) +
            signExtend<i32, 24>(static_cast<i32>(extractBits(inst, 20, 24))) /
                static_cast<i32>(sizeof(instruction_t)) +
            1);

        if (pred == PT && extractBits(inst, 0, 5) == 15) {
            if (!pred_inv) {
                inheritSyncPoints(target);
                context.builder.opBranch(target);
                endBlock();
            }
        } else {
            inheritSyncPoints(target);
            inheritSyncPoints(pc + 1);
            context.builder.opBranchConditional(
                getFlowCondition(context.builder, pred, pred_inv, static_cast<u32>(extractBits(inst, 0, 5))),
                target, pc + 1);
            endBlock();
        }
    }
    INST(0xe230000000000000, 0xfff0000000000000) { // pexit
        const auto offset = signExtend<i32, 24>(
            static_cast<i32>(extractBits(inst, 20, 24)));
        const auto target = static_cast<u32>(
            static_cast<i32>(pc) + offset / 8 + 1);
        crnt_block->exit_point_stack.push(target);
    }
    INST(0xe220000000000020, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("jcal");
    }
    INST(0xe220000000000000, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("jcal");
    }
    INST(0xe210000000000000, 0xfff0000000000000) { // absolute jmp
        u32 address = static_cast<u32>(extractBits(inst, 20, 32));
        if (extractBits(inst, 5, 1)) {
            const auto slot = static_cast<u32>(extractBits(inst, 36, 5));
            const auto byte_offset = signExtend<i32, 16>(
                static_cast<i32>(extractBits(inst, 20, 16)));
            const auto value = byte_offset >= 0 && byte_offset % 4 == 0 &&
                                       context.decomp_context.read_const_word
                ? context.decomp_context.read_const_word(
                      slot, static_cast<u32>(byte_offset) / 4)
                : std::nullopt;
            if (!value) {
                SHADER_UNSUPPORTED("JMP unresolved cbuf target");
                return;
            }
            address = *value;
            if (context.decomp_context.branch_constants)
                (*context.decomp_context.branch_constants)[
                    {slot, static_cast<u32>(byte_offset) / 4}] = address;
        }
        if (context.decomp_context.code_read_size)
            *context.decomp_context.code_read_size = invalid<u64>();
        if (address < context.code_offset || address % 8 != 0 ||
            address - context.code_offset >= context.code_stream->getSize()) {
            LOG_ERROR(ShaderDecompiler, "JMP target outside shader: {:#x}", address);
            const auto condition = getFlowCondition(
                context.builder, static_cast<pred_t>(extractBits(inst, 16, 3)),
                extractBits(inst, 19, 1) != 0,
                static_cast<u32>(extractBits(inst, 0, 5)));
            context.builder.opBeginIf(condition);
            context.builder.opExit();
            context.builder.opEndIf();
            inheritSyncPoints(pc + 1);
            context.builder.opBranch(pc + 1);
            endBlock();
            return;
        }
        const auto target = static_cast<u32>((address - context.code_offset) / 8);
        const auto pred = static_cast<pred_t>(extractBits(inst, 16, 3));
        const auto pred_inv = extractBits(inst, 19, 1) != 0;
        if (pred == PT && extractBits(inst, 0, 5) == 15) {
            if (!pred_inv) {
                inheritSyncPoints(target);
                context.builder.opBranch(target);
                endBlock();
            }
        } else {
            inheritSyncPoints(target);
            inheritSyncPoints(pc + 1);
            context.builder.opBranchConditional(
                getFlowCondition(context.builder, pred, pred_inv,
                                 static_cast<u32>(extractBits(inst, 0, 5))),
                target, pc + 1);
            endBlock();
        }
    }
    INST(0xe200000000000020, 0xfff0000000000020) {
        COMMENT_NOT_IMPLEMENTED("jmx");
    }
    INST(0xe200000000000000, 0xfff0000000000020) {
        if (!emitIndirectBranch(inst, pc))
            SHADER_UNSUPPORTED("JMX unbounded/unsupported target pattern at {:#x}", pc);
    }
    INST(0xe000000000000000, 0xff00000000000000) { EMIT(Ipa); }
    INST(0xdf60000000000000, 0xfff8000000000000) { EMIT(TmmlB); }
    INST(0xdf58000000000000, 0xfff8000000000000) { EMIT(Tmml); }
    INST(0xdf50000000000000, 0xfff8000000000000) { EMIT(TxqB); }
    INST(0xdf48000000000000, 0xfff8000000000000) { EMIT(Txq); }
    INST(0xdf40000000000000, 0xfff8000000000000) {
        COMMENT_NOT_IMPLEMENTED("txa");
    }
    INST(0xdf00000000000000, 0xff40000000000000) { EMIT(Tld4s); }
    INST(0xdef8000000000000, 0xfff8000000000000) { EMIT(Tld4B); }
    INST(0xdeb8000000000000, 0xfff8000000000000) { EMIT(TexB); }
    INST(0xde78000000000000, 0xfffc000000000000) { EMIT(TxdB); }
    INST(0xde38000000000000, 0xfffc000000000000) { EMIT(Txd); }
    INST(0xdd38000000000000, 0xff38000000000000) { EMIT(TldB); }
    INST(0xdc38000000000000, 0xff38000000000000) { EMIT(Tld); }
    INST(0xd200000000000000, 0xf600000000000000) { EMIT(Tlds); }
    INST(0xd000000000000000, 0xf600000000000000) { EMIT(Texs); }
    INST(0xc838000000000000, 0xfc38000000000000) { EMIT(Tld4); }
    INST(0xc038000000000000, 0xfc38000000000000) { EMIT(Tex); }
    INST(0xa000000000000000, 0xe000000000000000) {
        COMMENT_NOT_IMPLEMENTED("st");
    }
    INST(0x8000000000000000, 0xe000000000000000) {
        COMMENT_NOT_IMPLEMENTED("ld");
    }
    INST(0x7e80000000000000, 0xfe80000000000000) {
        emitHsetp2(context, inst, HalfOperandForm::ConstBuffer);
    }
    INST(0x7e00000000000000, 0xfe80000000000000) {
        emitHsetp2(context, inst, HalfOperandForm::Immediate);
    }
    INST(0x7c80000000000000, 0xfe80000000000000) {
        emitHset2(context, inst, HalfOperandForm::ConstBuffer);
    }
    INST(0x7c00000000000000, 0xfe80000000000000) {
        emitHset2(context, inst, HalfOperandForm::Immediate);
    }
    INST(0x7a00000000000000, 0xfe80000000000000) { EMIT(Hadd2I); }
    INST(0x7a80000000000000, 0xfe80000000000000) { EMIT(Hadd2C); }
    INST(0x7800000000000000, 0xfe80000000000000) { EMIT(Hmul2I); }
    INST(0x7880000000000000, 0xfe80000000000000) { EMIT(Hmul2C); }
    INST(0x7080000000000000, 0xf880000000000000) { EMIT(Hfma2C); }
    INST(0x7000000000000000, 0xf880000000000000) { EMIT(Hfma2I); }
    INST(0x6080000000000000, 0xf880000000000000) { EMIT(Hfma2RC); }
    INST(0x5f00000000000000, 0xff00000000000000) {
        emitVmad(context, inst);
    }
    INST(0x5d20000000000000, 0xfff8000000000000) {
        emitHsetp2(context, inst, HalfOperandForm::Register);
    }
    INST(0x5d18000000000000, 0xfff8000000000000) {
        emitHset2(context, inst, HalfOperandForm::Register);
    }
    INST(0x5d10000000000000, 0xfff8000000000000) { EMIT(Hadd2R); }
    INST(0x5d08000000000000, 0xfff8000000000000) { EMIT(Hmul2R); }
    INST(0x5d00000000000000, 0xfff8000000000000) { EMIT(Hfma2R); }
    INST(0x5cf8000000000000, 0xfff8000000000000) {
        emitShf(context, inst, true, false);
    }
    INST(0x5cf0000000000000, 0xfff8000000000000) {
        emitR2p(context, inst, 0);
    }
    INST(0x5ce8000000000000, 0xfff8000000000000) {
        emitP2r(context, inst, OperandForm::Register);
    }
    INST(0x5ce0000000000000, 0xfff8000000000000) { EMIT(I2iR); }
    INST(0x5cc0000000000000, 0xfff0000000000000) { EMIT(Iadd3R); }
    INST(0x5cb8000000000000, 0xfff8000000000000) { EMIT(I2fR); }
    INST(0x5cb0000000000000, 0xfff8000000000000) { EMIT(F2iR); }
    INST(0x5ca8000000000000, 0xfff8000000000000) { EMIT(F2fR); }
    INST(0x5ca0000000000000, 0xfff8000000000000) { EMIT(SelR); }
    INST(0x5c98000000000000, 0xfff8000000000000) { EMIT(MovR); }
    INST(0x5c90000000000000, 0xfff8000000000000) { EMIT(RroR); }
    INST(0x5c88000000000000, 0xfff8000000000000) {
        emitFchk(context, inst, OperandForm::Register);
    }
    INST(0x5c80000000000000, 0xfff8000000000000) { emitDmul(context, inst, DoubleSource::Register); }
    INST(0x5c70000000000000, 0xfff8000000000000) { emitDadd(context, inst, DoubleSource::Register); }
    INST(0x5c68000000000000, 0xfff8000000000000) { EMIT(FmulR); }
    INST(0x5c60000000000000, 0xfff8000000000000) { EMIT(FmnmxR); }
    INST(0x5c58000000000000, 0xfff8000000000000) { EMIT(FaddR); }
    INST(0x5c50000000000000, 0xfff8000000000000) { emitDmnmx(context, inst, DoubleSource::Register); }
    INST(0x5c48000000000000, 0xfff8000000000000) { EMIT(ShlR); }
    INST(0x5c40000000000000, 0xfff8000000000000) { EMIT(LopR); }
    INST(0x5c38000000000000, 0xfff8000000000000) {
        emitImul(context, inst, OperandForm::Register);
    }
    INST(0x5c30000000000000, 0xfff8000000000000) { emitFloR(context, inst); }
    INST(0x5c28000000000000, 0xfff8000000000000) { EMIT(ShrR); }
    INST(0x5c20000000000000, 0xfff8000000000000) { EMIT(ImnmxR); }
    INST(0x5c18000000000000, 0xfff8000000000000) { EMIT(IscaddR); }
    INST(0x5c10000000000000, 0xfff8000000000000) { EMIT(IaddR); }
    INST(0x5c08000000000000, 0xfff8000000000000) { emitPopcR(context, inst); }
    INST(0x5c00000000000000, 0xfff8000000000000) { EMIT(BfeR); }
    INST(0x5bf8000000000000, 0xfff8000000000000) {
        emitShf(context, inst, false, false);
    }
    INST(0x5bf0000000000000, 0xfff8000000000000) { EMIT(BfiR); }
    INST(0x5be0000000000000, 0xfff8000000000000) { emitLop3R(context, inst); }
    INST(0x5bd8000000000000, 0xfff8000000000000) { EMIT(LeaHiR); }
    INST(0x5bd0000000000000, 0xfff8000000000000) { EMIT(LeaLoR); }
    INST(0x5bc0000000000000, 0xfff0000000000000) {
        emitPrmt(context, inst, OperandForm::Register);
    }
    INST(0x5bb0000000000000, 0xfff0000000000000) { EMIT(FsetpR); }
    INST(0x5ba0000000000000, 0xfff0000000000000) { EMIT(FcmpR); }
    INST(0x5b80000000000000, 0xfff0000000000000) { emitDsetp(context, inst, DoubleSource::Register); }
    INST(0x5b70000000000000, 0xfff0000000000000) { emitDfma(context, inst, DoubleSource::Register); }
    INST(0x5b60000000000000, 0xfff0000000000000) { EMIT(IsetpR); }
    INST(0x5b50000000000000, 0xfff0000000000000) { EMIT(IsetR); }
    INST(0x5b40000000000000, 0xfff0000000000000) { EMIT(IcmpR); }
    INST(0x5b00000000000000, 0xffc0000000000000) { EMIT(XmadR); }
    INST(0x5a80000000000000, 0xff80000000000000) {
        COMMENT_NOT_IMPLEMENTED("imadsp");
    }
    INST(0x5a00000000000000, 0xff80000000000000) {
        emitImad(context, inst, ImadForm::Register);
    }
    INST(0x5980000000000000, 0xff80000000000000) { EMIT(FfmaR); }
    INST(0x5900000000000000, 0xff80000000000000) { emitDset(context, inst, DoubleSource::Register); }
    INST(0x5800000000000000, 0xff00000000000000) { EMIT(FsetR); }
    INST(0x5700000000000000, 0xff00000000000000) {
        COMMENT_NOT_IMPLEMENTED("vshl");
    }
    INST(0x5600000000000000, 0xff00000000000000) {
        COMMENT_NOT_IMPLEMENTED("vshr");
    }
    INST(0x5400000000000000, 0xff00000000000000) {
        COMMENT_NOT_IMPLEMENTED("vabsdiff");
    }
    INST(0x53f8000000000000, 0xfff8000000000000) {
        // SM 6.x only, not part of the Switch's Maxwell (SM 5.3)
        COMMENT_NOT_IMPLEMENTED("idp");
    }
    INST(0x53f0000000000000, 0xfff8000000000000) {
        emitBfiRC(context, std::bit_cast<InstBfiC>(inst));
    }
    INST(0x53d8000000000000, 0xfff8000000000000) {
        COMMENT_NOT_IMPLEMENTED("idp");
    }
    INST(0x53c0000000000000, 0xfff0000000000000) {
        emitPrmt(context, inst, OperandForm::RegisterConstBuffer);
    }
    INST(0x53a0000000000000, 0xfff0000000000000) {
        emitFcmpRC(context, std::bit_cast<InstFcmpC>(inst));
    }
    INST(0x5370000000000000, 0xfff0000000000000) {
        emitDfmaConstantAddend(context, inst);
    }
    INST(0x5340000000000000, 0xfff0000000000000) {
        emitIcmpRC(context, std::bit_cast<InstIcmpC>(inst));
    }
    INST(0x5280000000000000, 0xff80000000000000) {
        COMMENT_NOT_IMPLEMENTED("imadsp");
    }
    INST(0x5200000000000000, 0xff80000000000000) {
        emitImad(context, inst, ImadForm::RegisterConstBuffer);
    }
    INST(0x5180000000000000, 0xff80000000000000) { EMIT(FfmaRC); }
    INST(0x5100000000000000, 0xff80000000000000) { EMIT(XmadRC); }
    INST(0x50f8000000000000, 0xfff8000000000000) { // fswzadd
        // TODO: ndv, rounding, ftz, cc
        const auto pred = static_cast<pred_t>(extractBits(inst, 16, 3));
        const auto pred_inv = static_cast<bool>(extractBits(inst, 19, 1));
        const auto dst = static_cast<reg_t>(extractBits(inst, 0, 8));
        const auto src_a = static_cast<reg_t>(extractBits(inst, 8, 8));
        const auto src_b = static_cast<reg_t>(extractBits(inst, 20, 8));
        const auto swizzle = static_cast<u32>(extractBits(inst, 28, 8));

        const auto conditional =
            handlePredCond(context.builder, pred, pred_inv);
        const auto res = context.builder.opSwizzleAdd(
            ir::Value::createRegister(src_a, ir::ScalarType::F32),
            ir::Value::createRegister(src_b, ir::ScalarType::F32), swizzle);
        context.builder.opCopy(
            ir::Value::createRegister(dst, ir::ScalarType::F32), res);
        if (conditional)
            context.builder.opEndIf();
    }
    INST(0x50f0000000000000, 0xfff8000000000000) {
        emitVsetp(context, inst);
    }
    INST(0x50e0000000000000, 0xfff8000000000000) {
        // VOTE.VTG: a vote across the vertex thread group that writes no
        // register or predicate
        COMMENT("vote.vtg");
    }
    INST(0x50d8000000000000, 0xfff8000000000000) { emitVote(context, inst); }
    INST(0x50d0000000000000, 0xfff8000000000000) {
        COMMENT_NOT_IMPLEMENTED("lepc");
    }
    INST(0x50c8000000000000, 0xfff8000000000000) { EMIT(S2R); } // cs2r
    INST(0x50b0000000000000, 0xfff8000000000000) { COMMENT("nop"); }
    INST(0x50a0000000000000, 0xfff8000000000000) {
        emitCsetp(context, inst);
    }
    INST(0x5098000000000000, 0xfff8000000000000) { emitCset(context, inst); }
    INST(0x5090000000000000, 0xfff8000000000000) { EMIT(Psetp); }
    INST(0x5088000000000000, 0xfff8000000000000) { EMIT(Pset); }
    INST(0x5080000000000000, 0xfff8000000000000) { EMIT(Mufu); }
    INST(0x5000000000000000, 0xff80000000000000) {
        COMMENT_NOT_IMPLEMENTED("vabsdiff4");
    }
    INST(0x4e00000000000000, 0xfe00000000000000) { EMIT(XmadC); }
    INST(0x4cf0000000000000, 0xfff8000000000000) {
        emitR2p(context, inst, 1);
    }
    INST(0x4ce8000000000000, 0xfff8000000000000) {
        emitP2r(context, inst, OperandForm::ConstBuffer);
    }
    INST(0x4ce0000000000000, 0xfff8000000000000) { EMIT(I2iC); }
    INST(0x4cc0000000000000, 0xfff0000000000000) { EMIT(Iadd3C); }
    INST(0x4cb8000000000000, 0xfff8000000000000) { EMIT(I2fC); }
    INST(0x4cb0000000000000, 0xfff8000000000000) { EMIT(F2iC); }
    INST(0x4ca8000000000000, 0xfff8000000000000) { EMIT(F2fC); }
    INST(0x4ca0000000000000, 0xfff8000000000000) { EMIT(SelC); }
    INST(0x4c98000000000000, 0xfff8000000000000) { EMIT(MovC); }
    INST(0x4c90000000000000, 0xfff8000000000000) { EMIT(RroC); }
    INST(0x4c88000000000000, 0xfff8000000000000) {
        emitFchk(context, inst, OperandForm::ConstBuffer);
    }
    INST(0x4c80000000000000, 0xfff8000000000000) { emitDmul(context, inst, DoubleSource::Constant); }
    INST(0x4c70000000000000, 0xfff8000000000000) { emitDadd(context, inst, DoubleSource::Constant); }
    INST(0x4c68000000000000, 0xfff8000000000000) { EMIT(FmulC); }
    INST(0x4c60000000000000, 0xfff8000000000000) { EMIT(FmnmxC); }
    INST(0x4c58000000000000, 0xfff8000000000000) { EMIT(FaddC); }
    INST(0x4c50000000000000, 0xfff8000000000000) { emitDmnmx(context, inst, DoubleSource::Constant); }
    INST(0x4c48000000000000, 0xfff8000000000000) { EMIT(ShlC); }
    INST(0x4c40000000000000, 0xfff8000000000000) { EMIT(LopC); }
    INST(0x4c38000000000000, 0xfff8000000000000) {
        emitImul(context, inst, OperandForm::ConstBuffer);
    }
    INST(0x4c30000000000000, 0xfff8000000000000) { emitFloC(context, inst); }
    INST(0x4c28000000000000, 0xfff8000000000000) { EMIT(ShrC); }
    INST(0x4c20000000000000, 0xfff8000000000000) { EMIT(ImnmxC); }
    INST(0x4c18000000000000, 0xfff8000000000000) { EMIT(IscaddC); }
    INST(0x4c10000000000000, 0xfff8000000000000) { EMIT(IaddC); }
    INST(0x4c08000000000000, 0xfff8000000000000) { emitPopcC(context, inst); }
    INST(0x4c00000000000000, 0xfff8000000000000) { EMIT(BfeC); }
    INST(0x4bf0000000000000, 0xfff8000000000000) { EMIT(BfiC); }
    INST(0x4bd0000000000000, 0xfff0000000000000) { EMIT(LeaLoC); }
    INST(0x4bc0000000000000, 0xfff0000000000000) {
        emitPrmt(context, inst, OperandForm::ConstBuffer);
    }
    INST(0x4bb0000000000000, 0xfff0000000000000) { EMIT(FsetpC); }
    INST(0x4ba0000000000000, 0xfff0000000000000) { EMIT(FcmpC); }
    INST(0x4b80000000000000, 0xfff0000000000000) { emitDsetp(context, inst, DoubleSource::Constant); }
    INST(0x4b70000000000000, 0xfff0000000000000) { emitDfma(context, inst, DoubleSource::Constant); }
    INST(0x4b60000000000000, 0xfff0000000000000) { EMIT(IsetpC); }
    INST(0x4b50000000000000, 0xfff0000000000000) { EMIT(IsetC); }
    INST(0x4b40000000000000, 0xfff0000000000000) { EMIT(IcmpC); }
    INST(0x4a80000000000000, 0xff80000000000000) {
        COMMENT_NOT_IMPLEMENTED("imadsp");
    }
    INST(0x4a00000000000000, 0xff80000000000000) {
        emitImad(context, inst, ImadForm::ConstBuffer);
    }
    INST(0x4980000000000000, 0xff80000000000000) { EMIT(FfmaC); }
    INST(0x4900000000000000, 0xff80000000000000) { emitDset(context, inst, DoubleSource::Constant); }
    INST(0x4800000000000000, 0xfe00000000000000) { EMIT(FsetC); }
    INST(0x4000000000000000, 0xfe00000000000000) {
        COMMENT_NOT_IMPLEMENTED("vset");
    }
    INST(0x3c00000000000000, 0xfc00000000000000) { emitLop3I(context, inst); }
    INST(0x3a00000000000000, 0xfe00000000000000) {
        emitVmnmx(context, inst);
    }
    INST(0x38f8000000000000, 0xfef8000000000000) {
        emitShf(context, inst, true, true);
    }
    INST(0x38f0000000000000, 0xfef8000000000000) {
        emitR2p(context, inst, 2);
    }
    INST(0x38e8000000000000, 0xfef8000000000000) {
        emitP2r(context, inst, OperandForm::Immediate);
    }
    INST(0x38e0000000000000, 0xfef8000000000000) { EMIT(I2iI); }
    INST(0x38c0000000000000, 0xfef0000000000000) { EMIT(Iadd3I); }
    INST(0x38b8000000000000, 0xfef8000000000000) { EMIT(I2fI); }
    INST(0x38b0000000000000, 0xfef8000000000000) { EMIT(F2iI); }
    INST(0x38a8000000000000, 0xfef8000000000000) { EMIT(F2fI); }
    INST(0x38a0000000000000, 0xfef8000000000000) { EMIT(SelI); }
    INST(0x3898000000000000, 0xfef8000000000000) { EMIT(MovI); }
    INST(0x3890000000000000, 0xfef8000000000000) { EMIT(RroI); }
    INST(0x3888000000000000, 0xfef8000000000000) {
        emitFchk(context, inst, OperandForm::Immediate);
    }
    INST(0x3880000000000000, 0xfef8000000000000) { emitDmul(context, inst, DoubleSource::Immediate); }
    INST(0x3870000000000000, 0xfef8000000000000) { emitDadd(context, inst, DoubleSource::Immediate); }
    INST(0x3868000000000000, 0xfef8000000000000) { EMIT(FmulI); }
    INST(0x3860000000000000, 0xfef8000000000000) { EMIT(FmnmxI); }
    INST(0x3858000000000000, 0xfef8000000000000) { EMIT(FaddI); }
    INST(0x3850000000000000, 0xfef8000000000000) { emitDmnmx(context, inst, DoubleSource::Immediate); }
    INST(0x3848000000000000, 0xfef8000000000000) { EMIT(ShlI); }
    INST(0x3840000000000000, 0xfef8000000000000) { EMIT(LopI); }
    INST(0x3838000000000000, 0xfef8000000000000) {
        emitImul(context, inst, OperandForm::Immediate);
    }
    INST(0x3830000000000000, 0xfef8000000000000) { emitFloI(context, inst); }
    INST(0x3828000000000000, 0xfef8000000000000) { EMIT(ShrI); }
    INST(0x3820000000000000, 0xfef8000000000000) { EMIT(ImnmxI); }
    INST(0x3818000000000000, 0xfef8000000000000) { EMIT(IscaddI); }
    INST(0x3810000000000000, 0xfef8000000000000) { EMIT(IaddI); }
    INST(0x3808000000000000, 0xfef8000000000000) { emitPopcI(context, inst); }
    INST(0x3800000000000000, 0xfef8000000000000) { EMIT(BfeI); }
    INST(0x36f8000000000000, 0xfef8000000000000) {
        emitShf(context, inst, false, true);
    }
    INST(0x36f0000000000000, 0xfef8000000000000) { EMIT(BfiI); }
    INST(0x36d0000000000000, 0xfef8000000000000) { EMIT(LeaLoI); }
    INST(0x36c0000000000000, 0xfef0000000000000) {
        emitPrmt(context, inst, OperandForm::Immediate);
    }
    INST(0x36b0000000000000, 0xfef0000000000000) { EMIT(FsetpI); }
    INST(0x36a0000000000000, 0xfef0000000000000) { EMIT(FcmpI); }
    INST(0x3680000000000000, 0xfef0000000000000) { emitDsetp(context, inst, DoubleSource::Immediate); }
    INST(0x3670000000000000, 0xfef0000000000000) { emitDfma(context, inst, DoubleSource::Immediate); }
    INST(0x3660000000000000, 0xfef0000000000000) { EMIT(IsetpI); }
    INST(0x3650000000000000, 0xfef0000000000000) { EMIT(IsetI); }
    INST(0x3640000000000000, 0xfef0000000000000) { EMIT(IcmpI); }
    INST(0x3600000000000000, 0xfec0000000000000) { EMIT(XmadI); }
    INST(0x3480000000000000, 0xfe80000000000000) {
        COMMENT_NOT_IMPLEMENTED("imadsp");
    }
    INST(0x3400000000000000, 0xfe80000000000000) {
        emitImad(context, inst, ImadForm::Immediate);
    }
    INST(0x3280000000000000, 0xfe80000000000000) { EMIT(FfmaI); }
    INST(0x3200000000000000, 0xfe80000000000000) { emitDset(context, inst, DoubleSource::Immediate); }
    INST(0x3000000000000000, 0xfe00000000000000) { EMIT(FsetI); }
    INST(0x2c00000000000000, 0xfe00000000000000) { EMIT(Hadd2_32I); }
    INST(0x2a00000000000000, 0xfe00000000000000) { EMIT(Hmul2_32I); }
    INST(0x2800000000000000, 0xfe00000000000000) { EMIT(Hfma2_32I); }
    INST(0x2000000000000000, 0xfc00000000000000) {
        COMMENT_NOT_IMPLEMENTED("vadd");
    }
    INST(0x1f00000000000000, 0xff00000000000000) {
        emitImul32I(context, inst);
    }
    INST(0x1e00000000000000, 0xff00000000000000) { EMIT(Fmul32I); }
    // TODO: is this necessary?
    // INST(0x1d80000000000000, 0xff80000000000000) {
    //    COMMENT_NOT_IMPLEMENTED("iadd32i");
    //}
    INST(0x1c00000000000000, 0xfe00000000000000) { EMIT(Iadd32I); }
    INST(0x1800000000000000, 0xfc00000000000000) { EMIT(LeaHiC); }
    INST(0x1400000000000000, 0xfc00000000000000) { EMIT(Iscadd32I); }
    INST(0x1000000000000000, 0xfc00000000000000) {
        // The operand layout is not known reliably (envytools marks it as
        // uncertain): report it instead of guessing
        COMMENT_NOT_IMPLEMENTED("imad32i");
    }
    INST(0x0c00000000000000, 0xfc00000000000000) { EMIT(Ffma32I); }
    INST(0x0800000000000000, 0xfc00000000000000) { EMIT(Fadd32I); }
    INST(0x0400000000000000, 0xfc00000000000000) { EMIT(Lop32I); }
    INST(0x0200000000000000, 0xfe00000000000000) { emitLop3C(context, inst); }
    INST(0x0100000000000000, 0xfff0000000000000) { EMIT(Mov32I); }
    else {
        visual.decoded(pc, inst, "UNKNOWN");
        LOG_ERROR(ShaderDecompiler, "Unknown instruction 0x{:016x}", inst);
        // Zeros or consecutive unknown instructions mean decoding data past
        // the end of the shader (a lone unknown one is just unsupported)
        const bool garbage = inst == 0x0 || last_unknown_pc + 1 == pc ||
                             last_unknown_pc + 2 == pc;
        last_unknown_pc = pc;
        if (garbage) {
            context.builder.opExit();
            endBlock();
        }
    }
} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::decoder
