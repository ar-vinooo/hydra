// Python supplies raw Maxwell attribute words and bytes, then checks float32
// pixels from Hydra's vertex fetch, MSL stage I/O and Metal pipeline.
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/pipeline.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/analyzer/memory_analyzer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/msl/emitter.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/ir/builder.hpp"

#include <array>
#include <bit>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp;
using metal::stackAutoRelease;

namespace {
constexpr u32 ATTRIBUTE = 4;
constexpr u32 VARYING = 2;

std::unique_ptr<ShaderBase> makeShader(metal::Renderer& renderer,
                                      ShaderType type,
                                      engines::VertexAttribState attribute) {
    DecompilerContext context{};
    context.type = type;
    if (type == ShaderType::Fragment)
        context.frag.pixel_imaps[VARYING] = {
            PixelImapType::Perspective, PixelImapType::Perspective,
            PixelImapType::Perspective, PixelImapType::Perspective};
    GuestShaderState state{};
    state.depth_zero_to_one = true;
    state.color_target_data_types[0] = MTL::DataTypeFloat;
    attribute.size = engines::getFetchSize(attribute.size);
    state.vertex_attrib_states[ATTRIBUTE] = attribute;
    ir::Module module;
    ir::Builder builder(module);
    if (type == ShaderType::Vertex) {
        const auto vid = ir::Value::createAttrMemory({RZ, SV_VERTEX_ID_BASE, true});
        for (u32 c = 0; c < 4; ++c) {
            auto position = ir::Value::createConstantF(c == 3 ? 1.0f : 0.0f);
            if (c < 2)
                position = builder.opSelect(
                    builder.opCompareEqual(vid, ir::Value::createConstant(c + 1)),
                    ir::Value::createConstantF(3.0f),
                    ir::Value::createConstantF(-1.0f));
            builder.opCopy(ir::Value::createAttrMemory(
                               {RZ, SV_POSITION_BASE + c * 4, false}, ir::ScalarType::F32),
                           position);
            auto scalar = ir::ScalarType::F32;
            if (attribute.type == engines::VertexAttribType::Uint)
                scalar = ir::ScalarType::U32;
            else if (attribute.type == engines::VertexAttribType::Sint)
                scalar = ir::ScalarType::I32;
            auto input = ir::Value::createAttrMemory(
                {RZ, SV_USER_IN_OUT_BASE + ATTRIBUTE * 16 + c * 4, true}, scalar);
            if (scalar != ir::ScalarType::F32)
                input = builder.opCast(input, ir::ScalarType::F32);
            builder.opCopy(ir::Value::createAttrMemory(
                               {RZ, SV_USER_IN_OUT_BASE + VARYING * 16 + c * 4, false},
                               ir::ScalarType::F32), input);
        }
    } else {
        for (u32 c = 0; c < 4; ++c)
            builder.opCopy(ir::Value::createRegister(c, ir::ScalarType::F32),
                           ir::Value::createAttrMemory(
                               {RZ, SV_USER_IN_OUT_BASE + VARYING * 16 + c * 4, true},
                               ir::ScalarType::F32));
    }
    builder.opExit();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    ShaderDescriptor descriptor{};
    descriptor.type = type;
    descriptor.backend = ShaderBackend::Msl;
    codegen::lang::msl::MslEmitter emitter(
        context, memory, state, descriptor.code, descriptor.resource_mapping);
    emitter.emit(module);
    return std::unique_ptr<ShaderBase>(renderer.createShader(descriptor));
}

struct Program {
    std::unique_ptr<ShaderBase> vertex;
    std::unique_ptr<PipelineBase> pipeline;
};
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("usage: cases.txt out.txt");
        setenv("HYDRA_GPU_SYNC", "1", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        metal::Renderer renderer;
        auto* device = renderer.getDevice();
        NS_STACK_SCOPED auto queue = device->newCommandQueue();
        const auto fragment = makeShader(renderer, ShaderType::Fragment, {});
        std::map<u32, Program> programs;
        NS_STACK_SCOPED auto td = MTL::TextureDescriptor::alloc()->init();
        td->setPixelFormat(MTL::PixelFormatRGBA32Float);
        td->setWidth(1);
        td->setHeight(1);
        td->setStorageMode(MTL::StorageModeShared);
        td->setUsage(MTL::TextureUsageRenderTarget);
        NS_STACK_SCOPED auto texture = device->newTexture(td);
        std::ifstream input(argv[1]);
        std::ofstream output(argv[2]);
        if (!input || !output)
            throw std::runtime_error("cannot open case files");
        std::string line;
        while (std::getline(input, line)) {
            NS_STACK_SCOPED auto case_pool = NS::AutoreleasePool::alloc()->init();
            std::istringstream fields(line);
            u32 word;
            std::string hex;
            if (!(fields >> std::hex >> word >> hex) || hex.size() != 32)
                throw std::runtime_error("invalid case: " + line);
            auto attribute = std::bit_cast<engines::VertexAttribState>(word);
            if (attribute.offset > 64 ||
                attribute.buffer_id >= VERTEX_ARRAY_COUNT)
                throw std::runtime_error("attribute outside test buffer");
            std::array<u8, 96> bytes{};
            for (usize i = 0; i < 16; ++i)
                bytes[attribute.offset + i] = static_cast<u8>(
                    std::stoul(hex.substr(i * 2, 2), nullptr, 16));
            auto it = programs.find(word);
            if (it == programs.end()) {
                Program program;
                program.vertex =
                    makeShader(renderer, ShaderType::Vertex, attribute);
                PipelineDescriptor descriptor{};
                descriptor.shaders[static_cast<u32>(ShaderType::Vertex)] = program.vertex.get();
                descriptor.shaders[static_cast<u32>(ShaderType::Fragment)] = fragment.get();
                attribute.size = engines::getFetchSize(attribute.size);
                descriptor.vertex_state.vertex_attrib_states[ATTRIBUTE] = attribute;
                descriptor.vertex_state.vertex_arrays[attribute.buffer_id].enable = true;
                // Constant stepping gives every vertex the same input bytes.
                descriptor.vertex_state.vertex_arrays[attribute.buffer_id].stride = 0;
                descriptor.color_target_states[0].format = TextureFormat::RGBA32Float;
                descriptor.color_target_states[0].write_mask = engines::ColorWriteMask::All;
                program.pipeline.reset(renderer.createPipeline(descriptor));
                it = programs.emplace(word, std::move(program)).first;
            }
            auto* pipeline =
                static_cast<metal::Pipeline*>(it->second.pipeline.get())
                    ->getPipeline();
            if (!pipeline)
                throw std::runtime_error("pipeline failed: " + line);
            auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* attachment = pass->colorAttachments()->object(0);
            attachment->setTexture(texture);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setStoreAction(MTL::StoreActionStore);
            attachment->setClearColor(MTL::ClearColor(-99, -99, -99, -99));
            auto commands = std::make_unique<metal::CommandBuffer>(queue);
            auto* encoder = commands->getRenderCommandEncoder(pass);
            commands->setRenderPipelineState(pipeline);
            encoder->setVertexBytes(bytes.data(), bytes.size(),
                                    metal::getVertexBufferIndex(attribute.buffer_id));
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                    NS::UInteger(3));
            auto* native = commands->getCommandBuffer()->retain();
            commands.reset();
            const bool ok = native->status() == MTL::CommandBufferStatusCompleted;
            native->release();
            if (!ok)
                throw std::runtime_error("submission failed: " + line);
            std::array<u8, 16> result{};
            texture->getBytes(result.data(), result.size(),
                              MTL::Region::Make2D(0, 0, 1, 1), 0);
            for (u8 byte : result)
                output << std::hex << std::setfill('0') << std::setw(2)
                       << static_cast<u32>(byte);
            output << '\n';
        }
        if (!input.eof() || !output)
            throw std::runtime_error("case file read/write failed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
