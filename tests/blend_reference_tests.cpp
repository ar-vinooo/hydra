// Draws blend cases with Hydra's Maxwell->Metal format table and dumps the raw
// target bytes. tests/blend_reference.py computes the expected values from the
// blend/format rules and compares. Usage: <binary> <cases.txt> <out.txt>
// Case line: <format> <srcFactor> <dstFactor> <op> <dst rgba> <src rgba>
#include "core/hw/tegra_x1/gpu/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::renderer::metal;

namespace {
const std::map<std::string, renderer::TextureFormat> formats = {
    {"RGBA16Float", renderer::TextureFormat::RGBA16Float},
    {"RG11B10Float", renderer::TextureFormat::RG11B10Float},
    {"RGBA8Unorm", renderer::TextureFormat::RGBA8Unorm},
};
const std::map<std::string, MTL::BlendFactor> factors = {
    {"zero", MTL::BlendFactorZero},
    {"one", MTL::BlendFactorOne},
    {"src_alpha", MTL::BlendFactorSourceAlpha},
    {"inv_src_alpha", MTL::BlendFactorOneMinusSourceAlpha},
    {"src_color", MTL::BlendFactorSourceColor},
    {"dst_color", MTL::BlendFactorDestinationColor},
};
const std::map<std::string, MTL::BlendOperation> ops = {
    {"add", MTL::BlendOperationAdd},
    {"sub", MTL::BlendOperationSubtract},
    {"rsub", MTL::BlendOperationReverseSubtract},
    {"min", MTL::BlendOperationMin},
    {"max", MTL::BlendOperationMax},
};
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("usage: cases.txt out.txt");
        setenv("HYDRA_GPU_SYNC", "1", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        NS_STACK_SCOPED auto device = MTL::CreateSystemDefaultDevice();
        NS_STACK_SCOPED auto queue = device->newCommandQueue();
        const char* source = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 v(uint id [[vertex_id]]) {
    float2 p[] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(p[id], 0, 1);
}
fragment float4 f(constant float4& c [[buffer(0)]]) { return c; }
)";
        NS::Error* error = nullptr;
        NS_STACK_SCOPED auto library =
            device->newLibrary(toNsString(source), nullptr, &error);
        if (!library)
            throw std::runtime_error("shader compile failed");
        NS_STACK_SCOPED auto vs = library->newFunction(toNsString("v"));
        NS_STACK_SCOPED auto fs = library->newFunction(toNsString("f"));

        std::ifstream in(argv[1]);
        std::ofstream out(argv[2]);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string fmt, sf, df, op;
            float dst[4], src[4];
            ss >> fmt >> sf >> df >> op;
            for (auto& v : dst)
                ss >> v;
            for (auto& v : src)
                ss >> v;
            // The format goes through Hydra's Maxwell -> Metal table
            const auto pixel_format =
                toMtlPixelFormat(formats.at(fmt));

            NS_STACK_SCOPED auto pd =
                MTL::RenderPipelineDescriptor::alloc()->init();
            pd->setVertexFunction(vs);
            pd->setFragmentFunction(fs);
            auto* ca = pd->colorAttachments()->object(0);
            ca->setPixelFormat(pixel_format);
            ca->setBlendingEnabled(true);
            ca->setRgbBlendOperation(ops.at(op));
            ca->setAlphaBlendOperation(ops.at(op));
            ca->setSourceRGBBlendFactor(factors.at(sf));
            ca->setSourceAlphaBlendFactor(factors.at(sf));
            ca->setDestinationRGBBlendFactor(factors.at(df));
            ca->setDestinationAlphaBlendFactor(factors.at(df));
            NS_STACK_SCOPED auto pipeline =
                device->newRenderPipelineState(pd, &error);
            if (!pipeline)
                throw std::runtime_error("pipeline failed: " + line);

            NS_STACK_SCOPED auto td = MTL::TextureDescriptor::alloc()->init();
            td->setPixelFormat(pixel_format);
            td->setWidth(1);
            td->setHeight(1);
            td->setStorageMode(MTL::StorageModeShared);
            td->setUsage(MTL::TextureUsageRenderTarget);
            NS_STACK_SCOPED auto texture = device->newTexture(td);
            auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* att = pass->colorAttachments()->object(0);
            att->setTexture(texture);
            att->setLoadAction(MTL::LoadActionClear);
            att->setStoreAction(MTL::StoreActionStore);
            att->setClearColor(MTL::ClearColor(double(dst[0]), double(dst[1]),
                                                    double(dst[2]), double(dst[3])));
            auto commands = std::make_unique<CommandBuffer>(queue);
            auto* encoder = commands->getRenderCommandEncoder(pass);
            commands->setRenderPipelineState(pipeline);
            encoder->setFragmentBytes(src, sizeof(src), 0);
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                    NS::UInteger(3));
            auto* native = commands->getCommandBuffer()->retain();
            commands.reset();
            const bool ok = native->status() == MTL::CommandBufferStatusCompleted;
            native->release();
            if (!ok)
                throw std::runtime_error("submission failed: " + line);
            u8 bytes[16]{};
            texture->getBytes(bytes, 16, MTL::Region::Make2D(0, 0, 1, 1), 0);
            out << std::hex;
            for (u8 b : bytes)
                out << (b < 16 ? "0" : "") << static_cast<int>(b);
            out << std::dec << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
