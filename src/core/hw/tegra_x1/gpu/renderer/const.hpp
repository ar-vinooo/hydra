#pragma once

#include "core/hw/tegra_x1/gpu/engines/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class ITextureView;
class ShaderBase;

enum class TextureType {
    _1D,
    _1DArray,
    _1DBuffer,
    _2D,
    _2DArray,
    _3D,
    Cube,
    CubeArray,
};

enum class TextureTypeClass {
    _1D,
    _1DBuffer,
    _2D,
    _3D,
};

TextureTypeClass getTextureTypeClass(TextureType type);

// Adapts the shape of a guest texture to what the host can create: several
// layers need an array type, levels below 1x1 do not exist and 1D textures
// have a single level. Returns false for textures the host cannot create.
bool legalizeTextureShape(TextureType& type, u32 width, u32 height, u32 depth,
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
};

struct TextureFormatInfo {
    u32 bytes_per_block;
    u32 block_width;
    u32 block_height;
    bool is_depth_stencil;
};

const TextureFormatInfo& getTextureFormatInfo(TextureFormat format);

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

enum class ColorDataType : u8 {
    Invalid,
    Float,
    Int,
    UInt,
};

ColorDataType toColorDataType(ColorSurfaceFormat format);

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
    TextureType type;
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

    TextureDescriptor(uptr ptr_, TextureType type_, TextureFormat format_,
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

    uint3 getLevelDimensions(u32 level) const;
    uint3 getLevelBlockSizeLog2(u32 level) const;
    u32 getLevelOffset(u32 level) const;
    u32 getLevelSize(u32 level) const;

  private:
    void calculateSize();
};

struct TextureViewDescriptor {
    TextureType type;
    TextureFormat format;
    ztd::Range<u32> levels;
    ztd::Range<u32> layers;
    SwizzleChannels swizzle_channels;

    TextureViewDescriptor(TextureType type_, TextureFormat format_,
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

enum class SamplerMipFilter {
    NotMipmapped = 1,
    Nearest = 2,
    Linear = 3,
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
    SamplerMipFilter mip_filter;
    SamplerAddressMode address_mode_s;
    SamplerAddressMode address_mode_t;
    SamplerAddressMode address_mode_r;
    engines::CompareOp depth_compare_op;
    uint4 border_color_u;
    f32 min_lod = 0.0f;
    f32 max_lod = 15.0f;
    f32 lod_bias = 0.0f;
    u32 max_anisotropy = 1;
    // TODO: more
};

enum class BlendOperation {
    Add = 1,
    Sub = 2,
    RevSub = 3,
    Min = 4,
    Max = 5,
};

enum class BlendFactor {
    Zero = 1,
    One = 2,
    SrcColor = 3,
    InvSrcColor = 4,
    SrcAlpha = 5,
    InvSrcAlpha = 6,
    DstAlpha = 7,
    InvDstAlpha = 8,
    DstColor = 9,
    InvDstColor = 10,
    SrcAlphaSaturate = 11,
    Src1Color = 16,
    InvSrc1Color = 17,
    Src1Alpha = 18,
    InvSrc1Alpha = 19,
    ConstColor = 20,
    InvConstColor = 21,
    ConstAlpha = 22,
    InvConstAlpha = 23,
};

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

struct Viewport {
    FloatRect2D rect;
    f32 depth_near;
    f32 depth_far;
};

using Scissor = UIntRect2D;

struct ClearState {
    Scissor rect;
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
    // TODO: storage buffers
    std::map<u32, u32> textures;
    // Texture type the shader declares for each binding
    std::map<u32, TextureType> texture_types;
    u32 sampler_buffer_index{invalid<u32>()};
    // Fragment shader user inputs (bit per location)
    u32 stage_input_mask{0};
    // Storage images (surfaces), keyed like textures
    std::map<u32, u32> images;
    std::map<u32, TextureType> image_types;
    // The vertex shader exports the render target layer or viewport index
    bool writes_layer{false};

    ResourceMapping() {
        for (auto& uniform_buffer : uniform_buffers)
            uniform_buffer = invalid<u32>();
        // TODO: storage buffers
        // TODO: images
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
    BlendOperation rgb_op;
    BlendFactor src_rgb_factor;
    BlendFactor dst_rgb_factor;
    BlendOperation alpha_op;
    BlendFactor src_alpha_factor;
    BlendFactor dst_alpha_factor;
};

enum class StencilOp {
    Keep,
    Zero,
    Replace,
    IncrSat,
    DecrSat,
    Invert,
    Incr,
    Decr,
};

struct StencilFaceState {
    engines::CompareOp func{engines::CompareOp::Always};
    StencilOp fail_op{StencilOp::Keep};
    StencilOp depth_fail_op{StencilOp::Keep};
    StencilOp pass_op{StencilOp::Keep};
    u32 ref{0};
    u32 func_mask{0xff};
    u32 write_mask{0xff};
};

struct StencilState {
    bool enabled{false};
    StencilFaceState front{};
    StencilFaceState back{};
};

enum class PrimitiveTopologyClass : u32 {
    Unspecified,
    Point,
    Line,
    Triangle,
};

struct PipelineDescriptor {
    ShaderBase* shaders[static_cast<usize>(ShaderType::Count)];
    VertexState vertex_state;
    ColorTargetState color_target_states[COLOR_TARGET_COUNT];
    TextureFormat depth_stencil_format{TextureFormat::Invalid};
    // Required when the vertex shader selects the layer or viewport
    PrimitiveTopologyClass topology_class{PrimitiveTopologyClass::Unspecified};
};

usize getVertexFormatSize(engines::VertexAttribSize size);

enum class TextureUsage {
    Read,
    Write,
    Present,
};

} // namespace hydra::hw::tegra_x1::gpu::renderer

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::renderer::TextureType, _1D,
                       "1D", _1DArray, "1D array", _1DBuffer, "1D buffer", _2D,
                       "2D", _2DArray, "2D array", _3D, "3D", Cube, "cube",
                       CubeArray, "cube array")

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
    "r1g5b5a5unorm")

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
