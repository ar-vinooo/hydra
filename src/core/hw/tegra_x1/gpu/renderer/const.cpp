#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

#include "core/hw/tegra_x1/gpu/memory_util.hpp"

#include <array>
#include <atomic>
#include <cstdlib>

namespace hydra::hw::tegra_x1::gpu::renderer {

TextureTypeClass getTextureTypeClass(TextureType type) {
    switch (type) {
    case TextureType::_1D:
    case TextureType::_1DArray:
        return TextureTypeClass::_1D;
    case TextureType::_1DBuffer:
        return TextureTypeClass::_1DBuffer;
    case TextureType::_2D:
    case TextureType::_2DArray:
    case TextureType::Cube:
    case TextureType::CubeArray:
        return TextureTypeClass::_2D;
    case TextureType::_3D:
        return TextureTypeClass::_3D;
    }
}

bool legalizeTextureShape(TextureType& type, u32 width, u32 height, u32 depth,
                          u32 layer_count, u32& level_count) {
    if (layer_count > 1) {
        if (type == TextureType::_1D)
            type = TextureType::_1DArray;
        else if (type == TextureType::_2D)
            type = TextureType::_2DArray;
        else if (type == TextureType::Cube && layer_count > 6)
            type = TextureType::CubeArray;
    }

    constexpr u32 MAX_SIZE = 16384;
    constexpr u32 MAX_LAYERS = 2048;
    const bool is_cube =
        type == TextureType::Cube || type == TextureType::CubeArray;
    if (width == 0 || height == 0 || depth == 0 || layer_count == 0 ||
        width > MAX_SIZE || height > MAX_SIZE || depth > MAX_LAYERS ||
        layer_count > MAX_LAYERS ||
        (is_cube && (width != height || layer_count % 6 != 0)))
        return false;

    const u32 max_level_count = static_cast<u32>(std::bit_width(
        std::max({width, height, type == TextureType::_3D ? depth : 1u})));
    level_count = std::clamp(level_count, 1u, max_level_count);
    if (type == TextureType::_1D || type == TextureType::_1DArray ||
        type == TextureType::_1DBuffer)
        level_count = 1;
    return true;
}

namespace {

#define FORMAT(bytes_per_block, block_width, block_height, is_depth_stencil)   \
    TextureFormatInfo {                                                        \
        bytes_per_block, block_width, block_height, is_depth_stencil           \
    }
#define COLOR_FORMAT(bytes_per_block, block_width, block_height)               \
    FORMAT(bytes_per_block, block_width, block_height, false)
#define DEPTH_STENCIL_FORMAT(bytes_per_block, block_width, block_height)       \
    FORMAT(bytes_per_block, block_width, block_height, true)

TextureFormatInfo texture_format_infos[] = {
    FORMAT(0, 0, 0, false),        // Invalid
    COLOR_FORMAT(1, 1, 1),         // R8Unorm
    COLOR_FORMAT(1, 1, 1),         // R8Snorm
    COLOR_FORMAT(1, 1, 1),         // R8Uint
    COLOR_FORMAT(1, 1, 1),         // R8Sint
    COLOR_FORMAT(2, 1, 1),         // R16Float
    COLOR_FORMAT(2, 1, 1),         // R16Unorm
    COLOR_FORMAT(2, 1, 1),         // R16Snorm
    COLOR_FORMAT(2, 1, 1),         // R16Uint
    COLOR_FORMAT(2, 1, 1),         // R16Sint
    COLOR_FORMAT(4, 1, 1),         // R32Float
    COLOR_FORMAT(4, 1, 1),         // R32Uint
    COLOR_FORMAT(4, 1, 1),         // R32Sint
    COLOR_FORMAT(2, 1, 1),         // RG8Unorm
    COLOR_FORMAT(2, 1, 1),         // RG8Snorm
    COLOR_FORMAT(2, 1, 1),         // RG8Uint
    COLOR_FORMAT(2, 1, 1),         // RG8Sint
    COLOR_FORMAT(4, 1, 1),         // RG16Float
    COLOR_FORMAT(4, 1, 1),         // RG16Unorm
    COLOR_FORMAT(4, 1, 1),         // RG16Snorm
    COLOR_FORMAT(4, 1, 1),         // RG16Uint
    COLOR_FORMAT(4, 1, 1),         // RG16Sint
    COLOR_FORMAT(8, 1, 1),         // RG32Float
    COLOR_FORMAT(8, 1, 1),         // RG32Uint
    COLOR_FORMAT(8, 1, 1),         // RG32Sint
    COLOR_FORMAT(12, 1, 1),        // RGB32Float
    COLOR_FORMAT(12, 1, 1),        // RGB32Uint
    COLOR_FORMAT(12, 1, 1),        // RGB32Sint
    COLOR_FORMAT(4, 1, 1),         // RGBA8Unorm
    COLOR_FORMAT(4, 1, 1),         // RGBA8Snorm
    COLOR_FORMAT(4, 1, 1),         // RGBA8Uint
    COLOR_FORMAT(4, 1, 1),         // RGBA8Sint
    COLOR_FORMAT(8, 1, 1),         // RGBA16Float
    COLOR_FORMAT(8, 1, 1),         // RGBA16Unorm
    COLOR_FORMAT(8, 1, 1),         // RGBA16Snorm
    COLOR_FORMAT(8, 1, 1),         // RGBA16Uint
    COLOR_FORMAT(8, 1, 1),         // RGBA16Sint
    COLOR_FORMAT(16, 1, 1),        // RGBA32Float
    COLOR_FORMAT(16, 1, 1),        // RGBA32Uint
    COLOR_FORMAT(16, 1, 1),        // RGBA32Sint
    DEPTH_STENCIL_FORMAT(1, 1, 1), // S8Uint
    DEPTH_STENCIL_FORMAT(2, 1, 1), // Z16Unorm
    DEPTH_STENCIL_FORMAT(4, 1, 1), // Z24Unorm_X8Uint
    DEPTH_STENCIL_FORMAT(4, 1, 1), // Z32Float
    DEPTH_STENCIL_FORMAT(4, 1, 1), // Z24Unorm_S8Uint
    DEPTH_STENCIL_FORMAT(8, 1, 1), // Z32Float_X24S8Uint
    COLOR_FORMAT(4, 1, 1),         // RGBX8Unorm_sRGB
    COLOR_FORMAT(4, 1, 1),         // RGBA8Unorm_sRGB
    COLOR_FORMAT(2, 1, 1),         // RGBA4Unorm
    COLOR_FORMAT(2, 1, 1),         // RGB5Unorm
    COLOR_FORMAT(2, 1, 1),         // RGB5A1Unorm
    COLOR_FORMAT(2, 1, 1),         // R5G6B5Unorm
    COLOR_FORMAT(4, 1, 1),         // RGB10A2Unorm
    COLOR_FORMAT(4, 1, 1),         // RGB10A2Uint
    COLOR_FORMAT(4, 1, 1),         // RG11B10Float
    COLOR_FORMAT(4, 1, 1),         // E5BGR9Float
    COLOR_FORMAT(8, 4, 4),         // BC1_RGB
    COLOR_FORMAT(8, 4, 4),         // BC1_RGBA
    COLOR_FORMAT(16, 4, 4),        // BC2_RGBA
    COLOR_FORMAT(16, 4, 4),        // BC3_RGBA
    COLOR_FORMAT(8, 4, 4),         // BC1_RGB_sRGB
    COLOR_FORMAT(8, 4, 4),         // BC1_RGBA_sRGB
    COLOR_FORMAT(16, 4, 4),        // BC2_RGBA_sRGB
    COLOR_FORMAT(16, 4, 4),        // BC3_RGBA_sRGB
    COLOR_FORMAT(8, 4, 4),         // BC4_RUnorm
    COLOR_FORMAT(8, 4, 4),         // BC4_RSnorm
    COLOR_FORMAT(16, 4, 4),        // BC5_RGUnorm
    COLOR_FORMAT(16, 4, 4),        // BC5_RGSnorm
    COLOR_FORMAT(16, 4, 4),        // BC7_RGBAUnorm
    COLOR_FORMAT(16, 4, 4),        // BC7_RGBAUnorm_sRGB
    COLOR_FORMAT(16, 4, 4),        // BC6H_RGBA_SF16_Float
    COLOR_FORMAT(16, 4, 4),        // BC6H_RGBA_UF16_Float
    COLOR_FORMAT(4, 1, 1),         // RGBX8Unorm
    COLOR_FORMAT(4, 1, 1),         // RGBX8Snorm
    COLOR_FORMAT(4, 1, 1),         // RGBX8Uint
    COLOR_FORMAT(4, 1, 1),         // RGBX8Sint
    COLOR_FORMAT(8, 1, 1),         // RGBX16Float
    COLOR_FORMAT(8, 1, 1),         // RGBX16Unorm
    COLOR_FORMAT(8, 1, 1),         // RGBX16Snorm
    COLOR_FORMAT(8, 1, 1),         // RGBX16Uint
    COLOR_FORMAT(8, 1, 1),         // RGBX16Sint
    COLOR_FORMAT(16, 1, 1),        // RGBX32Float
    COLOR_FORMAT(16, 1, 1),        // RGBX32Uint
    COLOR_FORMAT(16, 1, 1),        // RGBX32Sint
    COLOR_FORMAT(16, 4, 4),        // ASTC_RGBA_4x4
    COLOR_FORMAT(16, 5, 4),        // ASTC_RGBA_5x4
    COLOR_FORMAT(16, 5, 5),        // ASTC_RGBA_5x5
    COLOR_FORMAT(16, 6, 5),        // ASTC_RGBA_6x5
    COLOR_FORMAT(16, 6, 6),        // ASTC_RGBA_6x6
    COLOR_FORMAT(16, 8, 5),        // ASTC_RGBA_8x5
    COLOR_FORMAT(16, 8, 6),        // ASTC_RGBA_8x6
    COLOR_FORMAT(16, 8, 8),        // ASTC_RGBA_8x8
    COLOR_FORMAT(16, 10, 5),       // ASTC_RGBA_10x5
    COLOR_FORMAT(16, 10, 6),       // ASTC_RGBA_10x6
    COLOR_FORMAT(16, 10, 8),       // ASTC_RGBA_10x8
    COLOR_FORMAT(16, 10, 10),      // ASTC_RGBA_10x10
    COLOR_FORMAT(16, 12, 10),      // ASTC_RGBA_12x10
    COLOR_FORMAT(16, 12, 12),      // ASTC_RGBA_12x12
    COLOR_FORMAT(16, 4, 4),        // ASTC_RGBA_4x4_sRGB
    COLOR_FORMAT(16, 5, 4),        // ASTC_RGBA_5x4_sRGB
    COLOR_FORMAT(16, 5, 5),        // ASTC_RGBA_5x5_sRGB
    COLOR_FORMAT(16, 6, 5),        // ASTC_RGBA_6x5_sRGB
    COLOR_FORMAT(16, 6, 6),        // ASTC_RGBA_6x6_sRGB
    COLOR_FORMAT(16, 8, 5),        // ASTC_RGBA_8x5_sRGB
    COLOR_FORMAT(16, 8, 6),        // ASTC_RGBA_8x6_sRGB
    COLOR_FORMAT(16, 8, 8),        // ASTC_RGBA_8x8_sRGB
    COLOR_FORMAT(16, 10, 5),       // ASTC_RGBA_10x5_sRGB
    COLOR_FORMAT(16, 10, 6),       // ASTC_RGBA_10x6_sRGB
    COLOR_FORMAT(16, 10, 8),       // ASTC_RGBA_10x8_sRGB
    COLOR_FORMAT(16, 10, 10),      // ASTC_RGBA_10x10_sRGB
    COLOR_FORMAT(16, 12, 10),      // ASTC_RGBA_12x10_sRGB
    COLOR_FORMAT(16, 12, 12),      // ASTC_RGBA_12x12_sRGB
    COLOR_FORMAT(2, 1, 1),         // B5G6R5Unorm
    COLOR_FORMAT(2, 1, 1),         // BGR5Unorm
    COLOR_FORMAT(2, 1, 1),         // BGR5A1Unorm
    COLOR_FORMAT(2, 1, 1),         // A1BGR5Unorm
    COLOR_FORMAT(4, 1, 1),         // BGRX8Unorm
    COLOR_FORMAT(4, 1, 1),         // BGRA8Unorm
    COLOR_FORMAT(4, 1, 1),         // BGRX8Unorm_sRGB
    COLOR_FORMAT(4, 1, 1),         // BGRA8Unorm_sRGB
    COLOR_FORMAT(8, 4, 4),         // ETC2_R_Unorm
    COLOR_FORMAT(8, 4, 4),         // ETC2_R_Snorm
    COLOR_FORMAT(16, 4, 4),        // ETC2_RG_Unorm
    COLOR_FORMAT(16, 4, 4),        // ETC2_RG_Snorm
    COLOR_FORMAT(8, 4, 4),         // ETC2_RGB
    COLOR_FORMAT(8, 4, 4),         // PTA_ETC2_RGB
    COLOR_FORMAT(16, 4, 4),        // ETC2_RGBA
    COLOR_FORMAT(8, 4, 4),         // ETC2_RGB_sRGB
    COLOR_FORMAT(8, 4, 4),         // PTA_ETC2_RGB_sRGB
    COLOR_FORMAT(16, 4, 4),        // ETC2_RGBA_sRGB
    COLOR_FORMAT(4, 1, 1),         // BGR10A2Unorm
    DEPTH_STENCIL_FORMAT(4, 1, 1), // S8Uint_Z24Unorm
    COLOR_FORMAT(2, 1, 1),         // R1G5B5A5Unorm
    COLOR_FORMAT(1, 1, 1),         // RG4Unorm (expanded to RG8 on the host)
};
static_assert(std::size(texture_format_infos) ==
              static_cast<usize>(TextureFormat::RG4Unorm) + 1);

#undef DEPTH_STENCIL_FORMAT
#undef COLOR_FORMAT
#undef FORMAT

} // namespace

const TextureFormatInfo& getTextureFormatInfo(TextureFormat format) {
    return texture_format_infos[static_cast<usize>(format)];
}

TextureFormat getHostTextureFormat(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGB32Float: return TextureFormat::RGBA32Float;
    case TextureFormat::RGB32Uint: return TextureFormat::RGBA32Uint;
    case TextureFormat::RGB32Sint: return TextureFormat::RGBA32Sint;
    case TextureFormat::RG4Unorm: return TextureFormat::RG8Unorm;
    default: return format;
    }
}

void expandTexturePixels(TextureFormat format, const u8* guest, u8* host,
                         usize pixel_count) {
    if (format == TextureFormat::RG4Unorm) {
        for (usize i = 0; i < pixel_count; ++i) {
            host[i * 2] = static_cast<u8>((guest[i] & 15) * 17);
            host[i * 2 + 1] = static_cast<u8>((guest[i] >> 4) * 17);
        }
    } else if (format == TextureFormat::RGB32Float ||
               format == TextureFormat::RGB32Uint ||
               format == TextureFormat::RGB32Sint) {
        const u32 alpha = format == TextureFormat::RGB32Float ? 0x3f800000u : 1u;
        for (usize i = 0; i < pixel_count; ++i) {
            std::memcpy(host + i * 16, guest + i * 12, 12);
            std::memcpy(host + i * 16 + 12, &alpha, sizeof(alpha));
        }
    } else {
        std::memcpy(host, guest,
                    pixel_count * getTextureFormatInfo(format).bytes_per_block);
    }
}

TextureFormat toTextureFormat(NvColorFormat color_format) {
#define NV_COLOR_FORMAT_CASE(color_format, texture_format)                     \
    case NvColorFormat::color_format:                                          \
        return TextureFormat::texture_format;

    // Formats of presented buffers. The names list the components from the
    // most significant bits, like the render target and TIC names, so each
    // entry matches the mapping of its counterpart there
    switch (color_format) {
        NV_COLOR_FORMAT_CASE(A8B8G8R8, RGBA8Unorm)
        NV_COLOR_FORMAT_CASE(A8B8G8R8_sRGB, RGBA8Unorm_sRGB)
        NV_COLOR_FORMAT_CASE(X8B8G8R8, RGBX8Unorm)
        NV_COLOR_FORMAT_CASE(X8B8G8R8_sRGB, RGBX8Unorm_sRGB)
        NV_COLOR_FORMAT_CASE(X8R8G8B8, BGRX8Unorm)
        NV_COLOR_FORMAT_CASE(R8G8B8, Invalid) // Doesn't work
        NV_COLOR_FORMAT_CASE(R5G6B5, B5G6R5Unorm)
        NV_COLOR_FORMAT_CASE(B5G6R5, R5G6B5Unorm)
        NV_COLOR_FORMAT_CASE(A8R8G8B8, BGRA8Unorm)
        NV_COLOR_FORMAT_CASE(R5G5B5A1, A1BGR5Unorm) // Doesn't work
        NV_COLOR_FORMAT_CASE(A4B4G4R4, RGBA4Unorm)
        NV_COLOR_FORMAT_CASE(A1B5G5R5, RGB5A1Unorm)
        NV_COLOR_FORMAT_CASE(A5B5G5R1, R1G5B5A5Unorm)
        NV_COLOR_FORMAT_CASE(A2B10G10R10, RGB10A2Unorm)
        NV_COLOR_FORMAT_CASE(A2R10G10B10, BGR10A2Unorm)
        NV_COLOR_FORMAT_CASE(A16B16G16R16, RGBA16Unorm)
        NV_COLOR_FORMAT_CASE(Float_A16B16G16R16, RGBA16Float)
        NV_COLOR_FORMAT_CASE(Float_B10G11R11, RG11B10Float)
        NV_COLOR_FORMAT_CASE(R8, R8Unorm)
        NV_COLOR_FORMAT_CASE(R8G8, RG8Unorm)
        NV_COLOR_FORMAT_CASE(R16, R16Unorm)
        NV_COLOR_FORMAT_CASE(R16G16, RG16Unorm)
        NV_COLOR_FORMAT_CASE(Float_R16, R16Float)
        NV_COLOR_FORMAT_CASE(Float_R16G16, RG16Float)
    default:
        LOG_NOT_IMPLEMENTED(Gpu, "NV color format {}", color_format);
        return TextureFormat::Invalid;
    }

#undef NV_COLOR_FORMAT_CASE
}

namespace {

// Components of a packed Z24 TIC format holding the depth and the stencil
std::optional<std::pair<ImageSwizzle, ImageSwizzle>>
getDepthStencilComponents(TextureFormat format) {
    switch (format) {
    case TextureFormat::S8Uint_Z24Unorm:
        return std::make_pair(ImageSwizzle::G, ImageSwizzle::R);
    case TextureFormat::Z24Unorm_S8Uint:
        return std::make_pair(ImageSwizzle::R, ImageSwizzle::G);
    default:
        return std::nullopt;
    }
}

} // namespace

TextureFormat toSampledTextureFormat(const ImageFormatWord image_format_word,
                                     bool is_srgb) {
    const auto format = toTextureFormat(image_format_word, is_srgb);
    const auto components = getDepthStencilComponents(format);
    if (!components)
        return format;

    const auto [depth, stencil] = *components;
    bool reads_stencil = false;
    for (const auto swizzle :
         {image_format_word.swizzle_x, image_format_word.swizzle_y,
          image_format_word.swizzle_z, image_format_word.swizzle_w}) {
        if (swizzle == depth)
            return format;
        if (swizzle == stencil)
            reads_stencil = true;
    }

    return reads_stencil ? TextureFormat::S8Uint : format;
}

SwizzleChannels toSampledSwizzleChannels(const ImageFormatWord image_format_word,
                                         bool is_srgb) {
    const auto view_format = toSampledTextureFormat(image_format_word, is_srgb);
    auto x = image_format_word.swizzle_x;
    auto y = image_format_word.swizzle_y;
    auto z = image_format_word.swizzle_z;
    auto w = image_format_word.swizzle_w;
    // Stencil views expose the stencil through R, Z24S8's is its G component
    if (view_format == TextureFormat::S8Uint &&
        toTextureFormat(image_format_word, is_srgb) ==
            TextureFormat::Z24Unorm_S8Uint) {
        for (auto* swizzle : {&x, &y, &z, &w}) {
            if (*swizzle == ImageSwizzle::G)
                *swizzle = ImageSwizzle::R;
        }
    }

    return SwizzleChannels(view_format, x, y, z, w);
}

namespace {

TextureFormat toTextureFormatUncached(const ImageFormatWord image_format_word,
                                      bool is_srgb);

} // namespace

// Resolved for every texture of every draw: results are cached by the fields
// the conversion reads
TextureFormat toTextureFormat(const ImageFormatWord image_format_word,
                              bool is_srgb) {
    static_assert(static_cast<u32>(TextureFormat::R1G5B5A5Unorm) < 0xff);
    static std::array<std::atomic<u8>, 1u << 20> cache{};
    const u32 key = static_cast<u32>(image_format_word.image_format) |
                    (static_cast<u32>(image_format_word.component_r) << 7) |
                    (static_cast<u32>(image_format_word.component_g) << 10) |
                    (static_cast<u32>(image_format_word.component_b) << 13) |
                    (static_cast<u32>(image_format_word.component_a) << 16) |
                    ((is_srgb ? 1u : 0u) << 19);
    // 0 is not cached yet, results are stored plus one
    const u8 cached = cache[key].load(std::memory_order_relaxed);
    if (cached != 0)
        return static_cast<TextureFormat>(cached - 1);

    const auto format = toTextureFormatUncached(image_format_word, is_srgb);
    cache[key].store(static_cast<u8>(static_cast<u32>(format) + 1),
                     std::memory_order_relaxed);
    return format;
}

namespace {

TextureFormat toTextureFormatUncached(const ImageFormatWord image_format_word,
                                      bool is_srgb) {
#define IMAGE_FORMAT_CASE_IMPL(img_format, c_r, c_g, c_b, c_a, texture_format, \
                               is_srgb_)                                       \
    else if (image_format_word.image_format == ImageFormat::img_format &&      \
             image_format_word.component_r == ImageComponent::c_r &&           \
             image_format_word.component_g == ImageComponent::c_g &&           \
             image_format_word.component_b == ImageComponent::c_b &&           \
             image_format_word.component_a == ImageComponent::c_a &&           \
             is_srgb == is_srgb_) {                                            \
        return TextureFormat::texture_format;                                  \
    }

#define IMAGE_FORMAT_CASE(img_format, c_r, c_g, c_b, c_a, texture_format)      \
    IMAGE_FORMAT_CASE_IMPL(img_format, c_r, c_g, c_b, c_a, texture_format,     \
                           false)
#define IMAGE_FORMAT_CASE_SRGB(img_format, c_r, c_g, c_b, c_a, texture_format) \
    IMAGE_FORMAT_CASE_IMPL(img_format, c_r, c_g, c_b, c_a, texture_format, true)

    // TODO: more formats
    // TODO: check
    if (image_format_word.image_format == ImageFormat::Invalid) {
        return TextureFormat::Invalid;
    }
    // DEBUG: bisect switch, remove with the other HYDRA_NO_* switches
    static const bool no_xbgr8 = std::getenv("HYDRA_NO_XBGR8") != nullptr;
    if (no_xbgr8 && image_format_word.image_format == ImageFormat::XBGR8)
        return TextureFormat::Invalid;
    IMAGE_FORMAT_CASE(R16, Float, Float, Float, Float, R16Float)
    IMAGE_FORMAT_CASE(R32, Float, Float, Float, Float, R32Float)
    IMAGE_FORMAT_CASE(ARGB8, Unorm, Unorm, Unorm, Unorm, RGBA8Unorm)
    IMAGE_FORMAT_CASE_SRGB(ARGB8, Unorm, Unorm, Unorm, Unorm, RGBA8Unorm_sRGB)
    // X8B8G8R8: ARGB8 above is NVIDIA's A8B8G8R8 (red in the low byte)
    IMAGE_FORMAT_CASE(XBGR8, Unorm, Unorm, Unorm, Unorm, RGBX8Unorm)
    IMAGE_FORMAT_CASE_SRGB(XBGR8, Unorm, Unorm, Unorm, Unorm,
                           RGBX8Unorm_sRGB)
    IMAGE_FORMAT_CASE(R8, Unorm, Unorm, Unorm, Unorm, R8Unorm)
    IMAGE_FORMAT_CASE(R16, Unorm, Unorm, Unorm, Unorm, R16Unorm)
    IMAGE_FORMAT_CASE(GR8, Unorm, Unorm, Unorm, Unorm, RG8Unorm)
    IMAGE_FORMAT_CASE(GR8, Snorm, Snorm, Snorm, Snorm, RG8Snorm)
    IMAGE_FORMAT_CASE(RG16, Unorm, Unorm, Unorm, Unorm, RG16Unorm)
    IMAGE_FORMAT_CASE(RG16, Snorm, Snorm, Snorm, Snorm, RG16Snorm)
    IMAGE_FORMAT_CASE(RG16, Uint, Uint, Uint, Uint, RG16Uint)
    IMAGE_FORMAT_CASE(RG16, Sint, Sint, Sint, Sint, RG16Sint)
    IMAGE_FORMAT_CASE(RG16, Float, Float, Float, Float, RG16Float)
    IMAGE_FORMAT_CASE(DXT1, Unorm, Unorm, Unorm, Unorm, BC1_RGB)
    IMAGE_FORMAT_CASE_SRGB(DXT1, Unorm, Unorm, Unorm, Unorm, BC1_RGB_sRGB)
    IMAGE_FORMAT_CASE(DXT23, Unorm, Unorm, Unorm, Unorm, BC2_RGBA)
    IMAGE_FORMAT_CASE_SRGB(DXT23, Unorm, Unorm, Unorm, Unorm, BC2_RGBA_sRGB)
    IMAGE_FORMAT_CASE(DXT45, Unorm, Unorm, Unorm, Unorm, BC3_RGBA)
    IMAGE_FORMAT_CASE_SRGB(DXT45, Unorm, Unorm, Unorm, Unorm, BC3_RGBA_sRGB)
    IMAGE_FORMAT_CASE(DXN2, Unorm, Unorm, Unorm, Unorm, BC5_RGUnorm)
    IMAGE_FORMAT_CASE(DXN2, Snorm, Snorm, Snorm, Snorm, BC5_RGSnorm)
    // TIC names list the components from the most significant bits
    IMAGE_FORMAT_CASE(B5G6R5, Unorm, Unorm, Unorm, Unorm, R5G6B5Unorm)
    IMAGE_FORMAT_CASE(ABGR4, Unorm, Unorm, Unorm, Unorm, RGBA4Unorm)
    IMAGE_FORMAT_CASE(A1BGR5, Unorm, Unorm, Unorm, Unorm, RGB5A1Unorm)
    IMAGE_FORMAT_CASE(ABG5R1, Unorm, Unorm, Unorm, Unorm, R1G5B5A5Unorm)
    IMAGE_FORMAT_CASE(B10GR11Float, Float, Float, Float, Float, RG11B10Float)
    IMAGE_FORMAT_CASE(E5BGR9_SHAREDEXP, Float, Float, Float, Float,
                      E5BGR9Float)
    IMAGE_FORMAT_CASE(A2BGR10, Unorm, Unorm, Unorm, Unorm, RGB10A2Unorm)
    IMAGE_FORMAT_CASE(A2BGR10, Uint, Uint, Uint, Uint, RGB10A2Uint)
    IMAGE_FORMAT_CASE(GR8, Uint, Uint, Uint, Uint, RG8Uint)
    IMAGE_FORMAT_CASE(GR8, Sint, Sint, Sint, Sint, RG8Sint)
    IMAGE_FORMAT_CASE(DXN1, Snorm, Snorm, Snorm, Snorm, BC4_RSnorm)
    IMAGE_FORMAT_CASE_SRGB(BC7U, Unorm, Unorm, Unorm, Unorm,
                           BC7_RGBAUnorm_sRGB)
    IMAGE_FORMAT_CASE(BC6H_SF16, Float, Float, Float, Float,
                      BC6H_RGBA_SF16_Float)
    IMAGE_FORMAT_CASE(BC6H_UF16, Float, Float, Float, Float,
                      BC6H_RGBA_UF16_Float)
    IMAGE_FORMAT_CASE(ETC2_RGB, Unorm, Unorm, Unorm, Unorm, ETC2_RGB)
    IMAGE_FORMAT_CASE_SRGB(ETC2_RGB, Unorm, Unorm, Unorm, Unorm, ETC2_RGB_sRGB)
    IMAGE_FORMAT_CASE(ETC2_RGB_PTA, Unorm, Unorm, Unorm, Unorm, PTA_ETC2_RGB)
    IMAGE_FORMAT_CASE_SRGB(ETC2_RGB_PTA, Unorm, Unorm, Unorm, Unorm,
                           PTA_ETC2_RGB_sRGB)
    IMAGE_FORMAT_CASE(ETC2_RGBA, Unorm, Unorm, Unorm, Unorm, ETC2_RGBA)
    IMAGE_FORMAT_CASE_SRGB(ETC2_RGBA, Unorm, Unorm, Unorm, Unorm,
                           ETC2_RGBA_sRGB)
    IMAGE_FORMAT_CASE(EAC, Unorm, Unorm, Unorm, Unorm, ETC2_R_Unorm)
    IMAGE_FORMAT_CASE(EAC, Snorm, Snorm, Snorm, Snorm, ETC2_R_Snorm)
    IMAGE_FORMAT_CASE(EACX2, Unorm, Unorm, Unorm, Unorm, ETC2_RG_Unorm)
    IMAGE_FORMAT_CASE(EACX2, Snorm, Snorm, Snorm, Snorm, ETC2_RG_Snorm)
    IMAGE_FORMAT_CASE(ASTC_2D_4X4, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_4x4)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_4X4, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_4x4_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_5X4, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_5x4)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_5X4, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_5x4_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_5X5, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_5x5)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_5X5, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_5x5_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_6X5, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_6x5)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_6X5, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_6x5_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_6X6, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_6x6)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_6X6, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_6x6_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_8X5, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_8x5)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_8X5, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_8x5_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_8X6, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_8x6)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_8X6, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_8x6_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_8X8, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_8x8)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_8X8, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_8x8_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_10X5, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_10x5)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_10X5, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_10x5_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_10X6, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_10x6)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_10X6, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_10x6_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_10X8, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_10x8)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_10X8, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_10x8_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_10X10, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_10x10)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_10X10, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_10x10_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_12X10, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_12x10)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_12X10, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_12x10_sRGB)
    IMAGE_FORMAT_CASE(ASTC_2D_12X12, Unorm, Unorm, Unorm, Unorm, ASTC_RGBA_12x12)
    IMAGE_FORMAT_CASE_SRGB(ASTC_2D_12X12, Unorm, Unorm, Unorm, Unorm,
                           ASTC_RGBA_12x12_sRGB)
    IMAGE_FORMAT_CASE(DXN1, Unorm, Unorm, Unorm, Unorm, BC4_RUnorm)
    // G24R8 is the uncompressed sampling alias of a Z24S8 surface. Both keep
    // the stencil in R (the low byte) and the depth in G, like the Z24S8
    // render target format they alias.
    IMAGE_FORMAT_CASE(G24R8, Uint, Unorm, Unorm, Unorm, S8Uint_Z24Unorm)
    IMAGE_FORMAT_CASE(Z24S8, Uint, Unorm, Unorm, Unorm, S8Uint_Z24Unorm)
    IMAGE_FORMAT_CASE(Z24S8, Uint, Unorm, Uint, Uint, S8Uint_Z24Unorm)
    IMAGE_FORMAT_CASE(S8Z24, Unorm, Uint, Uint, Uint, Z24Unorm_S8Uint)
    IMAGE_FORMAT_CASE(X8Z24, Unorm, Unorm, Unorm, Unorm, Z24Unorm_X8Uint)
    IMAGE_FORMAT_CASE(X8Z24, Unorm, Uint, Uint, Uint, Z24Unorm_X8Uint)
    IMAGE_FORMAT_CASE(Z16, Unorm, Unorm, Unorm, Unorm, Z16Unorm)
    IMAGE_FORMAT_CASE(Z16, Unorm, Uint, Uint, Uint, Z16Unorm)
    IMAGE_FORMAT_CASE(ZF32, Float, Float, Float, Float, Z32Float)
    IMAGE_FORMAT_CASE(ZF32, Float, Uint, Uint, Uint, Z32Float)
    IMAGE_FORMAT_CASE(ZF32_X24S8, Float, Uint, Unorm, Unorm, Z32Float_X24S8Uint)
    IMAGE_FORMAT_CASE(R32B24G8, Float, Uint, Unorm, Unorm, Z32Float_X24S8Uint)
    IMAGE_FORMAT_CASE(RGBA16, Float, Float, Float, Float, RGBA16Float)
    IMAGE_FORMAT_CASE(RGBA32, Float, Float, Float, Float, RGBA32Float)
    IMAGE_FORMAT_CASE(BC7U, Unorm, Unorm, Unorm, Unorm, BC7_RGBAUnorm)
    IMAGE_FORMAT_CASE(R8, Snorm, Snorm, Snorm, Snorm, R8Snorm)
    IMAGE_FORMAT_CASE(R8, Uint, Uint, Uint, Uint, R8Uint)
    IMAGE_FORMAT_CASE(R8, Sint, Sint, Sint, Sint, R8Sint)
    IMAGE_FORMAT_CASE(R16, Float, Float, Float, Float, R16Float)
    IMAGE_FORMAT_CASE(R16, Snorm, Snorm, Snorm, Snorm, R16Snorm)
    IMAGE_FORMAT_CASE(R16, Uint, Uint, Uint, Uint, R16Uint)
    IMAGE_FORMAT_CASE(R16, Sint, Sint, Sint, Sint, R16Sint)
    IMAGE_FORMAT_CASE(R32, Uint, Uint, Uint, Uint, R32Uint)
    IMAGE_FORMAT_CASE(R32, Sint, Sint, Sint, Sint, R32Sint)
    IMAGE_FORMAT_CASE(RGB32, Float, Float, Float, Float, RGB32Float)
    IMAGE_FORMAT_CASE(RGB32, Uint, Uint, Uint, Uint, RGB32Uint)
    IMAGE_FORMAT_CASE(RGB32, Sint, Sint, Sint, Sint, RGB32Sint)
    IMAGE_FORMAT_CASE(GR4, Unorm, Unorm, Unorm, Unorm, RG4Unorm)
    IMAGE_FORMAT_CASE(R32G32, Float, Float, Float, Float, RG32Float)
    IMAGE_FORMAT_CASE(R32G32, Uint, Uint, Uint, Uint, RG32Uint)
    IMAGE_FORMAT_CASE(R32G32, Sint, Sint, Sint, Sint, RG32Sint)
    IMAGE_FORMAT_CASE(ARGB8, Snorm, Snorm, Snorm, Snorm, RGBA8Snorm)
    IMAGE_FORMAT_CASE(ARGB8, Uint, Uint, Uint, Uint, RGBA8Uint)
    IMAGE_FORMAT_CASE(ARGB8, Sint, Sint, Sint, Sint, RGBA8Sint)
    IMAGE_FORMAT_CASE(RGBA16, Unorm, Unorm, Unorm, Unorm, RGBA16Unorm)
    IMAGE_FORMAT_CASE(RGBA16, Snorm, Snorm, Snorm, Snorm, RGBA16Snorm)
    IMAGE_FORMAT_CASE(RGBA16, Uint, Uint, Uint, Uint, RGBA16Uint)
    IMAGE_FORMAT_CASE(RGBA16, Sint, Sint, Sint, Sint, RGBA16Sint)
    IMAGE_FORMAT_CASE(RGBA32, Uint, Uint, Uint, Uint, RGBA32Uint)
    IMAGE_FORMAT_CASE(RGBA32, Sint, Sint, Sint, Sint, RGBA32Sint)
    else {
        // Usually a garbage descriptor; real hardware does not stop either
        static std::atomic<u32> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 32)
            LOG_ERROR(Gpu,
                      "Image format {}, components: {}, {}, {}, {}, sRGB: {}",
                      image_format_word.image_format,
                      image_format_word.component_r,
                      image_format_word.component_g,
                      image_format_word.component_b,
                      image_format_word.component_a, is_srgb);
        return TextureFormat::Invalid;
    }

#undef IMAGE_FORMAT_CASE
}

} // namespace

TextureFormat toTextureFormat(ColorSurfaceFormat color_surface_format) {
#define COLOR_SURFACE_FORMAT_CASE(color_surface_format, texture_format)        \
    case ColorSurfaceFormat::color_surface_format:                             \
        return TextureFormat::texture_format;

    switch (color_surface_format) {
        COLOR_SURFACE_FORMAT_CASE(Invalid, Invalid)
        COLOR_SURFACE_FORMAT_CASE(Bitmap, Invalid)
        COLOR_SURFACE_FORMAT_CASE(Unknown1D, Invalid)
        COLOR_SURFACE_FORMAT_CASE(RGBA32Float, RGBA32Float)
        COLOR_SURFACE_FORMAT_CASE(RGBA32Sint, RGBA32Sint)
        COLOR_SURFACE_FORMAT_CASE(RGBA32Uint, RGBA32Uint)
        COLOR_SURFACE_FORMAT_CASE(RGBX32Float, RGBX32Float)
        COLOR_SURFACE_FORMAT_CASE(RGBX32Sint, RGBX32Sint)
        COLOR_SURFACE_FORMAT_CASE(RGBX32Uint, RGBX32Uint)
        COLOR_SURFACE_FORMAT_CASE(RGBA16Unorm, RGBA16Unorm)
        COLOR_SURFACE_FORMAT_CASE(RGBA16Snorm, RGBA16Snorm)
        COLOR_SURFACE_FORMAT_CASE(RGBA16Sint, RGBA16Sint)
        COLOR_SURFACE_FORMAT_CASE(RGBA16Uint, RGBA16Uint)
        COLOR_SURFACE_FORMAT_CASE(RGBA16Float, RGBA16Float)
        COLOR_SURFACE_FORMAT_CASE(RG32Float, RG32Float)
        COLOR_SURFACE_FORMAT_CASE(RG32Sint, RG32Sint)
        COLOR_SURFACE_FORMAT_CASE(RG32Uint, RG32Uint)
        COLOR_SURFACE_FORMAT_CASE(RGBX16Float, RGBX16Float)
        COLOR_SURFACE_FORMAT_CASE(BGRA8Unorm, BGRA8Unorm)
        COLOR_SURFACE_FORMAT_CASE(BGRA8Unorm_sRGB, BGRA8Unorm_sRGB)
        COLOR_SURFACE_FORMAT_CASE(RGB10A2Unorm, RGB10A2Unorm)
        COLOR_SURFACE_FORMAT_CASE(RGB10A2Uint, RGB10A2Uint)
        COLOR_SURFACE_FORMAT_CASE(RGBA8Unorm, RGBA8Unorm)
        COLOR_SURFACE_FORMAT_CASE(RGBA8Unorm_sRGB, RGBA8Unorm_sRGB)
        COLOR_SURFACE_FORMAT_CASE(RGBA8Snorm, RGBA8Snorm)
        COLOR_SURFACE_FORMAT_CASE(RGBA8Sint, RGBA8Sint)
        COLOR_SURFACE_FORMAT_CASE(RGBA8Uint, RGBA8Uint)
        COLOR_SURFACE_FORMAT_CASE(RG16Unorm, RG16Unorm)
        COLOR_SURFACE_FORMAT_CASE(RG16Snorm, RG16Snorm)
        COLOR_SURFACE_FORMAT_CASE(RG16Sint, RG16Sint)
        COLOR_SURFACE_FORMAT_CASE(RG16Uint, RG16Uint)
        COLOR_SURFACE_FORMAT_CASE(RG16Float, RG16Float)
        COLOR_SURFACE_FORMAT_CASE(BGR10A2Unorm, BGR10A2Unorm)
        COLOR_SURFACE_FORMAT_CASE(RG11B10Float, RG11B10Float)
        COLOR_SURFACE_FORMAT_CASE(R32Sint, R32Sint)
        COLOR_SURFACE_FORMAT_CASE(R32Uint, R32Uint)
        COLOR_SURFACE_FORMAT_CASE(R32Float, R32Float)
        COLOR_SURFACE_FORMAT_CASE(BGRX8Unorm, BGRX8Unorm)
        COLOR_SURFACE_FORMAT_CASE(BGRX8Unorm_sRGB, BGRX8Unorm_sRGB)
        COLOR_SURFACE_FORMAT_CASE(B5G6R5Unorm, B5G6R5Unorm)
        COLOR_SURFACE_FORMAT_CASE(BGR5A1Unorm, BGR5A1Unorm)
        COLOR_SURFACE_FORMAT_CASE(RG8Unorm, RG8Unorm)
        COLOR_SURFACE_FORMAT_CASE(RG8Snorm, RG8Snorm)
        COLOR_SURFACE_FORMAT_CASE(RG8Sint, RG8Sint)
        COLOR_SURFACE_FORMAT_CASE(RG8Uint, RG8Uint)
        COLOR_SURFACE_FORMAT_CASE(R16Unorm, R16Unorm)
        COLOR_SURFACE_FORMAT_CASE(R16Snorm, R16Snorm)
        COLOR_SURFACE_FORMAT_CASE(R16Sint, R16Sint)
        COLOR_SURFACE_FORMAT_CASE(R16Uint, R16Uint)
        COLOR_SURFACE_FORMAT_CASE(R16Float, R16Float)
        COLOR_SURFACE_FORMAT_CASE(R8Unorm, R8Unorm)
        COLOR_SURFACE_FORMAT_CASE(R8Snorm, R8Snorm)
        COLOR_SURFACE_FORMAT_CASE(R8Sint, R8Sint)
        COLOR_SURFACE_FORMAT_CASE(R8Uint, R8Uint)
        COLOR_SURFACE_FORMAT_CASE(A8Unorm, Invalid) // TODO
        COLOR_SURFACE_FORMAT_CASE(BGR5X1Unorm, BGR5Unorm)
        COLOR_SURFACE_FORMAT_CASE(RGBX8Unorm, RGBX8Unorm)
        COLOR_SURFACE_FORMAT_CASE(RGBX8Unorm_sRGB, RGBX8Unorm_sRGB)
        // Z1R5G5B5/O1R5G5B5 and Z8R8G8B8/O8R8G8B8 write their unused bits as
        // zeros/ones, which only differs from the X formats in guest memory
        COLOR_SURFACE_FORMAT_CASE(BGR5X1UnormUnknownFB, BGR5Unorm)
        COLOR_SURFACE_FORMAT_CASE(BGR5X1UnormUnknownFC, BGR5Unorm)
        COLOR_SURFACE_FORMAT_CASE(BGRX8UnormUnknownFD, BGRX8Unorm)
        COLOR_SURFACE_FORMAT_CASE(BGRX8UnormUnknownFE, BGRX8Unorm)
        COLOR_SURFACE_FORMAT_CASE(Y32UintUnknownFF, Invalid)
    default:
        // Includes the alpha formats 0x40-0x47, which have no host equivalent
        ONCE(LOG_NOT_IMPLEMENTED(Gpu, "Color surface format {:#x}",
                                 static_cast<u32>(color_surface_format)));
        return TextureFormat::Invalid;
    }

#undef COLOR_SURFACE_FORMAT_CASE
}

TextureFormat toTextureFormat(DepthSurfaceFormat depth_surface_format) {
#define DEPTH_SURFACE_FORMAT_CASE(depth_surface_format, texture_format)        \
    case DepthSurfaceFormat::depth_surface_format:                             \
        return TextureFormat::texture_format;

    switch (depth_surface_format) {
        DEPTH_SURFACE_FORMAT_CASE(Invalid, Invalid)
        DEPTH_SURFACE_FORMAT_CASE(Z32Float, Z32Float)
        DEPTH_SURFACE_FORMAT_CASE(Z16Unorm, Z16Unorm)
        // Names list the components from the least significant bits: S8Z24
        // (NVIDIA's Z24S8) keeps the stencil in the low byte
        DEPTH_SURFACE_FORMAT_CASE(S8Z24Unorm, S8Uint_Z24Unorm)
        DEPTH_SURFACE_FORMAT_CASE(Z24X8Unorm, Z24Unorm_X8Uint)
        DEPTH_SURFACE_FORMAT_CASE(Z24S8Unorm, Z24Unorm_S8Uint)
        DEPTH_SURFACE_FORMAT_CASE(S8Uint, S8Uint)
        DEPTH_SURFACE_FORMAT_CASE(Z24C8Unorm, Invalid)
        DEPTH_SURFACE_FORMAT_CASE(Z32S8X24Float, Z32Float_X24S8Uint)
        DEPTH_SURFACE_FORMAT_CASE(Z24X8S8C8X16Unorm, Invalid)
        DEPTH_SURFACE_FORMAT_CASE(Z32X8C8X16Float, Invalid)
        DEPTH_SURFACE_FORMAT_CASE(Z32S8C8X16Float, Invalid)
    default:
        ONCE(LOG_NOT_IMPLEMENTED(Gpu, "Depth surface format {:#x}",
                                 static_cast<u32>(depth_surface_format)));
        return TextureFormat::Invalid;
    }

#undef DEPTH_SURFACE_FORMAT_CASE
}

u32 getTextureFormatStride(const TextureFormat format, u32 width) {
    const auto& info = getTextureFormatInfo(format);
    return ceilDivide(width, info.block_width) * info.bytes_per_block;
}

u32 getTextureFormatRows(const TextureFormat format, u32 height) {
    const auto& info = getTextureFormatInfo(format);
    return ceilDivide(height, info.block_height);
}

u32 getTextureFormatSliceStride(const TextureFormat format, u32 width,
                                u32 height) {
    return getTextureFormatRows(format, height) *
           getTextureFormatStride(format, width);
}

u32 getTextureFormatBpp(const TextureFormat format) {
    const auto& info = getTextureFormatInfo(format);
    ASSERT_DEBUG(info.block_width == 1 && info.block_height == 1, Gpu,
                 "BPP not supported for format {}", format);

    return info.bytes_per_block;
}

bool isTextureFormatCompressed(const TextureFormat format) {
    const auto& info = getTextureFormatInfo(format);
    return info.block_width != 1 || info.block_height != 1;
}

bool isTextureFormatDepthOrStencil(const TextureFormat format) {
    const auto& info = getTextureFormatInfo(format);
    return info.is_depth_stencil;
}

ColorDataType toColorDataType(ColorSurfaceFormat format) {
    switch (format) {
    case ColorSurfaceFormat::RGBA32Float:
    case ColorSurfaceFormat::RGBX32Float:
    case ColorSurfaceFormat::RGBA16Unorm:
    case ColorSurfaceFormat::RGBA16Snorm:
    case ColorSurfaceFormat::RGBA16Float:
    case ColorSurfaceFormat::RG32Float:
    case ColorSurfaceFormat::RGBX16Float:
    case ColorSurfaceFormat::BGRA8Unorm:
    case ColorSurfaceFormat::BGRA8Unorm_sRGB:
    case ColorSurfaceFormat::RGB10A2Unorm:
    case ColorSurfaceFormat::RGBA8Unorm:
    case ColorSurfaceFormat::RGBA8Unorm_sRGB:
    case ColorSurfaceFormat::RGBA8Snorm:
    case ColorSurfaceFormat::RG16Unorm:
    case ColorSurfaceFormat::RG16Snorm:
    case ColorSurfaceFormat::RG16Float:
    case ColorSurfaceFormat::BGR10A2Unorm:
    case ColorSurfaceFormat::RG11B10Float:
    case ColorSurfaceFormat::R32Float:
    case ColorSurfaceFormat::BGRX8Unorm:
    case ColorSurfaceFormat::BGRX8Unorm_sRGB:
    case ColorSurfaceFormat::B5G6R5Unorm:
    case ColorSurfaceFormat::BGR5A1Unorm:
    case ColorSurfaceFormat::RG8Unorm:
    case ColorSurfaceFormat::RG8Snorm:
    case ColorSurfaceFormat::R16Unorm:
    case ColorSurfaceFormat::R16Snorm:
    case ColorSurfaceFormat::R16Float:
    case ColorSurfaceFormat::R8Unorm:
    case ColorSurfaceFormat::R8Snorm:
    case ColorSurfaceFormat::A8Unorm:
    case ColorSurfaceFormat::BGR5X1Unorm:
    case ColorSurfaceFormat::RGBX8Unorm:
    case ColorSurfaceFormat::RGBX8Unorm_sRGB:
    case ColorSurfaceFormat::BGR5X1UnormUnknownFB:
    case ColorSurfaceFormat::BGR5X1UnormUnknownFC:
    case ColorSurfaceFormat::BGRX8UnormUnknownFD:
    case ColorSurfaceFormat::BGRX8UnormUnknownFE:
        return ColorDataType::Float;
    case ColorSurfaceFormat::RGBA32Sint:
    case ColorSurfaceFormat::RGBX32Sint:
    case ColorSurfaceFormat::RGBA16Sint:
    case ColorSurfaceFormat::RG32Sint:
    case ColorSurfaceFormat::RGBA8Sint:
    case ColorSurfaceFormat::RG16Sint:
    case ColorSurfaceFormat::R32Sint:
    case ColorSurfaceFormat::RG8Sint:
    case ColorSurfaceFormat::R16Sint:
    case ColorSurfaceFormat::R8Sint:
        return ColorDataType::Int;
    case ColorSurfaceFormat::RGBA32Uint:
    case ColorSurfaceFormat::RGBX32Uint:
    case ColorSurfaceFormat::RGBA16Uint:
    case ColorSurfaceFormat::RG32Uint:
    case ColorSurfaceFormat::RGB10A2Uint:
    case ColorSurfaceFormat::RGBA8Uint:
    case ColorSurfaceFormat::RG16Uint:
    case ColorSurfaceFormat::R32Uint:
    case ColorSurfaceFormat::RG8Uint:
    case ColorSurfaceFormat::R16Uint:
    case ColorSurfaceFormat::R8Uint:
        return ColorDataType::UInt;
    default:
        return ColorDataType::Invalid;
    }
}

// Guest components are remapped to host channels by the renderer (the host
// format of a packed TIC format can store the components in another order)
SwizzleChannels::SwizzleChannels(const TextureFormat format,
                                 const ImageSwizzle x, const ImageSwizzle y,
                                 const ImageSwizzle z, const ImageSwizzle w) {
    // The depth of S8Z24 is its G component, Metal exposes the depth plane
    // through R. Constant swizzles are preserved.
    const auto remap = [format](ImageSwizzle swizzle) {
        if (format == TextureFormat::S8Uint_Z24Unorm &&
            swizzle == ImageSwizzle::G)
            return ImageSwizzle::R;
        return swizzle;
    };
    r = remap(x);
    g = remap(y);
    b = remap(z);
    a = remap(w);
}

// Exact (not a hash): storages of one group can be viewed with each other's
// formats
u32 TextureDescriptor::getGroupHash() const {
    static_assert(static_cast<u32>(TextureFormat::R1G5B5A5Unorm) < 0x100);
    const auto& format_info = getTextureFormatInfo(format);
    // Equal block sizes do not imply compatible texture views: BC3, BC5,
    // BC7 and ASTC can share a footprint but use different encodings.
    // Keep compressed formats in separate storage groups. Uncompressed
    // formats retain their existing bit-compatible aliasing behaviour.
    const bool is_compressed =
        format_info.block_width > 1 || format_info.block_height > 1;
    u32 key = static_cast<u32>(getTextureTypeClass(type));
    key |= (is_compressed ? static_cast<u32>(format) : 0u) << 2;
    key |= format_info.bytes_per_block << 10;
    key |= format_info.block_width << 16;
    key |= format_info.block_height << 20;
    key |= (format_info.is_depth_stencil ? 1u : 0u) << 24;
    // Float depth and the packed Z24 formats have no common view format
    key |= (format == TextureFormat::Z32Float ? 1u : 0u) << 25;
    return key;
}

u32 TextureDescriptor::getStorageHash() const {
    ztd::hash::XxHash32 hash;
    hash.add(ptr);
    hash.add(is_linear);
    if (is_linear)
        hash.add(linear_stride);
    else {
        hash.add(block_width_gobs_log2);
        hash.add(block_height_gobs_log2);
        hash.add(block_depth_gobs_log2);
    }
    hash.add(width);
    hash.add(height);
    hash.add(depth);
    hash.add(level_count);
    hash.add(layer_count);
    hash.add(layer_size);

    return hash.toHashCode();
}

namespace {

u32 adjustDim(u32 dim, u32 level) { return std::max(dim >> level, 1u); }

} // namespace

uint3 TextureDescriptor::getLevelDimensions(u32 level) const {
    return uint3({adjustDim(width, level), adjustDim(height, level),
                  adjustDim(depth, level)});
}

namespace {

u32 adjustBlockSizeLog2(u32 size_log2, u32 gob_dim, u32 dim) {
    while (size_log2 != 0 && dim <= (gob_dim << (size_log2 - 1)))
        size_log2--;

    return size_log2;
}

} // namespace

uint3 TextureDescriptor::getLevelBlockSizeLog2(u32 level) const {
    const auto dims = getLevelDimensions(level);

    const u32 stride = getTextureFormatStride(format, dims.x());
    const u32 rows = getTextureFormatRows(format, dims.y());
    const u32 slices = dims.z();

    return uint3({adjustBlockSizeLog2(block_width_gobs_log2, GOB_WIDTH, stride),
                  adjustBlockSizeLog2(block_height_gobs_log2, GOB_HEIGHT, rows),
                  adjustBlockSizeLog2(block_depth_gobs_log2, 1, slices)});
}

u32 TextureDescriptor::getLevelOffset(u32 level) const {
    u32 offset = 0;
    for (u32 l = 0; l < level; l++)
        offset += getLevelSize(l);

    return offset;
}

u32 TextureDescriptor::getLevelSize(u32 level) const {
    const auto dims = getLevelDimensions(level);
    const auto block_size_log2 = getLevelBlockSizeLog2(level);

    const u32 block_width = GOB_WIDTH << block_size_log2.x();
    const u32 block_height = GOB_HEIGHT << block_size_log2.y();
    const u32 block_depth = 1u << block_size_log2.z();

    const u32 stride =
        align(getTextureFormatStride(format, dims.x()), block_width);
    const u32 rows =
        align(getTextureFormatRows(format, dims.y()), block_height);
    const u32 slices = align(dims.z(), block_depth);

    return slices * rows * stride;
}

namespace {

u32 alignLayerSize(u32 layer_size, u32 height, u32 depth, u32 block_height,
                   u32 block_height_gobs_log2, u32 block_depth_gobs_log2) {
    // Block rows, like getLevelBlockSizeLog2 (compressed formats have rows of
    // block_height texels)
    height = (height + block_height - 1) / block_height;
    while (block_height_gobs_log2 != 0 &&
           height <= 8U << (block_height_gobs_log2 - 1))
        --block_height_gobs_log2;

    while (block_depth_gobs_log2 != 0 &&
           depth <= 1U << (block_depth_gobs_log2 - 1))
        --block_depth_gobs_log2;

    u32 block_size = GOB_SIZE
                     << (block_height_gobs_log2 + block_depth_gobs_log2);
    u32 blocks = layer_size / block_size;

    if (layer_size != blocks * block_size)
        return (blocks + 1) * block_size;

    return layer_size;
}

} // namespace

void TextureDescriptor::calculateSize() {
    if (is_linear) {
        layer_size = 0;
        size = height * linear_stride;
    } else {
        const u32 layer_size_ = getLevelOffset(level_count);
        if (layer_count == 1) {
            layer_size = 0;
            size = layer_size_;
        } else {
            if (layer_size == 0) {
                layer_size = alignLayerSize(
                    layer_size_, height, depth,
                    getTextureFormatInfo(format).block_height,
                    block_height_gobs_log2, block_depth_gobs_log2);
            } else {
                // TODO: make sure the layer sizes match?
            }
            // The last layer only occupies its actual data, not the whole
            // layer stride. Otherwise a small mip level of a layered texture
            // (e.g. 1x1 of a cubemap with a large stride) would extend past
            // the end of the texture that owns it and never be merged into it
            // The last layer only occupies its actual data, not the whole
            // layer stride. Otherwise a small mip level of a layered texture
            // (e.g. 1x1 of a cubemap with a large stride) would extend past
            // the end of the texture that owns it and never be merged into it
            size = (layer_count - 1) * layer_size +
                   std::min(layer_size_, layer_size);
        }
    }
}

u32 TextureViewDescriptor::getHash() const {
    ztd::hash::XxHash32 hash;
    hash.add(type);
    hash.add(format);
    hash.add(levels.getBegin());
    hash.add(levels.getEnd());
    hash.add(layers.getBegin());
    hash.add(layers.getEnd());
    hash.add(swizzle_channels.r);
    hash.add(swizzle_channels.g);
    hash.add(swizzle_channels.b);
    hash.add(swizzle_channels.a);

    return hash.toHashCode();
}

usize getVertexFormatSize(engines::VertexAttribSize size) {
    switch (engines::getFetchSize(size)) {
    case engines::VertexAttribSize::_1x32:
        return 4;
    case engines::VertexAttribSize::_2x32:
        return 8;
    case engines::VertexAttribSize::_3x32:
        return 12;
    case engines::VertexAttribSize::_4x32:
        return 16;

    case engines::VertexAttribSize::_1x16:
        return 2;
    case engines::VertexAttribSize::_2x16:
        return 4;
    case engines::VertexAttribSize::_3x16:
        return 6;
    case engines::VertexAttribSize::_4x16:
        return 8;

    case engines::VertexAttribSize::_1x8:
        return 1;
    case engines::VertexAttribSize::_2x8:
        return 2;
    case engines::VertexAttribSize::_3x8:
        return 3;
    case engines::VertexAttribSize::_4x8:
        return 4;

    case engines::VertexAttribSize::_10_10_10_2:
    case engines::VertexAttribSize::_11_11_10:
        return 4;
    default:
        return 0;
    }
}

} // namespace hydra::hw::tegra_x1::gpu::renderer
