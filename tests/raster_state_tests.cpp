#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::renderer::metal;

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

int main() {
    try {
        setenv("HYDRA_GPU_SYNC", "1", 1);
        setenv("HYDRA_PERF_STATS", "1", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        NS_STACK_SCOPED auto device = MTL::CreateSystemDefaultDevice();
        check(device != nullptr, "No Metal device");
        NS_STACK_SCOPED auto queue = device->newCommandQueue();
        const char* source = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 v(uint id [[vertex_id]]) {
    float2 positions[] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(positions[id], 0, 1);
}
fragment float4 f() { return float4(1); }
)";
        NS::Error* error = nullptr;
        NS_STACK_SCOPED auto library =
            device->newLibrary(toNsString(source), nullptr, &error);
        check(library != nullptr, "Compile pixel-test shader");
        NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("v"));
        NS_STACK_SCOPED auto fragment = library->newFunction(toNsString("f"));
        NS_STACK_SCOPED auto pipeline_descriptor =
            MTL::RenderPipelineDescriptor::alloc()->init();
        pipeline_descriptor->setVertexFunction(vertex);
        pipeline_descriptor->setFragmentFunction(fragment);
        auto* color = pipeline_descriptor->colorAttachments()->object(0);
        color->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        color->setBlendingEnabled(true);
        color->setSourceRGBBlendFactor(MTL::BlendFactorBlendColor);
        color->setDestinationRGBBlendFactor(MTL::BlendFactorZero);
        NS_STACK_SCOPED auto pipeline =
            device->newRenderPipelineState(pipeline_descriptor, &error);
        check(pipeline != nullptr, "Create pixel-test pipeline");

        for (bool constrain_viewport : {false, true}) {
            NS_STACK_SCOPED auto descriptor =
                MTL::TextureDescriptor::alloc()->init();
            descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
            descriptor->setWidth(8);
            descriptor->setHeight(4);
            descriptor->setStorageMode(MTL::StorageModeShared);
            descriptor->setUsage(MTL::TextureUsageRenderTarget);
            NS_STACK_SCOPED auto texture = device->newTexture(descriptor);
            check(texture != nullptr, "Create pixel-test target");
            auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* attachment = pass->colorAttachments()->object(0);
            attachment->setTexture(texture);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setStoreAction(MTL::StoreActionStore);
            attachment->setClearColor(MTL::ClearColor(0, 0, 0, 0));
            auto commands = std::make_unique<CommandBuffer>(queue);

            auto draw_half = [&](bool right) {
                auto* encoder = commands->getRenderCommandEncoder(pass);
                commands->setRenderPipelineState(pipeline);
                const MTL::Viewport viewport{constrain_viewport && right ? 4.0
                                                                         : 0.0,
                                             0,
                                             constrain_viewport ? 4.0 : 8.0,
                                             4,
                                             0,
                                             1};
                const MTL::ScissorRect scissor{
                    !constrain_viewport && right ? NS::UInteger(4)
                                                 : NS::UInteger(0),
                    0, constrain_viewport ? NS::UInteger(8) : NS::UInteger(4),
                    4};
                commands->setViewports(std::span(&viewport, 1));
                commands->setScissors(std::span(&scissor, 1));
                commands->setBlendColor(right ? 0 : 1, right ? 1 : 0, 0, 1);
                commands->setDepthBias(0, 0, 0);
                commands->setStencilReferences(0, 0);
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                        NS::UInteger(0), NS::UInteger(3));
            };
            draw_half(false);
            draw_half(false); // Identical state may be skipped safely.
            draw_half(true);  // Changed rectangles/color must be applied.
            commands->getBlitCommandEncoder(); // Force an encoder transition.
            attachment->setLoadAction(MTL::LoadActionLoad);
            draw_half(
                true); // Identical state must be rebound on a new encoder.
            auto* native = commands->getCommandBuffer()->retain();
            commands.reset();
            const bool completed =
                native->status() == MTL::CommandBufferStatusCompleted;
            native->release();
            check(completed, "GPU pixel-test submission completed");
            u8 pixels[8 * 4 * 4]{};
            texture->getBytes(pixels, 8 * 4, MTL::Region::Make2D(0, 0, 8, 4),
                              0);
            for (usize y = 0; y < 4; ++y)
                for (usize x = 0; x < 8; ++x) {
                    const auto offset = (y * 8 + x) * 4;
                    check(pixels[offset] == (x < 4 ? 255 : 0) &&
                              pixels[offset + 1] == (x < 4 ? 0 : 255) &&
                              pixels[offset + 2] == 0 &&
                              pixels[offset + 3] == 255,
                          "Raster caching changed pixels across draws or "
                          "encoders");
                }
        }
        const auto updates = PerfStats::instance().raster_updates.load();
        const auto skips = PerfStats::instance().raster_skips.load();
        check(updates > 0, "Raster update telemetry");
        if (std::getenv("HYDRA_DISABLE_RASTER_CACHE") == nullptr)
            check(skips > 0, "Unchanged raster commands were skipped");
        else
            check(skips == 0, "Disabled cache applies all raster commands");
        std::cout << "PASS: pixels identical after repeated draws, changed "
                     "viewport/scissor/blend and render-blit-render; updates="
                  << updates << " skipped=" << skips << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
