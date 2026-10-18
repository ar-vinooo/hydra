// Guest colour-target formats 0x40-0x47 and A8: a guest fragment shader writes
// (r, g, b, a) through the production shader cache and pipeline into the Metal
// format selected for the guest format; the raw texel bytes are printed for
// tests/color_target_format_reference.py. Also prints the clear-colour swizzle.
//
// Input lines:  "R <format hex> <r> <g> <b> <a>"           -> texel bytes in hex
//               "C <format hex> <r> <g> <b> <a> <mask>"    -> clear colour words + mask
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/pipeline.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/shader.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using metal::stackAutoRelease;

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<u8> program(const std::array<u32, 4>& values) {
    std::vector<u64> words(10);
    words[0] = 3u << 5; // fragment stage
    words[9] = 0xf;     // RGBA output
    u32 instructions = 0;
    const auto emit = [&](u64 instruction) {
        if (instructions++ % 3 == 0) words.push_back(0); // scheduling word
        words.push_back(instruction);
    };
    for (u32 i = 0; i < 4; ++i)
        emit(0x010000000000f000ull | i | (u64(values[i]) << 20)); // MOV32I Ri
    emit(0xe30000000007000full); // EXIT
    std::vector<u8> bytes(0x10000);
    std::memcpy(bytes.data(), words.data(), words.size() * 8);
    return bytes;
}
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "usage: cases.txt results.txt");
        setenv("HYDRA_HOT_RELOAD", "0", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        Gpu gpu;
        auto& renderer = static_cast<metal::Renderer&>(gpu.getRenderer());
        auto& cache = renderer.getShaderCache();
        auto* device = renderer.getDevice();
        NS_STACK_SCOPED auto queue = device->newCommandQueue();
        ShaderDescriptor vertex_desc{};
        vertex_desc.type = ShaderType::Vertex;
        vertex_desc.backend = ShaderBackend::Msl;
        const std::string vertex_source = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 main_(uint i [[vertex_id]]) {
    return float4(i==1?3.0:-1.0, i==2?3.0:-1.0, 0, 1);
})";
        vertex_desc.code.assign(vertex_source.begin(), vertex_source.end());
        std::unique_ptr<ShaderBase> vertex(renderer.createShader(vertex_desc));
        std::vector<u8> code(0x10000);
        GMmu mmu(nullptr);
        constexpr uptr address = 0x20000;
        mmu.map(address, {reinterpret_cast<uptr>(code.data()), code.size()});
        tls_crnt_gmmu = &mmu;

        std::ifstream input(argv[1]);
        std::ofstream output(argv[2]);
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream row(line);
            std::string kind;
            u32 format_id;
            std::array<u32, 4> rgba{};
            row >> kind >> std::hex >> format_id;
            for (auto& value : rgba) row >> std::hex >> value;
            const auto format = static_cast<ColorSurfaceFormat>(format_id);
            const auto swizzle = toColorOutputSwizzle(format);
            if (kind == "C") {
                u32 mask_word;
                row >> std::hex >> mask_word;
                auto color = rgba;
                auto mask = static_cast<u8>(mask_word);
                swizzleClearColor(swizzle, color, mask);
                for (auto word : color) output << std::hex << word << ' ';
                output << std::hex << u32(mask) << '\n';
                continue;
            }
            const auto texture_format = toTextureFormat(format);
            check(texture_format != TextureFormat::Invalid, "Format has no Metal mapping");
            check(toColorDataType(format) == MTL::DataTypeFloat, "Format data type");
            const auto bytes_per_pixel = getTextureFormatInfo(texture_format).bytes_per_block;

            const auto bytes = program(rgba);
            std::copy(bytes.begin(), bytes.end(), code.begin());
            GuestShaderDescriptor descriptor{};
            descriptor.stage = engines::ShaderStage::Fragment;
            descriptor.gpu_addr = address;
            descriptor.code_ptr = reinterpret_cast<uptr>(code.data());
            descriptor.state.color_target_data_types[0] = toColorDataType(format);
            descriptor.state.color_target_swizzles[0] = static_cast<u8>(swizzle);
            auto* fragment = cache.find(descriptor);
            check(fragment != nullptr, "Missing shader");

            PipelineDescriptor pd{};
            pd.shaders[0] = vertex.get(); pd.shaders[1] = fragment;
            pd.topology_class = MTL::PrimitiveTopologyClassTriangle;
            pd.color_target_states[0].format = texture_format;
            pd.color_target_states[0].write_mask = engines::ColorWriteMask::All;
            auto* pipeline = static_cast<metal::Pipeline*>(renderer.getPipelineCache().find(pd));
            check(pipeline->getPipeline() != nullptr, "Pipeline compilation failed");

            auto* td = MTL::TextureDescriptor::texture2DDescriptor(
                metal::toMtlPixelFormat(texture_format), 1, 1, false);
            td->setStorageMode(MTL::StorageModeShared);
            td->setUsage(MTL::TextureUsageRenderTarget);
            NS_STACK_SCOPED auto texture = device->newTexture(td);
            auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* att = pass->colorAttachments()->object(0);
            att->setTexture(texture); att->setLoadAction(MTL::LoadActionClear);
            att->setClearColor(MTL::ClearColor::Make(0, 0, 0, 0));
            att->setStoreAction(MTL::StoreActionStore);
            auto* commands = queue->commandBuffer();
            auto* encoder = commands->renderCommandEncoder(pass);
            encoder->setRenderPipelineState(pipeline->getPipeline());
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
            encoder->endEncoding(); commands->commit(); commands->waitUntilCompleted();
            check(commands->status() == MTL::CommandBufferStatusCompleted, "Metal execution failed");
            std::array<u8, 16> pixel{};
            texture->getBytes(pixel.data(), 16, MTL::Region::Make2D(0, 0, 1, 1), 0);
            for (u32 i = 0; i < bytes_per_pixel; ++i) {
                char buffer[4];
                snprintf(buffer, sizeof(buffer), "%02x", pixel[i]);
                output << buffer;
            }
            output << '\n';
        }
        tls_crnt_gmmu = nullptr;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
