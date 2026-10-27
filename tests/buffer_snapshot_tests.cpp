#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        setenv("HYDRA_GPU_SYNC", "1", 1);
        setenv("HYDRA_PERF_STATS", "1", 1);
        setenv("HYDRA_DISABLE_SHADER_CACHE", "1", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        metal::Renderer renderer;
        auto* device = renderer.getDevice();
        const char* source = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 v(uint id [[vertex_id]]) {
    float2 p[] = {float2(-1,-1),float2(3,-1),float2(-1,3)};
    return float4(p[id],0,1);
}
fragment float4 f(constant float4& color [[buffer(0)]]) { return color; }
)";
        NS::Error* error = nullptr;
        NS_STACK_SCOPED auto library =
            device->newLibrary(metal::toNsString(source), nullptr, &error);
        check(library != nullptr, "Compile snapshot pixel shader");
        NS_STACK_SCOPED auto vertex =
            library->newFunction(metal::toNsString("v"));
        NS_STACK_SCOPED auto fragment =
            library->newFunction(metal::toNsString("f"));
        NS_STACK_SCOPED auto pipeline_desc =
            MTL::RenderPipelineDescriptor::alloc()->init();
        pipeline_desc->setVertexFunction(vertex);
        pipeline_desc->setFragmentFunction(fragment);
        pipeline_desc->colorAttachments()->object(0)->setPixelFormat(
            MTL::PixelFormatRGBA8Unorm);
        NS_STACK_SCOPED auto pipeline =
            device->newRenderPipelineState(pipeline_desc, &error);
        check(pipeline != nullptr, "Create snapshot pipeline");
        NS_STACK_SCOPED auto texture_desc =
            MTL::TextureDescriptor::alloc()->init();
        texture_desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        texture_desc->setWidth(8);
        texture_desc->setHeight(4);
        texture_desc->setStorageMode(MTL::StorageModeShared);
        texture_desc->setUsage(MTL::TextureUsageRenderTarget);
        NS_STACK_SCOPED auto target = device->newTexture(texture_desc);
        auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* attachment = pass->colorAttachments()->object(0);
        attachment->setTexture(target);
        attachment->setLoadAction(MTL::LoadActionLoad);
        attachment->setStoreAction(MTL::StoreActionStore);
        auto commands =
            std::unique_ptr<ICommandBuffer>(renderer.createCommandBuffer());
        auto* metal_commands =
            static_cast<metal::CommandBuffer*>(commands.get());
        std::vector<u8> guest(131072, 0);
        const auto ptr = reinterpret_cast<uptr>(guest.data());
        const auto range = ztd::Range<uptr>::fromSize(ptr, 16);
        auto& cache = renderer.getBufferCache();
        const auto draw = [&](BufferView view, bool right) {
            auto* encoder = metal_commands->getRenderCommandEncoder(pass);
            metal_commands->setRenderPipelineState(pipeline);
            const MTL::Viewport viewport{0, 0, 8, 4, 0, 1};
            const MTL::ScissorRect scissor{
                right ? NS::UInteger(4) : NS::UInteger(0), 0, 4, 4};
            metal_commands->setViewports(std::span(&viewport, 1));
            metal_commands->setScissors(std::span(&scissor, 1));
            auto* buffer = static_cast<metal::Buffer*>(view.getBase());
            metal_commands->setBuffer(buffer->getBuffer(), view.getOffset(),
                                      ShaderType::Fragment, 0);
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                    NS::UInteger(3));
        };
        const std::array<float, 4> red{1, 0, 0, 1}, green{0, 1, 0, 1};
        std::memcpy(guest.data(), red.data(), 16);
        auto first = cache.get(commands.get(), range);
        std::weak_ptr<BufferBase> old_owner = first.getOwner();
        draw(first, false);
        std::memcpy(guest.data(), green.data(), 16);
        cache.invalidateMemory(range);
        auto second = cache.get(commands.get(), range);
        const bool snapshots =
            std::getenv("HYDRA_DISABLE_BUFFER_SNAPSHOTS") == nullptr;
        if (snapshots)
            check(first.getBase() != second.getBase(),
                  "Updates replace snapshots without overwriting in-flight "
                  "bytes");
        first = {};
        if (snapshots)
            check(old_owner.expired(),
                  "Encoded GPU resource does not leak its CPU wrapper");
        draw(second, true);
        const auto before = PerfStats::instance().render_encoders.load();
        commands.reset();
        const auto encoders =
            PerfStats::instance().render_encoders.load() - before;
        check(encoders == (snapshots ? 1u : 2u),
              "Snapshots preserve one render encoder across uniform updates");
        std::array<u8, 8 * 4 * 4> pixels{};
        target->getBytes(pixels.data(), 8 * 4, MTL::Region::Make2D(0, 0, 8, 4),
                         0);
        for (usize y = 0; y < 4; ++y)
            for (usize x = 0; x < 8; ++x) {
                const auto i = (y * 8 + x) * 4;
                check(pixels[i] == (x < 4 ? 255 : 0) &&
                          pixels[i + 1] == (x < 4 ? 0 : 255) &&
                          pixels[i + 2] == 0 && pixels[i + 3] == 255,
                      "Both draws preserve their uniform colors");
            }
        commands.reset(renderer.createCommandBuffer());
        auto left = cache.get(commands.get(),
                              ztd::Range<uptr>::fromSize(ptr + 4096, 16));
        std::weak_ptr<BufferBase> merged_owner = left.getOwner();
        auto merged = cache.get(commands.get(),
                                ztd::Range<uptr>::fromSize(ptr + 4112, 16));
        check(!merged_owner.expired(),
              "Merge preserves an outstanding BufferView");
        left = {};
        check(merged_owner.expired(),
              "Merged-away buffer freed when last view releases it");
        // Two live entries, each in a 4 KiB snapshot size class: the buffer
        // merged away is no longer counted
        check(cache.getCachedBytes() == (snapshots ? 2 * 4096 : 48),
              "Only live merged resources count in cache budget");
        check(renderer.createCpuBufferSnapshot(ztd::Range<uptr>::fromSize(
                  ptr, BUFFER_SNAPSHOT_MAX_SIZE + 1)) == nullptr,
              "Large buffers retain the private buffer path");
        commands.reset();
        std::cout << "PASS: GPU snapshot pixel lifetime, render encoder count, "
                     "merge ownership and bounded snapshot size; encoders="
                  << encoders << '\n';
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
