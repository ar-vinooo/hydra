// Guest TIC formats sampled through Hydra's texture cache and Metal. Raw guest
// texel bytes go in, the values Metal returns for texel reads through the
// guest swizzle come out; tests/texture_format_reference.py computes the
// expected values from the NVIDIA format names and compares.
//
// Input lines:
//   "M <image format hex> <cr> <cg> <cb> <ca> <srgb>"
//        -> host format name chosen by toTextureFormat ("invalid" if none)
//   "S <image format hex> <cr> <cg> <cb> <ca> <srgb> <sx> <sy> <sz> <sw>
//      <width> <height> <block-linear 0|1> <block width log2> <block height
//      log2> <levels> <layers> <hex bytes>"   (block sizes are in GOBs)
//        -> "<F|I|U> <words>" for every layer, then level: width*height texels
//        -> "<F|I|U> <w0..w(4*width-1)>" 32-bit words (float bits for F)
#include "core/hw/tegra_x1/gpu/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using metal::stackAutoRelease;

namespace {

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

ImageFormatWord makeWord(u32 format, u32 r, u32 g, u32 b, u32 a, u32 sx,
                         u32 sy, u32 sz, u32 sw) {
    ImageFormatWord word{};
    word.image_format = static_cast<ImageFormat>(format);
    word.component_r = static_cast<ImageComponent>(r);
    word.component_g = static_cast<ImageComponent>(g);
    word.component_b = static_cast<ImageComponent>(b);
    word.component_a = static_cast<ImageComponent>(a);
    word.swizzle_x = static_cast<ImageSwizzle>(sx);
    word.swizzle_y = static_cast<ImageSwizzle>(sy);
    word.swizzle_z = static_cast<ImageSwizzle>(sz);
    word.swizzle_w = static_cast<ImageSwizzle>(sw);
    return word;
}

std::vector<u8> parseHex(const std::string& text) {
    std::vector<u8> bytes;
    for (usize i = 0; i + 1 < text.size(); i += 2)
        bytes.push_back(static_cast<u8>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "usage: cases.txt results.txt");
        setenv("HYDRA_HOT_RELOAD", "0", 1);
        // Production command buffers are committed without waiting; the results
        // are read right after, so wait for the GPU
        setenv("HYDRA_GPU_SYNC", "1", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        metal::Renderer renderer;
        auto& cache = renderer.getTextureCache();
        auto* device = renderer.getDevice();
        std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());

        std::map<std::string, MTL::ComputePipelineState*> pipelines;
        const auto pipeline_for = [&](const std::string& type, bool array) {
            auto& pipeline = pipelines[type + (array ? "[]" : "")];
            if (pipeline)
                return pipeline;
            const std::string source =
                "#include <metal_stdlib>\nusing namespace metal;\n"
                "kernel void read_texels(" +
                std::string(array ? "texture2d_array<" : "texture2d<") + type +
                ", access::read> texture [[texture(0)]], device " + type +
                "4* result [[buffer(0)]], constant uint2& sub [[buffer(1)]], "
                "uint2 gid [[thread_position_in_grid]]) {\n"
                "    const uint w = max(texture.get_width(sub.x), 1u);\n"
                "    const uint h = max(texture.get_height(sub.x), 1u);\n"
                "    if (gid.x >= w || gid.y >= h) return;\n"
                + std::string(array ? "    result[gid.y * w + gid.x] = texture.read(gid, sub.y, sub.x);\n"
                                    : "    result[gid.y * w + gid.x] = texture.read(gid, sub.x);\n")
                + "}\n";
            NS::Error* error = nullptr;
            auto* library = device->newLibrary(
                NS::String::string(source.c_str(), NS::UTF8StringEncoding),
                nullptr, &error);
            check(library != nullptr, "Texel read shader compiles");
            auto* function = library->newFunction(
                NS::String::string("read_texels", NS::UTF8StringEncoding));
            pipeline = device->newComputePipelineState(function, &error);
            check(pipeline != nullptr, "Texel read pipeline compiles");
            return pipeline;
        };

        std::vector<u8> memory(128 << 20, 0);
        const auto base = reinterpret_cast<uptr>(memory.data());
        uptr next_offset = 0;

        std::ifstream input(argv[1]);
        std::ofstream output(argv[2]);
        std::string line;
        while (std::getline(input, line)) {
            if (std::getenv("TEXTURE_FORMAT_VERBOSE") != nullptr)
                std::cerr << line << std::endl;
            std::istringstream row(line);
            std::string kind;
            u32 fmt, r, g, b, a, srgb;
            row >> kind >> std::hex >> fmt >> r >> g >> b >> a >> srgb;
            if (kind == "M") {
                const auto word = makeWord(fmt, r, g, b, a, 2, 3, 4, 5);
                const auto format = toTextureFormat(word, srgb != 0);
                output << (format == TextureFormat::Invalid
                               ? std::string("invalid")
                               : fmt::format("{}", format))
                       << '\n';
                continue;
            }
            u32 sx, sy, sz, sw, width, height;
            std::string hex;
            u32 block_linear, block_w_log2, block_h_log2, levels, layers;
            row >> sx >> sy >> sz >> sw >> width >> height >> block_linear >>
                block_w_log2 >> block_h_log2 >> levels >> layers >> hex;
            const auto word = makeWord(fmt, r, g, b, a, sx, sy, sz, sw);
            const auto format = toTextureFormat(word, srgb != 0);
            check(format != TextureFormat::Invalid, "Format must be mapped");
            const auto view_format = toSampledTextureFormat(word, srgb != 0);
            const std::string name = fmt::format("{}", view_format);
            const auto ends_with = [&](const char* suffix) {
                const std::string s(suffix);
                return name.size() >= s.size() &&
                       name.compare(name.size() - s.size(), s.size(), s) == 0;
            };
            const char* type = ends_with("uint") ? "uint"
                               : ends_with("sint") ? "int" : "float";

            // Fresh guest memory for each case so nothing is cached
            const auto bytes = parseHex(hex);
            const uptr address = base + 0x1000 + next_offset;
            next_offset += (bytes.size() + 0x2fff) & ~uptr{0xfff};
            check(address + bytes.size() + 0x1000 < base + memory.size(),
                  "Out of guest memory");
            std::memcpy(reinterpret_cast<void*>(address), bytes.data(),
                        bytes.size());
            // One row of blocks (or texels) per stride
            const auto block_height = getTextureFormatInfo(format).block_height;
            const u32 rows = (height + block_height - 1) / block_height;
            const u32 stride = block_linear != 0
                                   ? 0u
                                   : static_cast<u32>(bytes.size() / rows);
            const bool array = layers > 1;
            const auto texture_type =
                array ? MTL::TextureType2DArray : MTL::TextureType2D;
            const TextureDescriptor descriptor(address, texture_type, format,
                                               block_linear == 0, stride, width,
                                               height, 1, levels, layers,
                                               block_w_log2, block_h_log2, 0);
            auto* view = cache.find(
                commands.get(), descriptor,
                TextureViewDescriptor(texture_type, view_format,
                                      ztd::Range<u32>(0, levels),
                                      ztd::Range<u32>(0, layers),
                                      toSampledSwizzleChannels(word, srgb != 0)),
                TextureUsage::Read);
            check(view != nullptr, "Texture view");

            if (std::getenv("TEXTURE_FORMAT_VERBOSE") != nullptr) {
                auto* native = static_cast<metal::TextureView*>(view)->getTexture();
                std::cerr << "texture " << native->width() << "x" << native->height()
                          << " format " << static_cast<int>(native->pixelFormat())
                          << " expected " << width << "x" << height << std::endl;
            }
            u64 total_texels = 0;
            for (u32 level = 0; level < levels; ++level)
                total_texels += u64{std::max(width >> level, 1u)} *
                                std::max(height >> level, 1u);
            total_texels *= layers;
            auto* result = device->newBuffer(16ull * total_texels,
                                             MTL::ResourceStorageModeShared);
            auto* encoder = static_cast<metal::CommandBuffer*>(commands.get())
                                ->getComputeCommandEncoder();
            encoder->setComputePipelineState(pipeline_for(type, array));
            encoder->setTexture(
                static_cast<metal::TextureView*>(view)->getTexture(), 0);
            u64 offset = 0;
            for (u32 layer = 0; layer < layers; ++layer) {
                for (u32 level = 0; level < levels; ++level) {
                    const u32 w = std::max(width >> level, 1u);
                    const u32 h = std::max(height >> level, 1u);
                    const simd::uint2 sub{level, layer};
                    encoder->setBuffer(result, offset * 16, 0);
                    encoder->setBytes(&sub, sizeof(sub), 1);
                    encoder->dispatchThreads(MTL::Size(w, h, 1), MTL::Size(8, 8, 1));
                    offset += u64{w} * h;
                }
            }
            commands.reset(renderer.createCommandBuffer());

            output << (type[0] == 'f' ? 'F' : type[0] == 'i' ? 'I' : 'U');
            const auto* words = static_cast<const u32*>(result->contents());
            for (u64 i = 0; i < 4 * total_texels; ++i)
                output << ' ' << std::hex << words[i];
            output << std::dec << '\n';
            result->release();
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
