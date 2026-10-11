#include "core/hw/tegra_x1/gpu/renderer/metal/shader_library_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/clear_color_pipeline_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/emitter.hpp"
#include <iostream>
#include <stdexcept>
using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu::renderer::metal;
class ConstantEmitter
    : public hydra::hw::tegra_x1::gpu::renderer::shader_decomp::codegen::lang::LangEmitter {
  public:
    using LangEmitter::getConstantStr;
};
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void testClearColor(MTL::Device* device) {
    ClearColorPipelineCache cache(device);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    const std::array formats{MTL::PixelFormatR32Uint, MTL::PixelFormatR32Sint,
                             MTL::PixelFormatR32Float};
    const std::array<u32, 3> values{0xdeadbeefu, 0xff123456u, 0x3f000000u};
    for (usize index = 0; index < formats.size(); ++index) {
        // Writing color attachment 1 exercises explicit output routing too.
        ClearColorPipelineDescriptor descriptor{};
        descriptor.pixel_formats[1] = formats[index];
        descriptor.render_target_id = 1;
        descriptor.mask = 1;
        auto* pipeline = cache.find(descriptor);
        check(pipeline != nullptr, "Integer/float clear pipeline compiles");
        NS_STACK_SCOPED auto texture_descriptor = MTL::TextureDescriptor::alloc()->init();
        texture_descriptor->setPixelFormat(formats[index]);
        texture_descriptor->setWidth(4);
        texture_descriptor->setHeight(4);
        texture_descriptor->setStorageMode(MTL::StorageModeShared);
        texture_descriptor->setUsage(MTL::TextureUsageRenderTarget);
        NS_STACK_SCOPED auto texture = device->newTexture(texture_descriptor);
        check(texture != nullptr, "Clear texture exists");
        auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* attachment = pass->colorAttachments()->object(1);
        attachment->setTexture(texture);
        attachment->setLoadAction(MTL::LoadActionClear);
        attachment->setStoreAction(MTL::StoreActionStore);
        auto* command = queue->commandBuffer();
        auto* encoder = command->renderCommandEncoder(pass);
        encoder->setRenderPipelineState(pipeline);
        const u32 layer = 0;
        const std::array<u32, 4> color{values[index], 0, 0, 0};
        encoder->setVertexBytes(&layer, sizeof(layer), 0);
        encoder->setFragmentBytes(color.data(), sizeof(color), 0);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        encoder->endEncoding();
        command->commit();
        command->waitUntilCompleted();
        check(command->status() == MTL::CommandBufferStatusCompleted, "Typed clear executes");
        std::array<u32, 16> pixels{};
        texture->getBytes(pixels.data(), 16, MTL::Region::Make2D(0, 0, 4, 4), 0);
        for (auto pixel : pixels)
            check(pixel == values[index], "Typed clear preserves raw color bits on attachment 1");
    }
}

int main() {
    try {
        unsetenv("HYDRA_DISABLE_SHADER_CACHE");
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        NS_STACK_SCOPED auto device = MTL::CreateSystemDefaultDevice();
        testClearColor(device);
        // Compile and execute the actual emitter's special float constants.
        // Check the bits on the GPU so signed infinity and NaN payloads survive.
        const std::array<u32, 4> special_bits{
            0x7f800000u, 0xff800000u, 0x7fc12345u, 0xffc12345u};
        std::string special_source =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void main_(device uint* output [[buffer(0)]]) {\n";
        for (usize i = 0; i < special_bits.size(); ++i)
            special_source += fmt::format(
                "output[{}] = as_type<uint>({});\n", i,
                ConstantEmitter::getConstantStr(std::bit_cast<f32>(special_bits[i])));
        special_source += "}\n";
        {
            NS::Error* error = nullptr;
            NS_STACK_SCOPED auto options = MTL::CompileOptions::alloc()->init();
            options->setFastMathEnabled(false);
            NS_STACK_SCOPED auto library = device->newLibrary(
                toNsString(special_source), options, &error);
            check(library != nullptr, "Special float constants compile as MSL");
            NS_STACK_SCOPED auto function = library->newFunction(toNsString("main_"));
            NS_STACK_SCOPED auto pipeline = device->newComputePipelineState(function, &error);
            check(pipeline != nullptr, "Special float pipeline is valid");
            NS_STACK_SCOPED auto output = device->newBuffer(
                sizeof(special_bits), MTL::ResourceStorageModeShared);
            NS_STACK_SCOPED auto queue = device->newCommandQueue();
            auto* commands = queue->commandBuffer();
            auto* encoder = commands->computeCommandEncoder();
            encoder->setComputePipelineState(pipeline);
            encoder->setBuffer(output, 0, 0);
            encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
            encoder->endEncoding();
            commands->commit();
            commands->waitUntilCompleted();
            check(std::memcmp(output->contents(), special_bits.data(), sizeof(special_bits)) == 0,
                  "GPU preserves special float constant bits");
        }
        const auto dir = std::filesystem::temp_directory_path() /
                         fmt::format("hydra-library-test-{}", getpid());
        std::filesystem::remove_all(dir);
        const std::string source = R"(#include <metal_stdlib>
using namespace metal;
kernel void main_(device uint* output [[buffer(0)]]) { output[0] = 123456; }
)";
        NS::Error* error = nullptr;
        {
            ShaderLibraryCache cache(device, dir);
            NS_STACK_SCOPED auto first = cache.get(source, &error);
            check(first != nullptr, "Cold shader compilation succeeds");
            cache.flush();
        }
        {
            ShaderLibraryCache cache(device, dir);
            cache.flush();
            NS_STACK_SCOPED auto library = cache.get(source, &error);
            check(library && cache.getHits() == 1,
                  "Second session uses prewarmed library");
            NS_STACK_SCOPED auto function =
                library->newFunction(toNsString("main_"));
            NS_STACK_SCOPED auto pipeline =
                device->newComputePipelineState(function, &error);
            check(pipeline != nullptr,
                  "Prewarmed shader creates real GPU pipeline");
            NS_STACK_SCOPED auto output =
                device->newBuffer(sizeof(u32), MTL::ResourceStorageModeShared);
            *static_cast<u32*>(output->contents()) = 0;
            NS_STACK_SCOPED auto queue = device->newCommandQueue();
            auto* commands = queue->commandBuffer();
            auto* encoder = commands->computeCommandEncoder();
            encoder->setComputePipelineState(pipeline);
            encoder->setBuffer(output, 0, 0);
            encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
            encoder->endEncoding();
            commands->commit();
            commands->waitUntilCompleted();
            check(*static_cast<u32*>(output->contents()) == 123456,
                  "Warm shader computes correct output");
            for (usize i = 0; i < 132; ++i) {
                NS_STACK_SCOPED auto variant = cache.get(
                    source + fmt::format("\n// variant {}", i), &error);
                check(variant != nullptr, "Variant compiles");
                if (i % 32 == 31)
                    cache.flush();
            }
            check(cache.getResidentCount() == ShaderLibraryCache::MAX_LIBRARIES,
                  "Resident library count stays bounded");
            cache.flush();
            NS_STACK_SCOPED auto bad =
                cache.get("invalid metal source", &error);
            check(bad == nullptr, "Invalid source remains a compile failure");
        }
        usize files = 0, bytes = 0;
        for (const auto& file : std::filesystem::directory_iterator(dir)) {
            if (file.path().extension() == ".metal") {
                ++files;
                bytes += file.file_size();
            }
        }
        check(files == 128 &&
                  bytes <= 64 * ShaderLibraryCache::MAX_SOURCE_BYTES,
              "Disk cache evicts old entries within count/byte limits");
        {
            ShaderLibraryCache cache(device, dir);
            cache.flush();
            NS_STACK_SCOPED auto newest =
                cache.get(source + "\n// variant 131", &error);
            check(newest && cache.getHits() == 1,
                  "Newest source survives disk eviction and prewarms");
        }
        std::filesystem::remove_all(dir);
        std::cout << "PASS: disk source persistence, warm-session GPU output, "
                     "resident limit, invalid shader\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
