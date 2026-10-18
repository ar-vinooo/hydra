#pragma once

#include "core/hw/tegra_x1/gpu/engines/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/metal_cpp.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class ITextureView;
class ShaderBase;

enum class TextureTypeClass {
    _1D,
    _1DBuffer,
    _2D,
    _3D,
};

TextureTypeClass getTextureTypeClass(MTL::TextureType type);

// Adapts the shape of a guest texture to what the host can create: several
// layers need an array type, levels below 1x1 do not exist and 1D textures
// have a single level. Returns false for textures the host cannot create.
bool legalizeTextureShape(MTL::TextureType& type, u32 width, u32 height, u32 depth,
                          u32 layer_count, u32& level_count);

enum class TextureFormat {
    Invalid,

    R8Unorm,
    R8Snorm,
    R8Uint,
    R8Sint,
    R16Float,
    R16Unorm,
    R16Snorm,
    R16Uint,
    R16Sint,
    R32Float,
    R32Uint,
    R32Sint,
    RG8Unorm,
    RG8Snorm,
    RG8Uint,
    RG8Sint,
    RG16Float,
    RG16Unorm,
    RG16Snorm,
    RG16Uint,
    RG16Sint,
    RG32Float,
    RG32Uint,
    RG32Sint,
    RGB32Float,
    RGB32Uint,
    RGB32Sint,
    RGBA8Unorm,
    RGBA8Snorm,
    RGBA8Uint,
    RGBA8Sint,
    RGBA16Float,
    RGBA16Unorm,
    RGBA16Snorm,
    RGBA16Uint,
    RGBA16Sint,
    RGBA32Float,
    RGBA32Uint,
    RGBA32Sint,
    S8Uint,
    Z16Unorm,
    Z24Unorm_X8Uint,
    Z32Float,
    Z24Unorm_S8Uint,
    Z32Float_X24S8Uint,
    RGBX8Unorm_sRGB,
    RGBA8Unorm_sRGB,
    RGBA4Unorm,
    RGB5Unorm,
    RGB5A1Unorm,
    R5G6B5Unorm,
    RGB10A2Unorm,
    RGB10A2Uint,
    RG11B10Float,
    E5BGR9Float,
    BC1_RGB,
    BC1_RGBA,
    BC2_RGBA,
    BC3_RGBA,
    BC1_RGB_sRGB,
    BC1_RGBA_sRGB,
    BC2_RGBA_sRGB,
    BC3_RGBA_sRGB,
    BC4_RUnorm,
    BC4_RSnorm,
    BC5_RGUnorm,
    BC5_RGSnorm,
    BC7_RGBAUnorm,
    BC7_RGBAUnorm_sRGB,
    BC6H_RGBA_SF16_Float,
    BC6H_RGBA_UF16_Float,
    RGBX8Unorm,
    RGBX8Snorm,
    RGBX8Uint,
    RGBX8Sint,
    RGBX16Float,
    RGBX16Unorm,
    RGBX16Snorm,
    RGBX16Uint,
    RGBX16Sint,
    RGBX32Float,
    RGBX32Uint,
    RGBX32Sint,
    ASTC_RGBA_4x4,
    ASTC_RGBA_5x4,
    ASTC_RGBA_5x5,
    ASTC_RGBA_6x5,
    ASTC_RGBA_6x6,
    ASTC_RGBA_8x5,
    ASTC_RGBA_8x6,
    ASTC_RGBA_8x8,
    ASTC_RGBA_10x5,
    ASTC_RGBA_10x6,
    ASTC_RGBA_10x8,
    ASTC_RGBA_10x10,
    ASTC_RGBA_12x10,
    ASTC_RGBA_12x12,
    ASTC_RGBA_4x4_sRGB,
    ASTC_RGBA_5x4_sRGB,
    ASTC_RGBA_5x5_sRGB,
    ASTC_RGBA_6x5_sRGB,
    ASTC_RGBA_6x6_sRGB,
    ASTC_RGBA_8x5_sRGB,
    ASTC_RGBA_8x6_sRGB,
    ASTC_RGBA_8x8_sRGB,
    ASTC_RGBA_10x5_sRGB,
    ASTC_RGBA_10x6_sRGB,
    ASTC_RGBA_10x8_sRGB,
    ASTC_RGBA_10x10_sRGB,
    ASTC_RGBA_12x10_sRGB,
    ASTC_RGBA_12x12_sRGB,
    B5G6R5Unorm,
    BGR5Unorm,
    BGR5A1Unorm,
    A1BGR5Unorm,
    BGRX8Unorm,
    BGRA8Unorm,
    BGRX8Unorm_sRGB,
    BGRA8Unorm_sRGB,
    ETC2_R_Unorm,
    ETC2_R_Snorm,
    ETC2_RG_Unorm,
    ETC2_RG_Snorm,
    ETC2_RGB, // compatible with ETC1
    PTA_ETC2_RGB,
    ETC2_RGBA,
    ETC2_RGB_sRGB,
    PTA_ETC2_RGB_sRGB,
    ETC2_RGBA_sRGB,
    BGR10A2Unorm,
    S8Uint_Z24Unorm,
    R1G5B5A5Unorm,
    RG4Unorm,
};

struct TextureFormatInfo {
    u32 bytes_per_block;
    u32 block_width;
    u32 block_height;
    bool is_depth_stencil;
};

const TextureFormatInfo& getTextureFormatInfo(TextureFormat format);
// Metal stores these packed guest formats using wider host texels.
TextureFormat getHostTextureFormat(TextureFormat format);
void expandTexturePixels(TextureFormat format, const u8* guest, u8* host,
                         usize pixel_count);

TextureFormat toTextureFormat(NvColorFormat color_format);
TextureFormat toTextureFormat(const ImageFormatWord image_format_word,
                              bool is_srgb);
// Format a TIC is sampled as. A Z24 TIC whose swizzle only reads its stencil
// component (a uint component) samples the stencil plane, not the depth.
TextureFormat toSampledTextureFormat(const ImageFormatWord image_format_word,
                                     bool is_srgb);
TextureFormat toTextureFormat(ColorSurfaceFormat color_surface_format);
TextureFormat toTextureFormat(DepthSurfaceFormat depth_surface_format);

u32 getTextureFormatStride(const TextureFormat format, u32 width);
u32 getTextureFormatRows(const TextureFormat format, u32 height);
u32 getTextureFormatSliceStride(const TextureFormat format, u32 width,
                                u32 height);
u32 getTextureFormatBpp(const TextureFormat format);
bool isTextureFormatCompressed(const TextureFormat format);
bool isTextureFormatDepthOrStencil(const TextureFormat format);

// MTL::DataTypeNone for formats without a color data type
MTL::DataType toColorDataType(ColorSurfaceFormat format);

struct SwizzleChannels {
    ImageSwizzle r : 3;
    ImageSwizzle g : 3;
    ImageSwizzle b : 3;
    ImageSwizzle a : 3;

    SwizzleChannels()
        : r{ImageSwizzle::R}, g{ImageSwizzle::G}, b{ImageSwizzle::B},
          a{ImageSwizzle::A} {}

    SwizzleChannels(const ImageSwizzle r_, const ImageSwizzle g_,
                    const ImageSwizzle b_, const ImageSwizzle a_)
        : r{r_}, g{g_}, b{b_}, a{a_} {}

    SwizzleChannels(const TextureFormat format, const ImageSwizzle x,
                    const ImageSwizzle y, const ImageSwizzle z,
                    const ImageSwizzle w);

    bool operator==(const SwizzleChannels& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

// Swizzle of the view toSampledTextureFormat returns for a TIC
SwizzleChannels toSampledSwizzleChannels(const ImageFormatWord image_format_word,
                                         bool is_srgb);

struct TextureDescriptor {
    uptr ptr;
    MTL::TextureType type;
    TextureFormat format;
    bool is_linear;
    u32 linear_stride;
    u32 width;
    u32 height;
    u32 depth;
    u32 level_count;
    u32 layer_count;
    u32 block_width_gobs_log2;
    u32 block_height_gobs_log2;
    u32 block_depth_gobs_log2;
    u32 layer_size;
    u32 size;

    TextureDescriptor(uptr ptr_, MTL::TextureType type_, TextureFormat format_,
                      bool is_linear_, u32 linear_stride_, u32 width_,
                      u32 height_, u32 depth_, u32 level_count_,
                      u32 layer_count_, u32 block_width_gobs_log2_,
                      u32 block_height_gobs_log2_, u32 block_depth_gobs_log2_,
                      u32 layer_size_ = 0)
        : ptr{ptr_}, type{type_}, format{format_}, is_linear{is_linear_},
          linear_stride{linear_stride_}, width{width_}, height{height_},
          depth{depth_}, level_count{level_count_}, layer_count{layer_count_},
          block_width_gobs_log2{block_width_gobs_log2_},
          block_height_gobs_log2{block_height_gobs_log2_},
          block_depth_gobs_log2{block_depth_gobs_log2_},
          layer_size{layer_size_} {
        calculateSize();
    }

    ztd::Range<uptr> getRange() const {
        return ztd::Range<uptr>::fromSize(ptr, size);
    }

    u32 getGroupHash() const;
    u32 getStorageHash() const;

    MTL::Size getLevelDimensions(u32 level) const;
    MTL::Size getLevelBlockSizeLog2(u32 level) const;
    u32 getLevelOffset(u32 level) const;
    u32 getLevelSize(u32 level) const;

  private:
    void calculateSize();
};

struct TextureViewDescriptor {
    MTL::TextureType type;
    TextureFormat format;
    ztd::Range<u32> levels;
    ztd::Range<u32> layers;
    SwizzleChannels swizzle_channels;

    TextureViewDescriptor(MTL::TextureType type_, TextureFormat format_,
                          ztd::Range<u32> levels_, ztd::Range<u32> layers_,
                          SwizzleChannels swizzle_channels_ = SwizzleChannels())
        : type{type_}, format{format_}, levels{levels_}, layers{layers_},
          swizzle_channels{swizzle_channels_} {}

    bool operator==(const TextureViewDescriptor& other) const {
        return type == other.type && format == other.format &&
               levels.getBegin() == other.levels.getBegin() &&
               levels.getEnd() == other.levels.getEnd() &&
               layers.getBegin() == other.layers.getBegin() &&
               layers.getEnd() == other.layers.getEnd() &&
               swizzle_channels == other.swizzle_channels;
    }

    u32 getHash() const;
};

enum class SamplerFilter {
    Nearest = 1,
    Linear = 2,
    // MIN_ANISO for minification, VCAA_4_TAP for magnification
    Anisotropic = 3,
    Vcaa8Tap = 4,
};

enum class SamplerAddressMode {
    Repeat = 0,
    MirroredRepeat = 1,
    ClampToEdge = 2,
    ClampToBorder = 3,
    Clamp = 4,
    MirrorClampToEdge = 5,
    MirrorClampToBorder = 6,
    MirrorClamp = 7,
};

struct SamplerDescriptor {
    SamplerFilter min_filter;
    SamplerFilter mag_filter;
    MTL::SamplerMipFilter mip_filter;
    SamplerAddressMode address_mode_s;
    SamplerAddressMode address_mode_t;
    SamplerAddressMode address_mode_r;
    engines::CompareOp depth_compare_op;
    simd::uint4 border_color_u;
    f32 min_lod = 0.0f;
    f32 max_lod = 15.0f;
    f32 lod_bias = 0.0f;
    u32 max_anisotropy = 1;
    // The sampled texture has an integer format: the border color holds raw
    // component bits
    bool integer_texture = false;
};

constexpr bool isSrgbTextureFormat(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBX8Unorm_sRGB:
    case TextureFormat::RGBA8Unorm_sRGB:
    case TextureFormat::BGRX8Unorm_sRGB:
    case TextureFormat::BGRA8Unorm_sRGB:
    case TextureFormat::BC1_RGB_sRGB:
    case TextureFormat::BC1_RGBA_sRGB:
    case TextureFormat::BC2_RGBA_sRGB:
    case TextureFormat::BC3_RGBA_sRGB:
    case TextureFormat::BC7_RGBAUnorm_sRGB:
    case TextureFormat::ASTC_RGBA_4x4_sRGB:
    case TextureFormat::ASTC_RGBA_5x4_sRGB:
    case TextureFormat::ASTC_RGBA_5x5_sRGB:
    case TextureFormat::ASTC_RGBA_6x5_sRGB:
    case TextureFormat::ASTC_RGBA_6x6_sRGB:
    case TextureFormat::ASTC_RGBA_8x5_sRGB:
    case TextureFormat::ASTC_RGBA_8x6_sRGB:
    case TextureFormat::ASTC_RGBA_8x8_sRGB:
    case TextureFormat::ASTC_RGBA_10x5_sRGB:
    case TextureFormat::ASTC_RGBA_10x6_sRGB:
    case TextureFormat::ASTC_RGBA_10x8_sRGB:
    case TextureFormat::ASTC_RGBA_10x10_sRGB:
    case TextureFormat::ASTC_RGBA_12x10_sRGB:
    case TextureFormat::ASTC_RGBA_12x12_sRGB:
    case TextureFormat::ETC2_RGB_sRGB:
    case TextureFormat::PTA_ETC2_RGB_sRGB:
    case TextureFormat::ETC2_RGBA_sRGB:
        return true;
    default:
        return false;
    }
}

// Metal only has three border colors. Every other guest border color (and
// mirrored-to-border addressing) is emulated in the shader.
enum class SamplerBorderPreset {
    TransparentBlack,
    OpaqueBlack,
    OpaqueWhite,
    Custom,
};

constexpr bool isBorderAddressMode(SamplerAddressMode mode) {
    return mode == SamplerAddressMode::ClampToBorder ||
           mode == SamplerAddressMode::MirrorClampToBorder;
}

constexpr bool samplerUsesBorder(const SamplerDescriptor& descriptor) {
    return isBorderAddressMode(descriptor.address_mode_s) ||
           isBorderAddressMode(descriptor.address_mode_t) ||
           isBorderAddressMode(descriptor.address_mode_r);
}

constexpr bool samplerMirrorsToBorder(const SamplerDescriptor& descriptor) {
    return descriptor.address_mode_s == SamplerAddressMode::MirrorClampToBorder ||
           descriptor.address_mode_t == SamplerAddressMode::MirrorClampToBorder ||
           descriptor.address_mode_r == SamplerAddressMode::MirrorClampToBorder;
}

inline SamplerBorderPreset
getSamplerBorderPreset(const SamplerDescriptor& descriptor) {
    const auto r = descriptor.border_color_u.x;
    const auto g = descriptor.border_color_u.y;
    const auto b = descriptor.border_color_u.z;
    const auto a = descriptor.border_color_u.w;
    if (descriptor.integer_texture) {
        return (r | g | b | a) == 0 ? SamplerBorderPreset::TransparentBlack
                                    : SamplerBorderPreset::Custom;
    }
    constexpr u32 zero = 0x00000000u;
    constexpr u32 one = 0x3f800000u;
    // Depth comparisons use the first component as the border depth
    if (descriptor.depth_compare_op != engines::CompareOp::Invalid) {
        if (r == zero)
            return SamplerBorderPreset::TransparentBlack;
        if (r == one)
            return SamplerBorderPreset::OpaqueWhite;
        return SamplerBorderPreset::Custom;
    }
    if (r == zero && g == zero && b == zero) {
        if (a == zero)
            return SamplerBorderPreset::TransparentBlack;
        if (a == one)
            return SamplerBorderPreset::OpaqueBlack;
    } else if (r == one && g == one && b == one && a == one) {
        return SamplerBorderPreset::OpaqueWhite;
    }
    return SamplerBorderPreset::Custom;
}

// Whether the shader has to emulate the sampler's border handling because
// Metal cannot express it
inline bool samplerNeedsBorderEmulation(const SamplerDescriptor& descriptor) {
    return samplerMirrorsToBorder(descriptor) ||
           (samplerUsesBorder(descriptor) &&
            getSamplerBorderPreset(descriptor) == SamplerBorderPreset::Custom);
}

// Anisotropic filtering needs linear minification and mip filtering
constexpr u32 getEffectiveSamplerAnisotropy(const SamplerDescriptor& descriptor) {
    if (descriptor.min_filter != SamplerFilter::Nearest &&
        descriptor.mip_filter == MTL::SamplerMipFilterLinear)
        return std::clamp(descriptor.max_anisotropy, 1u, 16u);
    return 1u;
}

struct RenderTargetDescriptor {
    ITextureView* texture;
    bool load_action_clear = false;
    union {
        float color[4];
        struct {
            bool clear_depth;
            bool clear_stencil;
            float depth;
            u32 stencil;
        };
    } clear_data;
};

struct RenderPassDescriptor {
    RenderTargetDescriptor color_targets[COLOR_TARGET_COUNT];
    RenderTargetDescriptor depth_stencil_target;
};

struct ClearState {
    MTL::ScissorRect rect;
    u32 stencil_write_mask{0xff};
};

enum class ShaderType {
    Vertex,
    Fragment,
    Compute,

    Count,
};

// Shader types that are part of a render pipeline
constexpr usize GRAPHICS_SHADER_TYPE_COUNT = 2;

// Texture bindings normally index the bindless texture constant buffer. Bindless
// instructions take the handle from a register instead; when the decompiler can
// trace that register back to constant buffer loads, the binding names the
// constant buffer slot and words explicitly. The handle may be built from a
// texture word OR'd with a separate sampler word.
constexpr u32 TRACKED_TEXTURE_INDEX_FLAG = 0x80000000;
constexpr u32 TRACKED_TEXTURE_WORD_MASK = 0x1fff;
constexpr u32 TRACKED_TEXTURE_NO_SAMPLER = TRACKED_TEXTURE_WORD_MASK;

constexpr u32 makeTrackedTextureIndex(
    u32 slot, u32 word, u32 sampler_word = TRACKED_TEXTURE_NO_SAMPLER) {
    return TRACKED_TEXTURE_INDEX_FLAG | ((slot & 0x1f) << 26) |
           ((word & TRACKED_TEXTURE_WORD_MASK) << 13) |
           (sampler_word & TRACKED_TEXTURE_WORD_MASK);
}
constexpr bool isTrackedTextureIndex(u32 index) {
    return (index & TRACKED_TEXTURE_INDEX_FLAG) != 0;
}
constexpr u32 getTrackedTextureSlot(u32 index) { return (index >> 26) & 0x1f; }
constexpr u32 getTrackedTextureWord(u32 index) {
    return (index >> 13) & TRACKED_TEXTURE_WORD_MASK;
}
constexpr u32 getTrackedSamplerWord(u32 index) {
    return index & TRACKED_TEXTURE_WORD_MASK;
}

// Extra parameters of texture blits. A zero source extent means the extent of
// the source region is the one given by the source size; a negative extent
// mirrors the copy
struct BlitOptions {
    bool nearest = false;
    f32 src_width = 0.0f;
    f32 src_height = 0.0f;
};

// Component type shaders access an integer texture format with ("uint" or
// "int"), or nullptr for float formats
constexpr const char* getIntegerTextureComponentType(TextureFormat format) {
    switch (format) {
    case TextureFormat::R8Uint:
    case TextureFormat::R16Uint:
    case TextureFormat::R32Uint:
    case TextureFormat::RG8Uint:
    case TextureFormat::RG16Uint:
    case TextureFormat::RG32Uint:
    case TextureFormat::RGBA8Uint:
    case TextureFormat::RGBA16Uint:
    case TextureFormat::RGBA32Uint:
    case TextureFormat::S8Uint:
        return "uint";
    case TextureFormat::R8Sint:
    case TextureFormat::R16Sint:
    case TextureFormat::R32Sint:
    case TextureFormat::RG8Sint:
    case TextureFormat::RG16Sint:
    case TextureFormat::RG32Sint:
    case TextureFormat::RGBA8Sint:
    case TextureFormat::RGBA16Sint:
    case TextureFormat::RGBA32Sint:
        return "int";
    default:
        return nullptr;
    }
}

// Bindless handles that are shader constants are encoded with an invalid
// constant buffer slot
constexpr u32 CONSTANT_TEXTURE_SLOT = 0x1f;
constexpr u32 CONSTANT_TEXTURE_HANDLE_MASK = (1u << 26) - 1;

constexpr u32 makeConstantTextureIndex(u32 handle) {
    return TRACKED_TEXTURE_INDEX_FLAG | (CONSTANT_TEXTURE_SLOT << 26) |
           (handle & CONSTANT_TEXTURE_HANDLE_MASK);
}
constexpr bool isConstantTextureIndex(u32 index) {
    return isTrackedTextureIndex(index) &&
           getTrackedTextureSlot(index) == CONSTANT_TEXTURE_SLOT;
}
constexpr u32 getConstantTextureHandle(u32 index) {
    return index & CONSTANT_TEXTURE_HANDLE_MASK;
}

struct ResourceMapping {
    u32 fragment_output_mask{0xffffffffu};
    // Constant-buffer jump table words used to enumerate indirect branches.
    std::map<std::pair<u32, u32>, u32> branch_constants;
    u32 uniform_buffers[CONST_BUFFER_BINDING_COUNT];
    // Global memory buffers. Their address and size are read from the NVN
    // descriptor at `cbuf_offset` in constant buffer `cbuf_slot`
    struct StorageBuffer {
        u32 cbuf_slot;
        u32 cbuf_offset;
        u32 host_index;
        bool written;
    };
    std::vector<StorageBuffer> storage_buffers;
    std::map<u32, u32> textures;
    // Texture type the shader declares for each binding
    std::map<u32, MTL::TextureType> texture_types;
    u32 sampler_buffer_index{invalid<u32>()};
    // Fragment shader user inputs (bit per location)
    u32 stage_input_mask{0};
    // Storage images (surfaces), keyed like textures
    std::map<u32, u32> images;
    std::map<u32, MTL::TextureType> image_types;
    // The vertex shader exports the render target layer or viewport index
    bool writes_layer{false};

    ResourceMapping() {
        for (auto& uniform_buffer : uniform_buffers)
            uniform_buffer = invalid<u32>();
    }
};

struct ShaderDescriptor {
    ShaderType type;
    ShaderBackend backend;
    std::vector<u8> code;
    ResourceMapping resource_mapping;
    // The vertex shader writes the render target array index
    bool writes_layer{false};
};

struct VertexArray {
    bool enable;
    u32 stride;
    bool is_per_instance;
    u32 divisor;
};

struct VertexState {
    engines::VertexAttribState vertex_attrib_states[VERTEX_ATTRIB_COUNT];
    VertexArray vertex_arrays[VERTEX_ARRAY_COUNT];
};

struct ColorTargetState {
    TextureFormat format;
    engines::ColorWriteMask write_mask;
    bool blend_enabled;
    MTL::BlendOperation rgb_op;
    MTL::BlendFactor src_rgb_factor;
    MTL::BlendFactor dst_rgb_factor;
    MTL::BlendOperation alpha_op;
    MTL::BlendFactor src_alpha_factor;
    MTL::BlendFactor dst_alpha_factor;
};

struct StencilFaceState {
    engines::CompareOp func{engines::CompareOp::Always};
    MTL::StencilOperation fail_op{MTL::StencilOperationKeep};
    MTL::StencilOperation depth_fail_op{MTL::StencilOperationKeep};
    MTL::StencilOperation pass_op{MTL::StencilOperationKeep};
    u32 ref{0};
    u32 func_mask{0xff};
    u32 write_mask{0xff};
};

struct StencilState {
    bool enabled{false};
    StencilFaceState front{};
    StencilFaceState back{};
};

struct PipelineDescriptor {
    ShaderBase* shaders[static_cast<usize>(ShaderType::Count)];
    VertexState vertex_state;
    ColorTargetState color_target_states[COLOR_TARGET_COUNT];
    TextureFormat depth_stencil_format{TextureFormat::Invalid};
    // Required when the vertex shader selects the layer or viewport
    MTL::PrimitiveTopologyClass topology_class{
        MTL::PrimitiveTopologyClassUnspecified};
};

usize getVertexFormatSize(engines::VertexAttribSize size);

enum class TextureUsage {
    Read,
    Write,
    Present,
};

} // namespace hydra::hw::tegra_x1::gpu::renderer

ENABLE_ENUM_FORMATTING(
    hydra::hw::tegra_x1::gpu::renderer::TextureFormat, Invalid, "invalid",
    R8Unorm, "r8unorm", R8Snorm, "r8snorm", R8Uint, "r8uint", R8Sint, "r8sint",
    R16Float, "r16float", R16Unorm, "r16unorm", R16Snorm, "r16snorm", R16Uint,
    "r16uint", R16Sint, "r16sint", R32Float, "r32float", R32Uint, "r32uint",
    R32Sint, "r32sint", RG8Unorm, "rg8unorm", RG8Snorm, "rg8snorm", RG8Uint,
    "rg8uint", RG8Sint, "rg8sint", RG16Float, "rg16float", RG16Unorm,
    "rg16unorm", RG16Snorm, "rg16snorm", RG16Uint, "rg16uint", RG16Sint,
    "rg16sint", RG32Float, "rg32float", RG32Uint, "rg32uint", RG32Sint,
    "rg32sint", RGB32Float, "rgb32float", RGB32Uint, "rgb32uint", RGB32Sint,
    "rgb32sint", RGBA8Unorm, "rgba8unorm", RGBA8Snorm, "rgba8snorm", RGBA8Uint,
    "rgba8uint", RGBA8Sint, "rgba8sint", RGBA16Float, "rgba16float",
    RGBA16Unorm, "rgba16unorm", RGBA16Snorm, "rgba16snorm", RGBA16Uint,
    "rgba16uint", RGBA16Sint, "rgba16sint", RGBA32Float, "rgba32float",
    RGBA32Uint, "rgba32uint", RGBA32Sint, "rgba32sint", S8Uint, "s8uint",
    Z16Unorm, "z16unorm", Z24Unorm_X8Uint, "z24unorm x8uint", Z32Float,
    "z32float", Z24Unorm_S8Uint, "z24unorm s8uint", Z32Float_X24S8Uint,
    "z32float x24s8uint", RGBX8Unorm_sRGB, "rgbx8unorm_srgb", RGBA8Unorm_sRGB,
    "rgba8unorm_srgb", RGBA4Unorm, "rgba4unorm", RGB5Unorm, "rgb5unorm",
    RGB5A1Unorm, "rgb5a1unorm", R5G6B5Unorm, "r5g6b5unorm", RGB10A2Unorm,
    "rgb10a2unorm", RGB10A2Uint, "rgb10a2uint", RG11B10Float, "rg11b10float",
    E5BGR9Float, "e5bgr9float", BC1_RGB, "bc1_rgb", BC1_RGBA, "bc1_rgba",
    BC2_RGBA, "bc2_rgba", BC3_RGBA, "bc3_rgba", BC1_RGB_sRGB, "bc1_rgb_srgb",
    BC1_RGBA_sRGB, "bc1_rgba_srgb", BC2_RGBA_sRGB, "bc2_rgba_srgb",
    BC3_RGBA_sRGB, "bc3_rgba_srgb", BC4_RUnorm, "bc4_runorm", BC4_RSnorm,
    "bc4_rsnorm", BC5_RGUnorm, "bc5_rgunorm", BC5_RGSnorm, "bc5_rgsnorm",
    BC7_RGBAUnorm, "bc7_rgbaunorm", BC7_RGBAUnorm_sRGB, "bc7_rgbaunorm_srgb",
    BC6H_RGBA_SF16_Float, "bc6h_rgba_sf16_float", BC6H_RGBA_UF16_Float,
    "bc6h_rgba_uf16_float", RGBX8Unorm, "rgbx8unorm", RGBX8Snorm, "rgbx8snorm",
    RGBX8Uint, "rgbx8uint", RGBX8Sint, "rgbx8sint", RGBX16Float, "rgbx16float",
    RGBX16Unorm, "rgbx16unorm", RGBX16Snorm, "rgbx16snorm", RGBX16Uint,
    "rgbx16uint", RGBX16Sint, "rgbx16sint", RGBX32Float, "rgbx32float",
    RGBX32Uint, "rgbx32uint", RGBX32Sint, "rgbx32sint", ASTC_RGBA_4x4,
    "astc_rgba_4x4", ASTC_RGBA_5x4, "astc_rgba_5x4", ASTC_RGBA_5x5,
    "astc_rgba_5x5", ASTC_RGBA_6x5, "astc_rgba_6x5", ASTC_RGBA_6x6,
    "astc_rgba_6x6", ASTC_RGBA_8x5, "astc_rgba_8x5", ASTC_RGBA_8x6,
    "astc_rgba_8x6", ASTC_RGBA_8x8, "astc_rgba_8x8", ASTC_RGBA_10x5,
    "astc_rgba_10x5", ASTC_RGBA_10x6, "astc_rgba_10x6", ASTC_RGBA_10x8,
    "astc_rgba_10x8", ASTC_RGBA_10x10, "astc_rgba_10x10", ASTC_RGBA_12x10,
    "astc_rgba_12x10", ASTC_RGBA_12x12, "astc_rgba_12x12", ASTC_RGBA_4x4_sRGB,
    "astc_rgba_4x4_srgb", ASTC_RGBA_5x4_sRGB, "astc_rgba_5x4_srgb",
    ASTC_RGBA_5x5_sRGB, "astc_rgba_5x5_srgb", ASTC_RGBA_6x5_sRGB,
    "astc_rgba_6x5_srgb", ASTC_RGBA_6x6_sRGB, "astc_rgba_6x6_srgb",
    ASTC_RGBA_8x5_sRGB, "astc_rgba_8x5_srgb", ASTC_RGBA_8x6_sRGB,
    "astc_rgba_8x6_srgb", ASTC_RGBA_8x8_sRGB, "astc_rgba_8x8_srgb",
    ASTC_RGBA_10x5_sRGB, "astc_rgba_10x5_srgb", ASTC_RGBA_10x6_sRGB,
    "astc_rgba_10x6_srgb", ASTC_RGBA_10x8_sRGB, "astc_rgba_10x8_srgb",
    ASTC_RGBA_10x10_sRGB, "astc_rgba_10x10_srgb", ASTC_RGBA_12x10_sRGB,
    "astc_rgba_12x10_srgb", ASTC_RGBA_12x12_sRGB, "astc_rgba_12x12_srgb",
    B5G6R5Unorm, "b5g6r5unorm", BGR5Unorm, "bgr5unorm", BGR5A1Unorm,
    "bgr5a1unorm", A1BGR5Unorm, "a1bgr5unorm", BGRX8Unorm, "bgrx8unorm",
    BGRA8Unorm, "bgra8unorm", BGRX8Unorm_sRGB, "bgrx8unorm_srgb",
    BGRA8Unorm_sRGB, "bgra8unorm_srgb", ETC2_R_Unorm, "etc2_r_unorm",
    ETC2_R_Snorm, "etc2_r_snorm", ETC2_RG_Unorm, "etc2_rg_unorm", ETC2_RG_Snorm,
    "etc2_rg_snorm", ETC2_RGB, "etc2_rgb", PTA_ETC2_RGB, "pta_etc2_rgb",
    ETC2_RGBA, "etc2_rgba", ETC2_RGB_sRGB, "etc2_rgb_srgb", PTA_ETC2_RGB_sRGB,
    "pta_etc2_rgb_srgb", ETC2_RGBA_sRGB, "etc2_rgba_srgb", BGR10A2Unorm,
    "bgr10a2unorm", S8Uint_Z24Unorm, "s8uint z24unorm", R1G5B5A5Unorm,
    "r1g5b5a5unorm", RG4Unorm, "rg4unorm")

ENABLE_STRUCT_FORMATTING(hydra::hw::tegra_x1::gpu::renderer::SwizzleChannels, r,
                         "", "r", g, "", "g", b, "", "b", a, "", "a")

ENABLE_STRUCT_FORMATTING(hydra::hw::tegra_x1::gpu::renderer::TextureDescriptor,
                         ptr, ":#x", "ptr", type, "", "type", format, "",
                         "format", is_linear, "", "is linear", linear_stride,
                         "", "linear stride", width, "", "width", height, "",
                         "height", depth, "", "depth", level_count, "",
                         "level count", layer_count, "", "layer count",
                         block_width_gobs_log2, "", "log2(block width in GOBs)",
                         block_height_gobs_log2, "",
                         "log2(block height in GOBs)", block_depth_gobs_log2,
                         "", "log2(block depth in GOBs)", layer_size, ":#x",
                         "layer size", size, ":#x", "size")

ENABLE_STRUCT_FORMATTING(
    hydra::hw::tegra_x1::gpu::renderer::TextureViewDescriptor, type, "", "type",
    format, "", "format", levels, "", "levels", layers, "", "layers",
    swizzle_channels, "", "swizzle channels")

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::renderer::ShaderType, Vertex,
                       "vertex", Fragment, "fragment", Compute, "compute",
                       Count, "invalid")
