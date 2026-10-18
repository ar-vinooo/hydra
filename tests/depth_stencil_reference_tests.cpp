// Executes Python-generated depth/stencil cases through Hydra's renderer.
#include "core/hw/tegra_x1/gpu/engines/3d.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/render_pass.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"
#include "core/hw/tegra_x1/gpu/renderer/pipeline_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"

#include <array>
#include <bit>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using metal::stackAutoRelease;

namespace {
struct Draw {
    float z;
    bool depth_test;
    bool depth_write;
    u32 depth_func;
    bool stencil_test;
    u32 stencil_func;
    u32 ref;
    u32 read_mask;
    u32 write_mask;
    u32 fail;
    u32 depth_fail;
    u32 pass;
};

struct Target {
    std::unique_ptr<ITexture> texture;
    std::unique_ptr<ITextureView> view;
};

Target makeTarget(metal::Renderer& renderer, TextureFormat format) {
    Target target;
    target.texture.reset(renderer.createTexture(TextureDescriptor(
        0, MTL::TextureType2D, format, false, 0, 1, 1, 1, 1, 1, 0, 0, 0)));
    target.view.reset(target.texture->createView(TextureViewDescriptor(
        MTL::TextureType2D, format, {0, 1}, {0, 1})));
    return target;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("usage: cases.txt out.txt");
        setenv("HYDRA_GPU_SYNC", "1", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        metal::Renderer renderer;
        const auto shader = [&](ShaderType type, const char* source) {
            ShaderDescriptor descriptor{};
            descriptor.type = type;
            descriptor.backend = ShaderBackend::Msl;
            descriptor.code.assign(source, source + std::strlen(source));
            descriptor.resource_mapping.uniform_buffers[0] = 0;
            return std::unique_ptr<ShaderBase>(renderer.createShader(descriptor));
        };
        const auto vertex = shader(ShaderType::Vertex, R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 main_(uint id [[vertex_id]], constant float4& c [[buffer(0)]]) {
    float2 p[] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(p[id], c.x, 1);
}
)");
        const auto fragment = shader(ShaderType::Fragment, R"(
#include <metal_stdlib>
using namespace metal;
fragment float4 main_(constant float4& c [[buffer(0)]]) {
    return float4(c.y, c.z, c.w, 1);
}
)");
        auto color = makeTarget(renderer, TextureFormat::RGBA8Unorm);
        auto depth = makeTarget(renderer, TextureFormat::Z32Float_X24S8Uint);
        PipelineDescriptor pipeline_descriptor{};
        pipeline_descriptor.shaders[static_cast<u32>(ShaderType::Vertex)] = vertex.get();
        pipeline_descriptor.shaders[static_cast<u32>(ShaderType::Fragment)] = fragment.get();
        pipeline_descriptor.color_target_states[0].format = TextureFormat::RGBA8Unorm;
        pipeline_descriptor.color_target_states[0].write_mask = engines::ColorWriteMask::All;
        pipeline_descriptor.depth_stencil_format = TextureFormat::Z32Float_X24S8Uint;
        std::unique_ptr<PipelineBase> pipeline(renderer.createPipeline(pipeline_descriptor));
        renderer.bindPipeline(pipeline.get());
        renderer.setCullState(false, engines::CullFaceMode::Back,
                              engines::Winding::Clockwise);
        for (u32 i = 0; i < VIEWPORT_COUNT; ++i) {
            renderer.setViewport(i, {0, 0, 1, 1, 0, 1});
            renderer.setScissor(i, {0, 0, 1, 1});
        }
        auto* device = renderer.getDevice();
        NS_STACK_SCOPED auto output = device->newBuffer(768, MTL::ResourceStorageModeShared);
        std::ifstream input(argv[1]);
        std::ofstream result(argv[2]);
        if (!input || !result)
            throw std::runtime_error("cannot open case files");
        std::string line;
        while (std::getline(input, line)) {
            NS_STACK_SCOPED auto case_pool = NS::AutoreleasePool::alloc()->init();
            std::istringstream fields(line);
            float initial_depth;
            u32 initial_stencil;
            bool split_encoder;
            std::array<Draw, 2> draws{};
            fields >> initial_depth >> initial_stencil >> split_encoder;
            for (auto& draw : draws)
                fields >> draw.z >> draw.depth_test >> draw.depth_write
                       >> draw.depth_func >> draw.stencil_test >> draw.stencil_func
                       >> draw.ref >> draw.read_mask >> draw.write_mask
                       >> draw.fail >> draw.depth_fail >> draw.pass;
            if (!fields)
                throw std::runtime_error("invalid case: " + line);

            RenderPassDescriptor pass_descriptor{};
            pass_descriptor.color_targets[0].texture = color.view.get();
            pass_descriptor.color_targets[0].load_action_clear = true;
            pass_descriptor.depth_stencil_target.texture = depth.view.get();
            pass_descriptor.depth_stencil_target.load_action_clear = true;
            pass_descriptor.depth_stencil_target.clear_data.clear_depth = true;
            pass_descriptor.depth_stencil_target.clear_data.clear_stencil = true;
            pass_descriptor.depth_stencil_target.clear_data.depth = initial_depth;
            pass_descriptor.depth_stencil_target.clear_data.stencil = initial_stencil;
            std::unique_ptr<RenderPassBase> pass(renderer.createRenderPass(pass_descriptor));
            renderer.bindRenderPass(pass.get());
            std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
            auto* metal_commands = static_cast<metal::CommandBuffer*>(commands.get());
            for (usize i = 0; i < draws.size(); ++i) {
                const auto& draw = draws[i];
                renderer.setDepthTestEnabled(draw.depth_test);
                renderer.setDepthWriteEnabled(draw.depth_write);
                renderer.setDepthCompareOp(static_cast<engines::CompareOp>(draw.depth_func));
                StencilState stencil{};
                stencil.enabled = draw.stencil_test;
                stencil.front = {
                    .func = static_cast<engines::CompareOp>(draw.stencil_func),
                    .fail_op = engines::decodeStencilOp(draw.fail),
                    .depth_fail_op = engines::decodeStencilOp(draw.depth_fail),
                    .pass_op = engines::decodeStencilOp(draw.pass),
                    .ref = draw.ref, .func_mask = draw.read_mask,
                    .write_mask = draw.write_mask,
                };
                stencil.back = stencil.front;
                renderer.setStencilState(stencil);
                const std::array<f32, 4> params{draw.z, i == 0 ? 1.0f : 0.0f,
                                               i == 1 ? 1.0f : 0.0f, 0};
                const auto buffer = renderer.getBufferCache().snapshot(
                    ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(params.data()),
                                              sizeof(params)));
                renderer.bindUniformBuffer(buffer, ShaderType::Vertex, 0);
                renderer.bindUniformBuffer(buffer, ShaderType::Fragment, 0);
                renderer.draw(commands.get(), engines::PrimitiveType::Triangles, 0, 3, 0, 1);
                if (i == 0 && split_encoder) {
                    metal_commands->getBlitCommandEncoder();
                    auto* native_pass = static_cast<metal::RenderPass*>(pass.get())
                                            ->getRenderPassDescriptor();
                    native_pass->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionLoad);
                    native_pass->depthAttachment()->setLoadAction(MTL::LoadActionLoad);
                    native_pass->stencilAttachment()->setLoadAction(MTL::LoadActionLoad);
                }
            }
            auto* blit = metal_commands->getBlitCommandEncoder();
            auto* native_color = static_cast<metal::TextureView*>(color.view.get())->getTexture();
            auto* native_depth = static_cast<metal::TextureView*>(depth.view.get())->getTexture();
            blit->copyFromTexture(native_color, 0, 0, {0, 0, 0}, {1, 1, 1},
                                  output, 0, 256, 256);
            blit->copyFromTexture(native_depth, 0, 0, {0, 0, 0}, {1, 1, 1},
                                  output, 256, 256, 256, MTL::BlitOptionDepthFromDepthStencil);
            blit->copyFromTexture(native_depth, 0, 0, {0, 0, 0}, {1, 1, 1},
                                  output, 512, 256, 256, MTL::BlitOptionStencilFromDepthStencil);
            auto* native = metal_commands->getCommandBuffer()->retain();
            commands.reset();
            const bool ok = native->status() == MTL::CommandBufferStatusCompleted;
            native->release();
            if (!ok)
                throw std::runtime_error("submission failed: " + line);
            const auto* bytes = static_cast<const u8*>(output->contents());
            u32 depth_bits;
            std::memcpy(&depth_bits, bytes + 256, sizeof(depth_bits));
            result << u32(bytes[0]) << ' ' << u32(bytes[1]) << ' '
                   << u32(bytes[2]) << ' ' << u32(bytes[3]) << ' '
                   << depth_bits << ' ' << u32(bytes[512]) << '\n';
        }
        if (!input.eof() || !result)
            throw std::runtime_error("case file read/write failed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
