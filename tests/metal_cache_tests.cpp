#include "core/hw/tegra_x1/gpu/renderer/metal/binary_archive_cache.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace hydra::hw::tegra_x1::gpu::renderer::metal;

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    try {
        check(argc == 3, "Usage: hydra-metal-cache-tests <archive-path> "
                         "<cold|warm|changed|corrupt|disabled|unwritable>");
        const std::filesystem::path path(argv[1]);
        const std::string mode(argv[2]);
        std::filesystem::path blocked_entry;
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        NS_STACK_SCOPED auto device = MTL::CreateSystemDefaultDevice();
        check(device != nullptr, "No Metal GPU");

        if (mode == "cold") {
            check(!std::filesystem::exists(path),
                  "Cold test needs a new archive path");
        } else if (mode == "corrupt") {
            check(std::filesystem::is_directory(path),
                  "Corrupt test needs populated cache");
            for (const auto& entry :
                 std::filesystem::directory_iterator(path)) {
                std::ofstream file(entry.path(),
                                   std::ios::binary | std::ios::trunc);
                file << "invalid archive";
            }
        } else if (mode == "disabled") {
            setenv("HYDRA_DISABLE_SHADER_CACHE", "1", 1);
        } else if (mode == "write-failure") {
            for (const auto& entry :
                 std::filesystem::directory_iterator(path)) {
                if (blocked_entry.empty()) {
                    blocked_entry = entry.path();
                    std::filesystem::rename(blocked_entry,
                                            blocked_entry.string() + ".old");
                    std::filesystem::create_directory(blocked_entry);
                } else {
                    std::ofstream file(entry.path(),
                                       std::ios::binary | std::ios::trunc);
                    file << "invalid archive";
                }
            }
            check(!blocked_entry.empty(),
                  "Write-failure test needs populated cache");
        } else if (mode == "warm" || mode == "changed") {
            check(!std::filesystem::is_empty(path),
                  "Archive was not persisted");
        }

        BinaryArchiveCache cache(device, path);
        NS_STACK_SCOPED auto queue = device->newCommandQueue();
        const std::string source = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 v(uint id [[vertex_id]]) {
    const float2 p[] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(p[id], 0, 1);
}
fragment float4 red() { return float4(1, 0, 0, 1); }
fragment float4 green() { return float4(0, 1, 0, 1); }
fragment float4 blue() { return float4(0, 0, 1, 1); }
)";
        NS::Error* error = nullptr;
        NS_STACK_SCOPED auto library =
            device->newLibrary(toNsString(source), nullptr, &error);
        check(library != nullptr, "Test shader compilation failed");
        NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("v"));
        double total_ms = 0;
        const int count = mode == "changed" ? 3 : 2;
        for (int i = 0; i < count; i++) {
            const char* names[] = {"red", "green", "blue"};
            NS_STACK_SCOPED auto fragment =
                library->newFunction(toNsString(names[i]));
            NS_STACK_SCOPED auto descriptor =
                MTL::RenderPipelineDescriptor::alloc()->init();
            descriptor->setVertexFunction(vertex);
            descriptor->setFragmentFunction(fragment);
            if (mode == "state" || mode == "state-warm")
                descriptor->colorAttachments()->object(0)->setWriteMask(
                    MTL::ColorWriteMaskRed | MTL::ColorWriteMaskGreen |
                    MTL::ColorWriteMaskBlue);
            descriptor->colorAttachments()->object(0)->setPixelFormat(
                MTL::PixelFormatRGBA8Unorm);
            const auto start = std::chrono::steady_clock::now();
            NS_STACK_SCOPED auto pipeline =
                cache.createPipeline(descriptor, &error, source + names[i]);
            total_ms += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
            check(pipeline != nullptr, "Pipeline creation failed");

            NS_STACK_SCOPED auto texture_descriptor =
                MTL::TextureDescriptor::alloc()->init();
            texture_descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
            texture_descriptor->setWidth(4);
            texture_descriptor->setHeight(4);
            texture_descriptor->setStorageMode(MTL::StorageModeShared);
            texture_descriptor->setUsage(MTL::TextureUsageRenderTarget);
            NS_STACK_SCOPED auto texture =
                device->newTexture(texture_descriptor);
            check(texture != nullptr, "Test texture allocation failed");
            auto pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto attachment = pass->colorAttachments()->object(0);
            attachment->setTexture(texture);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setStoreAction(MTL::StoreActionStore);
            attachment->setClearColor(MTL::ClearColor(0, 0, 0, 0));
            auto command = queue->commandBuffer();
            auto encoder = command->renderCommandEncoder(pass);
            encoder->setRenderPipelineState(pipeline);
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                    NS::UInteger(3));
            encoder->endEncoding();
            command->commit();
            command->waitUntilCompleted();
            check(command->status() == MTL::CommandBufferStatusCompleted,
                  "Test render failed");
            uint8_t pixels[4 * 4 * 4]{};
            texture->getBytes(pixels, 4 * 4, MTL::Region::Make2D(0, 0, 4, 4),
                              0);
            for (int pixel = 0; pixel < 16; pixel++) {
                for (int channel = 0; channel < 4; channel++)
                    check(pixels[pixel * 4 + channel] ==
                              ((channel == i ||
                                (channel == 3 && mode != "state" &&
                                 mode != "state-warm"))
                                   ? 255
                                   : 0),
                          "Cached pipeline changed rendered pixels");
            }
        }
        cache.flush();
        const auto expected_hits =
            (mode == "warm" || mode == "changed" || mode == "state-warm") ? 2u
                                                                          : 0u;
        check(cache.getHits() == expected_hits,
              "Unexpected binary archive hits");
        check(cache.getMisses() == static_cast<unsigned>(count) - expected_hits,
              "Unexpected binary archive misses");
        if (mode != "disabled" && mode != "unwritable")
            check(!std::filesystem::is_empty(path), "Archive was not saved");
        if (!blocked_entry.empty()) {
            std::filesystem::remove(blocked_entry);
            std::filesystem::rename(blocked_entry.string() + ".old",
                                    blocked_entry);
        }
        if (mode == "disabled")
            check(!std::filesystem::exists(path), "Disabled cache wrote files");
        std::cout << "PASS " << mode << ": hits=" << cache.getHits()
                  << " misses=" << cache.getMisses()
                  << " pipeline_ms=" << total_ms << " pixels=identical\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
