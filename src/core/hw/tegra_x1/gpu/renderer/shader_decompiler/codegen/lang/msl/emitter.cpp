#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/msl/emitter.hpp"

#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/analyzer/memory_analyzer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/helper.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen::lang::
    msl {

namespace {

std::string_view pixelImapTypeToStr(PixelImapType type) {
    switch (type) {
    case PixelImapType::Constant:
        return "flat";
    case PixelImapType::Perspective:
        return "";
    case PixelImapType::ScreenLinear:
        return "center_no_perspective";
    default:
        LOG_ERROR(ShaderDecompiler, "Pixel imap unused");
        return "";
    }
}

renderer::TextureType toRendererTextureType(TextureType type) {
    switch (type) {
    case TextureType::_1D:
        return renderer::TextureType::_1D;
    case TextureType::_1DArray:
        return renderer::TextureType::_1DArray;
    case TextureType::_2D:
        return renderer::TextureType::_2D;
    case TextureType::_2DArray:
        return renderer::TextureType::_2DArray;
    case TextureType::_3D:
    case TextureType::_3DArray:
        return renderer::TextureType::_3D;
    case TextureType::Cube:
        return renderer::TextureType::Cube;
    case TextureType::CubeArray:
        return renderer::TextureType::CubeArray;
    case TextureType::Buffer:
        return renderer::TextureType::_1DBuffer;
    default:
        return renderer::TextureType::_2D;
    }
}

std::string textureTypeToStr(TextureType type, bool is_depth) {
    // TODO: check if depth can be used with the type
    std::string prefix = is_depth ? "depth" : "texture";
    switch (type) {
    case TextureType::_1D:
        return prefix + "1d";
    case TextureType::_1DArray:
        return prefix + "1d_array";
    case TextureType::_2D:
        return prefix + "2d";
    case TextureType::_2DArray:
        return prefix + "2d_array";
    case TextureType::_3D:
        return prefix + "3d";
    case TextureType::_3DArray:
        return prefix + "3d_array";
    case TextureType::Cube:
        return prefix + "cube";
    case TextureType::CubeArray:
        return prefix + "cube_array";
    case TextureType::Buffer:
        return prefix + "_buffer";
    }
}

std::string textureDeclaration(const GuestShaderState& state, u32 index,
                               const analyzer::TextureInfo& info) {
    const auto it = state.buffer_texture_formats.find(index);
    if (it == state.buffer_texture_formats.end())
        return textureTypeToStr(info.type, info.is_depth) + "<float>";
    switch (it->second) {
    case renderer::TextureFormat::R8Uint:
    case renderer::TextureFormat::R16Uint:
    case renderer::TextureFormat::R32Uint:
    case renderer::TextureFormat::RG8Uint:
    case renderer::TextureFormat::RG16Uint:
    case renderer::TextureFormat::RG32Uint:
    case renderer::TextureFormat::RGBA8Uint:
    case renderer::TextureFormat::RGBA16Uint:
    case renderer::TextureFormat::RGBA32Uint:
        return "texture_buffer<uint>";
    case renderer::TextureFormat::R8Sint:
    case renderer::TextureFormat::R16Sint:
    case renderer::TextureFormat::R32Sint:
    case renderer::TextureFormat::RG8Sint:
    case renderer::TextureFormat::RG16Sint:
    case renderer::TextureFormat::RG32Sint:
    case renderer::TextureFormat::RGBA8Sint:
    case renderer::TextureFormat::RGBA16Sint:
    case renderer::TextureFormat::RGBA32Sint:
        return "texture_buffer<int>";
    default:
        return "texture_buffer<float>";
    }
}

// Component type an image is accessed with
std::string_view imageComponentType(const GuestShaderState& state, u32 index,
                                    bool is_atomic) {
    // Atomics are only supported on integer textures
    const std::string_view fallback = is_atomic ? "uint" : "float";
    const auto it = state.image_formats.find(index);
    if (it == state.image_formats.end())
        return fallback;
    switch (it->second) {
    case renderer::TextureFormat::R8Uint:
    case renderer::TextureFormat::R16Uint:
    case renderer::TextureFormat::R32Uint:
    case renderer::TextureFormat::RG8Uint:
    case renderer::TextureFormat::RG16Uint:
    case renderer::TextureFormat::RG32Uint:
    case renderer::TextureFormat::RGBA8Uint:
    case renderer::TextureFormat::RGBA16Uint:
    case renderer::TextureFormat::RGBA32Uint:
        return "uint";
    case renderer::TextureFormat::R8Sint:
    case renderer::TextureFormat::R16Sint:
    case renderer::TextureFormat::R32Sint:
    case renderer::TextureFormat::RG8Sint:
    case renderer::TextureFormat::RG16Sint:
    case renderer::TextureFormat::RG32Sint:
    case renderer::TextureFormat::RGBA8Sint:
    case renderer::TextureFormat::RGBA16Sint:
    case renderer::TextureFormat::RGBA32Sint:
        return "int";
    default:
        return fallback;
    }
}

std::string imageDeclaration(const GuestShaderState& state, u32 index,
                             const analyzer::ImageInfo& info) {
    return fmt::format("{}<{}, access::{}>", textureTypeToStr(info.type, false),
                       imageComponentType(state, index, info.is_atomic),
                       info.is_atomic ? "read_write" : "write");
}

std::string_view componentToStr(u8 component) {
    switch (component) {
    case 0:
        return "x";
    case 1:
        return "y";
    case 2:
        return "z";
    case 3:
        return "w";
    default:
        return INVALID_VALUE;
    }
}

// TODO: adjust for individual texture types
std::string_view dimensionToStr(u32 dimension) {
    switch (dimension) {
    case 0:
        return "width";
    case 1:
        return "height";
    case 2:
        return "depth";
    default:
        return INVALID_VALUE;
    }
}

} // namespace

MslEmitter::MslEmitter(const DecompilerContext& context,
                       const analyzer::MemoryAnalyzer& memory_analyzer,
                       const GuestShaderState& state, std::vector<u8>& out_code,
                       ResourceMapping& out_resource_mapping)
    : LangEmitter(context, memory_analyzer, state, out_code,
                  out_resource_mapping) {
    for (auto index : memory_analyzer.getConstBuffers()) {
        out_resource_mapping.uniform_buffers[index] = index;
    }

    // TODO: storage buffers

    u32 texture_index = 0;
    for (const auto& [const_buffer_index, info] :
         memory_analyzer.getTextures()) {
        out_resource_mapping.textures[const_buffer_index] = texture_index++;
        out_resource_mapping.texture_types[const_buffer_index] =
            toRendererTextureType(info.type);
    }

    if (context.type == ShaderType::Fragment) {
        for (const auto input : memory_analyzer.getStageInputs())
            out_resource_mapping.stage_input_mask |= 1u << input;
    }

    if (texture_index > 16) {
        // Direct Metal sampler arguments are limited to 16. Use a separate
        // argument buffer for larger shaders, avoiding guest uniform buffers
        // and the upper 16 slots reserved for vertex buffers.
        const u32 limit = context.type == ShaderType::Vertex ? 15 : 31;
        for (u32 index = 0; index < limit; ++index) {
            if (out_resource_mapping.uniform_buffers[index] == invalid<u32>()) {
                out_resource_mapping.sampler_buffer_index = index;
                break;
            }
        }
        ASSERT(out_resource_mapping.sampler_buffer_index != invalid<u32>(),
               ShaderDecompiler, "No buffer slot available for shader samplers");
    }

    // Images share the texture slots
    for (const auto& [index, info] : memory_analyzer.getImages()) {
        out_resource_mapping.images[index] = texture_index++;
        out_resource_mapping.image_types[index] =
            toRendererTextureType(info.type);
    }

    // Only shaders that use local memory pay for the per-thread array
    local_word_count =
        memory_analyzer.getUsesLocalMemory()
            ? std::clamp((context.local_memory_size + 3) / 4, 1u, 0x1000u)
            : 1u;
    if (context.type == ShaderType::Compute)
        shared_word_count =
            std::clamp((state.shared_memory_size + 3) / 4, 1u, 0x2000u);
}

void MslEmitter::emitHeader() {
    write("#include <metal_stdlib>");
    write("using namespace metal;");
    if (context.type == ShaderType::Compute) {
        write("{}", R"(
            inline uint hydra_shared_wrap(threadgroup atomic_uint* word, uint limit, bool increment) {
                uint old = atomic_load_explicit(word, memory_order_relaxed);
                for (;;) {
                    uint next = increment ? (old >= limit ? 0u : old + 1u)
                                          : (old == 0u || old > limit ? limit : old - 1u);
                    if (atomic_compare_exchange_weak_explicit(word, &old, next,
                            memory_order_relaxed, memory_order_relaxed)) return old;
                }
            }
        )");
    }

    if (out_resource_mapping.sampler_buffer_index != invalid<u32>()) {
        write("struct HydraSamplers {{");
        write("    array<sampler, {}> values [[id(0)]];",
              memory_analyzer.getTextures().size());
        write("}};");
    }

    // Bit counting helpers
    writeNewline();
    write("inline uint hydra_find_umsb(uint v) {{");
    write("    return v == 0u ? 0xffffffffu : 31u - clz(v);");
    write("}}");
    write("inline uint hydra_find_smsb(uint v) {{");
    write("    return hydra_find_umsb(int(v) < 0 ? ~v : v);");
    write("}}");

    // Quad helpers (lanes are laid out as 2x2 pixel quads)
    if (context.type == ShaderType::Fragment) {
        writeNewline();
        // Exact value of the neighbouring lane in the quad
        // Guest lanes are laid out as x | (y << 1) within a 2x2 quad. The host
        // quad layout is unknown, so the neighbours are identified by their
        // positions instead
        write("inline ushort hydra_quad_host_mask_x(float px) {{");
        write("    return quad_shuffle_xor(px, ushort(1)) != px ? ushort(1) : "
              "ushort(2);");
        write("}}");
        write("inline uint hydra_quad_shuffle_xor(uint v, uint mask, float px) "
              "{{");
        write("    const ushort x_mask = hydra_quad_host_mask_x(px);");
        write("    const ushort y_mask = x_mask ^ ushort(3);");
        write("    ushort host_mask = 0;");
        write("    if ((mask & 1u) != 0u) host_mask |= x_mask;");
        write("    if ((mask & 2u) != 0u) host_mask |= y_mask;");
        write("    return quad_shuffle_xor(v, host_mask);");
        write("}}");
        write("inline float hydra_swizzle_add(float a, float b, uint swizzle, "
              "float px, float py) {{");
        write("    const ushort x_mask = hydra_quad_host_mask_x(px);");
        write("    const ushort y_mask = x_mask ^ ushort(3);");
        write("    const uint x_bit = px > quad_shuffle_xor(px, x_mask) ? 1u : 0u;");
        write("    const uint y_bit = py > quad_shuffle_xor(py, y_mask) ? 1u : 0u;");
        write("    const uint lane = x_bit | (y_bit << 1);");
        write("    const uint mode = (swizzle >> (lane * 2u)) & 3u;");
        write("    const float lut_a[4] = {{1.0, -1.0, 1.0, 0.0}};");
        write("    const float lut_b[4] = {{1.0, 1.0, -1.0, 1.0}};");
        write("    return a * lut_a[mode] + b * lut_b[mode];");
        write("}}");
    }
}

void MslEmitter::emitQuadShuffleXor(const ir::Value& dst, const ir::Value& src,
                                    const ir::Value& mask) {
    if (context.type != ShaderType::Fragment) {
        // TODO: implement for other stages
        storeValue(dst, "{}", getValueStr(src));
        return;
    }

    storeValue(dst, "hydra_quad_shuffle_xor({}, {}, {})", getValueStr(src),
               getValueStr(mask),
               getAttrMemoryStr({RZ, SV_POSITION_BASE + 0x0, true},
                                ir::ScalarType::F32));
}

void MslEmitter::emitSwizzleAdd(const ir::Value& dst, const ir::Value& src_a,
                                const ir::Value& src_b, u32 swizzle) {
    if (context.type != ShaderType::Fragment) {
        // TODO: implement for other stages
        storeValue(dst, "({} + {})", getValueStr(src_a), getValueStr(src_b));
        return;
    }

    storeValue(dst, "hydra_swizzle_add({}, {}, {}u, {}, {})",
               getValueStr(src_a), getValueStr(src_b), swizzle,
               getAttrMemoryStr({RZ, SV_POSITION_BASE + 0x0, true},
                                ir::ScalarType::F32),
               getAttrMemoryStr({RZ, SV_POSITION_BASE + 0x4, true},
                                ir::ScalarType::F32));
}

void MslEmitter::emitTypeAliases() {
    write("using u8 = uint8_t;");
    write("using u16 = uint16_t;");
    write("using u32 = uint32_t;");
    write("using i8 = int8_t;");
    write("using i16 = int16_t;");
    write("using i32 = int32_t;");
    write("using f16 = half;");
    write("using f32 = float;");
}

void MslEmitter::emitDeclarations() {
    // Stage inputs

    enterScope("struct StageIn");

    // SVs
    // Handled in GetMainArgs

    // Stage inputs
    switch (context.type) {
    case ShaderType::Vertex:
        for (u8 i = 0; i < VERTEX_ATTRIB_COUNT; i++) {
            const auto vertex_attrib_state = state.vertex_attrib_states[i];
            if (vertex_attrib_state.type == engines::VertexAttribType::None)
                continue;

            // HACK: how are attributes disabled?
            if (vertex_attrib_state.is_fixed)
                continue;

            const auto sv = Sv(SvSemantic::UserInOut, i);
            write("vec<{}, 4> {} [[{}]];", toType(vertex_attrib_state),
                  getSvStr(sv), getSvQualifierStr(sv, false));
        }
        break;
    case ShaderType::Fragment:
        write("float4 position [[position]];");
        for (const auto input : memory_analyzer.getStageInputs()) {
            const auto sv = Sv(SvSemantic::UserInOut, input);
            const auto attribute = pixelImapTypeToStr(
                context.frag.pixel_imaps[input].getFirstUsedType());
            // TODO: don't hardcode the type
            write("float4 {} [[{}{}{}]];", getSvStr(sv),
                  getSvQualifierStr(sv, false), attribute.empty() ? "" : ", ",
                  attribute);
        }
        break;
    default:
        break;
    }

    exitScopeEmpty(true);
    writeNewline();

    // Stage outputs

    enterScope("struct StageOut");

    // SVs
    // HACK: always write position in vertex shaders
    if (context.type == ShaderType::Vertex)
        write("float4 position [[position, invariant]];");
    for (const auto sv_semantic : memory_analyzer.getOutputSVs()) {
        // NOLINTNEXTLINE(readability-trivial-switch)
        switch (sv_semantic) {
        case SvSemantic::Position:
            // Write("float4 position [[position]];");
            break;
        default:
            LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Output SV semantic {}",
                                sv_semantic);
            break;
        }
    }

    // Stage outputs
    switch (context.type) {
    case ShaderType::Vertex: {
        // Also declare outputs read by the fragment shader, but never written
        // (they are zero initialized)
        u32 output_mask = state.fragment_input_mask;
        for (const auto output : memory_analyzer.getStageOutputs())
            output_mask |= 1u << output;
        for (u8 i = 0; i < 32; i++) {
            if ((output_mask & (1u << i)) == 0)
                continue;
            const auto sv = Sv(SvSemantic::UserInOut, i);
            // TODO: don't hardcode the type
            write("float4 {} [[{}]];", getSvStr(sv),
                  getSvQualifierStr(sv, true));
        }
        break;
    }
    case ShaderType::Fragment:
        for (u8 i = 0; i < COLOR_TARGET_COUNT; i++) {
            const auto color_target_data_type =
                state.color_target_data_types[i];
            if (color_target_data_type == ColorDataType::Invalid)
                continue;

            const auto sv = Sv(SvSemantic::UserInOut, i);
            write("vec<{}, 4> {} [[{}]];", toType(color_target_data_type),
                  getSvStr(sv), getSvQualifierStr(sv, true));
        }
        break;
    default:
        break;
    }

    exitScopeEmpty(true);
    writeNewline();
    ;
}

void MslEmitter::emitStateBindings() {
    // Local memory reads as zero until written, only initialized when used
    if (memory_analyzer.getUsesLocalMemory())
        writeStatement("Reg lmem[{}] = {{}}", local_word_count);
    else
        writeStatement("Reg lmem[1]");

    // Compute
    if (context.type == ShaderType::Compute) {
        writeStatement("uint3 tid");
        writeStatement("uint3 ctaid");
        writeStatement("uint lane");
        writeStatement("threadgroup Reg* smem");
    }

    // Storage buffers
    // TODO

    // Images
    for (const auto& [index, info] : memory_analyzer.getImages())
        writeStatement("{} img{}", imageDeclaration(state, index, info), index);

    // Textures
    for (const auto& [const_buffer_index, info] :
         memory_analyzer.getTextures()) {
        // TODO: don't hardcode type
        writeStatement("{} tex{}",
                       textureDeclaration(state, const_buffer_index, info),
                       const_buffer_index);
        writeStatement("sampler samplr{}", const_buffer_index);
    }
}

void MslEmitter::emitStateBindingAssignments() {
    // Storage buffers
    // TODO

    // Images
    for (const auto& [index, _] : memory_analyzer.getImages())
        writeStatement("state.img{} = img{}", index, index);

    // Textures
    for (const auto& [const_buffer_index, _] : memory_analyzer.getTextures()) {
        writeStatement("state.tex{} = tex{}", const_buffer_index,
                       const_buffer_index);
        if (out_resource_mapping.sampler_buffer_index != invalid<u32>())
            writeStatement("state.samplr{} = __samplers.values[{}]",
                           const_buffer_index,
                           out_resource_mapping.textures.at(const_buffer_index));
        else
            writeStatement("state.samplr{} = samplr{}", const_buffer_index,
                           const_buffer_index);
    }
}

void MslEmitter::emitMainPrototype() {
    switch (context.type) {
    case ShaderType::Vertex:
        writeRaw("vertex ");
        break;
    case ShaderType::Fragment:
        writeRaw("fragment ");
        break;
    case ShaderType::Compute:
        writeRaw("kernel ");
        break;
    default:
        writeRaw(INVALID_VALUE " ");
        break;
    }
    if (context.type == ShaderType::Compute)
        writeRaw("void main_(uint3 __tid [[thread_position_in_threadgroup]], "
                 "uint3 __ctaid [[threadgroup_position_in_grid]], uint __lane "
                 "[[thread_index_in_simdgroup]]");
    else
        writeRaw("StageOut main_(StageIn __in [[stage_in]]");

#define ADD_ARG(f, ...) writeRaw(", " f ZTD_PASS_VA_ARGS(__VA_ARGS__))

    // Input SVs
    switch (context.type) {
    case ShaderType::Vertex:
        ADD_ARG("uint iid [[instance_id]]");
        ADD_ARG("uint vid [[vertex_id]]");
        break;
    case ShaderType::Fragment:
        break;
    default:
        break;
    }

    // Uniform buffers
    for (auto index : memory_analyzer.getConstBuffers()) {
        ADD_ARG("constant Reg* c{} [[buffer({})]]", index, index,
                out_resource_mapping.uniform_buffers[index]);
    }

    // Storage buffers
    // TODO

    // Textures
    if (out_resource_mapping.sampler_buffer_index != invalid<u32>())
        ADD_ARG("constant HydraSamplers& __samplers [[buffer({})]]",
                out_resource_mapping.sampler_buffer_index);
    for (const auto& [const_buffer_index, info] :
         memory_analyzer.getTextures()) {
        const auto index = out_resource_mapping.textures[const_buffer_index];
        // TODO: don't hardcode type
        ADD_ARG("{} tex{} [[texture({})]]",
                textureDeclaration(state, const_buffer_index, info),
                const_buffer_index, index);
        if (out_resource_mapping.sampler_buffer_index == invalid<u32>())
            ADD_ARG("sampler samplr{} [[sampler({})]]", const_buffer_index, index);
    }

    // Images
    for (const auto& [index, info] : memory_analyzer.getImages()) {
        ADD_ARG("{} img{} [[texture({})]]",
                imageDeclaration(state, index, info), index,
                out_resource_mapping.images[index]);
    }

#undef ADD_ARG

    enterScope(")");

    if (context.type == ShaderType::Compute) {
        writeStatement("threadgroup Reg __smem[{}]", shared_word_count);
        writeNewline();
        emitMainFunctionPrologue();
        writeStatement("state.tid = __tid");
        writeStatement("state.ctaid = __ctaid");
        writeStatement("state.lane = __lane");
        writeStatement("state.smem = __smem");
        return;
    }

    // Output
    write("StageOut __out = {{}};");
    writeNewline();

    emitMainFunctionPrologue();
}

void MslEmitter::emitExitReturn() {
    if (context.type == ShaderType::Compute) {
        writeStatement("return");
        return;
    }

    if (context.type == ShaderType::Vertex) {
        // Flip vertically
        // TODO: handle this with viewports?
        // WriteStatement("__out.position.y = -__out.position.y");

        // Convert depth from < -1, 1 > to < 0, 1 >
        if (!state.depth_zero_to_one)
            writeStatement("__out.position.z = (__out.position.z + "
                           "__out.position.w) / 2.0");
    }

    // DEBUG: highlight
    if (context.type == ShaderType::Fragment && state.debug_highlight) {
        for (u8 i = 0; i < COLOR_TARGET_COUNT; i++) {
            const auto data_type = state.color_target_data_types[i];
            if (data_type == ColorDataType::Invalid)
                continue;
            const auto sv = Sv(SvSemantic::UserInOut, i);
            static const std::string color = [] {
                if (std::getenv("HYDRA_DEBUG_CAPTURE") != nullptr)
                    return std::string("state.dbg.xyz * 0.5 + 0.5, 1");
                const char* env = std::getenv("HYDRA_HIGHLIGHT_COLOR");
                return std::string(env != nullptr ? env : "1, 0, 1, 1");
            }();
            writeStatement("__out.{} = vec<{}, 4>({})", getSvStr(sv),
                           toType(data_type), color);
        }
    }

    // Return
    writeStatement("return __out");
}

// Data
void MslEmitter::emitBitCast(const ir::Value& dst, const ir::Value& src) {
    storeValue(dst, "as_type<{}>({})", getTypeStr(dst.getType()),
               getValueStr(src));
}

// Math
void MslEmitter::emitIsNan(const ir::Value& dst, const ir::Value& src) {
    storeValue(dst, "isnan({})", getValueStr(src));
}

void MslEmitter::emitReciprocal(const ir::Value& dst, const ir::Value& src) {
    storeValue(dst, "(1.0 / {})", getValueStr(src));
}

void MslEmitter::emitSin(const ir::Value& dst, const ir::Value& src) {
    // Precise, since games often use sin with large arguments for noise
    storeValue(dst, "precise::sin({})", getValueStr(src));
}

void MslEmitter::emitCos(const ir::Value& dst, const ir::Value& src) {
    storeValue(dst, "precise::cos({})", getValueStr(src));
}

void MslEmitter::emitExp2(const ir::Value& dst, const ir::Value& src) {
    storeValue(dst, "exp2({})", getValueStr(src));
}

void MslEmitter::emitLog2(const ir::Value& dst, const ir::Value& src) {
    storeValue(dst, "log2({})", getValueStr(src));
}

void MslEmitter::emitSqrt(const ir::Value& dst, const ir::Value& src) {
    storeValue(dst, "sqrt({})", getValueStr(src));
}

void MslEmitter::emitReciprocalSqrt(const ir::Value& dst,
                                    const ir::Value& src) {
    storeValue(dst, "rsqrt({})", getValueStr(src));
}

// Logical & Bitwise
void MslEmitter::emitBitfieldExtract(const ir::Value& dst,
                                     const ir::Value& src_a,
                                     const ir::Value& src_b,
                                     const ir::Value& src_c) {
    storeValue(dst, "extract_bits({}, {}, {})", getValueStr(src_a),
               getValueStr(src_b), getValueStr(src_c));
}

// Texture
void MslEmitter::emitTextureSample(const ir::Value& dst, u32 const_buffer_index,
                                   TextureType type, TextureSampleFlags flags,
                                   const ir::Value& array_index,
                                   const ir::Value& coords,
                                   const ir::Value& cmp_value,
                                   const ir::Value& lod) {
    // Flags
    const auto is_array = isTextureArray(type);
    const auto is_buffer =
        state.buffer_texture_formats.contains(const_buffer_index);
    const auto int_coords =
        is_buffer || any(flags & TextureSampleFlags::IntCoords);
    const auto depth_compare = any(flags & TextureSampleFlags::DepthCompare);
    const auto has_lod = any(flags & TextureSampleFlags::Lod);

    std::string func_name;
    std::string args;
    if (int_coords) {
        func_name = "read";
        // Number of coordinates provided by the instruction
        u32 coord_count = 1;
        switch (type) {
        case TextureType::_2D:
        case TextureType::_2DArray:
            coord_count = 2;
            break;
        case TextureType::_3D:
        case TextureType::_3DArray:
            coord_count = 3;
            break;
        default:
            break;
        }

        // Buffer textures are always addressed with a single coordinate
        const auto coords_str = getValueStr(coords);
        if (coord_count == 1)
            args = fmt::format("as_type<uint>({})", coords_str);
        else if (is_buffer)
            args = fmt::format("as_type<uint>(({}).x)", coords_str);
        else
            args = fmt::format("as_type<uint{}>({})", coord_count, coords_str);
        if (depth_compare) {
            // TODO: emulate
            LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Texture read depth compare");
        }
    } else {
        func_name = "sample";
        if (depth_compare)
            func_name += "_compare";
        args = fmt::format("state.samplr{}, {}", const_buffer_index,
                           getValueStr(coords));
    }

    // Args
    if (is_array && !is_buffer)
        args += fmt::format(", uint({})", getValueStr(array_index));
    if (depth_compare)
        args += fmt::format(", {}", getValueStr(cmp_value));
    if (has_lod && !is_buffer) {
        if (int_coords)
            args += fmt::format(", as_type<uint>({})", getValueStr(lod));
        else
            args += fmt::format(", level({})", getValueStr(lod));
    } else if (!int_coords && context.type == ShaderType::Compute) {
        // There are no derivatives in compute kernels
        args += ", level(0.0)";
    }

    std::string res =
        fmt::format("state.tex{}.{}({})", const_buffer_index, func_name, args);
    // HACK: construct float4 if sample_compare
    if (depth_compare)
        res = fmt::format("float4({}, 0.0, 0.0, 0.0)", res);
    if (is_buffer) {
        const auto decl = textureDeclaration(
            state, const_buffer_index, analyzer::TextureInfo{type, false});
        if (decl != "texture_buffer<float>")
            res = fmt::format("as_type<float4>({})", res);
    }
    storeValue(dst, "{}", res);
}

void MslEmitter::emitTextureGather(const ir::Value& dst, u32 const_buffer_index,
                                   const ir::Value& coords, u8 component,
                                   const ir::Value& cmp_value) {
    if (cmp_value.getKind() != ir::ValueKind::Undefined) {
        storeValue(dst, "state.tex{}.gather_compare(state.samplr{}, {}, {})",
                   const_buffer_index, const_buffer_index, getValueStr(coords),
                   getValueStr(cmp_value));
        return;
    }

    storeValue(dst,
               "state.tex{}.gather(state.samplr{}, {}, int2(0), component::{})",
               const_buffer_index, const_buffer_index, getValueStr(coords),
               componentToStr(component));
}

void MslEmitter::emitTextureQueryDimension(const ir::Value& dst,
                                           u32 const_buffer_index,
                                           u32 dimension) {
    storeValue(dst, "state.tex{}.get_{}()", const_buffer_index,
               dimensionToStr(dimension));
}

// Exit
void MslEmitter::emitDiscard() { writeStatement("discard_fragment()"); }

// Bit counting
void MslEmitter::emitPopCount(const ir::Value& dst, const ir::Value& src) {
    storeValue(dst, "popcount(uint({}))", getBitwiseOperandStr(src));
}

void MslEmitter::emitFindMsb(const ir::Value& dst, const ir::Value& src,
                             bool is_signed) {
    storeValue(dst, "hydra_find_{}msb(uint({}))", is_signed ? "s" : "u",
               getBitwiseOperandStr(src));
}

// Compute
void MslEmitter::emitReadSystemRegister(const ir::Value& dst, u32 sr) {
    std::string value;
    switch (sr) {
    case 0x00: // Lane ID
        value = "state.lane";
        break;
    case 0x20: // Packed thread ID
        value = "(state.tid.x | (state.tid.y << 16) | (state.tid.z << 26))";
        break;
    case 0x21:
    case 0x22:
    case 0x23:
        value = fmt::format("state.tid.{}", componentToStr(static_cast<u8>(sr - 0x21)));
        break;
    case 0x25:
    case 0x26:
    case 0x27:
        value = fmt::format("state.ctaid.{}", componentToStr(static_cast<u8>(sr - 0x25)));
        break;
    case 0x38: // Equal lane mask
        value = "(1u << state.lane)";
        break;
    case 0x39: // Less
        value = "((1u << state.lane) - 1u)";
        break;
    case 0x3a: // Less or equal
        value = "((2u << state.lane) - 1u)";
        break;
    case 0x3b: // Greater
        value = "(~((2u << state.lane) - 1u))";
        break;
    case 0x3c: // Greater or equal
        value = "(~((1u << state.lane) - 1u))";
        break;
    default:
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "System register {:#x}", sr);
        value = "0u";
        break;
    }
    storeValue(dst, "uint({})", value);
}

void MslEmitter::emitLoadShared(const ir::Value& dst, const ir::Value& addr) {
    storeValue(dst, "state.smem[min(uint({}) >> 2u, {}u)]._u32",
               getBitwiseOperandStr(addr), shared_word_count - 1);
}

void MslEmitter::emitStoreShared(const ir::Value& addr,
                                 const ir::Value& value) {
    writeStatement("state.smem[min(uint({}) >> 2u, {}u)]._u32 = uint({})",
                   getBitwiseOperandStr(addr), shared_word_count - 1,
                   getBitwiseOperandStr(value));
}

void MslEmitter::emitLoadLocal(const ir::Value& dst, const ir::Value& addr) {
    storeValue(dst, "state.lmem[min(uint({}) >> 2u, {}u)]._u32",
               getBitwiseOperandStr(addr), local_word_count - 1);
}

void MslEmitter::emitStoreLocal(const ir::Value& addr,
                                const ir::Value& value) {
    writeStatement("state.lmem[min(uint({}) >> 2u, {}u)]._u32 = uint({})",
                   getBitwiseOperandStr(addr), local_word_count - 1,
                   getBitwiseOperandStr(value));
}

void MslEmitter::emitAtomicShared(const ir::Value& dst, const ir::Value& addr,
                                  const ir::Value& value, u32 op, bool is_signed) {
    const bool signed_minmax = is_signed && (op == 1 || op == 2);
    const auto pointer = fmt::format(
        "reinterpret_cast<threadgroup atomic_{}*>(&state.smem[min(uint({}) >> 2u, {}u)]._u32)",
        signed_minmax ? "int" : "uint", getBitwiseOperandStr(addr), shared_word_count - 1);
    if (op == 3 || op == 4) {
        storeValue(dst, "hydra_shared_wrap({}, uint({}), {})", pointer,
                   getBitwiseOperandStr(value), op == 3 ? "true" : "false");
        return;
    }
    constexpr std::array<std::string_view, 9> functions{
        "atomic_fetch_add_explicit", "atomic_fetch_min_explicit", "atomic_fetch_max_explicit",
        "", "", "atomic_fetch_and_explicit", "atomic_fetch_or_explicit",
        "atomic_fetch_xor_explicit", "atomic_exchange_explicit"};
    ASSERT(op < functions.size(), ShaderDecompiler, "Invalid shared atomic operation");
    storeValue(dst, "as_type<uint>({}({}, as_type<{}>(uint({})), memory_order_relaxed))",
               functions[op], pointer, signed_minmax ? "int" : "uint", getBitwiseOperandStr(value));
}

void MslEmitter::emitBarrier() {
    writeStatement("threadgroup_barrier(mem_flags::mem_threadgroup | "
                   "mem_flags::mem_device | mem_flags::mem_texture)");
}

std::string MslEmitter::getImageBoundsStr(u32 index, TextureType type,
                                          const std::string& coords) {
    switch (type) {
    case TextureType::_1D:
    case TextureType::Buffer:
        return fmt::format("{} < state.img{}.get_width()", coords, index);
    case TextureType::_1DArray:
        return fmt::format("{0}.x < state.img{1}.get_width() && {0}.y < "
                           "state.img{1}.get_array_size()",
                           coords, index);
    case TextureType::_2D:
        return fmt::format("{0}.x < state.img{1}.get_width() && {0}.y < "
                           "state.img{1}.get_height()",
                           coords, index);
    case TextureType::_2DArray:
        return fmt::format("{0}.x < state.img{1}.get_width() && {0}.y < "
                           "state.img{1}.get_height() && {0}.z < "
                           "state.img{1}.get_array_size()",
                           coords, index);
    case TextureType::_3D:
        return fmt::format("{0}.x < state.img{1}.get_width() && {0}.y < "
                           "state.img{1}.get_height() && {0}.z < "
                           "state.img{1}.get_depth()",
                           coords, index);
    default:
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Image type {}",
                            static_cast<u32>(type));
        return "false";
    }
}

std::string MslEmitter::getImageCoordsStr(TextureType type,
                                          const std::string& coords) {
    switch (type) {
    case TextureType::_1DArray:
        return fmt::format("{0}.x, {0}.y", coords);
    case TextureType::_2D:
        return fmt::format("{}.xy", coords);
    case TextureType::_2DArray:
        return fmt::format("{0}.xy, {0}.z", coords);
    default:
        return coords;
    }
}

void MslEmitter::emitImageStore(u32 index, TextureType type,
                                const ir::Value& coords,
                                const std::array<ir::Value, 4>& values) {
    const auto& info = memory_analyzer.getImages().at(index);
    const auto component_type =
        imageComponentType(state, index, info.is_atomic);
    std::string value = fmt::format("vec<{}, 4>(", component_type);
    for (u32 i = 0; i < 4; i++) {
        if (i != 0)
            value += ", ";
        value += fmt::format("as_type<{}>(uint({}))", component_type,
                             getBitwiseOperandStr(values[i]));
    }
    value += ")";

    const auto coords_str = getValueStr(coords);
    writeStatement("if ({}) state.img{}.write({}, {})",
                   getImageBoundsStr(index, type, coords_str), index, value,
                   getImageCoordsStr(type, coords_str));
}

void MslEmitter::emitImageAtomic(const ir::Value& dst, u32 index,
                                 TextureType type, u32 op,
                                 const ir::Value& coords,
                                 const ir::Value& value) {
    std::string_view func;
    std::string operand = getBitwiseOperandStr(value);
    switch (op) {
    case 0:
        func = "atomic_fetch_add";
        break;
    case 1:
        func = "atomic_fetch_min";
        break;
    case 2:
        func = "atomic_fetch_max";
        break;
    case 3: // TODO: wrap
        func = "atomic_fetch_add";
        operand = "1u";
        break;
    case 4: // TODO: wrap
        func = "atomic_fetch_sub";
        operand = "1u";
        break;
    case 5:
        func = "atomic_fetch_and";
        break;
    case 6:
        func = "atomic_fetch_or";
        break;
    case 7:
        func = "atomic_fetch_xor";
        break;
    case 8:
        func = "atomic_exchange";
        break;
    default:
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Image atomic op {}", op);
        func = "atomic_fetch_add";
        operand = "0u";
        break;
    }

    const auto component_type = imageComponentType(state, index, true);
    const auto coords_str = getValueStr(coords);
    std::string operand_str =
        fmt::format("as_type<{}>(uint({}))", component_type, operand);
    if (op == 8)
        operand_str = fmt::format("vec<{}, 4>({})", component_type, operand_str);
    storeValue(dst, "(({}) ? as_type<uint>(state.img{}.{}({}, {}).x) : 0u)",
               getImageBoundsStr(index, type, coords_str), index, func,
               getImageCoordsStr(type, coords_str), operand_str);
}

// Warp
void MslEmitter::emitVote(const ir::Value& dst, const ir::Value& pred,
                          u32 mode) {
    const auto value = getValueStr(pred);
    switch (mode) {
    case 1:
        storeValue(dst, "simd_any({})", value);
        break;
    case 2:
        storeValue(dst, "(simd_all({0}) || !simd_any({0}))", value);
        break;
    default:
        storeValue(dst, "simd_all({})", value);
        break;
    }
}

void MslEmitter::emitBallot(const ir::Value& dst, const ir::Value& pred) {
    storeValue(dst,
               "uint(static_cast<simd_vote::vote_t>(simd_ballot({})))",
               getValueStr(pred));
}

void MslEmitter::emitShuffle(const ir::Value& dst, const ir::Value& value,
                             const ir::Value& lane) {
    storeValue(dst, "simd_shuffle(uint({}), ushort({}))",
               getBitwiseOperandStr(value), getValueStr(lane));
}

std::string MslEmitter::getSvAccessQualifiedStr(const SvAccess& sv_access,
                                                bool output) {
    bool needs_in_out = (sv_access.sv.semantic == SvSemantic::Position ||
                         sv_access.sv.semantic == SvSemantic::UserInOut);
    bool is_vec = (sv_access.sv.semantic == SvSemantic::Position ||
                   sv_access.sv.semantic == SvSemantic::UserInOut);

    // TODO: is it okay to access components just like this?
    return fmt::format(
        "{}{}{}", (needs_in_out ? (output ? "__out." : "__in.") : ""),
        getSvStr(sv_access.sv),
        (is_vec ? fmt::format(".{}", getComponentStrFromIndex(
                                         sv_access.component_index))
                : ""));
}

std::string MslEmitter::getSvStr(const Sv& sv) {
    switch (sv.semantic) {
    case SvSemantic::Position:
        return "position";
    case SvSemantic::UserInOut:
        return fmt::format("user{}", sv.index);
    case SvSemantic::InstanceID:
        return "iid";
    case SvSemantic::VertexID:
        return "vid";
    default:
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, "SV {} (index: {})", sv.semantic,
                            sv.index);
        return INVALID_VALUE;
    }
}

std::string MslEmitter::getSvQualifierStr(const Sv& sv, bool output) {
    switch (sv.semantic) {
    case SvSemantic::Position:
        return "position";
    case SvSemantic::UserInOut:
        switch (context.type) {
        case ShaderType::Vertex:
            if (output)
                return fmt::format("user(locn{})", sv.index);
            else
                return fmt::format("attribute({})", sv.index);
        case ShaderType::Fragment:
            if (output)
                return fmt::format("color({})", sv.index);
            else
                return fmt::format("user(locn{})", sv.index);
        default:
            return INVALID_VALUE;
        }
    case SvSemantic::InstanceID:
        return "instance_id";
    case SvSemantic::VertexID:
        return "vertex_id";
    default:
        LOG_ERROR(ShaderDecompiler, "Unknown SV semantic {}", sv.semantic);
        return INVALID_VALUE;
    }
}

} // namespace
  // hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen::lang::msl
