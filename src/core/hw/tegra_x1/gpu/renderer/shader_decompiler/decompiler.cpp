#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.hpp"
#include "core/hw/tegra_x1/gpu/renderer/geometry.hpp"
#include "core/hw/tegra_x1/gpu/renderer/tessellation.hpp"
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
    ztd::io::MemoryStream& code_stream, const ShaderType guest_type,
    const GuestShaderState& state, ShaderBackend& out_backend,
    std::vector<u8>& out_code, ResourceMapping& out_resource_mapping,
    const std::function<std::optional<u32>(u32, u32)>& read_const_word) {
    // Geometry and tessellation shaders become vertex functions (see
    // geometry.hpp, tessellation.hpp)
    const bool geometry_shader = guest_type == ShaderType::Geometry;
    const bool tess_control = guest_type == ShaderType::TessControl;
    const bool tess_eval = guest_type == ShaderType::TessEvaluation;
    const ShaderType type = geometry_shader || tess_control || tess_eval
                                ? ShaderType::Vertex
                                : guest_type;
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
        // The sample mask follows the colors, depth the sample mask's
        // register (whether it is written or not)
        if (header.ps.omap_sample_mask)
            context.fragment_sample_mask_register = color_registers;
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
    UnsupportedCollector unsupported;
    // Geometry shaders run as vertex functions (see geometry.hpp)
    if (geometry_shader && state.geometry_input_vertices != 0) {
        context.geometry = true;
        context.geo.input_vertices = state.geometry_input_vertices;
        context.geo.input_primitive = state.geometry_input_primitive;
        context.geo.output = header.output_topology;
        context.geo.max_output_vertices = header.max_out_vtx_cnt;
        context.geo.invocations = std::max<u32>(header.thr_per_input_prim, 1);
        if (header.is_fast_gs)
            SHADER_UNSUPPORTED("Passthrough geometry shader");
        if (context.geo.output != static_cast<u32>(geometry::Output::Points) &&
            context.geo.output != static_cast<u32>(geometry::Output::LineStrip) &&
            context.geo.output !=
                static_cast<u32>(geometry::Output::TriangleStrip))
            SHADER_UNSUPPORTED("Geometry output topology {}",
                               context.geo.output);
    }
    context.geometry_capture =
        type == ShaderType::Vertex && state.geometry_capture;
    if (tess_control) {
        context.tess_control = true;
        context.tess.input_vertices = std::max<u32>(state.tessellation_patch_size, 1);
        // The header's threads per input primitive (yuzu)
        context.tess.output_vertices =
            header.thr_per_input_prim != 0 ? header.thr_per_input_prim
                                           : context.tess.input_vertices;
    }
    if (tess_eval) {
        context.tess_eval = true;
        context.tess.output_vertices =
            std::max<u32>(state.tessellation_control_points, 1);
        context.tess.domain = state.tessellation_domain;
    }
    ir::Module modul;
    // VertexA runs first, VertexB continues with its registers (see the
    // emitter's prior functions)
    if (type == ShaderType::Vertex && !geometry_shader && !tess_control &&
        !tess_eval && state.vertex_a_code &&
        state.vertex_a_code->size() > sizeof(ShaderHeader)) {
        std::vector<u8> vertex_a_code(*state.vertex_a_code);
        ztd::io::MemoryStream vertex_a_stream(vertex_a_code);
        const auto vertex_a_header = vertex_a_stream.read<ShaderHeader>();
        // Both programs use the one local memory array
        context.local_memory_size = std::max<u32>(
            context.local_memory_size, vertex_a_header.sh_local_mem_lo_sz +
                                           vertex_a_header.sh_local_mem_hi_sz +
                                           vertex_a_header.sh_local_mem_crs_sz);
        // The validated code size is VertexB's
        u64 vertex_a_read_size = 0;
        auto vertex_a_context = context;
        vertex_a_context.code_read_size = &vertex_a_read_size;
        ir::Builder builder(modul);
        builder.setInsertFunction("vertex_a");
        builder.setInsertBlock(0x0);
        ztd::io::StreamView stream(&vertex_a_stream, vertex_a_stream.getSeek());
        decoder::Decoder decoder(
            {.decomp_context = vertex_a_context,
             .code_stream = &stream,
             .builder = builder,
             .code_offset = static_cast<u64>(state.vertex_a_program_offset) +
                            vertex_a_stream.getSeek()});
        decoder.decode();
    }
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
    mem_analyzer.setGeometry(context.geometry || context.tess_control ||
                             context.tess_eval);
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
    out_resource_mapping.unsupported = std::move(unsupported.features);
}

std::string generateGeometryPassthrough(const GeometryPassthroughState& state) {
    // Record word of an output attribute address
    const auto word = [](u32 address) {
        return (address - geometry::RECORD_BASE) / 4;
    };
    std::string code = "#include <metal_stdlib>\nusing namespace metal;\n"
                       "struct StageOut {\n"
                       "    float4 position [[position, invariant]];\n";
    if (state.layer)
        code += "    uint layer [[render_target_array_index]];\n";
    if (state.viewport_index)
        code += "    uint viewport_index [[viewport_array_index]];\n";
    if (state.point_list)
        code += "    float point_size [[point_size]];\n";
    for (u32 i = 0; i < 32; i++)
        if ((state.output_mask & (1u << i)) != 0)
            code += fmt::format("    float4 user{0} [[user(locn{0})]];\n", i);
    const u32 clip_distances =
        state.clip_distance_mask == 0
            ? 0
            : 32 - static_cast<u32>(std::countl_zero(state.clip_distance_mask &
                                                     0xffu));
    if (clip_distances != 0)
        code += fmt::format("    float clip_distance [[clip_distance]] [{}];\n",
                            clip_distances);
    code += "};\n";
    code += fmt::format(
        "vertex StageOut main_(uint vid [[vertex_id]], const device uint* "
        "__records [[buffer({})]]) {{\n",
        geometry::OUTPUT_BUFFER);
    code += fmt::format("    const device uint* r = __records + vid * {}u;\n",
                        geometry::RECORD_WORDS);
    code += "    StageOut __out = {};\n";
    const u32 position = word(SV_POSITION_BASE);
    code += fmt::format(
        "    __out.position = float4(as_type<float>(r[{}]), "
        "as_type<float>(r[{}]), as_type<float>(r[{}]), as_type<float>(r[{}]));\n",
        position, position + 1, position + 2, position + 3);
    // As the vertex shader epilogue (MslEmitter::emitExitReturn)
    const bool window_space =
        state.window_space_width != 0 && state.window_space_height != 0;
    if (window_space) {
        code += fmt::format("    __out.position.x = (__out.position.x * {} - "
                            "1.0) * __out.position.w;\n",
                            2.0 / state.window_space_width);
        const double y_scale = 2.0 / state.window_space_height;
        if (state.y_negate)
            code += fmt::format("    __out.position.y = (__out.position.y * {} "
                                "- 1.0) * __out.position.w;\n",
                                y_scale);
        else
            code += fmt::format("    __out.position.y = (1.0 - "
                                "__out.position.y * {}) * __out.position.w;\n",
                                y_scale);
    }
    if (!state.depth_zero_to_one && !window_space)
        code += "    __out.position.z = (__out.position.z + __out.position.w) / "
                "2.0;\n";
    if (state.layer)
        code += fmt::format("    __out.layer = r[{}];\n", word(SV_LAYER_BASE));
    if (state.viewport_index)
        code += fmt::format("    __out.viewport_index = r[{}];\n",
                            word(SV_VIEWPORT_INDEX_BASE));
    if (state.point_list) {
        if (state.program_point_size)
            code += fmt::format("    __out.point_size = as_type<float>(r[{}]);\n",
                                word(SV_POINT_SIZE_BASE));
        else
            code += fmt::format("    __out.point_size = {};\n", state.point_size);
    }
    for (u32 i = 0; i < 32; i++) {
        if ((state.output_mask & (1u << i)) == 0)
            continue;
        const u32 base = word(SV_USER_IN_OUT_BASE + i * 16);
        code += fmt::format(
            "    __out.user{} = float4(as_type<float>(r[{}]), "
            "as_type<float>(r[{}]), as_type<float>(r[{}]), "
            "as_type<float>(r[{}]));\n",
            i, base, base + 1, base + 2, base + 3);
    }
    // Distances the guest enabled clip, as the vertex epilogue
    for (u32 i = 0; i < clip_distances; i++) {
        if ((state.clip_distance_mask & (1u << i)) != 0)
            code += fmt::format("    __out.clip_distance[{}] = as_type<float>(r[{}]);\n",
                                i, word(SV_CLIP_DISTANCE_BASE + i * 4));
        else
            code += fmt::format("    __out.clip_distance[{}] = 0.0;\n", i);
    }
    code += "    return __out;\n}\n";
    return code;
}

namespace {

// The parameters and index reads of geometry.hpp
constexpr const char* PRIMITIVE_INPUT_HELPERS = R"(
struct HydraGeometryParams {
    uint primitive_count;
    uint index_type;
    uint first;
    int base_vertex;
    uint min_vertex;
    uint vertex_count;
};
inline uint hydra_read_index(const device uchar* data, uint type, uint position) {
    switch (type) {
    case 0: return data[position];
    case 1: return ((const device ushort*)data)[position];
    case 2: return ((const device uint*)data)[position];
    default: return position;
    }
}
)";

} // namespace

std::string generateTessellationFactors(u32 domain, bool from_patches,
                                        const std::array<f32, 6>& levels) {
    const bool quad = domain == static_cast<u32>(tessellation::Domain::Quad);
    std::string code = "#include <metal_stdlib>\nusing namespace metal;\n";
    code += PRIMITIVE_INPUT_HELPERS;
    // MTLTriangleTessellationFactorsHalf, MTLQuadTessellationFactorsHalf
    code += quad ? "struct Factors { half edge[4]; half inside[2]; };\n"
                 : "struct Factors { half edge[3]; half inside; };\n";
    code += fmt::format(
        "vertex void main_(uint vid [[vertex_id]], const device float* "
        "patches [[buffer({})]], device Factors* factors [[buffer({})]], "
        "constant HydraGeometryParams& params [[buffer({})]]) {{\n",
        tessellation::PATCH_BUFFER, tessellation::FACTOR_BUFFER,
        geometry::PARAMS_BUFFER);
    code += "    if (vid >= params.primitive_count) return;\n";
    code += fmt::format("    float l[{}];\n", tessellation::LEVEL_WORDS);
    for (u32 i = 0; i < tessellation::LEVEL_WORDS; i++) {
        if (from_patches)
            code += fmt::format("    l[{0}] = patches[vid * {1}u + {0}u];\n",
                                i, tessellation::PATCH_WORDS);
        else
            code += fmt::format("    l[{}] = as_type<float>({}u);\n", i,
                                std::bit_cast<u32>(levels[i]));
    }
    // Outer levels are the edges (u = 0, v = 0, w = 0 or u = 1, v = 1 for
    // quads, as Direct3D), inner levels the inside
    if (quad) {
        for (u32 i = 0; i < 4; i++)
            code += fmt::format("    factors[vid].edge[{0}] = half(l[{0}]);\n", i);
        code += "    factors[vid].inside[0] = half(l[4]);\n";
        code += "    factors[vid].inside[1] = half(l[5]);\n";
    } else {
        for (u32 i = 0; i < 3; i++)
            code += fmt::format("    factors[vid].edge[{0}] = half(l[{0}]);\n", i);
        code += "    factors[vid].inside = half(l[4]);\n";
    }
    code += "}\n";
    return code;
}

std::string generateTessellationCopy(u32 patch_size) {
    std::string code = "#include <metal_stdlib>\nusing namespace metal;\n";
    code += PRIMITIVE_INPUT_HELPERS;
    code += fmt::format(
        "vertex void main_(uint vid [[vertex_id]], const device uint* "
        "records [[buffer({})]], const device uchar* index_data "
        "[[buffer({})]], device uint* control_points [[buffer({})]], "
        "constant HydraGeometryParams& params [[buffer({})]]) {{\n",
        geometry::INPUT_BUFFER, geometry::INDEX_DATA_BUFFER,
        tessellation::CONTROL_POINT_BUFFER, geometry::PARAMS_BUFFER);
    code += fmt::format("    const uint patch = vid / {0}u, point = vid % {0}u;\n",
                        patch_size);
    code += "    if (patch >= params.primitive_count) return;\n";
    code += fmt::format("    const uint position = params.first + patch * {}u + "
                        "point;\n",
                        patch_size);
    code += "    const uint slot = (params.index_type == 3u ? position : "
            "uint(int(hydra_read_index(index_data, params.index_type, "
            "position)) + params.base_vertex)) - params.min_vertex;\n";
    code += fmt::format("    for (uint i = 0u; i < {0}u; i++) control_points[vid * "
                        "{0}u + i] = records[slot * {0}u + i];\n",
                        geometry::RECORD_WORDS);
    code += "}\n";
    return code;
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
