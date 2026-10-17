#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/msl/emitter.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"

#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/analyzer/memory_analyzer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/helper.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/msl/intrinsics.hpp"

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
    if (it == state.buffer_texture_formats.end()) {
        const auto int_it = state.integer_texture_formats.find(index);
        if (int_it != state.integer_texture_formats.end()) {
            if (const auto component =
                    renderer::getIntegerTextureComponentType(int_it->second))
                return textureTypeToStr(info.type, false) + "<" + component +
                       ">";
        }
        return textureTypeToStr(info.type, info.is_depth) + "<float>";
    }
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
                       info.is_atomic || info.is_read ? "read_write" : "write");
}

// Layout of the texel of an image format, to convert between the raw words
// the guest reads and writes (SULD.D, SUST.D) and the channels Metal uses
struct RawImageLayout {
    enum class Kind { Unorm, Snorm, Uint, Sint, Float };
    u32 channels;
    u32 bits;
    Kind kind;
    bool packed_1010102{false};

    u32 bytes() const { return packed_1010102 ? 4 : channels * bits / 8; }
};

std::optional<RawImageLayout> getRawImageLayout(renderer::TextureFormat format) {
    using F = renderer::TextureFormat;
    using K = RawImageLayout::Kind;
    switch (format) {
    case F::R8Unorm: return RawImageLayout{1, 8, K::Unorm};
    case F::R8Snorm: return RawImageLayout{1, 8, K::Snorm};
    case F::R8Uint: return RawImageLayout{1, 8, K::Uint};
    case F::R8Sint: return RawImageLayout{1, 8, K::Sint};
    case F::RG8Unorm: return RawImageLayout{2, 8, K::Unorm};
    case F::RG8Snorm: return RawImageLayout{2, 8, K::Snorm};
    case F::RG8Uint: return RawImageLayout{2, 8, K::Uint};
    case F::RG8Sint: return RawImageLayout{2, 8, K::Sint};
    case F::RGBA8Unorm: return RawImageLayout{4, 8, K::Unorm};
    case F::RGBA8Snorm: return RawImageLayout{4, 8, K::Snorm};
    case F::RGBA8Uint: return RawImageLayout{4, 8, K::Uint};
    case F::RGBA8Sint: return RawImageLayout{4, 8, K::Sint};
    case F::R16Float: return RawImageLayout{1, 16, K::Float};
    case F::R16Unorm: return RawImageLayout{1, 16, K::Unorm};
    case F::R16Snorm: return RawImageLayout{1, 16, K::Snorm};
    case F::R16Uint: return RawImageLayout{1, 16, K::Uint};
    case F::R16Sint: return RawImageLayout{1, 16, K::Sint};
    case F::RG16Float: return RawImageLayout{2, 16, K::Float};
    case F::RG16Unorm: return RawImageLayout{2, 16, K::Unorm};
    case F::RG16Snorm: return RawImageLayout{2, 16, K::Snorm};
    case F::RG16Uint: return RawImageLayout{2, 16, K::Uint};
    case F::RG16Sint: return RawImageLayout{2, 16, K::Sint};
    case F::RGBA16Float: return RawImageLayout{4, 16, K::Float};
    case F::RGBA16Unorm: return RawImageLayout{4, 16, K::Unorm};
    case F::RGBA16Snorm: return RawImageLayout{4, 16, K::Snorm};
    case F::RGBA16Uint: return RawImageLayout{4, 16, K::Uint};
    case F::RGBA16Sint: return RawImageLayout{4, 16, K::Sint};
    case F::R32Float: return RawImageLayout{1, 32, K::Float};
    case F::R32Uint: return RawImageLayout{1, 32, K::Uint};
    case F::R32Sint: return RawImageLayout{1, 32, K::Sint};
    case F::RG32Float: return RawImageLayout{2, 32, K::Float};
    case F::RG32Uint: return RawImageLayout{2, 32, K::Uint};
    case F::RG32Sint: return RawImageLayout{2, 32, K::Sint};
    case F::RGBA32Float: return RawImageLayout{4, 32, K::Float};
    case F::RGBA32Uint: return RawImageLayout{4, 32, K::Uint};
    case F::RGBA32Sint: return RawImageLayout{4, 32, K::Sint};
    case F::RGB10A2Unorm: return RawImageLayout{4, 10, K::Unorm, true};
    case F::RGB10A2Uint: return RawImageLayout{4, 10, K::Uint, true};
    default: return std::nullopt;
    }
}

// Bytes of a raw access (U8, S8, U16, S16, B32, B64, B128)
u32 rawAccessBytes(u32 raw_size) {
    constexpr u32 bytes[] = {1, 1, 2, 2, 4, 8, 16};
    return raw_size < 7 ? bytes[raw_size] : 0;
}

// The raw layout of an image if a raw access of the size matches its texel
std::optional<RawImageLayout> getRawLayout(const GuestShaderState& state,
                                           u32 index, u32 raw_size) {
    if (raw_size == ir::IMAGE_FORMATTED)
        return std::nullopt;
    const auto it = state.image_formats.find(index);
    if (it != state.image_formats.end()) {
        const auto layout = getRawImageLayout(it->second);
        if (layout && layout->bytes() == rawAccessBytes(raw_size))
            return layout;
    }
    ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler,
                             "Raw surface access of size {} to image {}",
                             raw_size, index));
    return std::nullopt;
}

// Value of channel `channel` (in the type of its kind) from raw words
std::string decodeRawChannel(const RawImageLayout& layout,
                             const std::array<std::string, 4>& words,
                             u32 channel) {
    using K = RawImageLayout::Kind;
    if (layout.packed_1010102) {
        const std::string raw =
            channel == 3 ? fmt::format("(({}) >> 30)", words[0])
                         : fmt::format("((({}) >> {}) & 0x3ffu)", words[0],
                                       channel * 10);
        if (layout.kind == K::Uint)
            return raw;
        return fmt::format("(float({}) / {}.0)", raw, channel == 3 ? 3 : 1023);
    }
    const u32 per_word = 32 / layout.bits;
    const u32 shift = (channel % per_word) * layout.bits;
    const std::string word = words[channel / per_word];
    const std::string raw =
        layout.bits == 32
            ? fmt::format("({})", word)
            : fmt::format("((({}) >> {}) & {:#x}u)", word, shift,
                          (1u << layout.bits) - 1);
    switch (layout.kind) {
    case K::Uint:
        return raw;
    case K::Sint:
        return layout.bits == 32
                   ? fmt::format("as_type<int>({})", raw)
                   : fmt::format("int(as_type<{}>({}({})))",
                                 layout.bits == 8 ? "char" : "short",
                                 layout.bits == 8 ? "uchar" : "ushort", raw);
    case K::Float:
        return layout.bits == 32
                   ? fmt::format("as_type<float>({})", raw)
                   : fmt::format("float(as_type<half>(ushort({})))", raw);
    case K::Unorm:
        return fmt::format("(float({}) / {}.0)", raw,
                           (1u << layout.bits) - 1);
    case K::Snorm:
        return fmt::format("max(float(as_type<{}>({}({}))) / {}.0, -1.0)",
                           layout.bits == 8 ? "char" : "short",
                           layout.bits == 8 ? "uchar" : "ushort", raw,
                           (1u << (layout.bits - 1)) - 1);
    }
    return "0";
}

// Bits of a channel value (of the type of the layout's kind), masked to the
// channel width
std::string encodeRawChannel(const RawImageLayout& layout,
                             const std::string& value) {
    using K = RawImageLayout::Kind;
    const u32 bits = layout.bits;
    const u32 mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
    switch (layout.kind) {
    case K::Uint:
        return fmt::format("(uint({}) & {:#x}u)", value, mask);
    case K::Sint:
        return fmt::format("(as_type<uint>(int({})) & {:#x}u)", value, mask);
    case K::Float:
        return bits == 32 ? fmt::format("as_type<uint>(float({}))", value)
                          : fmt::format("uint(as_type<ushort>(half({})))",
                                        value);
    case K::Unorm:
        return fmt::format("uint(rint(clamp(float({}), 0.0, 1.0) * {}.0))",
                           value, mask);
    case K::Snorm:
        return fmt::format(
            "(as_type<uint>(int(rint(clamp(float({}), -1.0, 1.0) * {}.0))) & "
            "{:#x}u)",
            value, mask >> 1, mask);
    }
    return "0u";
}

// Word `word` of the texel from the channel values
std::string encodeRawWord(const RawImageLayout& layout,
                          const std::array<std::string, 4>& channels,
                          u32 word) {
    if (layout.packed_1010102) {
        if (word != 0)
            return "0u";
        using K = RawImageLayout::Kind;
        const auto channel = [&](u32 c, u32 width, u32 shift) {
            const u32 max = (1u << width) - 1;
            const std::string bits =
                layout.kind == K::Uint
                    ? fmt::format("(uint({}) & {:#x}u)", channels[c], max)
                    : fmt::format(
                          "uint(rint(clamp(float({}), 0.0, 1.0) * {}.0))",
                          channels[c], max);
            return fmt::format("({} << {})", bits, shift);
        };
        return fmt::format("({} | {} | {} | {})", channel(0, 10, 0),
                           channel(1, 10, 10), channel(2, 10, 20),
                           channel(3, 2, 30));
    }
    const u32 per_word = 32 / layout.bits;
    std::string result;
    for (u32 i = 0; i < per_word; i++) {
        const u32 channel = word * per_word + i;
        if (channel >= layout.channels)
            break;
        if (!result.empty())
            result += " | ";
        result += fmt::format("({} << {})",
                              encodeRawChannel(layout, channels[channel]),
                              i * layout.bits);
    }
    return result.empty() ? "0u" : fmt::format("({})", result);
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

    if (context.type == ShaderType::Vertex) {
        const auto& outputs = memory_analyzer.getOutputSVs();
        out_resource_mapping.writes_layer =
            state.layer_output_addr != 0 ||
            outputs.contains(SvSemantic::Layer) ||
            outputs.contains(SvSemantic::ViewportIndex);
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
    out_resource_mapping.fragment_output_mask = context.fragment_output_mask;
    write("#include <metal_stdlib>");
    write("using namespace metal;");
    if (context.type == ShaderType::Compute) {
        write("{}", R"(
            // Retries spurious failures, returns the previous value
            inline uint hydra_shared_cas(threadgroup atomic_uint* word, uint compare, uint value) {
                uint expected = compare;
                while (!atomic_compare_exchange_weak_explicit(word, &expected, value,
                        memory_order_relaxed, memory_order_relaxed)) {
                    if (expected != compare) return expected;
                }
                return compare;
            }
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

    // Intrinsic helpers
    for (const auto intrinsic : memory_analyzer.getIntrinsics())
        write("{}", getIntrinsicSource(intrinsic));

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
        write("    const float lut_a[4] = {{1.0, 1.0, -1.0, 0.0}};");
        write("    const float lut_b[4] = {{1.0, -1.0, 1.0, 1.0}};");
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
    if (context.type == ShaderType::Vertex) {
        const auto& outputs = memory_analyzer.getOutputSVs();
        write("float4 position [[position, invariant]];");
        if (state.layer_output_addr != 0 || outputs.contains(SvSemantic::Layer))
            write("uint layer [[render_target_array_index]];");
        if (outputs.contains(SvSemantic::ViewportIndex))
            write("uint viewport_index [[viewport_array_index]];");
        // Metal reads the point size of every point, written or not
        if (state.point_list)
            write("float point_size [[point_size]];");
        if (const u32 count = getClipDistanceCount(); count != 0)
            write("float clip_distance [[clip_distance]] [{}];", count);
    }
    for (const auto sv_semantic : memory_analyzer.getOutputSVs()) {
        switch (sv_semantic) {
        case SvSemantic::Position:
        case SvSemantic::Layer:
        case SvSemantic::ViewportIndex:
        case SvSemantic::PointSize:
        case SvSemantic::ClipDistance:
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
        if (context.fragment_depth_register != invalid<u32>())
            write("float depth [[depth(any)]];");
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
    } else if (memory_analyzer.usesLaneId()) {
        // Vertex functions have no SIMD-group lane, they read lane 0
        writeStatement("uint lane");
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
    case ShaderType::Fragment: {
        const auto& inputs = memory_analyzer.getInputSVs();
        if (memory_analyzer.usesLaneId())
            ADD_ARG("uint __lane [[thread_index_in_simdgroup]]");
        if (inputs.contains(SvSemantic::FrontFacing))
            ADD_ARG("bool __front_facing [[front_facing]]");
        if (inputs.contains(SvSemantic::PointCoord))
            ADD_ARG("float2 __point_coord [[point_coord]]");
        if (inputs.contains(SvSemantic::PrimitiveID))
            ADD_ARG("uint __primitive_id [[primitive_id]]");
        if (inputs.contains(SvSemantic::Layer))
            ADD_ARG("uint __layer [[render_target_array_index]]");
        if (inputs.contains(SvSemantic::ViewportIndex))
            ADD_ARG("uint __viewport_index [[viewport_array_index]]");
        break;
    }
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

    if (context.type == ShaderType::Fragment &&
        state.visual_output == static_cast<uint32_t>(VisualDiagnostics::Output::Solid) &&
        context.fragment_depth_register == invalid<u32>()) {
        for (u8 i = 0; i < COLOR_TARGET_COUNT; ++i) {
            const auto type = state.color_target_data_types[i];
            if (type != ColorDataType::Invalid)
                writeStatement("__out.{} = vec<{}, 4>(1, 0, 1, 1)",
                               getSvStr(Sv(SvSemantic::UserInOut, i)), toType(type));
        }
        writeStatement("return __out");
        VisualDiagnostics::instance().event("solid_coverage_override", {
            {"guest_execution", "bypassed_including_discard_and_side_effects"},
            {"fixed_function", "depth_stencil_cull_and_write_masks_preserved"}});
    } else if (context.type == ShaderType::Fragment &&
               state.visual_output == static_cast<uint32_t>(VisualDiagnostics::Output::Solid)) {
        VisualDiagnostics::instance().event("solid_coverage_unavailable", {
            {"reason", "guest_depth_export_requires_guest_execution; output_only_override"}});
    }
    emitMainFunctionPrologue();
    if (memory_analyzer.usesLaneId())
        writeStatement("state.lane = {}", context.type == ShaderType::Fragment
                                              ? "__lane"
                                              : "0u");
    if (context.type == ShaderType::Fragment)
        emitFragmentSystemValueInputs();
}

void MslEmitter::emitFragmentSystemValueInputs() {
    const auto& inputs = memory_analyzer.getInputSVs();
    const auto attr = [&](u32 addr, ir::ScalarType type) {
        return getAttrMemoryStr({RZ, addr, true}, type);
    };
    // Front facing reads as all ones, like a predicate converted to an
    // integer mask
    if (inputs.contains(SvSemantic::FrontFacing))
        writeStatement("{} = __front_facing ? 0xffffffffu : 0u",
                       attr(SV_FRONT_FACING_BASE, ir::ScalarType::U32));
    if (inputs.contains(SvSemantic::PointCoord)) {
        // Metal point coordinates start at the top
        writeStatement("{} = __point_coord.x",
                       attr(SV_POINT_COORD_BASE, ir::ScalarType::F32));
        writeStatement("{} = {}",
                       attr(SV_POINT_COORD_BASE + 4, ir::ScalarType::F32),
                       state.point_coord_origin_bottom ? "1.0 - __point_coord.y"
                                                       : "__point_coord.y");
    }
    if (inputs.contains(SvSemantic::PrimitiveID))
        writeStatement("{} = __primitive_id",
                       attr(SV_PRIMITIVE_ID_BASE, ir::ScalarType::U32));
    if (inputs.contains(SvSemantic::Layer))
        writeStatement("{} = __layer", attr(SV_LAYER_BASE, ir::ScalarType::U32));
    if (inputs.contains(SvSemantic::ViewportIndex))
        writeStatement("{} = __viewport_index",
                       attr(SV_VIEWPORT_INDEX_BASE, ir::ScalarType::U32));
}

u32 MslEmitter::getClipDistanceCount() const {
    if (context.type != ShaderType::Vertex || state.clip_distance_mask == 0)
        return 0;
    return 32 - static_cast<u32>(std::countl_zero(
                    state.clip_distance_mask & ((1u << CLIP_DISTANCE_COUNT) - 1)));
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

        // Window coordinates (viewport transform disabled) to clip space.
        // The depth is already a window depth.
        const bool window_space = state.window_space_width != 0 &&
                                  state.window_space_height != 0;
        if (window_space) {
            writeStatement("__out.position.x = (__out.position.x * {} - 1.0) "
                           "* __out.position.w",
                           2.0 / state.window_space_width);
            // Window rows count from the top, or from the bottom when the
            // origin is lower left
            const double y_scale = 2.0 / state.window_space_height;
            if (state.y_negate)
                writeStatement("__out.position.y = (__out.position.y * {} - "
                               "1.0) * __out.position.w",
                               y_scale);
            else
                writeStatement("__out.position.y = (1.0 - __out.position.y * "
                               "{}) * __out.position.w",
                               y_scale);
        }

        // Convert depth from < -1, 1 > to < 0, 1 >
        if (!state.depth_zero_to_one && !window_space)
            writeStatement("__out.position.z = (__out.position.z + "
                           "__out.position.w) / 2.0");

        const auto& outputs = memory_analyzer.getOutputSVs();
        const auto out_attr = [&](u32 addr, ir::ScalarType type) {
            return getAttrMemoryStr({RZ, addr, false}, type);
        };
        // Layer selected by an unsupported geometry shader
        if (state.layer_output_addr != 0)
            writeStatement("__out.layer = {}",
                           getAttrMemoryStr(
                               {RZ, state.layer_output_addr, false}));
        else if (outputs.contains(SvSemantic::Layer))
            writeStatement("__out.layer = {}",
                           out_attr(SV_LAYER_BASE, ir::ScalarType::U32));
        if (outputs.contains(SvSemantic::ViewportIndex))
            writeStatement("__out.viewport_index = {}",
                           out_attr(SV_VIEWPORT_INDEX_BASE, ir::ScalarType::U32));
        if (state.point_list) {
            if (state.program_point_size &&
                outputs.contains(SvSemantic::PointSize))
                writeStatement("__out.point_size = {}",
                               out_attr(SV_POINT_SIZE_BASE, ir::ScalarType::F32));
            else
                writeStatement("__out.point_size = {}",
                               getConstantStr(state.point_size));
        }
        // Distances the guest enabled but the shader does not write do not
        // clip
        const u32 written = memory_analyzer.getClipDistanceOutputs();
        for (u32 i = 0; i < getClipDistanceCount(); i++) {
            if ((state.clip_distance_mask & written & (1u << i)) != 0)
                writeStatement("__out.clip_distance[{}] = {}", i,
                               out_attr(SV_CLIP_DISTANCE_BASE + i * 4,
                                        ir::ScalarType::F32));
            else
                writeStatement("__out.clip_distance[{}] = 0.0", i);
        }
    }

    // Alpha test against the alpha written to render target 0
    if (context.type == ShaderType::Fragment &&
        state.alpha_test_func != engines::CompareOp::Invalid &&
        state.alpha_test_func != engines::CompareOp::Always &&
        (context.fragment_output_mask & 0x8u) != 0 &&
        state.color_target_data_types[0] == ColorDataType::Float) {
        const auto alpha = getSvAccessQualifiedStr(
            SvAccess(Sv(SvSemantic::UserInOut, 0), 3), true);
        const auto ref = getConstantStr(state.alpha_test_ref);
        const char* op = "";
        switch (state.alpha_test_func) {
        case engines::CompareOp::Less: op = "<"; break;
        case engines::CompareOp::Equal: op = "=="; break;
        case engines::CompareOp::LessEqual: op = "<="; break;
        case engines::CompareOp::Greater: op = ">"; break;
        case engines::CompareOp::NotEqual: op = "!="; break;
        case engines::CompareOp::GreaterEqual: op = ">="; break;
        default: break;
        }
        if (state.alpha_test_func == engines::CompareOp::Never)
            writeStatement("discard_fragment()");
        else
            writeStatement("if (!(float({}) {} {})) discard_fragment()", alpha,
                           op, ref);
    }

    if (context.type == ShaderType::Fragment &&
        context.fragment_depth_register != invalid<u32>()) {
        writeStatement("__out.depth = {}",
                       getRegisterStr(static_cast<u8>(context.fragment_depth_register),
                                      ir::ScalarType::F32));
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

    if (context.type == ShaderType::Fragment && state.visual_output != 0) {
        std::string color = "float4(1.0, 0.0, 1.0, 1.0)";
        const auto mode = static_cast<VisualDiagnostics::Output>(state.visual_output);
        const auto& inputs = memory_analyzer.getStageInputs();
        const bool available = std::find(inputs.begin(), inputs.end(), state.visual_input) != inputs.end();
        if (mode == VisualDiagnostics::Output::Solid || available) {
            if (mode != VisualDiagnostics::Output::Solid) {
                const auto input = "__in." + getSvStr(Sv(SvSemantic::UserInOut, static_cast<u8>(state.visual_input)));
                if (mode == VisualDiagnostics::Output::Uv)
                    color = fmt::format("float4(fract({}.xy), 0.0, 1.0)", input);
                else if (mode == VisualDiagnostics::Output::Normal)
                    color = fmt::format("float4({}.xyz * 0.5 + 0.5, 1.0)", input);
                else if (mode == VisualDiagnostics::Output::Checker)
                    color = fmt::format("float4(float3(fmod(abs(floor({0}.x * 16.0) + floor({0}.y * 16.0)), 2.0)), 1.0)", input);
            }
            for (u8 i = 0; i < COLOR_TARGET_COUNT; ++i) {
                const auto type = state.color_target_data_types[i];
                if (type != ColorDataType::Invalid)
                    writeStatement("__out.{} = vec<{}, 4>({})", getSvStr(Sv(SvSemantic::UserInOut, i)), toType(type), color);
            }
        } else {
            VisualDiagnostics::instance().event("override_unavailable", {
                {"reason", "chosen fragment input is not consumed by this shader"},
                {"input", std::to_string(state.visual_input)}});
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
    storeValue(dst, "((uint({1}) >= 32u || uint({2}) == 0u) ? 0 : extract_bits({0}, min(uint({1}), 31u), min(uint({2}), 32u - min(uint({1}), 31u))))",
               getValueStr(src_a), getValueStr(src_b), getValueStr(src_c));
}

// Texture
void MslEmitter::emitTextureSample(
    const ir::Value& dst, u32 const_buffer_index, TextureType type,
    TextureSampleFlags flags, const ir::Value& array_index,
    const ir::Value& coords, const ir::Value& cmp_value, const ir::Value& lod,
    const ir::Value& ddx, const ir::Value& ddy, const ir::Value& offset,
    const ir::Value& offsets, u8 component) {
    const auto bias_it = state.texture_lod_bias.find(const_buffer_index);
    const float sampler_bias = bias_it == state.texture_lod_bias.end() ? 0.0f : bias_it->second;
    // Flags
    const auto is_array = isTextureArray(type);
    const auto is_buffer =
        state.buffer_texture_formats.contains(const_buffer_index);
    const auto int_it = state.integer_texture_formats.find(const_buffer_index);
    const bool is_integer =
        !is_buffer && int_it != state.integer_texture_formats.end() &&
        renderer::getIntegerTextureComponentType(int_it->second) != nullptr;
    // Integer textures are bound with nearest samplers (ThreeD::getSampler)
    const auto int_coords =
        is_buffer || any(flags & TextureSampleFlags::IntCoords);
    const auto depth_compare = any(flags & TextureSampleFlags::DepthCompare);
    const auto has_lod =
        any(flags & (TextureSampleFlags::Lod | TextureSampleFlags::LodZero));

    if (any(flags & TextureSampleFlags::Gather)) {
        const bool cube =
            type == TextureType::Cube || type == TextureType::CubeArray;
        ASSERT(!(cube && any(flags & (TextureSampleFlags::Offset |
                                      TextureSampleFlags::Offsets))),
               ShaderDecompiler, "Cube gathers cannot have texel offsets");
        const auto gather = [&](const std::string& location,
                                const std::string& texel_offset) {
            auto arguments =
                fmt::format("state.samplr{}, {}", const_buffer_index, location);
            if (is_array)
                arguments += fmt::format(", {}", getValueStr(array_index));
            if (depth_compare)
                arguments += fmt::format(", {}", getValueStr(cmp_value));
            if (!cube)
                arguments += ", " + texel_offset;
            if (!depth_compare)
                arguments +=
                    fmt::format(", component::{}", componentToStr(component));
            auto result = fmt::format(
                "state.tex{}.{}({})", const_buffer_index,
                depth_compare ? "gather_compare" : "gather", arguments);
            if (is_integer)
                result = fmt::format("as_type<float4>({})", result);
            return result;
        };
        if (any(flags & TextureSampleFlags::Offsets)) {
            // PTP packs eight signed six-bit values in two registers, one
            // x/y pair per gathered texel. Select that texel from each gather.
            std::array<std::string, 4> samples;
            for (u32 lane = 0; lane < 4; ++lane) {
                const auto word = fmt::format("({}).{}", getValueStr(offsets),
                                              lane < 2 ? "x" : "y");
                const u32 base = (lane % 2) * 16;
                const auto pair = fmt::format(
                    "int2(int({} << {}u) >> 26, int({} << {}u) >> 26)", word,
                    26 - base, word, 18 - base);
                samples[lane] = fmt::format(
                    "({}).{}", gather(getValueStr(coords), pair), "xyzw"[lane]);
            }
            storeValue(dst, "float4({}, {}, {}, {})", samples[0], samples[1],
                       samples[2], samples[3]);
        } else {
            storeValue(dst, "{}",
                       gather(getValueStr(coords),
                              any(flags & TextureSampleFlags::Offset)
                                  ? fmt::format("int2({})", getValueStr(offset))
                                  : "int2(0)"));
        }
        return;
    }

    std::string sample_coords = getValueStr(coords);
    // Metal 1D textures have a single level and no sampling options: texel
    // offsets move the normalized coordinate instead
    const bool is_1d = !is_buffer && (type == TextureType::_1D ||
                                      type == TextureType::_1DArray);
    if (any(flags & TextureSampleFlags::Offset)) {
        const auto count = coords.getType().getVectorType().getSize();
        const auto vector_type =
            count == 1 ? std::string("uint") : fmt::format("uint{}", count);
        if (int_coords) {
            sample_coords =
                fmt::format("(as_type<{}>({}) + {}({}))", vector_type,
                            sample_coords, vector_type, getValueStr(offset));
        } else if (is_1d) {
            sample_coords = fmt::format(
                "({}.x + float({}) / float(state.tex{}.get_width()))",
                sample_coords, getValueStr(offset), const_buffer_index);
        }
    }
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
        const auto coords_str = sample_coords;
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
                           sample_coords);
    }

    // Args
    if (is_array && !is_buffer)
        args += fmt::format(", uint({})", getValueStr(array_index));
    if (depth_compare)
        args += fmt::format(", {}", getValueStr(cmp_value));
    if (is_1d && !int_coords) {
        // No LOD, bias or gradient options
    } else if (any(flags & TextureSampleFlags::Derivatives) && !int_coords) {
        const char* gradient =
            type == TextureType::Cube || type == TextureType::CubeArray
                ? "gradientcube"
            : coords.getType().getVectorType().getSize() == 1 ? "gradient1d"
            : coords.getType().getVectorType().getSize() == 2 ? "gradient2d"
                                                              : "gradient3d";
        if (sampler_bias == 0.0f)
            args += fmt::format(", {}({}, {})", gradient, getValueStr(ddx), getValueStr(ddy));
        else
            args += fmt::format(", {}(({}) * exp2({}), ({}) * exp2({}))", gradient,
                                getValueStr(ddx), getValueStr(ir::Value::createConstantF(sampler_bias)),
                                getValueStr(ddy), getValueStr(ir::Value::createConstantF(sampler_bias)));
        if (has_lod)
            args += fmt::format(", min_lod_clamp({})", getValueStr(lod));
    } else if (has_lod && !is_buffer) {
        if (any(flags & TextureSampleFlags::LodZero))
            args += int_coords ? ", 0u" : ", level(0.0)";
        else if (int_coords)
            args += fmt::format(", as_type<uint>({})", getValueStr(lod));
        else
            args += fmt::format(", level({})", getValueStr(lod));
    } else if (!int_coords && any(flags & TextureSampleFlags::Bias) &&
               context.type == ShaderType::Fragment) {
        args += fmt::format(", bias(({}) + {})", getValueStr(lod), getValueStr(ir::Value::createConstantF(sampler_bias)));
    } else if (!int_coords && context.type == ShaderType::Fragment && sampler_bias != 0.0f) {
        args += fmt::format(", bias({})", getValueStr(ir::Value::createConstantF(sampler_bias)));
    } else if (!int_coords && context.type == ShaderType::Compute) {
        // There are no derivatives in compute kernels
        args += ", level(0.0)";
    }

    if (!int_coords && !is_1d && any(flags & TextureSampleFlags::Offset)) {
        const auto dimensions = coords.getType().getVectorType().getSize();
        args += dimensions == 1 ? fmt::format(", int({})", getValueStr(offset))
                                : fmt::format(", int{}({})", dimensions,
                                              getValueStr(offset));
    }

    std::string res =
        fmt::format("state.tex{}.{}({})", const_buffer_index, func_name, args);
    // Depth comparisons return the result in RGB and 1 in alpha
    if (depth_compare)
        res = fmt::format("float4(float3({}), 1.0)", res);
    if (is_buffer) {
        const auto decl = textureDeclaration(
            state, const_buffer_index, analyzer::TextureInfo{type, false});
        if (decl != "texture_buffer<float>")
            res = fmt::format("as_type<float4>({})", res);
    } else if (is_integer) {
        // Integer textures return raw bits
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

void MslEmitter::emitTextureQueryLod(const ir::Value& dst,
                                     u32 const_buffer_index, TextureType type,
                                     const ir::Value& coords, bool unclamped) {
    // Metal 1D textures and buffers have a single level
    const bool is_1d = state.buffer_texture_formats.contains(const_buffer_index) ||
                       type == TextureType::_1D || type == TextureType::_1DArray;
    // The query needs derivatives, which only fragment shaders have
    if (is_1d || context.type != ShaderType::Fragment) {
        storeValue(dst, "0.0");
        return;
    }
    storeValue(dst, "state.tex{}.calculate_{}_lod(state.samplr{}, {})",
               const_buffer_index, unclamped ? "unclamped" : "clamped",
               const_buffer_index, getValueStr(coords));
}

void MslEmitter::emitTextureQueryDimension(const ir::Value& dst,
                                           u32 const_buffer_index,
                                           u32 dimension,
                                           const ir::Value& lod) {
    // Only query dimensions the texture type has: depth is the array size
    // for arrays and 1 otherwise, and 1D textures have a height of 1
    auto type = TextureType::_2D;
    if (const auto it = memory_analyzer.getTextures().find(const_buffer_index);
        it != memory_analyzer.getTextures().end())
        type = it->second.type;
    const bool is_buffer = state.buffer_texture_formats.contains(const_buffer_index);
    const bool is_1d = is_buffer || type == TextureType::_1D ||
                       type == TextureType::_1DArray;
    const bool is_3d =
        type == TextureType::_3D || type == TextureType::_3DArray;
    // Metal 1D textures and buffers have a single level
    if (dimension == 3) {
        if (is_1d)
            storeValue(dst, "1u");
        else
            storeValue(dst, "state.tex{}.get_num_mip_levels()",
                       const_buffer_index);
        return;
    }
    if (dimension == 1 && is_1d) {
        storeValue(dst, "1u");
        return;
    }
    if (dimension == 2 && !is_3d) {
        if (isTextureArray(type))
            storeValue(dst, "state.tex{}.get_array_size()", const_buffer_index);
        else
            storeValue(dst, "1u");
        return;
    }
    // Buffers and 1D textures have no mip levels
    if (is_buffer || type == TextureType::_1D || type == TextureType::_1DArray)
        storeValue(dst, "state.tex{}.get_{}()", const_buffer_index,
                   dimensionToStr(dimension));
    else
        storeValue(dst, "state.tex{}.get_{}(uint({}))", const_buffer_index,
                   dimensionToStr(dimension), getBitwiseOperandStr(lod));
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
    case 0x12: // Y direction (+1.0 or -1.0)
        value = state.y_negate ? "0xbf800000u" : "0x3f800000u";
        break;
    case 0x13: // Thread kill: helper invocations of a fragment quad
        value = context.type == ShaderType::Fragment
                    ? "(simd_is_helper_thread() ? 0xffffffffu : 0u)"
                    : "0u";
        break;
    case 0x1d: // Invocation info
    case 0x1e: // W scale factor XY
    case 0x1f: // W scale factor Z
        // Values the hardware reports for stages without these inputs
        value = "0x00ff0000u";
        break;
    case 0x01: // Clock
    case 0x02: // Virtual configuration
    case 0x03: // Virtual ID
    case 0x0f: // Ordering ticket
    case 0x10: // Primitive type
    case 0x11: // Invocation ID
    case 0x14: // Shader type
    case 0x1c: // Affinity
    case 0x50: // Clock low
    case 0x51: // Clock high
    case 0x52: // Global timer low
    case 0x53: // Global timer high
        // Metal shaders cannot read these; hardware reports zero for
        // invocation inputs a stage does not have
        value = "0u";
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

void MslEmitter::emitAtomicSharedCas(const ir::Value& dst,
                                     const ir::Value& addr,
                                     const ir::Value& compare,
                                     const ir::Value& value) {
    storeValue(dst,
               "hydra_shared_cas(reinterpret_cast<threadgroup atomic_uint*>("
               "&state.smem[min(uint({}) >> 2u, {}u)]._u32), uint({}), uint({}))",
               getBitwiseOperandStr(addr), shared_word_count - 1,
               getBitwiseOperandStr(compare), getBitwiseOperandStr(value));
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
                                const std::array<ir::Value, 4>& values,
                                u32 raw_size) {
    const auto& info = memory_analyzer.getImages().at(index);
    const auto component_type =
        imageComponentType(state, index, info.is_atomic);
    std::string value = fmt::format("vec<{}, 4>(", component_type);
    if (const auto layout = getRawLayout(state, index, raw_size)) {
        // The register words are the texel
        std::array<std::string, 4> words;
        for (u32 i = 0; i < 4; i++)
            words[i] = getBitwiseOperandStr(values[i]);
        for (u32 i = 0; i < 4; i++) {
            if (i != 0)
                value += ", ";
            value += i < layout->channels
                         ? fmt::format("{}({})", component_type,
                                       decodeRawChannel(*layout, words, i))
                         : fmt::format("{}(0)", component_type);
        }
    } else {
        for (u32 i = 0; i < 4; i++) {
            if (i != 0)
                value += ", ";
            value += fmt::format("as_type<{}>(uint({}))", component_type,
                                 getBitwiseOperandStr(values[i]));
        }
    }
    value += ")";

    const auto coords_str = getValueStr(coords);
    writeStatement("if ({}) state.img{}.write({}, {})",
                   getImageBoundsStr(index, type, coords_str), index, value,
                   getImageCoordsStr(type, coords_str));
}

void MslEmitter::emitImageLoad(const ir::Value& dst, u32 index,
                               TextureType type, const ir::Value& coords,
                               u32 raw_size) {
    const auto& info = memory_analyzer.getImages().at(index);
    const auto component_type =
        imageComponentType(state, index, info.is_atomic);
    const auto coords_str = getValueStr(coords);
    const auto texel = getValueStr(dst) + "_texel";
    // Out of bounds reads return 0
    writeStatement("vec<{0}, 4> {1} = ({2}) ? state.img{3}.read({4}) : "
                   "vec<{0}, 4>(0)",
                   component_type, texel,
                   getImageBoundsStr(index, type, coords_str), index,
                   getImageCoordsStr(type, coords_str));

    std::array<std::string, 4> bits;
    const char* components[] = {"x", "y", "z", "w"};
    if (const auto layout = getRawLayout(state, index, raw_size)) {
        std::array<std::string, 4> channels;
        for (u32 i = 0; i < 4; i++)
            channels[i] = fmt::format("{}.{}", texel, components[i]);
        for (u32 word = 0; word < 4; word++)
            bits[word] = encodeRawWord(*layout, channels, word);
    } else {
        for (u32 i = 0; i < 4; i++)
            bits[i] = fmt::format("as_type<uint>({}.{})", texel, components[i]);
    }
    storeValue(dst, "vec<u32, 4>({}, {}, {}, {})", bits[0], bits[1], bits[2],
               bits[3]);
}

void MslEmitter::emitImageAtomic(const ir::Value& dst, u32 index,
                                 TextureType type, u32 op,
                                 const ir::Value& coords,
                                 const ir::Value& value) {
    // INC and DEC wrap around the operand, which texture atomics lack: a
    // compare and exchange loop
    if (op == 3 || op == 4) {
        const auto component_type = imageComponentType(state, index, true);
        const auto coords_str = getValueStr(coords);
        const auto expected = getValueStr(dst) + "_expected";
        writeStatement("vec<{0}, 4> {1} = vec<{0}, 4>(0)", component_type,
                       expected);
        enterScope("if ({})", getImageBoundsStr(index, type, coords_str));
        writeStatement("{} = state.img{}.read({})", expected, index,
                       getImageCoordsStr(type, coords_str));
        enterScope("while (true)");
        const auto operand = fmt::format("as_type<{}>(uint({}))", component_type,
                                         getBitwiseOperandStr(value));
        writeStatement("const {0} current = {1}.x; const {0} desired = {2}",
                       component_type, expected,
                       op == 3 ? fmt::format("(current >= {0}) ? {1}(0) : "
                                             "{1}(current + 1)",
                                             operand, component_type)
                               : fmt::format("(current == 0 || current > {0}) "
                                             "? {0} : {1}(current - 1)",
                                             operand, component_type));
        writeStatement("if (state.img{}.atomic_compare_exchange_weak({}, &{}, "
                       "vec<{}, 4>(desired))) break",
                       index, getImageCoordsStr(type, coords_str), expected,
                       component_type);
        exitScopeEmpty();
        exitScopeEmpty();
        storeValue(dst, "as_type<uint>({}.x)", expected);
        return;
    }

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

// Intrinsic
void MslEmitter::emitIntrinsic(const ir::Value& dst, ir::Intrinsic intrinsic,
                               std::span<const ir::Value> args) {
    std::string arguments;
    for (const auto& arg : args) {
        if (!arguments.empty())
            arguments += ", ";
        arguments += getBitwiseOperandStr(arg);
    }
    storeValue(dst, "{}({})", getIntrinsicFunctionName(intrinsic), arguments);
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
            if (output) {
                // The second blend input of render target 0
                if (state.dual_source_blending && sv.index == 1)
                    return "color(0), index(1)";
                return fmt::format("color({})", sv.index);
            }
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
