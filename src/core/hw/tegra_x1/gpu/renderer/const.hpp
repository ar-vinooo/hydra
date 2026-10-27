#pragma once

#include "core/hw/tegra_x1/gpu/engines/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/metal_cpp.hpp"

#include <array>
#include <set>

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

// Metal has no alpha-only or ARGB render targets. These guest formats use the
// Metal format with the same memory layout; the fragment output (and clear
// color) is rearranged so the alpha lands in the right channel.
enum class ColorOutputSwizzle : u8 {
    Identity,
    AlphaToRed,   // A16, AF16, AF32, A8: (r, g, b, a) -> (a, 0, 0, 0)
    AlphaToGreen, // A8R8, R16_A16, RF16_AF16, RF32_AF32: -> (r, a, 0, 0)
    Argb,         // B8G8R8A8 (bytes A, R, G, B): -> (a, r, g, b)
};
ColorOutputSwizzle toColorOutputSwizzle(ColorSurfaceFormat format);
// Rearranges a clear color (r, g, b, a) and write mask (R=1 G=2 B=4 A=8) for
// the channels the Metal format actually stores.
void swizzleClearColor(ColorOutputSwizzle swizzle, std::array<u32, 4>& color,
                       u8& mask);

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

// Maxwell multisample layout (SET_ANTI_ALIAS samples mode and the TIC multi
// sample count share the encoding). A multisampled surface is stored as a
// larger single-sample image: every pixel is a block of samples_x * samples_y
// texels (nouveau: width0 << ms_x), and surface dimensions in registers and
// texture headers are those of the larger image.
struct MsaaLayout {
    u32 samples{1};
    u32 samples_x_log2{0};
    u32 samples_y_log2{0};
    // Coverage sampling (VC) modes have extra coverage samples, the color
    // samples are those of the layout
    bool coverage_samples{false};
    // The D3D patterns have other sample positions in the pixel
    bool d3d_positions{false};
};
MsaaLayout getMsaaLayout(u32 mode);
MsaaLayout getMsaaLayoutForSamples(u32 samples);
// Position of sample `index` in its pixel's block of the larger image
// (nouveau nvc0_get_sample_locations: surface coords)
std::pair<u32, u32> getMsaaSampleTexel(u32 samples, u32 index);
// Sample position in the pixel, in 1/16 pixel
std::pair<u32, u32> getMsaaSamplePosition(const MsaaLayout& layout, u32 index);

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
    // Samples per pixel. width and height stay those of the guest surface,
    // whose pixels are blocks of samples (see MsaaLayout): the host texture
    // is multisampled at width >> samples_x_log2.
    u32 sample_count{1};

    TextureDescriptor(uptr ptr_, MTL::TextureType type_, TextureFormat format_,
                      bool is_linear_, u32 linear_stride_, u32 width_,
                      u32 height_, u32 depth_, u32 level_count_,
                      u32 layer_count_, u32 block_width_gobs_log2_,
                      u32 block_height_gobs_log2_, u32 block_depth_gobs_log2_,
                      u32 layer_size_ = 0, u32 sample_count_ = 1)
        : ptr{ptr_}, type{type_}, format{format_}, is_linear{is_linear_},
          linear_stride{linear_stride_}, width{width_}, height{height_},
          depth{depth_}, level_count{level_count_}, layer_count{layer_count_},
          block_width_gobs_log2{block_width_gobs_log2_},
          block_height_gobs_log2{block_height_gobs_log2_},
          block_depth_gobs_log2{block_depth_gobs_log2_},
          layer_size{layer_size_}, sample_count{sample_count_} {
        calculateSize();
    }

    bool isMultisampled() const { return sample_count > 1; }
    // Pixels of the multisampled image
    u32 getPixelWidth() const {
        return std::max(width >> getMsaaLayoutForSamples(sample_count).samples_x_log2, 1u);
    }
    u32 getPixelHeight() const {
        return std::max(height >> getMsaaLayoutForSamples(sample_count).samples_y_log2, 1u);
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

struct SamplerDescriptor {
    MTL::SamplerMinMagFilter min_filter;
    MTL::SamplerMinMagFilter mag_filter;
    MTL::SamplerMipFilter mip_filter;
    MTL::SamplerAddressMode address_mode_s;
    MTL::SamplerAddressMode address_mode_t;
    MTL::SamplerAddressMode address_mode_r;
    engines::CompareOp depth_compare_op;
    simd::uint4 border_color_u;
    f32 min_lod = 0.0f;
    f32 max_lod = 15.0f;
    f32 lod_bias = 0.0f;
    u32 max_anisotropy = 1;
    // The sampled texture has an integer format: the border color holds raw
    // component bits
    bool integer_texture = false;
    // Axes (bit 0 S, 1 T, 2 R) addressed with mirror-clamp-to-border. Metal
    // has no such mode: those axes carry ClampToBorderColor and the shader
    // emulates the mirroring.
    u8 mirror_to_border_mask = 0;
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

constexpr bool samplerUsesBorder(const SamplerDescriptor& descriptor) {
    return descriptor.address_mode_s == MTL::SamplerAddressModeClampToBorderColor ||
           descriptor.address_mode_t == MTL::SamplerAddressModeClampToBorderColor ||
           descriptor.address_mode_r == MTL::SamplerAddressModeClampToBorderColor;
}

constexpr bool samplerMirrorsToBorder(const SamplerDescriptor& descriptor) {
    return descriptor.mirror_to_border_mask != 0;
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
    if (descriptor.min_filter != MTL::SamplerMinMagFilterNearest &&
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
    // SET_ANTI_ALIAS samples mode of multisampled targets (sample positions)
    u32 msaa_mode{0};
};

struct ClearState {
    MTL::ScissorRect rect;
    u32 stencil_write_mask{0xff};
};

enum class ShaderType {
    Vertex,
    Fragment,
    Compute,
    // Emulated with vertex functions (see geometry.hpp, tessellation.hpp)
    Geometry,
    TessControl,
    TessEvaluation,

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
    // Validated decoder prefix, including the graphics header; zero is unknown.
    u64 code_read_size{0};
    u32 fragment_output_mask{0xffffffffu};
    // Constant-buffer jump table words used to enumerate indirect branches.
    std::map<std::pair<u32, u32>, u32> branch_constants;
    u32 uniform_buffers[CONST_BUFFER_BINDING_COUNT];
    // Required prefix for direct reads; zero requires the complete binding.
    u64 uniform_buffer_sizes[CONST_BUFFER_BINDING_COUNT]{};
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
    // Bindings whose declared type is a default: the shader only queries the
    // size of the texture, so nothing in its code tells the type
    std::set<u32> guessed_texture_types;
    u32 sampler_buffer_index{invalid<u32>()};
    // Fragment shader user inputs (bit per location)
    u32 stage_input_mask{0};
    // Storage images (surfaces), keyed like textures
    std::map<u32, u32> images;
    std::map<u32, MTL::TextureType> image_types;
    // The vertex shader exports the render target layer or viewport index
    bool writes_layer{false};
    // A vertex shader capturing its outputs for a geometry shader (see
    // geometry.hpp): its record buffer and parameter slots
    u32 capture_buffer_index{invalid<u32>()};
    u32 capture_params_index{invalid<u32>()};
    // A geometry shader: its header's output topology (geometry::Output, 0
    // otherwise), output vertex limit and invocations per primitive
    u32 geometry_output{0};
    u32 geometry_max_output_vertices{0};
    u32 geometry_invocations{1};
    // A tessellation control shader: the control points it outputs per patch
    u32 tess_output_vertices{0};
    // Guest features the decompile skipped; the shader may compute wrong values
    std::vector<std::string> unsupported;

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
    // With a geometry shader: the vertex function drawing what it emits (the
    // vertex shader captures its outputs, see geometry.hpp)
    ShaderBase* geometry_passthrough{nullptr};
    // Tessellation (see tessellation.hpp): the function writing the factors,
    // SET_TESSELLATION_PARAMETERS and the control points per input patch
    ShaderBase* tessellation_factors{nullptr};
    u32 tessellation_parameters{0};
    u32 tessellation_patch_size{0};
    VertexState vertex_state;
    ColorTargetState color_target_states[COLOR_TARGET_COUNT];
    TextureFormat depth_stencil_format{TextureFormat::Invalid};
    // Required when the vertex shader selects the layer or viewport
    MTL::PrimitiveTopologyClass topology_class{
        MTL::PrimitiveTopologyClassUnspecified};
    // SET_ANTI_ALIAS_ALPHA_CONTROL: coverage from the alpha of output 0, and
    // that alpha replaced by 1
    bool alpha_to_coverage{false};
    bool alpha_to_one{false};
    // Samples of the render targets (SET_ANTI_ALIAS)
    u32 sample_count{1};
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
                         "", "log2(block depth in GOBs)", sample_count, "",
                         "samples", layer_size, ":#x",
                         "layer size", size, ":#x", "size")

ENABLE_STRUCT_FORMATTING(
    hydra::hw::tegra_x1::gpu::renderer::TextureViewDescriptor, type, "", "type",
    format, "", "format", levels, "", "levels", layers, "", "layers",
    swizzle_channels, "", "swizzle channels")

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::renderer::ShaderType, Vertex,
                       "vertex", Fragment, "fragment", Compute, "compute",
                       Count, "invalid")
