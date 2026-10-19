#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"
#include <memory>

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/analyzer/memory_analyzer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/msl/emitter.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/decoder.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/ir/builder.hpp"

// HACK
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/analyzer/cfg.hpp"

#include <array>
#include <bit>
#include <cstring>

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp {

#pragma pack(push, 1)
struct ShaderHeader {
    // CommonWord0
    u32 sph_type : 5;
    u32 version : 5;
    u32 shader_type : 4;
    u32 mrt_enable : 1;
    u32 kills_pixels : 1;
    u32 does_global_store : 1;
    u32 sass_version : 4;
    u32 _reserved0_1 : 1;
    u32 _reserved0_2 : 1;
    u32 _reserved0_3 : 1;
    u32 is_fast_gs : 1;
    u32 vsh_unk_flag : 1;
    u32 does_load_or_store : 1;
    u32 does_fp64 : 1;
    u32 stream_out_mask : 4;

    // CommonWord1
    u32 sh_local_mem_lo_sz : 24;
    u32 per_patch_attrib_cnt : 8;

    // CommonWord2
    u32 sh_local_mem_hi_sz : 24;
    u32 thr_per_input_prim : 8;

    // CommonWord3
    u32 sh_local_mem_crs_sz : 24;
    u32 output_topology : 4;
    u32 _reserved3 : 4;

    // CommonWord4
    u32 max_out_vtx_cnt : 12;
    u32 store_req_start : 8;
    u32 _reserved4 : 4;
    u32 store_req_end : 8;

    // ImapSystemValuesA/ImapSystemValuesB
    u32 imap_sysvals_ab;

    union {
        struct {
            u8 imap_generic_vector[16];
            u16 imap_color;
            u16 imap_sysvals_c;
            u8 imap_fixed_fnc_tex[5];
            u8 _reserved0;
            u32 omap_sysvals_ab;
            u8 omap_generic_vector[16];
            u16 omap_color;
            u16 omap_sysvals_c;
            u8 omap_fixed_fnc_tex[5];
            u8 omap_extra;
        } vtg;

        struct {
            u8 imap_generic_vector[32];
            u16 imap_color;
            u16 imap_sysvals_c;
            u8 imap_fixed_fnc_tex[10];
            u16 _reserved0;
            u32 omap_target;
            u32 omap_sample_mask : 1;
            u32 omap_depth : 1;
            u32 _reserved1 : 30;
        } ps;
    };
};
#pragma pack(pop)

void decompileHot(
    ztd::io::MemoryStream& code_stream, const ShaderType type,
    const GuestShaderState& state, ShaderBackend& out_backend,
    std::vector<u8>& out_code, ResourceMapping& out_resource_mapping,
    const std::function<std::optional<u32>(u32, u32)>& read_const_word) {
    out_resource_mapping.code_read_size = 0;
    // Header (compute shaders don't have one)
    ShaderHeader header{};
    if (type != ShaderType::Compute) {
        header = code_stream.read<ShaderHeader>();
        const u32 version = header.version;
        if (version != 3)
            ONCE(LOG_WARN(ShaderDecompiler, "Unexpected shader version {}",
                          version));
    }

    // Context
    DecompilerContext context;
    u64 code_read_size = 0;
    context.code_read_size = &code_read_size;
    context.type = type;
    // The header often reports 0 even for shaders that use local memory, so
    // keep a sensible minimum. Compute shaders have no header at all.
    context.local_memory_size =
        (type == ShaderType::Compute
             ? 0x400u
             : std::max<u32>(header.sh_local_mem_lo_sz +
                                 header.sh_local_mem_hi_sz +
                                 header.sh_local_mem_crs_sz,
                             0x100u));
    context.read_const_word = read_const_word;
    context.branch_constants = &out_resource_mapping.branch_constants;
    if (type == ShaderType::Fragment) {
        context.fragment_output_mask = header.ps.omap_target;
        u32 color_registers = 0;
        for (u32 i = 0; i < COLOR_TARGET_COUNT; ++i)
            if (((header.ps.omap_target >> (i * 4)) & 0xf) != 0)
                color_registers += 4;
        if (header.ps.omap_depth)
            context.fragment_depth_register = color_registers + 1;
        for (u32 i = 0; i < PIXEL_IMAP_COUNT; i++) {
            const auto imap = header.ps.imap_generic_vector[i];
            context.frag.pixel_imaps[i] = {
                .x = static_cast<PixelImapType>(extractBits(imap, 0, 2)),
                .y = static_cast<PixelImapType>(extractBits(imap, 2, 2)),
                .z = static_cast<PixelImapType>(extractBits(imap, 4, 2)),
                .w = static_cast<PixelImapType>(extractBits(imap, 6, 2)),
            };
        }
    }

    // Build IR
    ir::Module modul;
    {
        ir::Builder builder(modul);
        ztd::io::StreamView stream(&code_stream, code_stream.getSeek());
        decoder::Decoder decoder({.decomp_context = context,
                                  .code_stream = &stream,
                                  .builder = builder,
                                  .code_offset = static_cast<u64>(state.program_offset) +
                                                 code_stream.getSeek()});
        decoder.decode();
        if (code_read_size != invalid<u64>())
            out_resource_mapping.code_read_size = code_read_size +
                (type == ShaderType::Compute ? 0 : sizeof(ShaderHeader));
    }

#define DUMP_CFG 0
#if DUMP_CFG
    {
        LOG_INFO(ShaderDecompiler, "Dumping shader CFG 0x{}",
                 (void*)code_reader.GetBase());
        std::ofstream out(
            fmt::format("/Users/samuliak/Downloads/extracted/0x{}.gv",
                        (void*)code_reader.GetBase()),
            std::ios::binary);
        const auto& func = modul.GetFunction("main");
        analyzer::CfgBuilder cfg_builder;
        auto entry_block = cfg_builder.Build(func);
        entry_block->WriteToDot(out);
    }
#endif

    auto& visual = VisualDiagnostics::instance();
    if (visual.shaderActive()) visual.shaderArtifact("decoded.ir", fmt::format("{}", modul));

    // Analyze

    // Memory
    analyzer::MemoryAnalyzer mem_analyzer;
    std::map<u32, TextureType> texture_type_hints;
    for (const auto& [index, hint_type] : state.texture_type_hints) {
        switch (hint_type) {
        case MTL::TextureType1D:
            texture_type_hints[index] = TextureType::_1D;
            break;
        case MTL::TextureType1DArray:
            texture_type_hints[index] = TextureType::_1DArray;
            break;
        case MTL::TextureType2D:
            texture_type_hints[index] = TextureType::_2D;
            break;
        case MTL::TextureType2DArray:
            texture_type_hints[index] = TextureType::_2DArray;
            break;
        case MTL::TextureType3D:
            texture_type_hints[index] = TextureType::_3D;
            break;
        case MTL::TextureTypeCube:
            texture_type_hints[index] = TextureType::Cube;
            break;
        case MTL::TextureTypeCubeArray:
            texture_type_hints[index] = TextureType::CubeArray;
            break;
        default:
            break;
        }
    }
    mem_analyzer.analyze(modul, texture_type_hints);
    if (visual.shaderActive()) visual.shaderArtifact("analyzed.ir", fmt::format("{}", modul));

    // Debug
    LOG_DEBUG(ShaderDecompiler, "Module:\n{}", modul);

    // Decompile
    std::unique_ptr<codegen::Emitter> emitter;
    out_backend = CONFIG_INSTANCE.getShaderBackend();
    // NOLINTNEXTLINE(readability-trivial-switch)
    switch (out_backend) {
    case ShaderBackend::Msl: {
        emitter = std::make_unique<codegen::lang::msl::MslEmitter>(
            context, mem_analyzer, state, out_code, out_resource_mapping);
        break;
    }
    // case ShaderBackend::Air:
    //     builder = new IR::AIR::Builder(analyzer, type, state, out_code,
    //                                    out_resource_mapping);
    //     break;
    default:
        LOG_FATAL(ShaderDecompiler, "Unsupported shader backend {}",
                  out_backend);
        break;
    }

    emitter->emit(modul);
}

std::optional<u32> findGeometryLayerSource(std::span<const u8> code) {
    constexpr usize HEADER_SIZE = 0x50;
    constexpr u32 LAYER_ADDR = 0x64;
    constexpr u32 USER_ATTR_BASE = 0x80;
    constexpr u32 USER_ATTR_END = 0x280;
    constexpr u32 UNKNOWN = invalid<u32>();

    // Attribute address each register was loaded from
    std::array<u32, 256> reg_sources;
    reg_sources.fill(UNKNOWN);
    std::optional<u32> source;
    for (usize offset = HEADER_SIZE; offset + 8 <= code.size(); offset += 8) {
        // Every 4th word holds scheduling info
        if (((offset - HEADER_SIZE) / 8) % 4 == 0)
            continue;

        u64 inst;
        std::memcpy(&inst, code.data() + offset, sizeof(inst));
        if (inst == 0)
            break;

        const auto count = static_cast<u32>(extractBits(inst, 47, 2)) + 1;
        if ((inst & 0xfff8000000000000) == 0xefd8000000000000) {
            // ALD: only loads from input attributes are tracked
            const auto dst = static_cast<u32>(extractBits(inst, 0, 8));
            const auto addr = static_cast<u32>(extractBits(inst, 20, 11));
            const bool is_output = extractBits(inst, 32, 1) != 0;
            for (u32 i = 0; i < count && dst + i < RZ; i++)
                reg_sources[dst + i] = is_output ? UNKNOWN : addr + i * 4;
        } else if ((inst & 0xfff8000000000000) == 0x5c98000000000000) {
            // MOV register
            const auto dst = static_cast<u32>(extractBits(inst, 0, 8));
            const auto src = static_cast<u32>(extractBits(inst, 20, 8));
            if (dst != RZ)
                reg_sources[dst] = (src == RZ ? UNKNOWN : reg_sources[src]);
        } else if ((inst & 0xfff8000000000000) == 0xeff0000000000000) {
            // AST
            const auto src = static_cast<u32>(extractBits(inst, 0, 8));
            const auto addr = static_cast<u32>(extractBits(inst, 20, 11));
            if (addr > LAYER_ADDR || addr + count * 4 <= LAYER_ADDR)
                continue;

            const auto reg = src + (LAYER_ADDR - addr) / 4;
            const auto layer_source = (reg < RZ ? reg_sources[reg] : UNKNOWN);
            if (layer_source < USER_ATTR_BASE ||
                layer_source >= USER_ATTR_END ||
                (source && *source != layer_source))
                return std::nullopt;
            source = layer_source;
        } else {
            // Conservatively assume other instructions overwrite their usual
            // destination register
            const auto dst = static_cast<u32>(extractBits(inst, 0, 8));
            if (dst != RZ)
                reg_sources[dst] = UNKNOWN;
        }
    }

    return source;
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp
