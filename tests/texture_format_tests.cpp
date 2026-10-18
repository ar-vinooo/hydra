#include "core/hw/tegra_x1/gpu/engines/3d.hpp"
#include "core/hw/tegra_x1/gpu/renderer/sampler_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::renderer;

void check(bool value, const char* message);

namespace {

void testSamplerDecoding() {
    engines::TextureSamplerControl tsc{};
    tsc.min_filter = tsc.mag_filter = 2;
    tsc.mip_filter = 3;
    tsc.address_u = 4;
    tsc.address_v = 6;
    tsc.max_lod_clamp = 3840;
    constexpr u32 values[] = {1,2,4,6,8,10,12,16};
    for (u32 i = 0; i < 8; ++i) {
        tsc.max_anisotropy = i;
        tsc.mip_lod_bias = 0x1e80;
        const auto descriptor = engines::decodeSampler(tsc);
        check(descriptor.lod_bias == -1.5f && descriptor.max_anisotropy == values[i],
              "TSC signed 4.8 bias and anisotropy table");
        check(descriptor.address_mode_s == SamplerAddressMode::ClampToBorder,
              "Linear CLAMP_OGL uses border filtering");
        check(metal::toMtlSamplerAddressMode(descriptor.address_mode_t) ==
                  MTL::SamplerAddressModeMirrorClampToEdge,
              "Mirror-once-border uses the reference fallback");
        auto other = descriptor;
        other.lod_bias = 1.5f;
        check(!SamplerCache::equals(descriptor, other) &&
                  SamplerCache::hash(descriptor) != SamplerCache::hash(other),
              "LOD bias participates in sampler cache identity");
        other = descriptor;
        other.max_anisotropy = 3;
        check(!SamplerCache::equals(descriptor, other), "Anisotropy participates in sampler cache identity");
    }
    tsc.min_filter = tsc.mag_filter = 1;
    check(engines::decodeSampler(tsc).address_mode_s == SamplerAddressMode::ClampToEdge,
          "Nearest CLAMP_OGL uses the edge");
    for (u32 i = 0; i < 8; ++i)
        check(metal::toMtlCompareFunc(static_cast<engines::CompareOp>(0x200 + i)) ==
                  metal::toMtlCompareFunc(static_cast<engines::CompareOp>(1 + i)),
              "GL and D3D depth comparisons are equivalent");
    std::cout << "PASS: sampler bias, anisotropy, wrap decoding and GL depth functions\n";
}

void testSamplerBorders() {
    constexpr u32 one = 0x3f800000u;
    engines::TextureSamplerControl tsc{};
    tsc.min_filter = tsc.mag_filter = 2;
    tsc.mip_filter = 3;
    tsc.address_u = tsc.address_v = tsc.address_p = 3;

    const auto set_border = [&](u32 r, u32 g, u32 b, u32 a) {
        tsc.border_color_r = r;
        tsc.border_color_g = g;
        tsc.border_color_b = b;
        tsc.border_color_a = a;
    };
    const auto preset = [&](TextureFormat format = TextureFormat::Invalid) {
        return getSamplerBorderPreset(engines::decodeSampler(tsc, format));
    };
    const auto emulated = [&](TextureFormat format = TextureFormat::Invalid) {
        return samplerNeedsBorderEmulation(engines::decodeSampler(tsc, format));
    };

    set_border(0, 0, 0, 0);
    check(preset() == SamplerBorderPreset::TransparentBlack && !emulated(),
          "Zero border uses the transparent preset");
    set_border(0, 0, 0, one);
    check(preset() == SamplerBorderPreset::OpaqueBlack && !emulated(),
          "Opaque black border uses the preset");
    set_border(one, one, one, one);
    check(preset() == SamplerBorderPreset::OpaqueWhite && !emulated(),
          "Opaque white border uses the preset");
    set_border(one, 0, 0, one);
    check(preset() == SamplerBorderPreset::Custom && emulated(),
          "A red border is not snapped to a preset");
    set_border(0, 0, std::bit_cast<u32>(0.99f), one);
    check(emulated(), "Near black borders are emulated exactly");

    // Integer textures hold raw bits: 1.0f is not the integer 1
    set_border(1, 1, 1, 1);
    check(emulated(TextureFormat::R32Uint) &&
              engines::decodeSampler(tsc, TextureFormat::R32Uint)
                      .border_color_u.x == 1u,
          "Integer borders keep their raw bits and are emulated");
    set_border(0, 0, 0, 0);
    check(!emulated(TextureFormat::R32Uint),
          "A zero integer border uses the transparent preset");
    set_border(0, 0, 0, one);
    check(emulated(TextureFormat::R32Uint),
          "Opaque black float bits are not an integer preset");
    const auto integer = engines::decodeSampler(tsc, TextureFormat::R32Uint);
    check(integer.min_filter == SamplerFilter::Nearest &&
              integer.mag_filter == SamplerFilter::Nearest &&
              integer.mip_filter == MTL::SamplerMipFilterNearest &&
              integer.integer_texture,
          "Integer textures sample with nearest filters");

    // Depth comparisons use the first component as the border depth
    tsc.depth_compare = true;
    tsc.depth_compare_op = 3;
    set_border(one, 0, 0, 0);
    check(preset() == SamplerBorderPreset::OpaqueWhite && !emulated(),
          "A border depth of 1 uses the white preset");
    set_border(std::bit_cast<u32>(0.5f), 0, 0, 0);
    check(emulated(), "A fractional border depth is emulated");
    tsc.depth_compare = false;

    // sRGB textures use the separate 8-bit border color
    tsc.srgb_conversion = 1;
    tsc.srgb_border_color_r = tsc.srgb_border_color_g =
        tsc.srgb_border_color_b = 188;
    set_border(std::bit_cast<u32>(0.5f), std::bit_cast<u32>(0.5f),
               std::bit_cast<u32>(0.5f), one);
    const auto srgb = engines::decodeSampler(tsc, TextureFormat::RGBA8Unorm_sRGB);
    const auto linear_188 = std::pow((188.0f / 255.0f + 0.055f) / 1.055f, 2.4f);
    check(std::abs(std::bit_cast<f32>(srgb.border_color_u.x) - linear_188) <
                  1e-5f &&
              srgb.border_color_u.w == one,
          "sRGB textures use the sRGB border color converted to linear");
    check(std::bit_cast<f32>(engines::decodeSampler(
                                 tsc, TextureFormat::RGBA8Unorm)
                                 .border_color_u.x) == 0.5f,
          "Non-sRGB textures use the float border color");
    tsc.srgb_conversion = 0;
    check(std::bit_cast<f32>(engines::decodeSampler(
                                 tsc, TextureFormat::RGBA8Unorm_sRGB)
                                 .border_color_u.x) == 0.5f,
          "Disabled sRGB conversion keeps the float border color");

    // Metal cannot mirror to the border
    tsc.address_u = 6;
    set_border(0, 0, 0, 0);
    check(emulated(), "Mirror to border is emulated");
    tsc.address_u = tsc.address_v = tsc.address_p = 2;
    set_border(one, 0, 0, one);
    check(!emulated() &&
              engines::decodeSampler(tsc).border_color_u.x == 0u,
          "Samplers that never address the border ignore its color");
    std::cout << "PASS: sampler border presets, integer, depth and sRGB "
                 "borders\n";
}

void testTicViewLodRange() {
    engines::TextureSamplerControl tsc{};
    tsc.min_filter = tsc.mag_filter = 2;
    tsc.mip_filter = 3;
    tsc.min_lod_clamp = 0;
    tsc.max_lod_clamp = 15 * 256;
    auto descriptor = engines::decodeSampler(
        tsc, TextureFormat::Invalid, engines::TicLodRange{2, 4});
    check(descriptor.min_lod == 2.0f && descriptor.max_lod == 4.0f,
          "The TIC view range clamps the sampler LOD range");
    tsc.min_lod_clamp = 3 * 256;
    tsc.max_lod_clamp = 3 * 256 + 128;
    descriptor = engines::decodeSampler(
        tsc, TextureFormat::Invalid, engines::TicLodRange{2, 4});
    check(descriptor.min_lod == 3.0f && descriptor.max_lod == 3.5f,
          "A sampler range inside the view range is kept");
    tsc.min_lod_clamp = 0;
    tsc.max_lod_clamp = 256;
    descriptor = engines::decodeSampler(
        tsc, TextureFormat::Invalid, engines::TicLodRange{2, 4});
    check(descriptor.min_lod == 2.0f && descriptor.max_lod == 2.0f,
          "A sampler range below the view range clamps to the view minimum");

    engines::TextureImageControl tic{};
    tic.view_mip_min_level = 1;
    tic.view_mip_max_level = 5;
    tic.hdr_version = engines::TicHdrVersion::BlockLinear;
    auto range = engines::getTicLodRange(tic);
    check(range.min == 1 && range.max == 5, "Block linear TICs carry a view range");
    tic.hdr_version = engines::TicHdrVersion::Pitch;
    range = engines::getTicLodRange(tic);
    check(range.min == 1 && range.max == 5, "Pitch TICs carry a view range");
    tic.hdr_version = engines::TicHdrVersion::BlockLinearColorKey;
    range = engines::getTicLodRange(tic);
    check(range.min == 0 && range.max == 15,
          "Color key TICs hold the key where the view range would be");
    tic.hdr_version = engines::TicHdrVersion::BlockLinear;
    tic.view_mip_min_level = 6;
    tic.view_mip_max_level = 2;
    range = engines::getTicLodRange(tic);
    check(range.min == 0 && range.max == 15, "An inverted view range is ignored");
    std::cout << "PASS: TIC resource view LOD range\n";
}

ImageFormatWord makeTic(ImageFormat format, ImageComponent r, ImageComponent g,
                        ImageComponent b, ImageComponent a,
                        ImageSwizzle x = ImageSwizzle::R,
                        ImageSwizzle y = ImageSwizzle::G,
                        ImageSwizzle z = ImageSwizzle::B,
                        ImageSwizzle w = ImageSwizzle::A) {
    ImageFormatWord word{};
    word.image_format = format;
    word.component_r = r;
    word.component_g = g;
    word.component_b = b;
    word.component_a = a;
    word.swizzle_x = x;
    word.swizzle_y = y;
    word.swizzle_z = z;
    word.swizzle_w = w;
    return word;
}

ImageFormatWord makeTic(ImageFormat format, ImageComponent component) {
    return makeTic(format, component, component, component, component);
}

// Every host format a guest format converts to, found by sweeping the guest
// encodings
std::set<TextureFormat> collectGuestFormats() {
    std::set<TextureFormat> formats;
    for (u32 value = 0; value < 0x100; value++) {
        formats.insert(
            toTextureFormat(static_cast<ColorSurfaceFormat>(value)));
    }
    for (u32 value = 0; value < 0x20; value++) {
        formats.insert(
            toTextureFormat(static_cast<DepthSurfaceFormat>(value)));
    }
    for (u32 image_format = 0; image_format < 0x80; image_format++) {
        for (u32 r = 1; r <= 7; r++) {
            for (u32 g = 1; g <= 7; g++) {
                for (u32 b = 1; b <= 7; b++) {
                    for (u32 a = 1; a <= 7; a++) {
                        const auto word = makeTic(
                            static_cast<ImageFormat>(image_format),
                            static_cast<ImageComponent>(r),
                            static_cast<ImageComponent>(g),
                            static_cast<ImageComponent>(b),
                            static_cast<ImageComponent>(a));
                        formats.insert(toTextureFormat(word, false));
                        formats.insert(toTextureFormat(word, true));
                    }
                }
            }
        }
    }
    formats.erase(TextureFormat::Invalid);
    return formats;
}

void testSurfaceFormatConversions() {
    check(toTextureFormat(ColorSurfaceFormat::RGBA32Sint) ==
              TextureFormat::RGBA32Sint,
          "RGBA32 SINT render targets keep their format");
    // A2R10G10B10 keeps blue in the low bits
    check(toTextureFormat(ColorSurfaceFormat::BGR10A2Unorm) ==
              TextureFormat::BGR10A2Unorm,
          "A2R10G10B10 render targets are BGR10A2");
    check(toTextureFormat(ColorSurfaceFormat::BGR5X1Unorm) ==
              TextureFormat::BGR5Unorm,
          "X1R5G5B5 render targets are BGR5");
    check(toTextureFormat(ColorSurfaceFormat::BGRX8UnormUnknownFE) ==
              TextureFormat::BGRX8Unorm,
          "O8R8G8B8 render targets are BGRX8");
    for (u32 value = 0x40; value <= 0x47; value++) {
        check(toTextureFormat(static_cast<ColorSurfaceFormat>(value)) ==
                  TextureFormat::Invalid,
              "Alpha render target formats are reported as unsupported");
    }

    check(toTextureFormat(DepthSurfaceFormat::S8Z24Unorm) ==
              TextureFormat::S8Uint_Z24Unorm,
          "ZT Z24S8 keeps the stencil in the low byte");
    check(toTextureFormat(DepthSurfaceFormat::Z24X8Unorm) ==
              TextureFormat::Z24Unorm_X8Uint,
          "ZT X8Z24 is a depth-only Z24 format");
    check(toTextureFormat(DepthSurfaceFormat::Z24S8Unorm) ==
              TextureFormat::Z24Unorm_S8Uint,
          "ZT S8Z24 keeps the stencil in the high byte");
    check(toTextureFormat(DepthSurfaceFormat::Z24C8Unorm) ==
                  TextureFormat::Invalid &&
              toTextureFormat(DepthSurfaceFormat::Z24X8S8C8X16Unorm) ==
                  TextureFormat::Invalid &&
              toTextureFormat(static_cast<DepthSurfaceFormat>(0x1b)) ==
                  TextureFormat::Invalid,
          "Coverage and unknown depth formats are reported as unsupported");

    std::cout << "PASS: render target and depth target format conversions\n";
}

void testTicFormatConversions() {
    using C = ImageComponent;
    struct Case {
        ImageFormatWord word;
        bool is_srgb;
        TextureFormat expected;
    };
    const Case cases[] = {
        {makeTic(ImageFormat::A2BGR10, C::Uint), false,
         TextureFormat::RGB10A2Uint},
        {makeTic(ImageFormat::GR8, C::Uint), false, TextureFormat::RG8Uint},
        {makeTic(ImageFormat::GR8, C::Sint), false, TextureFormat::RG8Sint},
        {makeTic(ImageFormat::DXN1, C::Snorm), false,
         TextureFormat::BC4_RSnorm},
        {makeTic(ImageFormat::BC7U, C::Unorm), true,
         TextureFormat::BC7_RGBAUnorm_sRGB},
        {makeTic(ImageFormat::BC6H_SF16, C::Float), false,
         TextureFormat::BC6H_RGBA_SF16_Float},
        {makeTic(ImageFormat::BC6H_UF16, C::Float), false,
         TextureFormat::BC6H_RGBA_UF16_Float},
        {makeTic(ImageFormat::E5BGR9_SHAREDEXP, C::Float), false,
         TextureFormat::E5BGR9Float},
        {makeTic(ImageFormat::ETC2_RGB, C::Unorm), false,
         TextureFormat::ETC2_RGB},
        {makeTic(ImageFormat::ETC2_RGB, C::Unorm), true,
         TextureFormat::ETC2_RGB_sRGB},
        {makeTic(ImageFormat::ETC2_RGB_PTA, C::Unorm), false,
         TextureFormat::PTA_ETC2_RGB},
        {makeTic(ImageFormat::ETC2_RGB_PTA, C::Unorm), true,
         TextureFormat::PTA_ETC2_RGB_sRGB},
        {makeTic(ImageFormat::ETC2_RGBA, C::Unorm), false,
         TextureFormat::ETC2_RGBA},
        {makeTic(ImageFormat::ETC2_RGBA, C::Unorm), true,
         TextureFormat::ETC2_RGBA_sRGB},
        {makeTic(ImageFormat::EAC, C::Unorm), false,
         TextureFormat::ETC2_R_Unorm},
        {makeTic(ImageFormat::EAC, C::Snorm), false,
         TextureFormat::ETC2_R_Snorm},
        {makeTic(ImageFormat::EACX2, C::Unorm), false,
         TextureFormat::ETC2_RG_Unorm},
        {makeTic(ImageFormat::EACX2, C::Snorm), false,
         TextureFormat::ETC2_RG_Snorm},
        {makeTic(ImageFormat::Z24S8, C::Uint, C::Unorm, C::Unorm, C::Unorm),
         false, TextureFormat::S8Uint_Z24Unorm},
        {makeTic(ImageFormat::Z24S8, C::Uint, C::Unorm, C::Uint, C::Uint),
         false, TextureFormat::S8Uint_Z24Unorm},
        {makeTic(ImageFormat::G24R8, C::Uint, C::Unorm, C::Unorm, C::Unorm),
         false, TextureFormat::S8Uint_Z24Unorm},
        {makeTic(ImageFormat::S8Z24, C::Unorm, C::Uint, C::Uint, C::Uint),
         false, TextureFormat::Z24Unorm_S8Uint},
        {makeTic(ImageFormat::X8Z24, C::Unorm), false,
         TextureFormat::Z24Unorm_X8Uint},
        {makeTic(ImageFormat::X8Z24, C::Unorm, C::Uint, C::Uint, C::Uint),
         false, TextureFormat::Z24Unorm_X8Uint},
        {makeTic(ImageFormat::Z16, C::Unorm, C::Uint, C::Uint, C::Uint), false,
         TextureFormat::Z16Unorm},
        {makeTic(ImageFormat::ZF32, C::Float, C::Uint, C::Uint, C::Uint),
         false, TextureFormat::Z32Float},
        {makeTic(ImageFormat::R32B24G8, C::Float, C::Uint, C::Unorm, C::Unorm),
         false, TextureFormat::Z32Float_X24S8Uint},
        {makeTic(ImageFormat::A1BGR5, C::Unorm), false,
         TextureFormat::RGB5A1Unorm},
        {makeTic(ImageFormat::ABG5R1, C::Unorm), false,
         TextureFormat::R1G5B5A5Unorm},
        {makeTic(ImageFormat::B5G6R5, C::Unorm), false,
         TextureFormat::R5G6B5Unorm},
    };
    for (const auto& test_case : cases) {
        check(toTextureFormat(test_case.word, test_case.is_srgb) ==
                  test_case.expected,
              "TIC format converts to the expected host format");
    }

    // Stencil reads of S8Z24 go through its G component
    const auto s8z24_stencil =
        makeTic(ImageFormat::S8Z24, C::Unorm, C::Uint, C::Uint, C::Uint,
                ImageSwizzle::G, ImageSwizzle::G, ImageSwizzle::Zero,
                ImageSwizzle::OneInt);
    check(toSampledTextureFormat(s8z24_stencil, false) == TextureFormat::S8Uint,
          "S8Z24 TIC reading only G samples the stencil plane");
    const auto stencil_swizzle = toSampledSwizzleChannels(s8z24_stencil, false);
    check(stencil_swizzle.r == ImageSwizzle::R &&
              stencil_swizzle.g == ImageSwizzle::R &&
              stencil_swizzle.b == ImageSwizzle::Zero &&
              stencil_swizzle.a == ImageSwizzle::OneInt,
          "S8Z24 stencil reads are remapped to the stencil view's R");
    const auto s8z24_depth =
        makeTic(ImageFormat::S8Z24, C::Unorm, C::Uint, C::Uint, C::Uint,
                ImageSwizzle::R, ImageSwizzle::R, ImageSwizzle::R,
                ImageSwizzle::OneFloat);
    check(toSampledTextureFormat(s8z24_depth, false) ==
              TextureFormat::Z24Unorm_S8Uint,
          "S8Z24 TIC reading R samples the depth plane");

    std::cout << "PASS: TIC format table covers Eden's encodings\n";
}

void testHostFormatCoverage() {
    const auto formats = collectGuestFormats();
    check(formats.size() > 100, "Guest format sweep finds the format table");

    std::vector<u8> memory(1 << 16, 0);
    const auto ptr = reinterpret_cast<uptr>(memory.data());
    metal::Renderer renderer;
    auto& cache = renderer.getTextureCache();
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    u32 slot = 0;
    for (const auto format : formats) {
        const auto& pixel_format_info = metal::toMtlPixelFormatInfo(format);
        if (pixel_format_info.pixel_format == MTL::PixelFormatInvalid) {
            std::cerr << "No host format for " << fmt::format("{}", format)
                      << '\n';
            check(false, "Every guest format has a host format");
        }

        const auto& info = getTextureFormatInfo(format);
        const u32 width = info.block_width * 2;
        const u32 height = info.block_height * 2;
        const u32 stride = getTextureFormatStride(format, width);
        const TextureDescriptor descriptor(ptr + slot * 0x200,
                                           MTL::TextureType2D, format, true,
                                           stride, width, height, 1, 1, 1, 0,
                                           0, 0);
        slot++;
        auto* view = cache.find(commands.get(), descriptor, TextureUsage::Read);
        check(view != nullptr, "Every guest format creates a texture");
        auto* native = static_cast<metal::TextureView*>(view)->getTexture();
        check(native != nullptr &&
                  native->pixelFormat() == pixel_format_info.pixel_format,
              "Texture views of every guest format are valid");
    }
    commands.reset();

    check(metal::toMtlSamplerMinMagFilter(static_cast<SamplerFilter>(0)) ==
                  MTL::SamplerMinMagFilterNearest &&
              metal::toMtlSamplerMinMagFilter(SamplerFilter::Anisotropic) ==
                  MTL::SamplerMinMagFilterLinear &&
              metal::toMtlSamplerMinMagFilter(SamplerFilter::Vcaa8Tap) ==
                  MTL::SamplerMinMagFilterLinear &&
              metal::toMtlSwizzle(static_cast<ImageSwizzle>(1)) ==
                  MTL::TextureSwizzleZero,
          "Sampler and swizzle conversions handle every encoding");

    std::cout << "PASS: " << formats.size()
              << " guest formats have valid Metal textures\n";
}

template <typename T>
std::vector<std::array<T, 4>> readTexels(metal::Renderer& renderer,
                                         std::unique_ptr<ICommandBuffer>& commands,
                                         MTL::Texture* texture, u32 count) {
    auto* device = renderer.getDevice();
    const std::string type = std::is_same_v<T, f32> ? "float" : "uint";
    const std::string source =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void read_texels(texture2d<" +
        type + ", access::read> texture [[texture(0)]], device " + type +
        "4* result [[buffer(0)]], uint index [[thread_position_in_grid]]) {\n"
        "    result[index] = texture.read(uint2(index, 0));\n}\n";
    NS::Error* error = nullptr;
    auto* library = device->newLibrary(
        NS::String::string(source.c_str(), NS::UTF8StringEncoding), nullptr,
        &error);
    check(library != nullptr, "Texel read shader compiles");
    auto* function = library->newFunction(
        NS::String::string("read_texels", NS::UTF8StringEncoding));
    auto* pipeline = device->newComputePipelineState(function, &error);
    check(pipeline != nullptr, "Texel read pipeline compiles");
    auto* result = device->newBuffer(sizeof(T) * 4 * count,
                                     MTL::ResourceStorageModeShared);

    auto* encoder = static_cast<metal::CommandBuffer*>(commands.get())
                        ->getComputeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    encoder->setTexture(texture, 0);
    encoder->setBuffer(result, 0, 0);
    encoder->dispatchThreads(MTL::Size(count, 1, 1), MTL::Size(count, 1, 1));
    commands.reset(renderer.createCommandBuffer());

    std::vector<std::array<T, 4>> texels(count);
    std::memcpy(texels.data(), result->contents(), sizeof(T) * 4 * count);
    result->release();
    pipeline->release();
    function->release();
    library->release();
    return texels;
}

// The packed 16-bit TIC formats store their components in another order than
// their host formats: swizzles must select the host channel of each component
void testPackedFormatSwizzles() {
    struct PackedFormat {
        const char* name;
        ImageFormat format;
        // Texels setting a single component to its maximum (R, G, B, A)
        std::array<u16, 4> texels;
        bool has_alpha;
    };
    const PackedFormat packed_formats[] = {
        {"A1B5G5R5", ImageFormat::A1BGR5, {0x001f, 0x03e0, 0x7c00, 0x8000},
         true},
        {"A5B5G5R1", ImageFormat::ABG5R1, {0x0001, 0x003e, 0x07c0, 0xf800},
         true},
        {"A4B4G4R4", ImageFormat::ABGR4, {0x000f, 0x00f0, 0x0f00, 0xf000},
         true},
        {"B5G6R5", ImageFormat::B5G6R5, {0x001f, 0x07e0, 0xf800, 0x0000},
         false},
    };
    using S = ImageSwizzle;
    const std::array<std::array<S, 4>, 4> swizzles = {{
        {S::R, S::G, S::B, S::A},
        {S::R, S::R, S::R, S::OneFloat},
        {S::A, S::A, S::A, S::A},
        {S::B, S::G, S::R, S::Zero},
    }};

    std::vector<u8> memory(1 << 14, 0);
    const auto ptr = reinterpret_cast<uptr>(memory.data());
    metal::Renderer renderer;
    auto& cache = renderer.getTextureCache();
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    u32 slot = 0;
    for (const auto& packed : packed_formats) {
        const uptr address = ptr + slot++ * 0x100;
        std::memcpy(reinterpret_cast<void*>(address), packed.texels.data(),
                    sizeof(packed.texels));
        for (const auto& swizzle : swizzles) {
            const auto word =
                makeTic(packed.format, ImageComponent::Unorm,
                        ImageComponent::Unorm, ImageComponent::Unorm,
                        ImageComponent::Unorm, swizzle[0], swizzle[1],
                        swizzle[2], swizzle[3]);
            const auto format = toTextureFormat(word, false);
            check(format != TextureFormat::Invalid,
                  "Packed TIC format is supported");
            const TextureDescriptor descriptor(address, MTL::TextureType2D,
                                               format, true, 8, 4, 1, 1, 1, 1,
                                               0, 0, 0);
            auto* view = cache.find(
                commands.get(), descriptor,
                TextureViewDescriptor(MTL::TextureType2D,
                                      toSampledTextureFormat(word, false),
                                      ztd::Range<u32>(0, 1),
                                      ztd::Range<u32>(0, 1),
                                      toSampledSwizzleChannels(word, false)),
                TextureUsage::Read);
            const auto texels = readTexels<f32>(
                renderer, commands,
                static_cast<metal::TextureView*>(view)->getTexture(), 4);
            for (u32 texel = 0; texel < 4; texel++) {
                // Value of each guest component in this texel
                std::array<f32, 4> components = {0.0f, 0.0f, 0.0f,
                                                 packed.has_alpha ? 0.0f
                                                                  : 1.0f};
                if (texel < 3 || packed.has_alpha)
                    components[texel] = 1.0f;
                for (u32 channel = 0; channel < 4; channel++) {
                    f32 expected;
                    switch (swizzle[channel]) {
                    case S::Zero:
                        expected = 0.0f;
                        break;
                    case S::OneFloat:
                        expected = 1.0f;
                        break;
                    default:
                        expected = components[static_cast<u32>(
                                                  swizzle[channel]) -
                                              static_cast<u32>(S::R)];
                        break;
                    }
                    if (std::abs(texels[texel][channel] - expected) > 0.001f) {
                        std::cerr << packed.name << " texel " << texel
                                  << " channel " << channel << ": "
                                  << texels[texel][channel] << " (expected "
                                  << expected << ")\n";
                        check(false,
                              "Packed format swizzle selects guest components");
                    }
                }
            }
        }
    }
    std::cout << "PASS: packed 16-bit TIC formats with arbitrary swizzles\n";
}

// Guest memory of the two Z24 layouts uploads to the right depth and stencil
void testZ24UploadLayouts() {
    std::vector<u8> memory(1 << 14, 0);
    const auto ptr = reinterpret_cast<uptr>(memory.data());
    const u32 word = 0x80000001u;
    struct Layout {
        TextureFormat format;
        f32 depth;
        u32 stencil;
    };
    const Layout layouts[] = {
        {TextureFormat::S8Uint_Z24Unorm,
         static_cast<f32>(0x800000) / static_cast<f32>(0xffffff), 0x01},
        {TextureFormat::Z24Unorm_S8Uint, 1.0f / static_cast<f32>(0xffffff),
         0x80},
    };

    metal::Renderer renderer;
    auto& cache = renderer.getTextureCache();
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    u32 slot = 0;
    for (const auto& layout : layouts) {
        const uptr address = ptr + slot++ * 0x400;
        std::memcpy(reinterpret_cast<void*>(address), &word, sizeof(word));
        const TextureDescriptor descriptor(address, MTL::TextureType2D,
                                           layout.format, true, 4, 1, 1, 1, 1,
                                           1, 0, 0, 0);
        auto* depth_view = cache.find(
            commands.get(), descriptor,
            TextureViewDescriptor(MTL::TextureType2D, layout.format,
                                  ztd::Range<u32>(0, 1), ztd::Range<u32>(0, 1)),
            TextureUsage::Read);
        const auto depth = readTexels<f32>(
            renderer, commands,
            static_cast<metal::TextureView*>(depth_view)->getTexture(), 1);
        check(std::abs(depth[0][0] - layout.depth) < 1e-6f,
              "Z24 upload decodes the depth of its layout");

        auto* stencil_view = cache.find(
            commands.get(), descriptor,
            TextureViewDescriptor(MTL::TextureType2D, TextureFormat::S8Uint,
                                  ztd::Range<u32>(0, 1), ztd::Range<u32>(0, 1)),
            TextureUsage::Read);
        const auto stencil = readTexels<u32>(
            renderer, commands,
            static_cast<metal::TextureView*>(stencil_view)->getTexture(), 1);
        check(stencil[0][0] == layout.stencil,
              "Z24 upload decodes the stencil of its layout");
    }

    // Float depth cannot be viewed from the packed formats
    TextureDescriptor packed(ptr + 0x1000, MTL::TextureType2D,
                             TextureFormat::S8Uint_Z24Unorm, true, 16, 4, 4, 1,
                             1, 1, 0, 0, 0);
    auto* packed_view = cache.find(commands.get(), packed, TextureUsage::Read);
    auto float_depth = packed;
    float_depth.format = TextureFormat::Z32Float;
    auto* float_view =
        cache.find(commands.get(), float_depth, TextureUsage::Read);
    check(float_view->getBase() != packed_view->getBase() &&
              static_cast<metal::TextureView*>(float_view)->getTexture() !=
                  nullptr,
          "Float depth gets its own storage instead of an illegal view");
    commands.reset();
    std::cout << "PASS: Z24S8 and S8Z24 uploads decode their own layouts\n";
}

void testDepthStencilReadback() {
    std::vector<u8> memory(1 << 16, 0);
    const auto ptr = reinterpret_cast<uptr>(memory.data());
    struct Case {
        TextureFormat format;
        u32 bytes_per_pixel;
        uptr offset;
    };
    const Case cases[] = {
        {TextureFormat::Z24Unorm_S8Uint, 4, 0x0000},
        {TextureFormat::S8Uint_Z24Unorm, 4, 0x1000},
        {TextureFormat::Z24Unorm_X8Uint, 4, 0x2000},
        {TextureFormat::Z32Float_X24S8Uint, 8, 0x3000},
    };
    for (const auto& test_case : cases) {
        for (u32 i = 0; i < 16; i++) {
            if (test_case.bytes_per_pixel == 8) {
                const u32 words[]{
                    std::bit_cast<u32>(static_cast<f32>(i) / 15.0f),
                    static_cast<u32>(0x30 + i),
                };
                std::memcpy(memory.data() + test_case.offset + i * 8, words,
                            sizeof(words));
            } else {
                const u32 depth = (i * 0x111111u) & 0xffffffu;
                u32 word = depth | ((0x40u + i) << 24);
                if (test_case.format == TextureFormat::S8Uint_Z24Unorm)
                    word = (depth << 8) | (0x40u + i);
                if (test_case.format == TextureFormat::Z24Unorm_X8Uint)
                    word = depth;
                std::memcpy(memory.data() + test_case.offset + i * 4, &word,
                            sizeof(word));
            }
        }
    }

    metal::Renderer renderer;
    auto& cache = renderer.getTextureCache();
    for (const auto& test_case : cases) {
        std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
        const u32 stride = 4 * test_case.bytes_per_pixel;
        const TextureDescriptor descriptor(
            ptr + test_case.offset, MTL::TextureType2D, test_case.format, true,
            stride, 4, 4, 1, 1, 1, 0, 0, 0);
        auto* view = cache.find(commands.get(), descriptor, TextureUsage::Read);
        auto* native = renderer.getDevice()->newBuffer(
            16 + 16 * test_case.bytes_per_pixel,
            MTL::ResourceStorageModeShared);
        std::memset(native->contents(), 0xcd, native->length());
        {
            metal::Buffer result(native);
            result.copyFrom(commands.get(), view, MTL::Origin(0, 0, 0),
                            MTL::Size(4, 4, 1),
                            ztd::Range<u32>::fromSize(0, 1),
                            ztd::Range<u32>::fromSize(0, 1), 16);
            commands.reset();
            const auto* actual =
                static_cast<const u8*>(native->contents()) + 16;
            const auto* expected = memory.data() + test_case.offset;
            if (test_case.bytes_per_pixel == 8) {
                check(std::memcmp(actual, expected, 16 * 8) == 0,
                      "D32FS8 readback restores guest layout");
            } else {
                for (u32 i = 0; i < 16; i++) {
                    u32 actual_word;
                    u32 expected_word;
                    std::memcpy(&actual_word, actual + i * 4,
                                sizeof(actual_word));
                    std::memcpy(&expected_word, expected + i * 4,
                                sizeof(expected_word));
                    const bool stencil_low =
                        test_case.format == TextureFormat::S8Uint_Z24Unorm;
                    const u32 actual_depth =
                        stencil_low ? actual_word >> 8
                                    : actual_word & 0xffffffu;
                    const u32 expected_depth =
                        stencil_low ? expected_word >> 8
                                    : expected_word & 0xffffffu;
                    const u32 actual_stencil =
                        stencil_low ? actual_word & 0xffu : actual_word >> 24;
                    const u32 expected_stencil =
                        stencil_low ? expected_word & 0xffu
                                    : expected_word >> 24;
                    const u32 depth_difference =
                        actual_depth > expected_depth
                            ? actual_depth - expected_depth
                            : expected_depth - actual_depth;
                    if (depth_difference > 1)
                        std::cerr << "Z24 format "
                                  << static_cast<u32>(test_case.format)
                                  << " texel " << i << ": " << actual_depth
                                  << " expected " << expected_depth << "\n";
                    check(depth_difference <= 1,
                          "Z24 readback is within one normalized integer step");
                    check(actual_stencil == expected_stencil,
                          "Packed stencil readback restores its guest byte");
                }
            }
        }
    }
    std::cout << "PASS: packed depth/stencil texture readback\n";
}

// A2R10G10B10 render targets keep red in bits 20-29
void testBgr10A2RenderTarget() {
    std::vector<u8> memory(1 << 12, 0);
    const auto ptr = reinterpret_cast<uptr>(memory.data());
    metal::Renderer renderer;
    auto& cache = renderer.getTextureCache();
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    const TextureDescriptor descriptor(
        ptr, MTL::TextureType2D,
        toTextureFormat(ColorSurfaceFormat::BGR10A2Unorm), true, 16, 4, 1, 1,
        1, 1, 0, 0, 0);
    auto* view = cache.find(commands.get(), descriptor, TextureUsage::Write);
    auto* native =
        static_cast<metal::Texture*>(view->getBase())->getTexture();
    auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* attachment = pass->colorAttachments()->object(0);
    attachment->setTexture(native);
    attachment->setLoadAction(MTL::LoadActionClear);
    attachment->setStoreAction(MTL::StoreActionStore);
    attachment->setClearColor(MTL::ClearColor(1.0, 0.0, 0.0, 1.0));
    auto* metal_commands = static_cast<metal::CommandBuffer*>(commands.get());
    metal_commands->getRenderCommandEncoder(pass);
    auto* result = renderer.getDevice()->newBuffer(
        16, MTL::ResourceStorageModeShared);
    metal_commands->getBlitCommandEncoder()->copyFromTexture(
        native, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(4, 1, 1), result, 0, 16,
        16);
    commands.reset();
    u32 word;
    std::memcpy(&word, result->contents(), sizeof(word));
    result->release();
    check(word == 0xfff00000u, "A2R10G10B10 stores red in bits 20-29");
    std::cout << "PASS: A2R10G10B10 render target bit layout\n";
}

// Guest descriptors Metal rejects are adapted (or refused) before creation
void testTextureShapeLegalization() {
    struct Case {
        MTL::TextureType type;
        u32 width, height, depth, layers, levels;
        bool valid;
        MTL::TextureType expected_type;
        u32 expected_levels;
    };
    const Case cases[] = {
        {MTL::TextureType2D, 4, 4, 1, 3, 8, true, MTL::TextureType2DArray, 3},
        {MTL::TextureType1D, 8, 1, 1, 1, 3, true, MTL::TextureType1D, 1},
        {MTL::TextureType1D, 8, 1, 1, 2, 1, true, MTL::TextureType1DArray, 1},
        {MTL::TextureTypeCube, 8, 8, 1, 12, 4, true, MTL::TextureTypeCubeArray, 4},
        {MTL::TextureType3D, 4, 4, 16, 1, 8, true, MTL::TextureType3D, 5},
        {MTL::TextureTypeCube, 8, 4, 1, 6, 1, false, MTL::TextureTypeCube, 1},
        {MTL::TextureType2D, 32768, 4, 1, 1, 1, false, MTL::TextureType2D, 1},
    };

    std::vector<u8> memory(1 << 20, 0);
    const auto ptr = reinterpret_cast<uptr>(memory.data());
    metal::Renderer renderer;
    auto& cache = renderer.getTextureCache();
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    u32 slot = 0;
    for (const auto& test_case : cases) {
        auto type = test_case.type;
        u32 levels = test_case.levels;
        const bool valid = legalizeTextureShape(
            type, test_case.width, test_case.height, test_case.depth,
            test_case.layers, levels);
        check(valid == test_case.valid, "Texture shape validity");
        if (!valid)
            continue;
        check(type == test_case.expected_type &&
                  levels == test_case.expected_levels,
              "Texture shape is adapted to the host");

        const TextureDescriptor descriptor(
            ptr + slot++ * 0x20000, type, TextureFormat::RGBA8Unorm, false, 0,
            test_case.width, test_case.height, test_case.depth, levels,
            test_case.layers, 0, 0, 0);
        auto* view = cache.find(commands.get(), descriptor, TextureUsage::Read);
        auto* native = static_cast<metal::TextureView*>(view)->getTexture();
        check(native != nullptr &&
                  native->mipmapLevelCount() == test_case.expected_levels,
              "Adapted textures are created by Metal");
    }
    commands.reset();
    std::cout << "PASS: guest texture shapes are adapted to Metal limits\n";
}

// Storages are keyed by a 32-bit hash: two textures of one memory whose
// descriptors collide must not share a storage
// Formats the host cannot hold are expanded on upload: G4R4 to RG8 and RGB32 to
// RGBA32 with an opaque alpha
void testExpandedFormats() {
    using namespace hydra::hw::tegra_x1::gpu::renderer;
    check(getHostTextureFormat(TextureFormat::RG4Unorm) == TextureFormat::RG8Unorm,
           "G4R4 is stored as RG8 on the host");
    check(getHostTextureFormat(TextureFormat::RGB32Float) == TextureFormat::RGBA32Float &&
               getHostTextureFormat(TextureFormat::RGB32Uint) == TextureFormat::RGBA32Uint &&
               getHostTextureFormat(TextureFormat::RGB32Sint) == TextureFormat::RGBA32Sint,
           "RGB32 is stored as RGBA32 on the host");
    check(getHostTextureFormat(TextureFormat::RGBA8Unorm) == TextureFormat::RGBA8Unorm,
           "Other formats are not expanded");

    const std::array<u8, 3> g4r4{0x21, 0xf0, 0x0f};
    std::array<u8, 6> rg8{};
    expandTexturePixels(TextureFormat::RG4Unorm, g4r4.data(), rg8.data(), 3);
    // The low nibble is red and the high nibble green, both scaled to 8 bits
    check(rg8[0] == 0x11 && rg8[1] == 0x22 && rg8[2] == 0x00 &&
               rg8[3] == 0xff && rg8[4] == 0xff && rg8[5] == 0x00,
           "G4R4 expands each nibble to a full byte");

    const std::array<u32, 6> rgb{0x3f800000, 0x40000000, 0x40400000,
                                 0x00000001, 0x00000002, 0x00000003};
    std::array<u32, 8> rgba{};
    expandTexturePixels(TextureFormat::RGB32Float,
                        reinterpret_cast<const u8*>(rgb.data()),
                        reinterpret_cast<u8*>(rgba.data()), 2);
    check(rgba[0] == rgb[0] && rgba[1] == rgb[1] && rgba[2] == rgb[2] &&
               rgba[3] == 0x3f800000 && rgba[4] == rgb[3] && rgba[7] == 0x3f800000,
           "RGB32 float gains an alpha of 1.0");
    expandTexturePixels(TextureFormat::RGB32Uint,
                        reinterpret_cast<const u8*>(rgb.data()),
                        reinterpret_cast<u8*>(rgba.data()), 2);
    check(rgba[3] == 1 && rgba[7] == 1 && rgba[5] == rgb[4],
           "RGB32 integer formats gain an alpha of 1");
    std::cout << "PASS: G4R4 and RGB32 upload expansion\n";
}

void testStorageHashCollision() {
    // The hashes depend on the address: try a few until two collide
    std::vector<u8> memory(4096 * 256 * 4 + 64 * 0x100, 0);
    std::optional<std::pair<TextureDescriptor, TextureDescriptor>> collision;
    for (u32 offset = 0; offset < 64 && !collision; offset++) {
        const auto ptr = reinterpret_cast<uptr>(memory.data()) + offset * 0x100;
        const auto make = [ptr](u32 width, u32 height) {
            return TextureDescriptor(ptr, MTL::TextureType2D,
                                     TextureFormat::RGBA8Unorm, true,
                                     width * 4, width, height, 1, 1, 1, 0, 0,
                                     0);
        };
        std::map<u32, std::pair<u32, u32>> seen;
        for (u32 width = 1; width <= 4096 && !collision; width++) {
            for (u32 height = 1; height <= 256; height++) {
                const auto descriptor = make(width, height);
                const auto [it, inserted] = seen.emplace(
                    descriptor.getStorageHash(), std::make_pair(width, height));
                if (!inserted) {
                    collision.emplace(
                        make(it->second.first, it->second.second), descriptor);
                    break;
                }
            }
        }
    }
    check(collision.has_value(), "Found colliding storage descriptors");

    metal::Renderer renderer;
    auto& cache = renderer.getTextureCache();
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    const auto& [first, second] = *collision;
    auto* first_view = cache.find(commands.get(), first, TextureUsage::Read);
    auto* second_view = cache.find(commands.get(), second, TextureUsage::Read);
    check(first_view->getBase() != second_view->getBase(),
          "Colliding descriptors get separate storages");
    auto* second_native =
        static_cast<metal::TextureView*>(second_view)->getTexture();
    check(second_native->width() == second.width &&
              second_native->height() == second.height,
          "The second storage has its own dimensions");
    check(cache.find(commands.get(), first, TextureUsage::Read)->getBase() ==
                  first_view->getBase() &&
              cache.find(commands.get(), second, TextureUsage::Read)
                      ->getBase() == second_view->getBase(),
          "Colliding storages are found again");
    commands.reset();
    std::cout << "PASS: texture storages with colliding hashes ("
              << first.width << "x" << first.height << ", " << second.width
              << "x" << second.height << ") stay distinct\n";
}

// The 2D engine copies guest memory only for compressed aliases whose source
// guest memory is current
void testAliasQueries() {
    std::vector<u8> memory(1 << 16, 0);
    const auto ptr = reinterpret_cast<uptr>(memory.data());
    metal::Renderer renderer;
    auto& cache = renderer.getTextureCache();
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    std::scoped_lock lock(cache.getMutex());
    const TextureDescriptor compressed(ptr, MTL::TextureType2D,
                                       TextureFormat::BC1_RGBA, false, 0, 16,
                                       16, 1, 1, 1, 0, 0, 0);
    const TextureDescriptor target(ptr + 0x8000, MTL::TextureType2D,
                                   TextureFormat::RGBA32Float, false, 0, 4, 4,
                                   1, 1, 1, 0, 0, 0);
    cache.find(commands.get(), compressed, TextureUsage::Read);
    cache.find(commands.get(), target, TextureUsage::Write);
    check(cache.overlapsCompressedStorage(compressed.getRange()) &&
              !cache.overlapsCompressedStorage(target.getRange()),
          "Compressed storages are found by range");
    check(cache.hasUnflushedGpuWrites(target.getRange()) &&
              !cache.hasUnflushedGpuWrites(compressed.getRange()),
          "Rendered storages have unflushed GPU writes");
    cache.flushMemory(commands.get(), target.getRange());
    commands.reset();
    check(!cache.hasUnflushedGpuWrites(target.getRange()),
          "Flushed storages have no unflushed GPU writes");
    std::cout << "PASS: compressed alias and unflushed write queries\n";
}

// Copies between storages of different formats keep the bytes, without
// breaking Metal's equal format rule for blits
void testCrossFormatCopies() {
    std::vector<u8> memory(1 << 16, 0);
    const auto ptr = reinterpret_cast<uptr>(memory.data());
    for (u32 i = 0; i < 64; i++)
        memory[i] = static_cast<u8>(i * 7 + 1);
    const float depth[16] = {0.25f, 0.5f, 0.75f, 1.0f, 0.125f, 0.0f,
                             0.375f, 0.625f, 0.25f, 0.5f, 0.75f, 1.0f,
                             0.125f, 0.0f, 0.375f, 0.625f};
    std::memcpy(memory.data() + 0x1000, depth, sizeof(depth));

    metal::Renderer renderer;
    auto& cache = renderer.getTextureCache();
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    const auto make = [ptr](uptr offset, TextureFormat format) {
        return TextureDescriptor(ptr + offset, MTL::TextureType2D, format, true,
                                 16, 4, 4, 1, 1, 1, 0, 0, 0);
    };
    struct Case {
        uptr src_offset;
        TextureFormat src;
        TextureFormat dst;
    };
    const Case cases[] = {
        {0x0, TextureFormat::RGBA8Unorm, TextureFormat::R32Uint},
        {0x1000, TextureFormat::Z32Float, TextureFormat::R32Float},
    };
    u32 slot = 0;
    for (const auto& test_case : cases) {
        auto* src = cache.find(commands.get(), make(test_case.src_offset,
                                                    test_case.src),
                               TextureUsage::Read);
        auto* dst = cache.find(commands.get(),
                               make(0x4000 + slot++ * 0x1000, test_case.dst),
                               TextureUsage::Read);
        dst->getBase()->copyFrom(commands.get(), src->getBase(), 0, 0, 0, 0, 1,
                                 1);
        auto* native =
            static_cast<metal::Texture*>(dst->getBase())->getTexture();
        auto* result = renderer.getDevice()->newBuffer(
            64, MTL::ResourceStorageModeShared);
        static_cast<metal::CommandBuffer*>(commands.get())
            ->getBlitCommandEncoder()
            ->copyFromTexture(native, 0, 0, MTL::Origin(0, 0, 0),
                              MTL::Size(4, 4, 1), result, 0, 16, 64);
        commands.reset(renderer.createCommandBuffer());
        check(std::memcmp(result->contents(),
                          memory.data() + test_case.src_offset, 64) == 0,
              "Cross-format copies keep the source bytes");
        result->release();
    }
    commands.reset();
    std::cout << "PASS: cross-format texture copies through views and "
                 "buffers\n";
}

// NVN copies compressed blocks through RGBA32 pixels: both directions must
// preserve the 16 byte blocks even though a block covers 4x4 texels
void testBlockShapeCopies() {
    std::vector<u8> memory(1 << 16, 0);
    const auto ptr = reinterpret_cast<uptr>(memory.data());
    for (u32 i = 0; i < 256; i++) {
        memory[i] = static_cast<u8>(i * 5 + 3);
        memory[0x2000 + i] = static_cast<u8>(i * 11 + 7);
    }
    metal::Renderer renderer;
    auto& cache = renderer.getTextureCache();
    std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
    const auto texels = [ptr](uptr offset) {
        return TextureDescriptor(ptr + offset, MTL::TextureType2D,
                                 TextureFormat::RGBA32Uint, true, 64, 4, 4, 1,
                                 1, 1, 0, 0, 0);
    };
    const auto blocks = [ptr](uptr offset) {
        return TextureDescriptor(ptr + offset, MTL::TextureType2D,
                                 TextureFormat::BC3_RGBA, true, 64, 16, 16, 1,
                                 1, 1, 0, 0, 0);
    };
    struct Case {
        const char* name;
        uptr src_offset;
        bool src_is_blocks;
        uptr dst_offset;
    };
    const Case cases[] = {
        {"RGBA32 texels to BC3 blocks", 0x0, false, 0x4000},
        {"BC3 blocks to RGBA32 texels", 0x2000, true, 0x6000},
    };
    for (const auto& test_case : cases) {
        auto* src = cache.find(commands.get(),
                               test_case.src_is_blocks
                                   ? blocks(test_case.src_offset)
                                   : texels(test_case.src_offset),
                               TextureUsage::Read);
        auto* dst = cache.find(commands.get(),
                               test_case.src_is_blocks
                                   ? texels(test_case.dst_offset)
                                   : blocks(test_case.dst_offset),
                               TextureUsage::Read);
        dst->getBase()->copyFrom(commands.get(), src->getBase(), 0, 0, 0, 0, 1,
                                 1);
        auto* native =
            static_cast<metal::Texture*>(dst->getBase())->getTexture();
        auto* result = renderer.getDevice()->newBuffer(
            256, MTL::ResourceStorageModeShared);
        static_cast<metal::CommandBuffer*>(commands.get())
            ->getBlitCommandEncoder()
            ->copyFromTexture(native, 0, 0, MTL::Origin(0, 0, 0),
                              test_case.src_is_blocks ? MTL::Size(4, 4, 1)
                                                      : MTL::Size(16, 16, 1),
                              result, 0, 64, 256);
        commands.reset(renderer.createCommandBuffer());
        check(std::memcmp(result->contents(),
                          memory.data() + test_case.src_offset, 256) == 0,
              test_case.name);
        result->release();
    }
    commands.reset();
    std::cout << "PASS: copies between texels and compressed blocks of equal "
                 "size\n";
}

} // namespace

void runTextureFormatTests() {
    testBlockShapeCopies();
    testCrossFormatCopies();
    testSamplerDecoding();
    testSamplerBorders();
    testTicViewLodRange();
    testTextureShapeLegalization();
    testExpandedFormats();
    testStorageHashCollision();
    testAliasQueries();
    testSurfaceFormatConversions();
    testTicFormatConversions();
    testHostFormatCoverage();
    testPackedFormatSwizzles();
    testZ24UploadLayouts();
    testDepthStencilReadback();
    testBgr10A2RenderTarget();
}
