#include "core/hw/tegra_x1/gpu/renderer/metal/binary_archive_cache.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/gpu_pass_profiler.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/occlusion.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/pipeline.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/shader.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/shader_library_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/spatial_upscaler.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/analyzer/memory_analyzer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/msl/emitter.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/ir/builder.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

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
        if (mode == "many-samplers") {
            using namespace hydra;
            using namespace hydra::hw::tegra_x1::gpu::renderer;
            namespace decomp = shader_decomp;
            decomp::ir::Module module;
            decomp::ir::Builder builder(module);
            const auto coords = builder.opVectorConstruct(
                decomp::ir::ScalarType::F32,
                {decomp::ir::Value::createConstantF(0.5f),
                 decomp::ir::Value::createConstantF(0.5f)});
            const auto undefined = decomp::ir::Value::createUndefined();
            for (u32 i = 0; i < 18; ++i)
                builder.opTextureSample(i, decomp::TextureType::_2D,
                                        decomp::TextureSampleFlags::Lod,
                                        undefined, coords, undefined,
                                        decomp::ir::Value::createConstantF(0));
            builder.opExit();
            decomp::analyzer::MemoryAnalyzer analyzer;
            analyzer.analyze(module);
            const decomp::DecompilerContext context{.type = ShaderType::Compute};
            GuestShaderState state{};
            ShaderDescriptor descriptor{};
            descriptor.type = ShaderType::Compute;
            descriptor.backend = ShaderBackend::Msl;
            decomp::codegen::lang::msl::MslEmitter emitter(
                context, analyzer, state, descriptor.code,
                descriptor.resource_mapping);
            emitter.emit(module);
            check(descriptor.resource_mapping.sampler_buffer_index != invalid<u32>(),
                  "Large shader did not use a sampler argument buffer");
            ShaderLibraryCache libraries(device, path / "sources");
            Shader shader(device, descriptor, libraries);
            check(shader.getFunction() != nullptr &&
                      shader.getSamplerEncoder() != nullptr,
                  "Generated 18-sampler shader failed to compile");
            const auto argument_encoder = shader.getSamplerEncoder();
            NS_STACK_SCOPED auto buffer = device->newBuffer(
                argument_encoder->encodedLength(), MTL::ResourceStorageModeShared);
            NS_STACK_SCOPED auto sampler_desc = MTL::SamplerDescriptor::alloc()->init();
            sampler_desc->setSupportArgumentBuffers(true);
            NS_STACK_SCOPED auto sampler = device->newSamplerState(sampler_desc);
            check(sampler != nullptr, "Argument buffer sampler creation failed");
            argument_encoder->setArgumentBuffer(buffer, 0);
            for (u32 i = 0; i < 18; ++i)
                argument_encoder->setSamplerState(sampler, i);
            std::cout << "PASS: generated 18-sampler shader and argument encoding\n";
            return 0;
        }
        if (mode == "depth-only") {
            using namespace hydra::hw::tegra_x1::gpu::renderer;
            const std::string source = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 main_(uint id [[vertex_id]]) {
    return float4(float(id), 0, 0.5, 1);
}
)";
            ShaderLibraryCache libraries(device, path / "sources");
            ShaderDescriptor shader_descriptor{};
            shader_descriptor.type = ShaderType::Vertex;
            shader_descriptor.backend = hydra::ShaderBackend::Msl;
            shader_descriptor.code.assign(source.begin(), source.end());
            Shader vertex_shader(device, shader_descriptor, libraries);
            check(vertex_shader.getFunction() != nullptr,
                  "Depth-only vertex shader compilation failed");
            PipelineDescriptor descriptor{};
            descriptor.shaders[static_cast<unsigned>(ShaderType::Vertex)] =
                &vertex_shader;
            descriptor.depth_stencil_format = TextureFormat::Z32Float;
            Pipeline depth_only(device, descriptor, cache);
            check(depth_only.getPipeline() != nullptr,
                  "Pipeline with disabled fragment stage failed");
            descriptor.shaders[static_cast<unsigned>(ShaderType::Vertex)] = nullptr;
            Pipeline missing_vertex(device, descriptor, cache);
            check(missing_vertex.getPipeline() == nullptr,
                  "Pipeline with missing vertex stage was accepted");
            std::cout << "PASS: depth-only pipeline and missing vertex handling\n";
            return 0;
        }
        if (mode == "occlusion-merge" || mode == "pass-lifetime") {
            const bool release_descriptors = mode == "pass-lifetime";
            // Occlusion counting across two render passes on the same target
            // (descriptors differ, the second loads): merged into one encoder
            // or not, every sample of both draws counts
            using hydra::hw::tegra_x1::gpu::PerfStats;
            const std::string source = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 v(uint id [[vertex_id]]) {
    const float2 p[] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(p[id], 0, 1);
}
fragment float4 red() { return float4(1, 0, 0, 1); }
)";
            NS::Error* error = nullptr;
            NS_STACK_SCOPED auto library =
                device->newLibrary(toNsString(source), nullptr, &error);
            check(library != nullptr, "Test shader compilation failed");
            NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("v"));
            NS_STACK_SCOPED auto fragment =
                library->newFunction(toNsString("red"));
            NS_STACK_SCOPED auto pipeline_descriptor =
                MTL::RenderPipelineDescriptor::alloc()->init();
            pipeline_descriptor->setVertexFunction(vertex);
            pipeline_descriptor->setFragmentFunction(fragment);
            pipeline_descriptor->colorAttachments()->object(0)->setPixelFormat(
                MTL::PixelFormatRGBA8Unorm);
            NS_STACK_SCOPED auto pipeline =
                device->newRenderPipelineState(pipeline_descriptor, &error);
            check(pipeline != nullptr, "Test pipeline creation failed");
            constexpr hydra::u32 size = 64;
            NS_STACK_SCOPED auto texture_descriptor =
                MTL::TextureDescriptor::alloc()->init();
            texture_descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
            texture_descriptor->setWidth(size);
            texture_descriptor->setHeight(size);
            texture_descriptor->setStorageMode(MTL::StorageModePrivate);
            texture_descriptor->setUsage(MTL::TextureUsageRenderTarget);
            NS_STACK_SCOPED auto target = device->newTexture(texture_descriptor);
            NS_STACK_SCOPED auto queue = device->newCommandQueue();
            auto completions = std::make_shared<CompletionQueue>();
            OcclusionCounter occlusion(device, completions);
            occlusion.setEnabled(true);
            auto result = std::make_shared<std::optional<hydra::u64>>();
            auto& stats = PerfStats::instance();
            stats.render_pass_merges = 0;
            hydra::u64 sequence;
            {
                CommandBuffer commands(queue, completions);
                NS_STACK_SCOPED auto clear_pass =
                    MTL::RenderPassDescriptor::alloc()->init();
                clear_pass->colorAttachments()->object(0)->setTexture(target);
                clear_pass->colorAttachments()->object(0)->setLoadAction(
                    MTL::LoadActionClear);
                clear_pass->colorAttachments()->object(0)->setStoreAction(
                    MTL::StoreActionStore);
                NS_STACK_SCOPED auto load_pass = clear_pass->copy();
                load_pass->colorAttachments()->object(0)->setLoadAction(
                    MTL::LoadActionLoad);
                std::vector<MTL::RenderPassDescriptor*> passes{clear_pass, load_pass};
                if (release_descriptors)
                    passes = {clear_pass->copy(), load_pass->copy(), load_pass->copy()};
                for (auto* pass : passes) {
                    // As the renderer does: the query before the encoder
                    const auto offset = occlusion.prepareDraw(commands, pass);
                    check(offset.has_value(), "Counting draw has no slot");
                    auto* encoder = commands.getRenderCommandEncoder(pass);
                    encoder->setRenderPipelineState(pipeline);
                    encoder->setVisibilityResultMode(
                        MTL::VisibilityResultModeCounting, *offset);
                    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                            NS::UInteger(0), NS::UInteger(3));
                    // Model a transient pass or eviction of the renderer cache
                    // entry before the next occlusion/merge comparison.
                    if (release_descriptors) pass->release();
                }
                sequence = occlusion.queueReport(
                    commands,
                    [result](std::optional<hydra::u64> value) {
                        *result = value;
                    },
                    {});
            }
            check(completions->wait(sequence, std::chrono::seconds(5)),
                  "Occlusion report did not complete");
            const bool merging = std::getenv("HYDRA_NO_PASS_MERGE") == nullptr;
            const auto draws = release_descriptors ? 3u : 2u;
            check(result->has_value() && **result == draws * size * size,
                  "Occlusion count across the passes is wrong");
            if (PerfStats::isEnabled())
                check(stats.render_pass_merges == (merging ? draws - 1 : 0u),
                      "Passes were not merged as expected");
            std::cout << "PASS " << mode << ": " << **result
                      << " samples, merged passes " << stats.render_pass_merges
                      << "\n";
            return 0;
        }
        if (mode == "direct-merge") {
            // A pass leaves a small scissor and back-face culling; a direct
            // draw (as a texture blit) on the same target continues it and
            // must still cover the whole target, as on a new encoder
            using hydra::hw::tegra_x1::gpu::PerfStats;
            const std::string source = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 v(uint id [[vertex_id]]) {
    const float2 p[] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(p[id], 0, 1);
}
fragment float4 green() { return float4(0, 1, 0, 1); }
)";
            NS::Error* error = nullptr;
            NS_STACK_SCOPED auto library =
                device->newLibrary(toNsString(source), nullptr, &error);
            check(library != nullptr, "Test shader compilation failed");
            NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("v"));
            NS_STACK_SCOPED auto fragment =
                library->newFunction(toNsString("green"));
            NS_STACK_SCOPED auto pipeline_descriptor =
                MTL::RenderPipelineDescriptor::alloc()->init();
            pipeline_descriptor->setVertexFunction(vertex);
            pipeline_descriptor->setFragmentFunction(fragment);
            pipeline_descriptor->colorAttachments()->object(0)->setPixelFormat(
                MTL::PixelFormatRGBA8Unorm);
            NS_STACK_SCOPED auto pipeline =
                device->newRenderPipelineState(pipeline_descriptor, &error);
            check(pipeline != nullptr, "Test pipeline creation failed");
            NS_STACK_SCOPED auto depth_descriptor =
                MTL::DepthStencilDescriptor::alloc()->init();
            NS_STACK_SCOPED auto no_depth =
                device->newDepthStencilState(depth_descriptor);
            constexpr hydra::u32 size = 64;
            NS_STACK_SCOPED auto texture_descriptor =
                MTL::TextureDescriptor::alloc()->init();
            texture_descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
            texture_descriptor->setWidth(size);
            texture_descriptor->setHeight(size);
            texture_descriptor->setStorageMode(MTL::StorageModeShared);
            texture_descriptor->setUsage(MTL::TextureUsageRenderTarget);
            NS_STACK_SCOPED auto target = device->newTexture(texture_descriptor);
            NS_STACK_SCOPED auto queue = device->newCommandQueue();
            auto& stats = PerfStats::instance();
            stats.render_pass_merges = 0;
            MTL::CommandBuffer* native;
            {
                CommandBuffer commands(queue);
                NS_STACK_SCOPED auto clear_pass =
                    MTL::RenderPassDescriptor::alloc()->init();
                clear_pass->colorAttachments()->object(0)->setTexture(target);
                clear_pass->colorAttachments()->object(0)->setLoadAction(
                    MTL::LoadActionClear);
                clear_pass->colorAttachments()->object(0)->setClearColor(
                    MTL::ClearColor(0, 0, 1, 1));
                clear_pass->colorAttachments()->object(0)->setStoreAction(
                    MTL::StoreActionStore);
                auto* encoder = commands.getRenderCommandEncoder(clear_pass);
                // State a direct draw does not set
                encoder->setScissorRect(MTL::ScissorRect{0, 0, 4, 4});
                encoder->setCullMode(MTL::CullModeFront);
                encoder->setFrontFacingWinding(MTL::WindingClockwise);
                {
                    // Separately allocated, released after use, like a blit
                    NS_STACK_SCOPED auto blit_pass = clear_pass->copy();
                    blit_pass->colorAttachments()->object(0)->setLoadAction(
                        MTL::LoadActionLoad);
                    auto* direct =
                        commands.getDirectRenderCommandEncoder(blit_pass, no_depth);
                    direct->setRenderPipelineState(pipeline);
                    direct->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                           NS::UInteger(0), NS::UInteger(3));
                }
                native = commands.getCommandBuffer()->retain();
            }
            native->waitUntilCompleted();
            check(native->status() == MTL::CommandBufferStatusCompleted,
                  "Command buffer failed");
            native->release();
            const bool merging = std::getenv("HYDRA_NO_PASS_MERGE") == nullptr;
            if (PerfStats::isEnabled())
                check(stats.render_pass_merges == (merging ? 1u : 0u),
                      "The direct pass was not merged as expected");
            std::vector<uint8_t> pixels(size * size * 4);
            target->getBytes(pixels.data(), size * 4,
                             MTL::Region::Make2D(0, 0, size, size), 0);
            for (hydra::usize i = 0; i < pixels.size(); i += 4)
                check(pixels[i] == 0 && pixels[i + 1] == 255 &&
                          pixels[i + 2] == 0 && pixels[i + 3] == 255,
                      "Direct draw did not cover the whole target");
            std::cout << "PASS direct-merge: merged passes "
                      << stats.render_pass_merges << ", all pixels drawn\n";
            return 0;
        }
        if (mode == "pass-profile") {
            // Two render passes on the same target (the second continues the
            // first) and a blit: their GPU times and traffic are reported
            using hydra::hw::tegra_x1::gpu::PerfStats;
            check(PerfStats::isEnabled() &&
                      std::getenv("HYDRA_GPU_PASS_TIMES") != nullptr,
                  "Run with HYDRA_PERF_STATS=1 HYDRA_GPU_PASS_TIMES=1");
            auto* profiler = GpuPassProfiler::get(device);
            check(profiler != nullptr, "GPU pass timestamps are unavailable");
            const std::string source = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 v(uint id [[vertex_id]]) {
    const float2 p[] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(p[id], 0, 1);
}
fragment float4 red() { return float4(1, 0, 0, 1); }
)";
            NS::Error* error = nullptr;
            NS_STACK_SCOPED auto library =
                device->newLibrary(toNsString(source), nullptr, &error);
            check(library != nullptr, "Test shader compilation failed");
            NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("v"));
            NS_STACK_SCOPED auto fragment =
                library->newFunction(toNsString("red"));
            NS_STACK_SCOPED auto pipeline_descriptor =
                MTL::RenderPipelineDescriptor::alloc()->init();
            pipeline_descriptor->setVertexFunction(vertex);
            pipeline_descriptor->setFragmentFunction(fragment);
            pipeline_descriptor->colorAttachments()->object(0)->setPixelFormat(
                MTL::PixelFormatRGBA8Unorm);
            NS_STACK_SCOPED auto pipeline =
                device->newRenderPipelineState(pipeline_descriptor, &error);
            check(pipeline != nullptr, "Test pipeline creation failed");
            constexpr hydra::u32 size = 256;
            NS_STACK_SCOPED auto texture_descriptor =
                MTL::TextureDescriptor::alloc()->init();
            texture_descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
            texture_descriptor->setWidth(size);
            texture_descriptor->setHeight(size);
            texture_descriptor->setStorageMode(MTL::StorageModeShared);
            texture_descriptor->setUsage(MTL::TextureUsageRenderTarget |
                                         MTL::TextureUsageShaderRead);
            NS_STACK_SCOPED auto target = device->newTexture(texture_descriptor);
            NS_STACK_SCOPED auto copy = device->newTexture(texture_descriptor);
            NS_STACK_SCOPED auto queue = device->newCommandQueue();
            auto& stats = PerfStats::instance();
            stats.render_passes = 0;
            stats.render_pass_load_bytes = 0;
            stats.render_pass_store_bytes = 0;
            stats.render_pass_continuations = 0;
            stats.render_pass_merges = 0;
            for (auto& cause : stats.render_pass_continuation_causes)
                cause = 0;
            // The second pass continues the open first one
            const bool merging = std::getenv("HYDRA_NO_PASS_MERGE") == nullptr;
            MTL::CommandBuffer* native;
            {
                CommandBuffer commands(queue);
                NS_STACK_SCOPED auto clear_pass =
                    MTL::RenderPassDescriptor::alloc()->init();
                clear_pass->colorAttachments()->object(0)->setTexture(target);
                clear_pass->colorAttachments()->object(0)->setLoadAction(
                    MTL::LoadActionClear);
                clear_pass->colorAttachments()->object(0)->setStoreAction(
                    MTL::StoreActionStore);
                NS_STACK_SCOPED auto load_pass = clear_pass->copy();
                load_pass->colorAttachments()->object(0)->setLoadAction(
                    MTL::LoadActionLoad);
                for (auto* pass : {clear_pass, load_pass}) {
                    auto* encoder = commands.getRenderCommandEncoder(pass);
                    encoder->setRenderPipelineState(pipeline);
                    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                            NS::UInteger(0), NS::UInteger(3),
                                            NS::UInteger(200));
                }
                auto* blit = commands.getBlitCommandEncoder();
                blit->copyFromTexture(target, copy);
                native = commands.getCommandBuffer()->retain();
            }
            native->waitUntilCompleted();
            check(native->status() == MTL::CommandBufferStatusCompleted,
                  "Profiled command buffer failed");
            native->release();
            // Completion handlers may still be running
            GpuPassProfiler::Total kinds[3]{}, total[3]{};
            std::vector<std::pair<std::string, GpuPassProfiler::Total>> top;
            for (int attempt = 0; attempt < 200; attempt++) {
                auto current = profiler->takeTop(8, kinds);
                for (int k = 0; k < 3; k++) {
                    total[k].ns += kinds[k].ns;
                    total[k].count += kinds[k].count;
                }
                top.insert(top.end(), current.begin(), current.end());
                if (total[0].count == (merging ? 1u : 2u) && total[2].count == 1)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const hydra::u64 render_count = merging ? 1 : 2;
            check(total[0].count == render_count && total[0].ns > 0,
                  "Render pass times were not resolved");
            check(total[2].count == 1, "Blit pass time was not resolved");
            check(!top.empty() &&
                      top[0].first.find("render 256x256") == 0,
                  "Render pass label is wrong");
            const hydra::u64 bytes = size * size * 4;
            if (merging) {
                // One pass: cleared, drawn twice, stored once
                check(stats.render_passes == 1 && stats.render_pass_merges == 1,
                      "The loading pass was not merged");
                check(stats.render_pass_load_bytes == 0 &&
                          stats.render_pass_store_bytes == bytes,
                      "Merged pass traffic is wrong");
            } else {
                check(stats.render_passes == 2,
                      "Render passes were not counted");
                check(stats.render_pass_continuations == 1 &&
                          stats.render_pass_continuation_causes[0] == 1,
                      "The loading pass is not a continuation of a render pass");
                check(stats.render_pass_load_bytes == bytes,
                      "Load traffic is wrong");
                check(stats.render_pass_store_bytes == 2 * bytes,
                      "Store traffic is wrong");
            }
            std::vector<uint8_t> pixels(bytes);
            copy->getBytes(pixels.data(), size * 4,
                           MTL::Region::Make2D(0, 0, size, size), 0);
            for (hydra::usize i = 0; i < pixels.size(); i += 4)
                check(pixels[i] == 255 && pixels[i + 1] == 0 &&
                          pixels[i + 3] == 255,
                      "Profiling changed rendered pixels");
            std::cout << "PASS pass-profile: render " << total[0].count
                      << " passes " << static_cast<double>(total[0].ns) / 1000.0 << " us, blit "
                      << static_cast<double>(total[2].ns) / 1000.0 << " us | " << top[0].first
                      << "\n";
            return 0;
        }
        if (mode == "upscale") {
            // Left half red, right half blue: the upscaled image keeps both
            // halves and their colours
            SpatialUpscaler upscaler(device);
            NS_STACK_SCOPED auto queue = device->newCommandQueue();
            const MTL::PixelFormat formats[] = {
                MTL::PixelFormatRGBA8Unorm, MTL::PixelFormatRGBA8Unorm_sRGB,
                MTL::PixelFormatBGRA8Unorm, MTL::PixelFormatRGBA16Float,
                MTL::PixelFormatRGB10A2Unorm};
            const char* names[] = {"RGBA8Unorm", "RGBA8Unorm_sRGB",
                                   "BGRA8Unorm", "RGBA16Float", "RGB10A2Unorm"};
            constexpr hydra::u32 size = 64;
            bool any_upscaled = false;
            for (hydra::usize f = 0; f < std::size(formats); f++) {
                NS_STACK_SCOPED auto descriptor =
                    MTL::TextureDescriptor::alloc()->init();
                descriptor->setPixelFormat(formats[f]);
                descriptor->setWidth(size);
                descriptor->setHeight(size);
                descriptor->setStorageMode(MTL::StorageModeShared);
                descriptor->setUsage(MTL::TextureUsageShaderRead |
                                     MTL::TextureUsageRenderTarget);
                NS_STACK_SCOPED auto input = device->newTexture(descriptor);
                auto pass = MTL::RenderPassDescriptor::renderPassDescriptor();
                pass->colorAttachments()->object(0)->setTexture(input);
                pass->colorAttachments()->object(0)->setLoadAction(
                    MTL::LoadActionClear);
                pass->colorAttachments()->object(0)->setStoreAction(
                    MTL::StoreActionStore);
                // Clear to blue, then the left half to red with a scissor
                auto* command = queue->commandBuffer();
                pass->colorAttachments()->object(0)->setClearColor(
                    MTL::ClearColor(0, 0, 1, 1));
                command->renderCommandEncoder(pass)->endEncoding();
                NS_STACK_SCOPED auto blit_source =
                    device->newTexture(descriptor);
                pass->colorAttachments()->object(0)->setTexture(blit_source);
                pass->colorAttachments()->object(0)->setClearColor(
                    MTL::ClearColor(1, 0, 0, 1));
                command->renderCommandEncoder(pass)->endEncoding();
                auto* blit = command->blitCommandEncoder();
                blit->copyFromTexture(blit_source, 0, 0, MTL::Origin(0, 0, 0),
                                      MTL::Size(size / 2, size, 1), input, 0,
                                      0, MTL::Origin(0, 0, 0));
                blit->endEncoding();
                auto* output = upscaler.upscale(command, input, size * 2,
                                                size * 2);
                std::cout << "MetalFX " << names[f] << ": "
                          << (output ? "upscaled" : "not supported, bilinear")
                          << '\n';
                if (!output) {
                    command->commit();
                    command->waitUntilCompleted();
                    continue;
                }
                any_upscaled = true;
                check(output->width() == size * 2 && output->height() == size * 2,
                      "Upscaled texture has the wrong size");
                // Copy the private output to a shared texture to read it
                NS_STACK_SCOPED auto shared_descriptor =
                    MTL::TextureDescriptor::alloc()->init();
                shared_descriptor->setPixelFormat(formats[f]);
                shared_descriptor->setWidth(size * 2);
                shared_descriptor->setHeight(size * 2);
                shared_descriptor->setStorageMode(MTL::StorageModeShared);
                NS_STACK_SCOPED auto shared =
                    device->newTexture(shared_descriptor);
                auto* copy = command->blitCommandEncoder();
                copy->copyFromTexture(output, shared);
                copy->endEncoding();
                command->commit();
                command->waitUntilCompleted();
                check(command->status() == MTL::CommandBufferStatusCompleted,
                      "MetalFX command buffer failed");
                if (formats[f] != MTL::PixelFormatRGBA8Unorm &&
                    formats[f] != MTL::PixelFormatBGRA8Unorm)
                    continue;
                // Sample well inside each half (edges may blend)
                std::vector<uint8_t> pixels(size * 2 * size * 2 * 4);
                shared->getBytes(pixels.data(), size * 2 * 4,
                                 MTL::Region::Make2D(0, 0, size * 2, size * 2), 0);
                const bool bgra = formats[f] == MTL::PixelFormatBGRA8Unorm;
                const auto at = [&](hydra::u32 x, hydra::u32 y, hydra::u32 c) {
                    const auto channel = bgra && c != 3 ? 2 - c : c;
                    return pixels[(y * size * 2 + x) * 4 + channel];
                };
                for (hydra::u32 y = 8; y < size * 2 - 8; y += 8) {
                    check(at(16, y, 0) > 240 && at(16, y, 2) < 15,
                          "Left half is not red after upscaling");
                    check(at(size * 2 - 16, y, 2) > 240 &&
                              at(size * 2 - 16, y, 0) < 15,
                          "Right half is not blue after upscaling");
                }
            }
            check(any_upscaled, "MetalFX upscaled no format");
            std::cout << "PASS: MetalFX spatial upscaling keeps the image\n";
            return 0;
        }
        if (mode == "recipe-cold" || mode == "recipe-warm") {
            using namespace hydra::hw::tegra_x1::gpu;
            using namespace hydra::hw::tegra_x1::gpu::renderer;
            const bool warm = mode == "recipe-warm";
            const std::string vertex_source = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 main_(uint id [[vertex_id]]) {
    const float2 p[] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(p[id], 0, 1);
}
)";
            const std::string fragment_source = R"(
#include <metal_stdlib>
using namespace metal;
fragment float4 main_() { return float4(0, 1, 0, 1); }
)";
            ShaderLibraryCache libraries(device, path / "sources");
            cache.startPrewarm(path / "recipes", libraries);
            // Waits for the prewarm
            libraries.flush();
            cache.flush();
            check(cache.getPrewarmed() == (warm ? 1u : 0u),
                  "Unexpected number of prewarmed pipelines");
            const auto make_shader = [&](ShaderType type,
                                         const std::string& source) {
                ShaderDescriptor shader_descriptor{};
                shader_descriptor.type = type;
                shader_descriptor.backend = hydra::ShaderBackend::Msl;
                shader_descriptor.code.assign(source.begin(), source.end());
                shader_descriptor.resource_mapping.fragment_output_mask = 0xf;
                return std::make_unique<Shader>(device, shader_descriptor,
                                                libraries);
            };
            const auto vertex_shader =
                make_shader(ShaderType::Vertex, vertex_source);
            const auto fragment_shader =
                make_shader(ShaderType::Fragment, fragment_source);
            check(libraries.getHits() == (warm ? 2u : 0u),
                  "Shaders did not use the prewarmed libraries");
            PipelineDescriptor descriptor{};
            descriptor.shaders[static_cast<unsigned>(ShaderType::Vertex)] =
                vertex_shader.get();
            descriptor.shaders[static_cast<unsigned>(ShaderType::Fragment)] =
                fragment_shader.get();
            descriptor.color_target_states[0].format = TextureFormat::RGBA8Unorm;
            descriptor.color_target_states[0].write_mask =
                engines::ColorWriteMask::All;
            Pipeline pipeline(device, descriptor, cache);
            check(pipeline.getPipeline() != nullptr,
                  "Pipeline creation failed");
            check(cache.getPrewarmHits() == (warm ? 1u : 0u),
                  "Draw did not use the prewarmed pipeline");

            NS_STACK_SCOPED auto queue = device->newCommandQueue();
            NS_STACK_SCOPED auto texture_descriptor =
                MTL::TextureDescriptor::alloc()->init();
            texture_descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
            texture_descriptor->setWidth(4);
            texture_descriptor->setHeight(4);
            texture_descriptor->setStorageMode(MTL::StorageModeShared);
            texture_descriptor->setUsage(MTL::TextureUsageRenderTarget);
            NS_STACK_SCOPED auto texture =
                device->newTexture(texture_descriptor);
            auto pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto attachment = pass->colorAttachments()->object(0);
            attachment->setTexture(texture);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setStoreAction(MTL::StoreActionStore);
            attachment->setClearColor(MTL::ClearColor(0, 0, 0, 0));
            auto command = queue->commandBuffer();
            auto encoder = command->renderCommandEncoder(pass);
            encoder->setRenderPipelineState(pipeline.getPipeline());
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
            for (int pixel = 0; pixel < 16; pixel++)
                for (int channel = 0; channel < 4; channel++)
                    check(pixels[pixel * 4 + channel] ==
                              (channel == 1 || channel == 3 ? 255 : 0),
                          "Prewarmed pipeline changed rendered pixels");

            libraries.flush();
            cache.flush();
            hydra::usize recipes = 0;
            for (const auto& entry :
                 std::filesystem::directory_iterator(path / "recipes"))
                recipes += entry.path().extension() == ".recipe";
            check(recipes == 1, "Pipeline recipe was not stored once");
            std::cout << "PASS " << mode
                      << ": prewarmed=" << cache.getPrewarmed()
                      << " prewarm_hits=" << cache.getPrewarmHits()
                      << " library_hits=" << libraries.getHits()
                      << " pixels=identical\n";
            return 0;
        }
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
