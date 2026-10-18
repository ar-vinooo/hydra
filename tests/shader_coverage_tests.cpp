// GPU tests for shader stage I/O, special registers and instruction families.
// Shaders are built from decoded guest instructions (or IR for the parts that
// are not under test), emitted as MSL and executed on Metal.
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/analyzer/memory_analyzer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/codegen/lang/msl/emitter.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/decoder.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/memory.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decoder/move.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <cfenv>
#include <cmath>
#include <set>
#include <iostream>
#include <stdexcept>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;
using hydra::hw::tegra_x1::gpu::renderer::metal::toNsString;

// Runs the blocks of main, each returns the next one
std::string runProgramSource(const ir::Module& module) {
    std::string source = "\ninline void run_program(thread State& state) { auto next = Block_main::label0x0; uint guard=0; while(next != Block_main::None && guard++ < 4096u) { switch(next) {";
    for (const auto& [label, block] :
         const_cast<ir::Module&>(module).getFunction("main").getBlocks()) {
        (void)block;
        source += fmt::format("case Block_main::{}: next=func_{}(state); break;", label, label);
    }
    source += "default: return; } } }\n";
    return source;
}

namespace {

void expect(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string emitMsl(const DecompilerContext& context,
                    const GuestShaderState& state, const ir::Module& module,
                    ResourceMapping* out_mapping = nullptr) {
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    std::vector<u8> generated;
    ResourceMapping mapping;
    codegen::lang::msl::MslEmitter emitter(context, memory, state, generated,
                                           mapping);
    emitter.emit(module);
    if (out_mapping != nullptr)
        *out_mapping = mapping;
    return std::string(generated.begin(), generated.end()) +
           runProgramSource(module);
}

MTL::Library* compileLibrary(MTL::Device* device, const std::string& source,
                             const char* what) {
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto options = MTL::CompileOptions::alloc()->init();
    options->setFastMathEnabled(false);
    auto* library = device->newLibrary(toNsString(source), options, &error);
    if (library == nullptr) {
        if (error != nullptr)
            std::cerr << error->localizedDescription()->utf8String() << '\n';
        std::cerr << source << '\n';
    }
    expect(library != nullptr, what);
    return library;
}

// Fullscreen triangle positions from the vertex ID
void writeFullscreenPosition(ir::Builder& builder) {
    const auto vid = ir::Value::createAttrMemory({RZ, SV_VERTEX_ID_BASE, true});
    for (u32 component = 0; component < 2; ++component) {
        const auto condition =
            builder.opCompareEqual(vid, ir::Value::createConstant(component + 1));
        builder.opCopy(ir::Value::createAttrMemory(
                           {RZ, SV_POSITION_BASE + component * 4, false},
                           ir::ScalarType::F32),
                       builder.opSelect(condition, ir::Value::createConstantF(3.0f),
                                        ir::Value::createConstantF(-1.0f)));
    }
}

struct RenderSetup {
    MTL::Function* vertex{nullptr};
    MTL::Function* fragment{nullptr};
    MTL::PixelFormat format{MTL::PixelFormatRGBA8Unorm};
    u32 width{8};
    u32 height{8};
    u32 bytes_per_pixel{4};
    std::optional<MTL::PrimitiveTopologyClass> topology;
    std::function<void(MTL::RenderPipelineColorAttachmentDescriptor*)> blend;
    std::function<void(MTL::RenderCommandEncoder*)> draw;
};

std::vector<u8> render(MTL::Device* device, const RenderSetup& setup,
                       const char* what) {
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto descriptor =
        MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(setup.vertex);
    descriptor->setFragmentFunction(setup.fragment);
    descriptor->colorAttachments()->object(0)->setPixelFormat(setup.format);
    if (setup.topology)
        descriptor->setInputPrimitiveTopology(*setup.topology);
    if (setup.blend)
        setup.blend(descriptor->colorAttachments()->object(0));
    NS_STACK_SCOPED auto pipeline =
        device->newRenderPipelineState(descriptor, &error);
    if (pipeline == nullptr && error != nullptr)
        std::cerr << error->localizedDescription()->utf8String() << '\n';
    expect(pipeline != nullptr, what);

    auto* texture_descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        setup.format, setup.width, setup.height, false);
    texture_descriptor->setStorageMode(MTL::StorageModeShared);
    texture_descriptor->setUsage(MTL::TextureUsageRenderTarget);
    NS_STACK_SCOPED auto texture = device->newTexture(texture_descriptor);
    auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* color = pass->colorAttachments()->object(0);
    color->setTexture(texture);
    color->setLoadAction(MTL::LoadActionClear);
    color->setStoreAction(MTL::StoreActionStore);
    color->setClearColor(MTL::ClearColor(0, 0, 0, 0));
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->renderCommandEncoder(pass);
    encoder->setRenderPipelineState(pipeline);
    setup.draw(encoder);
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    expect(commands->status() == MTL::CommandBufferStatusCompleted, what);
    std::vector<u8> pixels(usize(setup.width) * setup.height *
                           setup.bytes_per_pixel);
    texture->getBytes(pixels.data(), setup.width * setup.bytes_per_pixel,
                      MTL::Region::Make2D(0, 0, setup.width, setup.height), 0);
    return pixels;
}

template <typename T>
T pixelAt(const std::vector<u8>& pixels, u32 width, u32 x, u32 y,
          u32 component = 0, u32 components = 1) {
    T value;
    std::memcpy(&value,
                pixels.data() +
                    ((usize(y) * width + x) * components + component) * sizeof(T),
                sizeof(T));
    return value;
}

// Runs a compute kernel appended to a decompiled compute shader. The kernel
// receives an output buffer at index 0.
std::vector<u32> runKernel(MTL::Device* device, const std::string& source,
                           const char* kernel, u32 output_words,
                           const char* what) {
    NS_STACK_SCOPED auto library = compileLibrary(device, source, what);
    NS_STACK_SCOPED auto function = library->newFunction(toNsString(kernel));
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    expect(pipeline != nullptr, what);
    NS_STACK_SCOPED auto output = device->newBuffer(
        output_words * sizeof(u32), MTL::ResourceStorageModeShared);
    std::memset(output->contents(), 0xcd, output_words * sizeof(u32));
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->computeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    encoder->setBuffer(output, 0, 0);
    encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    expect(commands->status() == MTL::CommandBufferStatusCompleted, what);
    const auto* words = static_cast<const u32*>(output->contents());
    return std::vector<u32>(words, words + output_words);
}


// Decodes a straight-line guest program (scheduling words are inserted) and
// runs it once per input set. Registers R0-R31, predicates P0-P6 and the
// condition flags are loaded from each input and returned after execution.
struct AluState {
    std::array<u32, 32> regs{};
    u32 preds{0};
    u32 cc{0};
};

std::vector<AluState> runAlu(MTL::Device* device, const std::vector<u64>& insts,
                             const std::vector<AluState>& inputs,
                             const std::vector<u32>& cbuf, u32 cbuf_slot,
                             const char* what, u64 code_offset = 0,
                             std::vector<u32>* storage = nullptr) {
    std::vector<u64> words;
    for (const auto inst : insts) {
        if (words.size() % 4 == 0)
            words.push_back(0);
        words.push_back(inst);
    }
    if (words.size() % 4 == 0)
        words.push_back(0);
    words.push_back(0xe30000000007000full); // EXIT
    std::vector<u8> bytes(words.size() * 8);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    DecompilerContext context{};
    context.type = ShaderType::Compute;
    context.read_const_word = [&](u32 slot, u32 word) -> std::optional<u32> {
        if (slot != cbuf_slot || word >= cbuf.size())
            return std::nullopt;
        return cbuf[word];
    };
    ir::Module module;
    ir::Builder builder(module);
    decoder::Decoder decoder({context, &stream, builder, code_offset});
    decoder.decode();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    GuestShaderState state{};
    std::vector<u8> generated;
    ResourceMapping mapping;
    codegen::lang::msl::MslEmitter emitter(context, memory, state, generated,
                                           mapping);
    emitter.emit(module);
    std::string source(generated.begin(), generated.end());
    source += runProgramSource(module);

    std::string cbuf_assignments;
    for (const auto slot : memory.getConstBuffers()) {
        expect(slot == cbuf_slot, "Test program reads one constant buffer");
        cbuf_assignments += fmt::format("state.c{} = cb;", slot);
    }
    std::string storage_assignments;
    for (const auto& [binding, info] : memory.getStorageBuffers())
        storage_assignments += fmt::format("state.ssbo{}_{} = ssbo;",
                                           binding >> 16, binding & 0xffff);
    source += fmt::format(R"(
kernel void run_alu(device uint* out [[buffer(0)]], constant uint* in [[buffer(1)]],
                    constant Reg* cb [[buffer(2)]], device uint* ssbo [[buffer(3)]],
                    uint tid [[thread_position_in_grid]]) {{
    State state;
    constant uint* src = in + tid * 34u;
    for (uint i = 0; i < 32; i++) state.r[i]._u32 = src[i];
    for (uint i = 0; i < 8; i++) state.p[i] = ((src[32] >> i) & 1u) != 0u;
    state.p[7] = true;
    for (uint i = 0; i < 4; i++) state.cc[i] = ((src[33] >> i) & 1u) != 0u;
    {}{}
    run_program(state);
    device uint* dst = out + tid * 34u;
    for (uint i = 0; i < 32; i++) dst[i] = state.r[i]._u32;
    uint preds = 0u, cc = 0u;
    for (uint i = 0; i < 7; i++) preds |= state.p[i] ? (1u << i) : 0u;
    for (uint i = 0; i < 4; i++) cc |= state.cc[i] ? (1u << i) : 0u;
    dst[32] = preds;
    dst[33] = cc;
}}
)", cbuf_assignments, storage_assignments);
    NS_STACK_SCOPED auto library = compileLibrary(device, source, what);
    NS_STACK_SCOPED auto function = library->newFunction(toNsString("run_alu"));
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    expect(pipeline != nullptr, what);
    std::vector<u32> input_words;
    for (const auto& input : inputs) {
        input_words.insert(input_words.end(), input.regs.begin(), input.regs.end());
        input_words.push_back(input.preds);
        input_words.push_back(input.cc);
    }
    std::vector<u32> cbuf_words = cbuf;
    cbuf_words.resize(std::max<usize>(cbuf_words.size(), 64));
    NS_STACK_SCOPED auto in = device->newBuffer(
        input_words.data(), input_words.size() * 4, MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto cb = device->newBuffer(
        cbuf_words.data(), cbuf_words.size() * 4, MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto out = device->newBuffer(
        input_words.size() * 4, MTL::ResourceStorageModeShared);
    std::vector<u32> dummy_storage(4);
    auto& storage_words = storage != nullptr ? *storage : dummy_storage;
    NS_STACK_SCOPED auto ssbo = device->newBuffer(
        storage_words.data(), std::max<usize>(storage_words.size(), 1) * 4,
        MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->computeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    encoder->setBuffer(out, 0, 0);
    encoder->setBuffer(in, 0, 1);
    encoder->setBuffer(cb, 0, 2);
    encoder->setBuffer(ssbo, 0, 3);
    encoder->dispatchThreads(MTL::Size(inputs.size(), 1, 1), MTL::Size(1, 1, 1));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    expect(commands->status() == MTL::CommandBufferStatusCompleted, what);
    std::memcpy(storage_words.data(), ssbo->contents(), storage_words.size() * 4);
    std::vector<AluState> results(inputs.size());
    const auto* words_out = static_cast<const u32*>(out->contents());
    for (usize i = 0; i < inputs.size(); ++i) {
        std::memcpy(results[i].regs.data(), words_out + i * 34, 32 * 4);
        results[i].preds = words_out[i * 34 + 32];
        results[i].cc = words_out[i * 34 + 33];
    }
    return results;
}

u64 ipa(u32 dst, u32 attr, decoder::IpaOp op = decoder::IpaOp::Pass) {
    return 0xe000000000000000ull | dst | (u64(RZ) << 8) | (7ull << 16) |
           (u64(RZ) << 20) | (u64(attr) << 28) | (u64(RZ) << 39) |
           (7ull << 47) | (u64(op) << 54);
}

const char* const VERTEX_PASSTHROUGH = R"(
struct HydraTestVertexOut { float4 position [[position]]; };
vertex HydraTestVertexOut test_vertex(uint id [[vertex_id]],
                                      constant float4* positions [[buffer(0)]]) {
    return {positions[id]};
}
)";

// Front facing reads as an all ones mask through IPA, for either winding
void testFrontFacing(MTL::Device* device) {
    DecompilerContext context{};
    context.type = ShaderType::Fragment;
    GuestShaderState guest{};
    guest.color_target_data_types[0] = MTL::DataTypeUInt;
    ir::Module module;
    ir::Builder builder(module);
    decoder::DecoderContext decoder_context{context, nullptr, builder};
    decoder::emitIpa(decoder_context,
                     std::bit_cast<decoder::InstIpa>(ipa(0, SV_FRONT_FACING_BASE)));
    for (u8 i = 1; i < 4; ++i)
        builder.opCopy(ir::Value::createRegister(i), ir::Value::createConstantU(0));
    builder.opExit();
    std::string source = emitMsl(context, guest, module);
    source += VERTEX_PASSTHROUGH;
    source += R"(
fragment uint4 reference_fragment(bool front [[front_facing]]) {
    return uint4(front ? 0xffffffffu : 0u, 0, 0, 0);
}
)";
    NS_STACK_SCOPED auto library =
        compileLibrary(device, source, "Front facing shader compiles");
    NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("test_vertex"));
    NS_STACK_SCOPED auto guest_fragment = library->newFunction(toNsString("main_"));
    NS_STACK_SCOPED auto reference =
        library->newFunction(toNsString("reference_fragment"));
    // Left half counter clockwise, right half clockwise in clip space
    const std::array<float, 24> positions{
        -1, -1, 0, 1, 0, -1, 0, 1, -1, 1, 0, 1,
        0,  -1, 0, 1, 0, 1,  0, 1, 1,  -1, 0, 1};
    for (const auto winding :
         {MTL::WindingCounterClockwise, MTL::WindingClockwise}) {
        RenderSetup setup{.vertex = vertex,
                          .format = MTL::PixelFormatR32Uint,
                          .width = 8,
                          .height = 8,
                          .bytes_per_pixel = 4};
        setup.draw = [&](MTL::RenderCommandEncoder* encoder) {
            encoder->setFrontFacingWinding(winding);
            encoder->setVertexBytes(positions.data(), sizeof(positions), 0);
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                    NS::UInteger(6));
        };
        setup.fragment = guest_fragment;
        const auto actual = render(device, setup, "Front facing draw");
        setup.fragment = reference;
        const auto expected = render(device, setup, "Reference front facing draw");
        u32 front = 0, back = 0;
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                const auto value = pixelAt<u32>(actual, 8, x, y);
                const auto native = pixelAt<u32>(expected, 8, x, y);
                expect(value == native,
                       "IPA front facing matches the native front facing mask");
                front += value == 0xffffffffu;
                back += native == 0 && pixelAt<u32>(actual, 8, x, y) == 0;
            }
        }
        expect(front > 0 && back > 0,
               "Both front and back facing triangles are covered");
    }
    std::cout << "PASS: IPA front facing mask for both windings\n";
}

// Points: the vertex shader exports the fixed point size, the fragment shader
// reads point coordinates with either origin
void testPointSizeAndCoord(MTL::Device* device) {
    for (const bool origin_bottom : {false, true}) {
        DecompilerContext vs_context{};
        vs_context.type = ShaderType::Vertex;
        GuestShaderState vs_state{};
        for (auto& attribute : vs_state.vertex_attrib_states)
            attribute.is_fixed = true;
        vs_state.depth_zero_to_one = true;
        vs_state.point_list = true;
        vs_state.point_size = 4.0f;
        ir::Module vs_module;
        ir::Builder vs_builder(vs_module);
        for (u32 c = 0; c < 2; ++c)
            vs_builder.opCopy(ir::Value::createAttrMemory(
                                  {RZ, SV_POSITION_BASE + c * 4, false},
                                  ir::ScalarType::F32),
                              ir::Value::createConstantF(0.0f));
        vs_builder.opExit();
        const auto vs_source = emitMsl(vs_context, vs_state, vs_module);

        DecompilerContext fs_context{};
        fs_context.type = ShaderType::Fragment;
        GuestShaderState fs_state{};
        fs_state.color_target_data_types[0] = MTL::DataTypeFloat;
        fs_state.point_coord_origin_bottom = origin_bottom;
        ir::Module fs_module;
        ir::Builder fs_builder(fs_module);
        decoder::DecoderContext decoder_context{fs_context, nullptr, fs_builder};
        decoder::emitIpa(decoder_context, std::bit_cast<decoder::InstIpa>(
                                              ipa(0, SV_POINT_COORD_BASE)));
        decoder::emitIpa(decoder_context, std::bit_cast<decoder::InstIpa>(
                                              ipa(1, SV_POINT_COORD_BASE + 4)));
        fs_builder.opCopy(ir::Value::createRegister(2, ir::ScalarType::F32),
                          ir::Value::createConstantF(1.0f));
        fs_builder.opCopy(ir::Value::createRegister(3, ir::ScalarType::F32),
                          ir::Value::createConstantF(1.0f));
        fs_builder.opExit();
        const auto fs_source = emitMsl(fs_context, fs_state, fs_module);

        NS_STACK_SCOPED auto vs_library =
            compileLibrary(device, vs_source, "Point vertex shader compiles");
        NS_STACK_SCOPED auto fs_library =
            compileLibrary(device, fs_source, "Point coord shader compiles");
        NS_STACK_SCOPED auto vertex = vs_library->newFunction(toNsString("main_"));
        NS_STACK_SCOPED auto fragment = fs_library->newFunction(toNsString("main_"));
        RenderSetup setup{.vertex = vertex,
                          .fragment = fragment,
                          .format = MTL::PixelFormatRGBA32Float,
                          .width = 8,
                          .height = 8,
                          .bytes_per_pixel = 16};
        setup.draw = [](MTL::RenderCommandEncoder* encoder) {
            encoder->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0),
                                    NS::UInteger(1));
        };
        const auto pixels = render(device, setup, "Point draw");
        u32 covered = 0;
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                const bool inside = x >= 2 && x < 6 && y >= 2 && y < 6;
                const auto alpha = pixelAt<f32>(pixels, 8, x, y, 3, 4);
                expect((alpha == 1.0f) == inside,
                       "Fixed point size covers 4x4 pixels");
                if (!inside)
                    continue;
                covered++;
                const auto s = pixelAt<f32>(pixels, 8, x, y, 0, 4);
                const auto t = pixelAt<f32>(pixels, 8, x, y, 1, 4);
                const f32 expected_s = (f32(x - 2) + 0.5f) / 4.0f;
                const f32 top_t = (f32(y - 2) + 0.5f) / 4.0f;
                const f32 expected_t = origin_bottom ? 1.0f - top_t : top_t;
                expect(std::abs(s - expected_s) < 1e-3f &&
                           std::abs(t - expected_t) < 1e-3f,
                       "Point coordinates follow the sprite origin");
            }
        }
        expect(covered == 16, "Point coverage");
    }
    std::cout << "PASS: fixed point size and point coordinates with top and "
                 "bottom origins\n";
}

std::string solidFragment() {
    return R"(
fragment float4 solid_fragment() { return float4(1.0); }
)";
}

// Clip distances clip only when enabled, unwritten enabled distances keep the
// primitive
void testClipDistances(MTL::Device* device) {
    struct Case {
        u32 mask;
        bool write;
        bool expect_clip;
    };
    for (const auto& c : {Case{1, true, true}, Case{0, true, false},
                          Case{1, false, false}, Case{0x80, true, false}}) {
        DecompilerContext context{};
        context.type = ShaderType::Vertex;
        GuestShaderState state{};
        for (auto& attribute : state.vertex_attrib_states)
            attribute.is_fixed = true;
        state.depth_zero_to_one = true;
        state.clip_distance_mask = c.mask;
        ir::Module module;
        ir::Builder builder(module);
        writeFullscreenPosition(builder);
        if (c.write)
            builder.opCopy(
                ir::Value::createAttrMemory({RZ, SV_CLIP_DISTANCE_BASE, false},
                                            ir::ScalarType::F32),
                ir::Value::createAttrMemory({RZ, SV_POSITION_BASE, false},
                                            ir::ScalarType::F32));
        builder.opExit();
        auto source = emitMsl(context, state, module) + solidFragment();
        NS_STACK_SCOPED auto library =
            compileLibrary(device, source, "Clip distance shader compiles");
        NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("main_"));
        NS_STACK_SCOPED auto fragment =
            library->newFunction(toNsString("solid_fragment"));
        RenderSetup setup{.vertex = vertex, .fragment = fragment};
        setup.draw = [](MTL::RenderCommandEncoder* encoder) {
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                    NS::UInteger(3));
        };
        const auto pixels = render(device, setup, "Clip distance draw");
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                const bool drawn = pixels[(y * 8 + x) * 4] == 255;
                const bool clipped = c.expect_clip && x < 4;
                expect(drawn == !clipped,
                       "Clip distances clip exactly when enabled and written");
            }
        }
    }
    std::cout << "PASS: clip distances follow the enable mask and shader "
                 "writes\n";
}

// The vertex shader selects the viewport and the render target layer
void testViewportAndLayerOutputs(MTL::Device* device) {
    {
        DecompilerContext context{};
        context.type = ShaderType::Vertex;
        GuestShaderState state{};
        for (auto& attribute : state.vertex_attrib_states)
            attribute.is_fixed = true;
        state.depth_zero_to_one = true;
        ir::Module module;
        ir::Builder builder(module);
        writeFullscreenPosition(builder);
        builder.opCopy(ir::Value::createAttrMemory({RZ, SV_VIEWPORT_INDEX_BASE, false}),
                       ir::Value::createConstantU(1));
        builder.opExit();
        ResourceMapping mapping;
        auto source = emitMsl(context, state, module, &mapping) + solidFragment();
        expect(mapping.writes_layer,
               "Viewport index output requires an input primitive topology");
        NS_STACK_SCOPED auto library =
            compileLibrary(device, source, "Viewport index shader compiles");
        NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("main_"));
        NS_STACK_SCOPED auto fragment =
            library->newFunction(toNsString("solid_fragment"));
        RenderSetup setup{.vertex = vertex,
                          .fragment = fragment,
                          .topology = MTL::PrimitiveTopologyClassTriangle};
        setup.draw = [](MTL::RenderCommandEncoder* encoder) {
            const std::array<MTL::Viewport, 2> viewports{
                MTL::Viewport{0, 0, 4, 8, 0, 1}, MTL::Viewport{4, 0, 4, 8, 0, 1}};
            encoder->setViewports(viewports.data(), viewports.size());
            const std::array<MTL::ScissorRect, 2> scissors{
                MTL::ScissorRect{0, 0, 4, 8}, MTL::ScissorRect{4, 0, 4, 8}};
            encoder->setScissorRects(scissors.data(), scissors.size());
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                    NS::UInteger(3));
        };
        const auto pixels = render(device, setup, "Viewport index draw");
        for (u32 y = 0; y < 8; ++y)
            for (u32 x = 0; x < 8; ++x)
                expect((pixels[(y * 8 + x) * 4] == 255) == (x >= 4),
                       "Vertex shader viewport index selects the viewport");
    }

    // Layer from the instance ID, which excludes the base instance: the
    // engine draws instance i with host base instance i
    constexpr u32 SLICES = 3;
    DecompilerContext context{};
    context.type = ShaderType::Vertex;
    GuestShaderState state{};
    for (auto& attribute : state.vertex_attrib_states)
        attribute.is_fixed = true;
    state.depth_zero_to_one = true;
    ir::Module module;
    ir::Builder builder(module);
    writeFullscreenPosition(builder);
    builder.opCopy(ir::Value::createAttrMemory({RZ, SV_LAYER_BASE, false}),
                   ir::Value::createAttrMemory({RZ, SV_INSTANCE_ID_BASE, true}));
    builder.opExit();
    auto source = emitMsl(context, state, module);
    source += "\nfragment float4 layer_fragment(uint layer "
              "[[render_target_array_index]]) { "
              "return float4(1, float(layer) / 255.0, 0, 1); }\n";
    NS_STACK_SCOPED auto library =
        compileLibrary(device, source, "Layer shader compiles");
    NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("main_"));
    NS_STACK_SCOPED auto fragment =
        library->newFunction(toNsString("layer_fragment"));
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto descriptor =
        MTL::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunction(vertex);
    descriptor->setFragmentFunction(fragment);
    descriptor->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassTriangle);
    descriptor->colorAttachments()->object(0)->setPixelFormat(
        MTL::PixelFormatRGBA8Unorm);
    NS_STACK_SCOPED auto pipeline =
        device->newRenderPipelineState(descriptor, &error);
    expect(pipeline != nullptr, "Layer pipeline compiles");
    NS_STACK_SCOPED auto texture_descriptor =
        MTL::TextureDescriptor::alloc()->init();
    texture_descriptor->setTextureType(MTL::TextureType2DArray);
    texture_descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    texture_descriptor->setWidth(4);
    texture_descriptor->setHeight(4);
    texture_descriptor->setArrayLength(SLICES);
    texture_descriptor->setStorageMode(MTL::StorageModeShared);
    texture_descriptor->setUsage(MTL::TextureUsageRenderTarget);
    NS_STACK_SCOPED auto texture = device->newTexture(texture_descriptor);
    auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* color = pass->colorAttachments()->object(0);
    color->setTexture(texture);
    color->setLoadAction(MTL::LoadActionClear);
    color->setStoreAction(MTL::StoreActionStore);
    pass->setRenderTargetArrayLength(SLICES);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->renderCommandEncoder(pass);
    encoder->setRenderPipelineState(pipeline);
    for (u32 instance = 0; instance < SLICES; ++instance)
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                NS::UInteger(3), NS::UInteger(1),
                                NS::UInteger(instance));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    expect(commands->status() == MTL::CommandBufferStatusCompleted,
           "Layer draw completes");
    for (u32 slice = 0; slice < SLICES; ++slice) {
        std::array<u8, 64> pixels{};
        texture->getBytes(pixels.data(), 16, 64,
                          MTL::Region::Make2D(0, 0, 4, 4), 0, slice);
        for (u32 pixel = 0; pixel < 16; ++pixel)
            expect(pixels[pixel * 4] == 255 && pixels[pixel * 4 + 1] == slice,
                   "Vertex shader layer output selects the array slice");
    }
    std::cout << "PASS: vertex shader viewport index and layer outputs\n";
}

// SR_Y_DIRECTION follows the window origin, SR_THREAD_KILL is zero outside of
// fragment shaders, unknown counters read zero
void testSpecialRegisters(MTL::Device* device) {
    for (const bool y_negate : {false, true}) {
        DecompilerContext context{};
        context.type = ShaderType::Compute;
        GuestShaderState state{};
        state.y_negate = y_negate;
        ir::Module module;
        ir::Builder builder(module);
        decoder::DecoderContext decoder_context{context, nullptr, builder};
        const std::array<u32, 4> registers{0x12, 0x13, 0x1d, 0x50};
        for (u32 i = 0; i < registers.size(); ++i)
            decoder::emitS2R(decoder_context,
                             std::bit_cast<decoder::InstS2R>(
                                 0xf0c8000000000000ull | (16 + i) |
                                 (7ull << 16) | (u64(registers[i]) << 20)));
        builder.opExit();
        auto source = emitMsl(context, state, module);
        source += R"(
kernel void check_sr(device uint* out [[buffer(0)]]) {
    State state;
    func_label0x0(state);
    for (uint i = 0; i < 4; i++) out[i] = state.r[16 + i]._u32;
}
)";
        const auto result = runKernel(device, source, "check_sr", 4,
                                      "Special register shader runs");
        expect(result[0] == (y_negate ? 0xbf800000u : 0x3f800000u),
               "SR_Y_DIRECTION is -1.0 for a lower left window origin");
        expect(result[1] == 0, "SR_THREAD_KILL is zero in compute shaders");
        expect(result[2] == 0x00ff0000u, "SR_INVOCATION_INFO default value");
        expect(result[3] == 0, "SR_CLOCKLO reads zero");
    }

    // Fragment shaders read the lane of their SIMD-group
    DecompilerContext context{};
    context.type = ShaderType::Fragment;
    GuestShaderState state{};
    state.color_target_data_types[0] = MTL::DataTypeUInt;
    ir::Module module;
    ir::Builder builder(module);
    decoder::DecoderContext decoder_context{context, nullptr, builder};
    decoder::emitS2R(decoder_context, std::bit_cast<decoder::InstS2R>(
                                          0xf0c8000000000000ull | 0 |
                                          (7ull << 16) | (0ull << 20)));
    decoder::emitS2R(decoder_context, std::bit_cast<decoder::InstS2R>(
                                          0xf0c8000000000000ull | 1 |
                                          (7ull << 16) | (0x38ull << 20)));
    builder.opCopy(ir::Value::createRegister(2), ir::Value::createConstantU(0));
    builder.opCopy(ir::Value::createRegister(3), ir::Value::createConstantU(0));
    builder.opExit();
    auto source = emitMsl(context, state, module) + VERTEX_PASSTHROUGH;
    NS_STACK_SCOPED auto library =
        compileLibrary(device, source, "Fragment lane ID shader compiles");
    NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("test_vertex"));
    NS_STACK_SCOPED auto fragment = library->newFunction(toNsString("main_"));
    const std::array<float, 12> positions{-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    RenderSetup setup{.vertex = vertex,
                      .fragment = fragment,
                      .format = MTL::PixelFormatRGBA32Uint,
                      .width = 8,
                      .height = 8,
                      .bytes_per_pixel = 16};
    setup.draw = [&](MTL::RenderCommandEncoder* encoder) {
        encoder->setVertexBytes(positions.data(), sizeof(positions), 0);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                NS::UInteger(3));
    };
    const auto pixels = render(device, setup, "Fragment lane ID draw");
    std::set<u32> lanes;
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const auto lane = pixelAt<u32>(pixels, 8, x, y, 0, 4);
            const auto mask = pixelAt<u32>(pixels, 8, x, y, 1, 4);
            expect(lane < 32 && mask == (1u << lane),
                   "Fragment lane ID and equal lane mask agree");
            lanes.insert(lane);
        }
    }
    expect(lanes.size() > 1, "Fragment lanes differ within a SIMD-group");
    std::cout << "PASS: SR_Y_DIRECTION, SR_THREAD_KILL, default special "
                 "registers and fragment lane IDs\n";
}

constexpr u64 PT_GUARD = 7ull << 16;

void testFlowConditions(MTL::Device* device) {
    const std::array<u32, 26> tests{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
                                    16,17,18,19,20,21,22,23,30,31};
    std::vector<AluState> inputs;
    for (u32 cc = 0; cc < 16; ++cc)
        for (u32 pred = 0; pred < 2; ++pred) {
            AluState input{};
            input.cc = cc;
            input.preds = pred;
            inputs.push_back(input);
        }
    for (u32 inv = 0; inv < 2; ++inv) {
        std::vector<u64> program;
        for (u32 i = 0; i < tests.size(); ++i) {
            program.push_back(0x0100000000000000ull | PT_GUARD | i);
            program.push_back(0xe240000000000000ull | (16ull << 20) |
                              (u64(inv) << 19) | tests[i]);
            program.push_back(0x0100000000000000ull | PT_GUARD | (1ull << 20) | i);
        }
        const auto results = runAlu(device, program, inputs, {}, 2, "CC flow branches");
        for (usize row = 0; row < inputs.size(); ++row) {
            const auto cc = inputs[row].cc;
            const bool z = (cc & 1) != 0, n = (cc & 2) != 0;
            const bool c = (cc & 4) != 0, o = (cc & 8) != 0;
            const std::array<bool, 26> expected{
                false, (n && !z) != o, !n && z, n != (z || o),
                ((!n) != o) && !z, !z, n == o, !n || !z, n && z,
                n != o, z, (n != o) || z, (!n) != (z || o), n || !z,
                ((!n) || z) != o, true, !o, !c, !n, z || !c,
                c && !z, n, c, o, n || z, !n && !z};
            for (usize i = 0; i < tests.size(); ++i) {
                const bool guard = (inputs[row].preds != 0) != (inv != 0);
                if (results[row].regs[i] != (guard && expected[i] ? 0u : 1u))
                    std::cerr << "flow=" << tests[i] << " cc=" << cc << " pred=" << inputs[row].preds << " inv=" << inv << " got=" << results[row].regs[i] << '\n';
                expect(results[row].regs[i] == (guard && expected[i] ? 0u : 1u),
                       "BRA combines flow test and predicate for every CC state");
            }
        }
    }
    // EXIT is conditional on CC even when guarded by PT.
    const auto exits = runAlu(device,
        {0xe30000000000000dull | PT_GUARD,
         0x0100000000000000ull | PT_GUARD | (1ull << 20) | 10ull},
        inputs, {}, 2, "CC conditional exit");
    for (usize i = 0; i < inputs.size(); ++i)
        expect(exits[i].regs[10] == (((inputs[i].cc & 2) || !(inputs[i].cc & 1)) ? 0u : 1u),
               "EXIT.NEU evaluates condition codes");
    std::cout << "PASS: all ordinary CC flow tests, predicated BRA and EXIT\n";
}

void testSetConditionFlags(MTL::Device* device) {
    std::vector<AluState> inputs;
    for (u32 equal = 0; equal < 2; ++equal)
        for (u32 pred = 0; pred < 2; ++pred) {
            AluState input{};
            input.regs[1] = std::bit_cast<u32>(1.0f);
            input.regs[2] = std::bit_cast<u32>(equal ? 1.0f : 2.0f);
            input.preds = pred;
            input.cc = 15;
            inputs.push_back(input);
        }
    for (u32 bf = 0; bf < 2; ++bf) {
        for (const auto op : {0x5800000000000000ull, 0x5b50000000000000ull,
                              0x5088000000000000ull}) {
            const bool pset = op == 0x5088000000000000ull;
            const u64 inst = pset
                ? op | PT_GUARD | 255ull | (7ull << 29) | (7ull << 39) |
                      (u64(bf) << 44) | (1ull << 47)
                : op | PT_GUARD | 255ull | (1ull << 8) | (2ull << 20) |
                      (7ull << 39) | (1ull << 47) |
                      (pset ? 0 : (op == 0x5800000000000000ull ? (2ull << 48) : (2ull << 49))) |
                      (u64(bf) << (op == 0x5800000000000000ull ? 52 : 44));
            const auto results = runAlu(device, {inst}, inputs, {}, 2, "SET.CC to RZ");
            for (usize i = 0; i < inputs.size(); ++i) {
                const bool value = pset ? inputs[i].preds != 0 : inputs[i].regs[1] == inputs[i].regs[2];
                expect(results[i].cc == (value ? (bf ? 0u : 2u) : 1u),
                       "FSET/ISET/PSET.CC writes Z/S and clears C/O even with RZ destination");
            }
        }
    }
    // Crash 4 idiom: FSET.EQ.CC RZ, R1, R2, PT; @P0 BRA CC.NEU.
    const auto results = runAlu(device,
        {0x5800000000000000ull | PT_GUARD | 255ull | (1ull << 8) |
             (2ull << 20) | (7ull << 39) | (1ull << 47) | (2ull << 48),
         0xe24000000000000dull | (16ull << 20),
         0x0100000000000000ull | PT_GUARD | (1ull << 20) | 10ull},
        inputs, {}, 2, "Crash CC branch idiom");
    for (usize i = 0; i < inputs.size(); ++i)
        expect(results[i].regs[10] ==
                   (inputs[i].preds && inputs[i].regs[1] == inputs[i].regs[2] ? 0u : 1u),
               "CC branch observes FSET result as well as P0");
    std::cout << "PASS: SET condition flags and Crash CC branch idiom\n";
}


u32 referencePrmt(u32 a, u32 b, u32 sel, u32 mode) {
    // Result byte order b0..b3 for each selector value, from the PTX ISA
    static constexpr u8 TABLES[6][4][4] = {
        {{0, 1, 2, 3}, {1, 2, 3, 4}, {2, 3, 4, 5}, {3, 4, 5, 6}}, // f4e
        {{0, 7, 6, 5}, {1, 0, 7, 6}, {2, 1, 0, 7}, {3, 2, 1, 0}}, // b4e
        {{0, 0, 0, 0}, {1, 1, 1, 1}, {2, 2, 2, 2}, {3, 3, 3, 3}}, // rc8
        {{0, 1, 2, 3}, {1, 1, 2, 3}, {2, 2, 2, 3}, {3, 3, 3, 3}}, // ecl
        {{0, 0, 0, 0}, {0, 1, 1, 1}, {0, 1, 2, 2}, {0, 1, 2, 3}}, // ecr
        {{0, 1, 0, 1}, {2, 3, 2, 3}, {0, 1, 0, 1}, {2, 3, 2, 3}}, // rc16
    };
    const u64 bytes = u64(a) | (u64(b) << 32);
    u32 result = 0;
    for (u32 i = 0; i < 4; ++i) {
        u32 byte;
        if (mode == 0) {
            const u32 nibble = (sel >> (i * 4)) & 0xf;
            byte = u32(bytes >> ((nibble & 7) * 8)) & 0xff;
            if (nibble & 8)
                byte = (byte & 0x80) ? 0xff : 0;
        } else {
            byte = u32(bytes >> (TABLES[mode - 1][sel & 3][i] * 8)) & 0xff;
        }
        result |= byte << (i * 8);
    }
    return result;
}

// PRMT in every mode and operand form
void testPrmt(MTL::Device* device) {
    const u32 selectors[] = {0x3210, 0x7654, 0x0123, 0x89ab, 0xcdef, 0x4c08,
                             0x1, 0x2, 0x3, 0xfffd};
    struct Case {
        u32 mode, form, sel_index;
    };
    std::vector<Case> cases;
    for (u32 mode = 0; mode < 7; ++mode)
        for (u32 form = 0; form < 4; ++form)
            cases.push_back({mode, form, (mode * 4 + form) % u32(std::size(selectors))});
    std::vector<u32> cbuf(64, 0);
    for (u32 i = 0; i < std::size(selectors); ++i)
        cbuf[i] = selectors[i];
    cbuf[0x10] = 0x89abcdefu;
    const std::array<std::array<u32, 2>, 3> operand_sets{
        std::array<u32, 2>{0x03020100u, 0x07060504u},
        std::array<u32, 2>{0x8091a2b3u, 0xc4d5e6f7u},
        std::array<u32, 2>{0xdeadbeefu, 0x12345678u}};
    // Results go to R10..R31, 22 cases per program
    for (usize chunk = 0; chunk < cases.size(); chunk += 22) {
        const usize end = std::min(cases.size(), chunk + 22);
        std::vector<u64> program;
        for (usize i = chunk; i < end; ++i) {
            const auto& c = cases[i];
            const u64 base = u64(10 + i - chunk) | (1ull << 8) | PT_GUARD |
                             (u64(c.mode) << 48);
            switch (c.form) {
            case 0: // Selector R2, high bytes R3
                program.push_back(0x5bc0000000000000ull | base | (2ull << 20) |
                                  (3ull << 39));
                break;
            case 1: // Immediate selector
                program.push_back(0x36c0000000000000ull | base |
                                  (u64(selectors[c.sel_index]) << 20) |
                                  (3ull << 39));
                break;
            case 2: // Constant selector c2[sel_index]
                program.push_back(0x4bc0000000000000ull | base |
                                  (u64(c.sel_index) << 20) | (2ull << 34) |
                                  (3ull << 39));
                break;
            default: // Selector R2, high bytes c2[0x10]
                program.push_back(0x53c0000000000000ull | base | (2ull << 39) |
                                  (0x10ull << 20) | (2ull << 34));
                break;
            }
        }
        std::vector<AluState> inputs;
        for (const auto& operands : operand_sets) {
            for (const auto sel : selectors) {
                AluState input{};
                input.regs[1] = operands[0];
                input.regs[2] = sel;
                input.regs[3] = operands[1];
                inputs.push_back(input);
            }
        }
        const auto results = runAlu(device, program, inputs, cbuf, 2, "PRMT runs");
        for (usize run = 0; run < inputs.size(); ++run) {
            for (usize i = chunk; i < end; ++i) {
                const auto& c = cases[i];
                const u32 selector = c.form == 0 || c.form == 3
                                         ? inputs[run].regs[2]
                                         : selectors[c.sel_index];
                const u32 high = c.form == 3 ? cbuf[0x10] : inputs[run].regs[3];
                const u32 expected =
                    referencePrmt(inputs[run].regs[1], high, selector, c.mode);
                const u32 actual = results[run].regs[10 + i - chunk];
                if (actual != expected)
                    std::cerr << fmt::format(
                        "PRMT mode {} form {} sel {:#x}: {:#x} != {:#x}\n", c.mode,
                        c.form, selector, actual, expected);
                expect(actual == expected,
                       "PRMT matches the PTX byte selection tables");
            }
        }
    }
    std::cout << "PASS: PRMT index/f4e/b4e/rc8/ecl/ecr/rc16 in register, "
                 "immediate, constant and register-constant forms\n";
}

// IMUL and IMUL32I low and high words with every signedness, and CC flags
void testImul(MTL::Device* device) {
    std::vector<u64> insts;
    struct Expect {
        u32 reg;
        bool high, signed_a, signed_b;
        u32 form; // 0 reg, 1 cbuf, 2 imm, 3 imm32
    };
    std::vector<Expect> expects;
    u32 reg = 10;
    for (u32 form = 0; form < 4; ++form) {
        for (u32 variant = 0; variant < 8 && reg < 31; ++variant) {
            const bool high = variant & 1, sa = variant & 2, sb = variant & 4;
            const u64 dst = u64(reg) | (1ull << 8) | PT_GUARD;
            switch (form) {
            case 0:
                insts.push_back(0x5c38000000000000ull | dst | (2ull << 20) |
                                (u64(high) << 39) | (u64(sa) << 40) | (u64(sb) << 41));
                break;
            case 1:
                insts.push_back(0x4c38000000000000ull | dst | (2ull << 34) |
                                (u64(high) << 39) | (u64(sa) << 40) | (u64(sb) << 41));
                break;
            case 2: // -3 as a sign extended 20-bit immediate
                insts.push_back(0x3838000000000000ull | dst |
                                (u64(0x7fffd) << 20) | (1ull << 56) |
                                (u64(high) << 39) | (u64(sa) << 40) | (u64(sb) << 41));
                break;
            case 3:
                insts.push_back(0x1f00000000000000ull | dst |
                                (u64(0x9abcdef0u) << 20) | (u64(high) << 53) |
                                (u64(sa) << 54) | (u64(sb) << 55));
                break;
            }
            expects.push_back({reg, high, sa, sb, form});
            reg++;
            if (form == 2 && variant == 3)
                break;
        }
    }
    // IMUL.HI.CC on R30 sets Z/S and clears C/O
    insts.push_back(0x5c38000000000000ull | 31 | (1ull << 8) | PT_GUARD |
                    (2ull << 20) | (1ull << 47));
    const std::vector<u32> cbuf(64, 0xfffffff0u);
    const u32 values[] = {0, 1, 3, 0x7fffffffu, 0x80000000u, 0xffffffffu,
                          0x12345678u, 0xdeadbeefu};
    std::vector<AluState> inputs;
    for (const auto a : values)
        for (const auto b : values) {
            AluState input{};
            input.regs[1] = a;
            input.regs[2] = b;
            inputs.push_back(input);
        }
    const auto results = runAlu(device, insts, inputs, cbuf, 2, "IMUL runs");
    usize run = 0;
    for (const auto a : values) {
        for (const auto b : values) {
            for (const auto& e : expects) {
                const u32 operand = e.form == 0 ? b
                                    : e.form == 1 ? 0xfffffff0u
                                    : e.form == 2 ? 0xfffffffdu
                                                  : 0x9abcdef0u;
                const i64 wide_a = e.signed_a ? i64(i32(a)) : i64(a);
                const i64 wide_b = e.signed_b ? i64(i32(operand)) : i64(operand);
                const u64 product = u64(wide_a) * u64(wide_b);
                const u32 expected = e.high ? u32(product >> 32) : u32(product);
                expect(results[run].regs[e.reg] == expected,
                       "IMUL matches 64-bit reference products");
            }
            const u32 low = a * b;
            const u32 cc = (low == 0 ? 1u : 0u) | ((low >> 31) << 1);
            expect(results[run].regs[31] == low && results[run].cc == cc,
                   "IMUL.CC writes zero and sign flags");
            run++;
        }
    }
    std::cout << "PASS: IMUL/IMUL32I low/high words, operand signedness, "
                 "register/constant/immediate forms and CC flags\n";
}

// P2R packs predicates or condition flags into a byte under a mask
void testP2r(MTL::Device* device) {
    std::vector<u64> insts;
    struct Expect {
        u32 reg, byte, form;
        bool cc;
    };
    std::vector<Expect> expects;
    u32 reg = 10;
    for (u32 form = 0; form < 3; ++form) {
        for (u32 byte = 0; byte < 4; ++byte) {
            // Results are limited to R10..R31
            if (form == 2 && (byte == 1 || byte == 2))
                continue;
            for (const bool cc : {false, true}) {
                const u64 base = u64(reg) | (1ull << 8) | PT_GUARD |
                                 (u64(cc) << 40) | (u64(byte) << 41);
                if (form == 0)
                    insts.push_back(0x5ce8000000000000ull | base | (2ull << 20));
                else if (form == 1)
                    insts.push_back(0x4ce8000000000000ull | base | (2ull << 34));
                else
                    insts.push_back(0x38e8000000000000ull | base | (0x5bull << 20));
                expects.push_back({reg, byte, form, cc});
                reg++;
            }
        }
    }
    const std::vector<u32> cbuf(64, 0x3du);
    std::vector<AluState> inputs;
    for (const u32 mask : {0x7fu, 0x15u, 0u})
        for (const u32 preds : {0x55u, 0x2au, 0x7fu})
            for (const u32 cc : {0x5u, 0xau}) {
                AluState input{};
                input.regs[1] = 0xa5a5a5a5u;
                input.regs[2] = mask;
                input.preds = preds;
                input.cc = cc;
                inputs.push_back(input);
            }
    const auto results = runAlu(device, insts, inputs, cbuf, 2, "P2R runs");
    for (usize run = 0; run < inputs.size(); ++run) {
        for (const auto& e : expects) {
            const u32 count = e.cc ? 4 : 7;
            const u32 raw_mask = e.form == 0 ? inputs[run].regs[2]
                                 : e.form == 1 ? 0x3du
                                               : 0x5bu;
            const u32 mask = (raw_mask & ((1u << count) - 1)) << (e.byte * 8);
            const u32 source = e.cc ? inputs[run].cc : inputs[run].preds;
            const u32 expected = (0xa5a5a5a5u & ~mask) |
                                 ((source << (e.byte * 8)) & mask);
            if (results[run].regs[e.reg] != expected)
                std::cerr << fmt::format(
                    "P2R form {} byte {} cc {} mask {:#x} source {:#x}: {:#x} != "
                    "{:#x}\n",
                    e.form, e.byte, e.cc, raw_mask, source,
                    results[run].regs[e.reg], expected);
            expect(results[run].regs[e.reg] == expected,
                   "P2R inserts masked predicate or flag bits into the byte");
        }
    }
    std::cout << "PASS: P2R predicate and CC modes, byte selectors, register/"
                 "constant/immediate masks\n";
}

f32 halfToFloat(u16 h) {
    const u32 sign = u32(h >> 15) << 31;
    u32 exponent = (h >> 10) & 0x1f;
    u32 mantissa = h & 0x3ff;
    if (exponent == 0x1f)
        return std::bit_cast<f32>(sign | 0x7f800000u | (mantissa << 13));
    if (exponent == 0) {
        if (mantissa == 0)
            return std::bit_cast<f32>(sign);
        return (sign ? -1.0f : 1.0f) * std::ldexp(f32(mantissa), -24);
    }
    return std::bit_cast<f32>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

bool referenceFloatCompare(u32 op, f32 a, f32 b) {
    const bool unordered = std::isnan(a) || std::isnan(b);
    switch (op) {
    case 0: return false;
    case 1: return !unordered && a < b;
    case 2: return !unordered && a == b;
    case 3: return !unordered && a <= b;
    case 4: return !unordered && a > b;
    case 5: return !unordered && a != b;
    case 6: return !unordered && a >= b;
    case 7: return !unordered;
    case 8: return unordered;
    case 9: return unordered || a < b;
    case 10: return unordered || a == b;
    case 11: return unordered || a <= b;
    case 12: return unordered || a > b;
    case 13: return unordered || a != b;
    case 14: return unordered || a >= b;
    default: return true;
    }
}

// HSET2/HSETP2: swizzles, F32 constant operands, immediates, .BF, .H_AND,
// predicate combination and NaN ordering
void testHalfCompare(MTL::Device* device) {
    struct Case {
        u64 inst;
        u32 form;     // 0 reg, 1 cbuf, 2 imm
        bool predicate;
        u32 op, swizzle_a, swizzle_b, bop;
        bool bf, h_and, neg_b;
        u32 dst;
    };
    std::vector<Case> cases;
    u32 reg = 10, pred_slot = 0;
    const auto add = [&](u32 form, bool predicate, u32 op, u32 swz_a, u32 swz_b,
                         u32 bop, bool flag, bool neg_b) {
        u64 inst = (1ull << 8) | PT_GUARD | (u64(swz_a) << 47) |
                   (u64(bop) << 45) | (6ull << 39); // combine with P6
        if (form == 0)
            inst |= (2ull << 20) | (u64(swz_b) << 28) | (u64(neg_b) << 31) |
                    (u64(op) << 35) | (u64(flag) << 49);
        else
            inst |= (u64(op) << 49) | (u64(flag) << 53);
        if (form == 1)
            inst |= (u64(3) << 20) | (2ull << 34) | (u64(neg_b) << 56);
        if (form == 2) // 2.5 low (0x4100), -0.5 high (0xb800)
            inst |= (0x104ull << 20) | (0xe0ull << 30) | (1ull << 56);
        u32 dst;
        if (predicate) {
            dst = pred_slot;
            inst |= u64(pred_slot) | (u64(pred_slot + 1) << 3);
            inst |= form == 0 ? 0x5d20000000000000ull
                    : form == 1 ? 0x7e80000000000000ull
                                : 0x7e00000000000000ull;
            pred_slot += 2;
        } else {
            dst = reg++;
            inst |= dst;
            inst |= form == 0 ? 0x5d18000000000000ull
                    : form == 1 ? 0x7c80000000000000ull
                                : 0x7c00000000000000ull;
        }
        cases.push_back({inst, form, predicate, op, swz_a, swz_b, bop, flag,
                         predicate && flag, neg_b, dst});
    };
    // HSET2 register forms over all swizzles and several comparisons
    for (u32 swz_a = 0; swz_a < 4; ++swz_a)
        for (u32 swz_b = 0; swz_b < 4; ++swz_b)
            add(0, false, (swz_a * 4 + swz_b) % 16, swz_a, swz_b,
                (swz_a + swz_b) % 3, (swz_a & 1) != 0, swz_b == 2);
    add(1, false, 1, 0, 1, 0, true, false);   // cbuf F32, .BF
    add(1, false, 13, 2, 1, 1, false, true);  // cbuf F32, NEU, -b
    add(2, false, 3, 0, 0, 0, false, false);  // immediate
    add(2, false, 9, 3, 0, 2, true, false);   // immediate, LTU, .BF
    // HSETP2: without and with .H_AND (P0..P5)
    add(0, true, 1, 0, 0, 0, false, false);
    add(0, true, 6, 2, 3, 1, true, false);
    add(1, true, 4, 0, 1, 0, false, false);
    std::vector<u64> insts;
    for (const auto& c : cases)
        insts.push_back(c.inst);

    const u32 cbuf_f32 = std::bit_cast<u32>(1.5f);
    std::vector<u32> cbuf(64, 0);
    cbuf[3] = cbuf_f32;
    const u32 halves[] = {0x3c00u /*1*/, 0x3e00u /*1.5*/, 0xc000u /*-2*/,
                          0x7e00u /*nan*/, 0x0001u /*denormal*/, 0x4100u /*2.5*/,
                          0xb800u /*-0.5*/, 0x0000u};
    std::vector<AluState> inputs;
    for (u32 i = 0; i < 8; ++i) {
        for (u32 j = 0; j < 8; ++j) {
            AluState input{};
            input.regs[1] = halves[i] | (u32(halves[(i + 3) % 8]) << 16);
            input.regs[2] = halves[j] | (u32(halves[(j + 5) % 8]) << 16);
            if ((i + j) % 5 == 0) // F32 operands for the F32 swizzle
                input.regs[2] = std::bit_cast<u32>(1.5f);
            input.preds = ((i + j) & 1) ? 0x40u : 0u;
            inputs.push_back(input);
        }
    }
    const auto results = runAlu(device, insts, inputs, cbuf, 2, "Half compares run");
    // Metal flushes subnormal F32 operands of comparisons even without fast
    // math (a host limitation); converted halves are always normal F32 values
    const auto host_f32 = [](u32 word) {
        return (word & 0x7f800000u) == 0 ? std::bit_cast<f32>(word & 0x80000000u)
                                         : std::bit_cast<f32>(word);
    };
    const auto extract = [&](u32 word, u32 swizzle) -> std::pair<f32, f32> {
        switch (swizzle) {
        case 0: return {halfToFloat(u16(word)), halfToFloat(u16(word >> 16))};
        case 1: return {host_f32(word), host_f32(word)};
        case 2: return {halfToFloat(u16(word)), halfToFloat(u16(word))};
        default: return {halfToFloat(u16(word >> 16)), halfToFloat(u16(word >> 16))};
        }
    };
    const u32 immediate = 0x4100u | (0xb800u << 16);
    for (usize run = 0; run < inputs.size(); ++run) {
        const bool p6 = (inputs[run].preds >> 6) & 1;
        for (const auto& c : cases) {
            auto [a0, a1] = extract(inputs[run].regs[1], c.swizzle_a);
            std::pair<f32, f32> bv;
            if (c.form == 0)
                bv = extract(inputs[run].regs[2], c.swizzle_b);
            else if (c.form == 1)
                bv = {1.5f, 1.5f};
            else
                bv = extract(immediate, 0);
            if (c.neg_b) {
                bv.first = -bv.first;
                bv.second = -bv.second;
            }
            const auto combine = [&](bool v) {
                return c.bop == 0 ? (v && p6) : c.bop == 1 ? (v || p6) : (v != p6);
            };
            const bool r0 = combine(referenceFloatCompare(c.op, a0, bv.first));
            const bool r1 = combine(referenceFloatCompare(c.op, a1, bv.second));
            if (c.predicate) {
                bool pa = r0, pb = r1;
                if (c.h_and) {
                    pa = r0 && r1;
                    pb = !pa;
                }
                const u32 bits = results[run].preds;
                expect(((bits >> (c.dst + 1)) & 1) == pa &&
                           ((bits >> c.dst) & 1) == pb,
                       "HSETP2 writes both half predicates (or their AND)");
            } else {
                const u32 one = c.bf ? 0x3c00u : 0xffffu;
                const u32 expected = (r0 ? one : 0u) | (r1 ? one << 16 : 0u);
                if (results[run].regs[c.dst] != expected)
                    std::cerr << fmt::format(
                        "HSET2 form {} op {} swz {}/{} bop {} a {:#x} b {:#x} p6 {}: "
                        "{:#x} != {:#x}\n", c.form, c.op, c.swizzle_a, c.swizzle_b,
                        c.bop, inputs[run].regs[1], inputs[run].regs[2], p6,
                        results[run].regs[c.dst], expected);
                expect(results[run].regs[c.dst] == expected,
                       "HSET2 writes per-half masks or 1.0 results");
            }
        }
    }
    std::cout << "PASS: HSET2/HSETP2 swizzles, F32 constant operands, "
                 "immediates, .BF, .H_AND, predicate combination and NaN\n";
}

// LDC small integers at every byte offset, with signed register offsets
void testLdcSmallIntegers(MTL::Device* device) {
    std::vector<u64> insts;
    struct Expect {
        u32 reg, size, offset;
    };
    std::vector<Expect> expects;
    u32 reg = 10;
    for (u32 size = 0; size < 4; ++size) {
        for (u32 offset = 0; offset < 4; ++offset) {
            if (size >= 2 && (offset & 1))
                continue;
            // c2[R1 + offset - 4], R1 = 8
            const u32 imm = (offset - 4) & 0xffff;
            insts.push_back(0xef90000000000000ull | reg | (1ull << 8) |
                            PT_GUARD | (u64(imm) << 20) | (2ull << 36) |
                            (u64(size) << 48));
            expects.push_back({reg, size, offset + 4});
            reg++;
        }
    }
    std::vector<u32> cbuf(64, 0);
    cbuf[1] = 0x80ff7f01u;
    cbuf[2] = 0x9a8b7c6du;
    AluState input{};
    input.regs[1] = 8;
    const auto results = runAlu(device, insts, {input}, cbuf, 2, "LDC runs");
    for (const auto& e : expects) {
        const u32 word = cbuf[1];
        const u32 shift = ((e.offset - 4) & (e.size < 2 ? 3 : 2)) * 8;
        u32 expected;
        switch (e.size) {
        case 0: expected = (word >> shift) & 0xff; break;
        case 1: expected = u32(i32(i8(word >> shift))); break;
        case 2: expected = (word >> shift) & 0xffff; break;
        default: expected = u32(i32(i16(word >> shift))); break;
        }
        expect(results[0].regs[e.reg] == expected,
               "LDC U8/S8/U16/S16 extract and extend the addressed bytes");
    }
    std::cout << "PASS: LDC U8/S8/U16/S16 byte offsets and negative register "
                 "offsets\n";
}

// The compiler's 32-bit multiply by a uniform: XMAD with a constant buffer
// operand (.MRG, .PSL) followed by XMAD.PSL.CBCC
void testXmadConstantMultiply(MTL::Device* device) {
    const auto xmad_cr = [](u32 dst, u32 a, u32 c, bool hilo_b, bool psl,
                            bool mrg) {
        return 0x4e00000000000000ull | dst | (u64(a) << 8) | PT_GUARD |
               (0ull << 20) | (2ull << 34) | (u64(c) << 39) |
               (u64(hilo_b) << 52) | (u64(psl) << 55) | (u64(mrg) << 56);
    };
    const u64 cbcc = 0x5b00000000000000ull | 10 | (1ull << 8) | PT_GUARD |
                     (3ull << 20) | (1ull << 35) | (1ull << 36) |
                     (2ull << 39) | (4ull << 50) | (1ull << 53);
    const std::vector<u64> program{
        xmad_cr(2, 1, 0xff, false, false, false), // R2 = a.lo * b.lo
        xmad_cr(3, 1, 0xff, true, false, true),   // R3 = merge(a.lo * b.hi, b.lo)
        cbcc,                                     // R10 = a * b
        xmad_cr(11, 1, 0xff, false, true, false), // R11 = (a.lo * b.lo) << 16
    };
    const u32 values[] = {0, 1, 0xffff, 0x10000, 0x12345678u, 0xdeadbeefu,
                          0x80000000u, 0xffffffffu};
    for (const u32 b : values) {
        std::vector<AluState> inputs;
        for (const u32 a : values) {
            AluState input{};
            input.regs[1] = a;
            inputs.push_back(input);
        }
        const auto results =
            runAlu(device, program, inputs, {b}, 2, "XMAD constant multiply runs");
        for (usize i = 0; i < inputs.size(); ++i) {
            const u32 a = inputs[i].regs[1];
            expect(results[i].regs[10] == a * b,
                   "XMAD/XMAD.MRG/XMAD.PSL.CBCC with a constant operand "
                   "multiplies 32-bit values");
            expect(results[i].regs[11] == ((a & 0xffff) * (b & 0xffff)) << 16,
                   "XMAD.PSL with a constant operand shifts the product");
        }
    }
    std::cout << "PASS: XMAD constant-buffer .MRG/.PSL in the 32-bit multiply "
                 "sequence\n";
}

struct TextureBinding {
    std::string declaration; // MSL type of the binding
    MTL::Texture* texture;
    MTL::SamplerState* sampler;
};

// Like runAlu, with the textures the program samples bound in binding order
std::vector<AluState> runTextureProgram(
    MTL::Device* device, const std::vector<u64>& insts,
    const std::vector<AluState>& inputs, const std::vector<u32>& cbuf,
    const std::vector<TextureBinding>& textures, const char* what,
    const GuestShaderState& state = {}) {
    std::vector<u64> words;
    for (const auto inst : insts) {
        if (words.size() % 4 == 0)
            words.push_back(0);
        words.push_back(inst);
    }
    if (words.size() % 4 == 0)
        words.push_back(0);
    words.push_back(0xe30000000007000full);
    std::vector<u8> bytes(words.size() * 8);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    DecompilerContext context{};
    context.type = ShaderType::Compute;
    ir::Module module;
    ir::Builder builder(module);
    decoder::Decoder decoder({context, &stream, builder});
    decoder.decode();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    std::vector<u8> generated;
    ResourceMapping mapping;
    codegen::lang::msl::MslEmitter emitter(context, memory, state, generated,
                                           mapping);
    emitter.emit(module);
    std::string source(generated.begin(), generated.end());
    source += runProgramSource(module);
    std::string arguments, assignments;
    for (const auto slot : memory.getConstBuffers())
        assignments += fmt::format("state.c{} = cb;", slot);
    usize texture_index = 0;
    expect(memory.getTextures().size() == textures.size(),
           "Test program samples the provided textures");
    for (const auto& [binding, info] : memory.getTextures()) {
        (void)info;
        arguments += fmt::format(", {} t{} [[texture({})]], sampler s{} [[sampler({})]]",
                                 textures[texture_index].declaration, binding,
                                 texture_index, binding, texture_index);
        assignments += fmt::format("state.tex{0} = t{0}; state.samplr{0} = s{0};",
                                   binding);
        texture_index++;
    }
    source += fmt::format(R"(
kernel void run_tex(device uint* out [[buffer(0)]], constant uint* in [[buffer(1)]],
                    constant Reg* cb [[buffer(2)]], uint tid [[thread_position_in_grid]]{}) {{
    State state;
    constant uint* src = in + tid * 34u;
    for (uint i = 0; i < 32; i++) state.r[i]._u32 = src[i];
    for (uint i = 0; i < 8; i++) state.p[i] = ((src[32] >> i) & 1u) != 0u;
    state.p[7] = true;
    {}
    func_label0x0(state);
    device uint* dst = out + tid * 34u;
    for (uint i = 0; i < 32; i++) dst[i] = state.r[i]._u32;
}}
)", arguments, assignments);
    NS_STACK_SCOPED auto library = compileLibrary(device, source, what);
    NS_STACK_SCOPED auto function = library->newFunction(toNsString("run_tex"));
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    expect(pipeline != nullptr, what);
    std::vector<u32> input_words;
    for (const auto& input : inputs) {
        input_words.insert(input_words.end(), input.regs.begin(), input.regs.end());
        input_words.push_back(input.preds);
        input_words.push_back(input.cc);
    }
    std::vector<u32> cbuf_words = cbuf;
    cbuf_words.resize(std::max<usize>(cbuf_words.size(), 64));
    NS_STACK_SCOPED auto in = device->newBuffer(
        input_words.data(), input_words.size() * 4, MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto cb = device->newBuffer(
        cbuf_words.data(), cbuf_words.size() * 4, MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto out = device->newBuffer(
        input_words.size() * 4, MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->computeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    encoder->setBuffer(out, 0, 0);
    encoder->setBuffer(in, 0, 1);
    encoder->setBuffer(cb, 0, 2);
    for (usize i = 0; i < textures.size(); ++i) {
        encoder->setTexture(textures[i].texture, i);
        encoder->setSamplerState(textures[i].sampler, i);
    }
    encoder->dispatchThreads(MTL::Size(inputs.size(), 1, 1), MTL::Size(1, 1, 1));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    expect(commands->status() == MTL::CommandBufferStatusCompleted, what);
    std::vector<AluState> results(inputs.size());
    const auto* words_out = static_cast<const u32*>(out->contents());
    for (usize i = 0; i < inputs.size(); ++i)
        std::memcpy(results[i].regs.data(), words_out + i * 34, 32 * 4);
    return results;
}

struct ImageBinding {
    TextureFormat format; // guest format
    std::string declaration;        // MSL type of the binding
    MTL::Texture* texture;
};

// Like runTextureProgram, with images in binding order
std::vector<AluState> runImageProgram(MTL::Device* device,
                                      const std::vector<u64>& insts,
                                      const std::vector<AluState>& inputs,
                                      const std::vector<ImageBinding>& images,
                                      const char* what) {
    std::vector<u64> words;
    for (const auto inst : insts) {
        if (words.size() % 4 == 0)
            words.push_back(0);
        words.push_back(inst);
    }
    if (words.size() % 4 == 0)
        words.push_back(0);
    words.push_back(0xe30000000007000full);
    std::vector<u8> bytes(words.size() * 8);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    DecompilerContext context{};
    context.type = ShaderType::Compute;
    ir::Module module;
    ir::Builder builder(module);
    decoder::Decoder decoder({context, &stream, builder});
    decoder.decode();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    GuestShaderState state{};
    usize binding_index = 0;
    for (const auto& [binding, info] : memory.getImages()) {
        (void)info;
        state.image_formats.emplace(binding, images[binding_index++].format);
    }
    std::vector<u8> generated;
    ResourceMapping mapping;
    codegen::lang::msl::MslEmitter emitter(context, memory, state, generated,
                                           mapping);
    emitter.emit(module);
    std::string source(generated.begin(), generated.end());
    source += runProgramSource(module);
    std::string arguments, assignments;
    binding_index = 0;
    expect(memory.getImages().size() == images.size(),
           "Test program accesses the provided images");
    for (const auto& [binding, info] : memory.getImages()) {
        (void)info;
        arguments += fmt::format(", {} i{} [[texture({})]]",
                                 images[binding_index].declaration, binding,
                                 binding_index);
        assignments += fmt::format("state.img{0} = i{0};", binding);
        binding_index++;
    }
    source += fmt::format(R"(
kernel void run_img(device uint* out [[buffer(0)]], constant uint* in [[buffer(1)]],
                    uint tid [[thread_position_in_grid]]{}) {{
    State state;
    constant uint* src = in + tid * 34u;
    for (uint i = 0; i < 32; i++) state.r[i]._u32 = src[i];
    for (uint i = 0; i < 8; i++) state.p[i] = ((src[32] >> i) & 1u) != 0u;
    state.p[7] = true;
    {}
    func_label0x0(state);
    device uint* dst = out + tid * 34u;
    for (uint i = 0; i < 32; i++) dst[i] = state.r[i]._u32;
}}
)", arguments, assignments);
    NS_STACK_SCOPED auto library = compileLibrary(device, source, what);
    NS_STACK_SCOPED auto function = library->newFunction(toNsString("run_img"));
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    expect(pipeline != nullptr, what);
    std::vector<u32> input_words;
    for (const auto& input : inputs) {
        input_words.insert(input_words.end(), input.regs.begin(), input.regs.end());
        input_words.push_back(input.preds);
        input_words.push_back(input.cc);
    }
    NS_STACK_SCOPED auto in = device->newBuffer(
        input_words.data(), input_words.size() * 4, MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto out = device->newBuffer(
        input_words.size() * 4, MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->computeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    encoder->setBuffer(out, 0, 0);
    encoder->setBuffer(in, 0, 1);
    for (usize i = 0; i < images.size(); ++i)
        encoder->setTexture(images[i].texture, i);
    encoder->dispatchThreads(MTL::Size(inputs.size(), 1, 1), MTL::Size(1, 1, 1));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    expect(commands->status() == MTL::CommandBufferStatusCompleted, what);
    std::vector<AluState> results(inputs.size());
    const auto* words_out = static_cast<const u32*>(out->contents());
    for (usize i = 0; i < inputs.size(); ++i)
        std::memcpy(results[i].regs.data(), words_out + i * 34, 32 * 4);
    return results;
}

MTL::Texture* createImage(MTL::Device* device, MTL::PixelFormat format, u32 width,
                          u32 height) {
    auto* descriptor =
        MTL::TextureDescriptor::texture2DDescriptor(format, width, height, false);
    descriptor->setStorageMode(MTL::StorageModeShared);
    descriptor->setUsage(MTL::TextureUsageShaderRead |
                         MTL::TextureUsageShaderWrite);
    return device->newTexture(descriptor);
}

// SULD/SUST with formatted channels and raw words of the texel size
void testSurfaceAccess(MTL::Device* device) {
    const auto suld = [](u32 dst, u32 coords, bool raw, u32 size_or_mask) {
        return 0xeb00000000000000ull | dst | (u64(coords) << 8) | PT_GUARD |
               (u64(size_or_mask) << 20) | (3ull << 33) | (1ull << 36) |
               (1ull << 51) | (u64(raw) << 52);
    };
    const auto sust = [](u32 data, u32 coords, bool raw, u32 size_or_mask) {
        return 0xeb20000000000000ull | data | (u64(coords) << 8) | PT_GUARD |
               (u64(size_or_mask) << 20) | (3ull << 33) | (1ull << 36) |
               (1ull << 51) | (u64(raw) << 52);
    };
    using F = TextureFormat;
    AluState input{};
    input.regs[1] = 2; // x
    input.regs[2] = 1; // y

    // RGBA8Unorm: raw word is the packed bytes, formatted channels are floats
    {
        NS_STACK_SCOPED auto image =
            createImage(device, MTL::PixelFormatRGBA8Unorm, 4, 4);
        std::array<u8, 64> texels{};
        for (u32 i = 0; i < 64; i++)
            texels[i] = u8(i * 5 + 3);
        image->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0, texels.data(), 16);
        input.regs[3] = 0x80ff0010u; // written to (3, 1) as bytes
        input.regs[4] = 3;
        input.regs[5] = 1;
        const auto result = runImageProgram(
            device,
            {suld(10, 1, true, 4), suld(12, 1, false, 0xf),
             sust(3, 4, true, 4)},
            {input}, {{F::RGBA8Unorm, "texture2d<float, access::read_write>", image}},
            "RGBA8 surface access runs")[0];
        const u32 texel = 1 * 16 + 2 * 4; // (2, 1)
        u32 expected_word;
        std::memcpy(&expected_word, &texels[texel], 4);
        expect(result.regs[10] == expected_word, "SULD.D.32 reads the texel bytes");
        for (u32 c = 0; c < 4; c++)
            expect(std::bit_cast<f32>(result.regs[12 + c]) ==
                       f32(texels[texel + c]) / 255.0f,
                   "SULD.P returns the channels as floats");
        std::array<u8, 4> stored{};
        image->getBytes(stored.data(), 16, MTL::Region::Make2D(3, 1, 1, 1), 0);
        expect(std::memcmp(stored.data(), &input.regs[3], 4) == 0,
               "SUST.D.32 stores the bytes of the word");
    }
    // RGBA16Float: two raw words
    {
        NS_STACK_SCOPED auto image =
            createImage(device, MTL::PixelFormatRGBA16Float, 4, 4);
        std::array<u16, 64> texels{};
        for (u32 i = 0; i < 64; i++)
            texels[i] = u16(0x3c00 + i * 0x20);
        image->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0, texels.data(), 32);
        input.regs[3] = 0x3c004000u;
        input.regs[4] = 0x42004400u;
        input.regs[5] = 0;
        input.regs[6] = 3;
        const auto result = runImageProgram(
            device, {suld(10, 1, true, 5), sust(3, 5, true, 5)}, {input},
            {{F::RGBA16Float, "texture2d<float, access::read_write>", image}},
            "RGBA16F surface access runs")[0];
        const u32 texel = 1 * 16 + 2 * 4;
        u32 expected[2];
        std::memcpy(expected, &texels[texel], 8);
        expect(result.regs[10] == expected[0] && result.regs[11] == expected[1],
               "SULD.D.64 reads two words");
        std::array<u16, 4> stored{};
        image->getBytes(stored.data(), 32, MTL::Region::Make2D(0, 3, 1, 1), 0);
        expect(std::memcmp(stored.data(), &input.regs[3], 8) == 0,
               "SUST.D.64 stores two words");
    }
    // R32Uint: raw and formatted reads are the value
    {
        NS_STACK_SCOPED auto image =
            createImage(device, MTL::PixelFormatR32Uint, 4, 4);
        std::array<u32, 16> texels{};
        for (u32 i = 0; i < 16; i++)
            texels[i] = 0x11110000u + i;
        image->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0, texels.data(), 16);
        const auto result = runImageProgram(
            device, {suld(10, 1, true, 4), suld(12, 1, false, 0x1)}, {input},
            {{F::R32Uint, "texture2d<uint, access::read_write>", image}},
            "R32 surface access runs")[0];
        expect(result.regs[10] == texels[1 * 4 + 2] &&
                   result.regs[12] == texels[1 * 4 + 2],
               "R32 raw and formatted loads return the value");
    }
    // SUATOM INC/DEC wrap around the operand, SURED drops the result
    {
        const auto suatom = [](u32 dst, u32 coords, u32 operand, u32 op) {
            return 0xea00000000000000ull | dst | (u64(coords) << 8) | PT_GUARD |
                   (u64(operand) << 20) | (u64(op) << 29) | (3ull << 33) |
                   (1ull << 36);
        };
        const auto sured = [](u32 operand, u32 coords, u32 op) {
            return 0xeb40000000000000ull | operand | (u64(coords) << 8) |
                   PT_GUARD | (u64(op) << 24) | (3ull << 33) | (1ull << 36) |
                   (1ull << 51);
        };
        struct Case {
            u32 op, old, operand, expected_new;
        };
        const Case cases[] = {
            {3, 3, 5, 4}, {3, 5, 5, 0}, {3, 7, 5, 0}, {4, 0, 5, 5},
            {4, 3, 5, 2}, {4, 9, 5, 5}, {0, 3, 7, 10}, {7, 0xf0, 0x3c, 0xcc},
        };
        for (const auto& c : cases) {
            NS_STACK_SCOPED auto image =
                createImage(device, MTL::PixelFormatR32Uint, 4, 4);
            std::array<u32, 16> texels{};
            texels[1 * 4 + 2] = c.old;
            image->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0,
                                 texels.data(), 16);
            AluState atomic_input = input;
            atomic_input.regs[3] = c.operand;
            const auto result = runImageProgram(
                device, {suatom(10, 1, 3, c.op), sured(3, 1, c.op)},
                {atomic_input},
                {{F::R32Uint, "texture2d<uint, access::read_write>", image}},
                "Surface atomics run")[0];
            u32 stored[16];
            image->getBytes(stored, 16, MTL::Region::Make2D(0, 0, 4, 4), 0);
            // SUATOM applied once, SURED once more
            expect(result.regs[10] == c.old,
                   "SUATOM returns the previous value");
            u32 after_second = c.expected_new;
            switch (c.op) {
            case 3:
                after_second = after_second >= c.operand ? 0 : after_second + 1;
                break;
            case 4:
                after_second = (after_second == 0 || after_second > c.operand)
                                   ? c.operand
                                   : after_second - 1;
                break;
            case 0:
                after_second += c.operand;
                break;
            case 7:
                after_second ^= c.operand;
                break;
            }
            expect(stored[1 * 4 + 2] == after_second,
                   "SUATOM and SURED apply wrapping increment, decrement, add "
                   "and xor");
        }
    }
    std::cout << "PASS: SULD/SUST formatted and raw accesses, SUATOM wrap and "
                 "SURED\n";
}

// 8x8 RGBA32F texture with 4 levels; level L holds (L + 1, x, y, 10 + L)
MTL::Texture* createMippedTexture(MTL::Device* device) {
    auto* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatRGBA32Float, 8, 8, true);
    descriptor->setStorageMode(MTL::StorageModeShared);
    descriptor->setUsage(MTL::TextureUsageShaderRead);
    auto* texture = device->newTexture(descriptor);
    for (u32 level = 0; level < 4; ++level) {
        const u32 size = 8 >> level;
        std::vector<f32> texels(size * size * 4);
        for (u32 y = 0; y < size; ++y)
            for (u32 x = 0; x < size; ++x) {
                auto* t = &texels[(y * size + x) * 4];
                t[0] = f32(level + 1);
                t[1] = f32(x);
                t[2] = f32(y);
                t[3] = f32(10 + level);
            }
        texture->replaceRegion(MTL::Region::Make2D(0, 0, size, size), level,
                               texels.data(), size * 16);
    }
    return texture;
}

MTL::SamplerState* createSampler(MTL::Device* device, bool compare = false) {
    NS_STACK_SCOPED auto descriptor = MTL::SamplerDescriptor::alloc()->init();
    descriptor->setMinFilter(MTL::SamplerMinMagFilterNearest);
    descriptor->setMagFilter(MTL::SamplerMinMagFilterNearest);
    descriptor->setMipFilter(MTL::SamplerMipFilterNearest);
    descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    if (compare)
        descriptor->setCompareFunction(MTL::CompareFunctionLess);
    return device->newSamplerState(descriptor);
}

u32 f32Bits(f32 value) { return std::bit_cast<u32>(value); }

// TXQ (bound) and TXQ.B (bindless) dimensions at a LOD and the level count
void testTextureQuery(MTL::Device* device) {
    NS_STACK_SCOPED auto texture = createMippedTexture(device);
    NS_STACK_SCOPED auto sampler = createSampler(device);
    const auto txq = [](u32 dst, u32 src, u32 mask, bool bindless) {
        return (bindless ? 0xdf50000000000000ull : 0xdf48000000000000ull) |
               dst | (u64(src) << 8) | PT_GUARD | (1ull << 22) |
               (u64(mask) << 31) | (bindless ? 0 : (u64(1) << 36));
    };
    // Bound texture at binding 1: R10..R13 = width, height, depth, levels at
    // LOD R1; R14, R15 = width, levels (packed)
    const std::vector<u64> bound{txq(10, 1, 0xf, false), txq(14, 1, 0x9, false)};
    for (const u32 lod : {0u, 1u, 3u}) {
        AluState input{};
        input.regs[1] = lod;
        const auto result = runTextureProgram(
            device, bound, {input}, {}, {{"texture2d<float>", texture, sampler}},
            "Bound TXQ runs")[0];
        const u32 size = 8 >> lod;
        expect(result.regs[10] == size && result.regs[11] == size &&
                   result.regs[12] == 1 && result.regs[13] == 4,
               "TXQ returns the size at the LOD and the level count");
        expect(result.regs[14] == size && result.regs[15] == 4,
               "TXQ packs enabled components into consecutive registers");
    }
    // Bindless: handle in R2 (loaded from c2[0x10]), LOD in R3
    const std::vector<u64> bindless{
        0x4c98000000070000ull | 2 | (2ull << 34) | (4ull << 20), // MOV R2, c2[0x10]
        txq(10, 2, 0x3, true)};
    std::vector<u32> cbuf(64, 0);
    AluState input{};
    input.regs[3] = 2;
    const auto result = runTextureProgram(
        device, bindless, {input}, cbuf, {{"texture2d<float>", texture, sampler}},
        "Bindless TXQ runs")[0];
    expect(result.regs[10] == 2 && result.regs[11] == 2,
           "TXQ.B reads the LOD after the handle register");
    std::cout << "PASS: TXQ/TXQ.B opcodes, LOD operand, level count and packed "
                 "results\n";
}

// TMML: the instruction decodes in every dimension, writes its fixed point
// results to consecutive registers and returns 0 where the host has no
// derivatives (compute); the fragment form is compiled by the corpus test
void testMipLevelQuery(MTL::Device* device) {
    NS_STACK_SCOPED auto texture = createMippedTexture(device);
    NS_STACK_SCOPED auto sampler = createSampler(device);
    const auto tmml = [](u32 dst, u32 src, u32 dim, u32 mask) {
        return 0xdf58000000000000ull | dst | (u64(src) << 8) | PT_GUARD |
               (u64(dim) << 28) | (u64(mask) << 31) | (u64(1) << 36);
    };
    AluState input{};
    for (u32 i = 0; i < 8; i++)
        input.regs[i] = std::bit_cast<u32>(0.25f * float(i + 1));
    for (u32 i = 10; i < 20; i++)
        input.regs[i] = 0xdeadbeefu;
    // 2D, 2D array and cube queries of both levels
    const auto result = runTextureProgram(
        device, {tmml(10, 1, 2, 0x3), tmml(12, 1, 3, 0x1), tmml(14, 1, 6, 0x2)},
        {input}, {}, {{"texture2d<float>", texture, sampler}},
        "TMML runs")[0];
    expect(result.regs[10] == 0 && result.regs[11] == 0 && result.regs[12] == 0 &&
               result.regs[14] == 0 && result.regs[13] == 0xdeadbeefu,
           "TMML writes only the enabled components, 0 without derivatives");

    // Fragment shaders have derivatives: the generated queries must compile
    // for every texture type
    for (const auto type :
         {shader_decomp::TextureType::_2D, shader_decomp::TextureType::_2DArray,
          shader_decomp::TextureType::_3D, shader_decomp::TextureType::Cube,
          shader_decomp::TextureType::CubeArray, shader_decomp::TextureType::_1D}) {
        DecompilerContext context{};
        context.type = ShaderType::Fragment;
        GuestShaderState guest{};
        guest.unnormalized_textures.push_back(1);
        guest.color_target_data_types[0] = MTL::DataTypeFloat;
        ir::Module module;
        ir::Builder builder(module);
        const u32 axes = type == shader_decomp::TextureType::_1D     ? 1
                         : (type == shader_decomp::TextureType::_2D ||
                            type == shader_decomp::TextureType::_2DArray)
                             ? 2
                             : 3;
        std::vector<ir::Value> values;
        for (u32 axis = 0; axis < axes; axis++)
            values.push_back(ir::Value::createConstantF(0.25f * float(axis + 1)));
        const auto coords = builder.opVectorConstruct(ir::ScalarType::F32, values);
        builder.opCopy(ir::Value::createRegister(0, ir::ScalarType::F32),
                       builder.opTextureQueryLod(1, type, coords, false));
        builder.opCopy(ir::Value::createRegister(1, ir::ScalarType::F32),
                       builder.opTextureQueryLod(1, type, coords, true));
        builder.opExit();
        const auto source = emitMsl(context, guest, module) + VERTEX_PASSTHROUGH;
        NS_STACK_SCOPED auto library =
            compileLibrary(device, source, "Fragment TMML compiles");
        (void)library;
    }
    std::cout << "PASS: TMML decoding and result registers\n";
}

// TEXS: 2D.LL.DC takes the LOD before the depth reference, depth compare
// results are (r, r, r, 1); F16 results are packed when bit 59 is clear
void testTexsVariants(MTL::Device* device) {
    NS_STACK_SCOPED auto sampler = createSampler(device);
    NS_STACK_SCOPED auto compare_sampler = createSampler(device, true);
    auto* depth_descriptor = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatDepth32Float, 4, 4, false);
    depth_descriptor->setStorageMode(MTL::StorageModePrivate);
    depth_descriptor->setUsage(MTL::TextureUsageShaderRead |
                               MTL::TextureUsageRenderTarget);
    NS_STACK_SCOPED auto depth = device->newTexture(depth_descriptor);
    {
        auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        pass->depthAttachment()->setTexture(depth);
        pass->depthAttachment()->setLoadAction(MTL::LoadActionClear);
        pass->depthAttachment()->setStoreAction(MTL::StoreActionStore);
        pass->depthAttachment()->setClearDepth(0.4);
        NS_STACK_SCOPED auto queue = device->newCommandQueue();
        auto* commands = queue->commandBuffer();
        commands->renderCommandEncoder(pass)->endEncoding();
        commands->commit();
        commands->waitUntilCompleted();
    }
    const auto texs = [](u32 encoding, u32 dst0, u32 dst1, u32 a, u32 b,
                         u32 mask, bool f32) {
        return 0xd000000000000000ull | dst0 | (u64(a) << 8) | PT_GUARD |
               (u64(b) << 20) | (u64(dst1) << 28) | (1ull << 36) |
               (u64(mask) << 50) | (u64(encoding) << 53) | (u64(f32) << 59);
    };
    // 2D.LL.DC: coords R4, R5; LOD R6; reference R7; RGBA to R10, R11, R12, R13
    const std::vector<u64> program{texs(5, 10, 12, 4, 6, 4, true)};
    std::vector<AluState> inputs;
    for (const f32 reference : {0.3f, 0.5f}) {
        AluState input{};
        input.regs[4] = f32Bits(0.5f);
        input.regs[5] = f32Bits(0.5f);
        input.regs[6] = f32Bits(0.0f);
        input.regs[7] = f32Bits(reference);
        inputs.push_back(input);
    }
    const auto results = runTextureProgram(
        device, program, inputs, {}, {{"depth2d<float>", depth, compare_sampler}},
        "TEXS 2D.LL.DC runs");
    for (u32 i = 0; i < 2; ++i) {
        const f32 r = i == 0 ? 1.0f : 0.0f;
        expect(results[i].regs[10] == f32Bits(r) && results[i].regs[11] == f32Bits(r) &&
                   results[i].regs[12] == f32Bits(r) &&
                   results[i].regs[13] == f32Bits(1.0f),
               "TEXS 2D.LL.DC compares the reference after the LOD into (r,r,r,1)");
    }

    // F16: 2D.LZ RGBA packed into R10 (r, g) and R11 (b, a)
    NS_STACK_SCOPED auto texture = createMippedTexture(device);
    AluState input{};
    input.regs[4] = f32Bits(0.3f); // texel x 2
    input.regs[5] = f32Bits(0.6f); // texel y 4
    const auto packed = runTextureProgram(
        device, {texs(2, 10, 11, 4, 5, 4, false)}, {input}, {},
        {{"texture2d<float>", texture, sampler}}, "F16 TEXS runs")[0];
    expect(packed.regs[10] == (0x3c00u | (0x4000u << 16)) &&
               packed.regs[11] == (0x4400u | (0x4900u << 16)),
           "TEXS with bit 59 clear packs F16 results");
    std::cout << "PASS: TEXS 2D.LL.DC operand order, depth compare (r,r,r,1) and "
                 "F16 result packing\n";
}

// TLDS encodings fetch the documented coordinates, LOD and offsets
void testTldsEncodings(MTL::Device* device) {
    NS_STACK_SCOPED auto texture = createMippedTexture(device);
    NS_STACK_SCOPED auto sampler = createSampler(device);
    const auto tlds = [](u32 encoding, u32 dst0, u32 a, u32 b) {
        // RG results to dst0, dst0 + 1 (mask 4 with dst1 = RZ)
        return 0xd200000000000000ull | dst0 | (u64(a) << 8) | PT_GUARD |
               (u64(b) << 20) | (0xffull << 28) | (1ull << 36) |
               (4ull << 50) | (u64(encoding) << 53) | (1ull << 59);
    };
    struct Case {
        u32 encoding;
        u32 a[2], b[2];
        f32 level, x, y;
    };
    const Case cases[] = {
        {2, {3, 0}, {5, 0}, 1, 3, 5},           // 2D.LZ: (Ra, Rb)
        {5, {1, 2}, {1, 0}, 2, 1, 2},           // 2D.LL: (Ra, Ra+1), LOD Rb
        {4, {2, 1}, {0x31, 0}, 1, 3, 4},        // 2D.LZ.AOFFI: offset (1, 3)
        {12, {1, 1}, {1, 0xf1}, 2, 2, 0},       // 2D.LL.AOFFI: offset (1, -1)
    };
    for (const auto& c : cases) {
        AluState input{};
        input.regs[4] = c.a[0];
        input.regs[5] = c.a[1];
        input.regs[6] = c.b[0];
        input.regs[7] = c.b[1];
        const auto result = runTextureProgram(
            device, {tlds(c.encoding, 10, 4, 6)}, {input}, {},
            {{"texture2d<float>", texture, sampler}}, "TLDS runs")[0];
        if (result.regs[10] != f32Bits(c.level) || result.regs[11] != f32Bits(c.x))
            std::cerr << fmt::format("TLDS {}: {} {}\n", c.encoding,
                                     std::bit_cast<f32>(result.regs[10]),
                                     std::bit_cast<f32>(result.regs[11]));
        expect(result.regs[10] == f32Bits(c.level) &&
                   result.regs[11] == f32Bits(c.x),
               "TLDS fetches the texel its encoding addresses");
    }
    std::cout << "PASS: TLDS 2D.LZ, 2D.LL and AOFFI encodings\n";
}

void testTexAoffi(MTL::Device* device) {
    NS_STACK_SCOPED auto texture = createMippedTexture(device);
    NS_STACK_SCOPED auto sampler = createSampler(device);
    // TEX.LZ reads its signed nibbles from R8; TEX.LL consumes R8 as LOD
    // first, then reads the offsets from R9.
    for (const bool explicit_lod : {false, true}) {
        const u64 tex = 0xc038000000000000ull | PT_GUARD | 20 | (4ull << 8) |
                        (8ull << 20) | (2ull << 28) | (15ull << 31) |
                        (1ull << 36) | (1ull << 54) |
                        (u64(explicit_lod ? 3 : 1) << 55);
        std::vector<AluState> inputs;
        for (const u32 packed : {0x21u, 0xf1u, 0x8fu, 0x77u}) {
            AluState input{};
            input.regs[4] = f32Bits(0.4375f);
            input.regs[5] = f32Bits(0.5625f);
            input.regs[8] = explicit_lod ? f32Bits(1.0f) : packed;
            input.regs[9] = packed;
            inputs.push_back(input);
        }
        const auto results = runTextureProgram(
            device, {tex}, inputs, {}, {{"texture2d<float>", texture, sampler}},
            "TEX.AOFFI runs");
        for (usize i = 0; i < inputs.size(); ++i) {
            const u32 packed = inputs[i].regs[explicit_lod ? 9 : 8];
            const auto signed_nibble = [](u32 value) {
                return value < 8 ? static_cast<i32>(value)
                                 : static_cast<i32>(value) - 16;
            };
            const i32 size = explicit_lod ? 4 : 8;
            const i32 x = std::clamp((explicit_lod ? 1 : 3) +
                                         signed_nibble(packed & 15), 0, size - 1);
            const i32 y = std::clamp((explicit_lod ? 2 : 4) +
                                         signed_nibble((packed >> 4) & 15), 0, size - 1);
            expect(results[i].regs[20] == f32Bits(explicit_lod ? 2.0f : 1.0f) &&
                       results[i].regs[21] == f32Bits(f32(x)) &&
                       results[i].regs[22] == f32Bits(f32(y)),
                   "TEX.AOFFI sign extends both offsets at the selected mip");
        }
    }
    std::cout << "PASS: TEX.AOFFI signed offsets, LZ/LL and edge clamping\n";
}

void testUnnormalizedTextureCoordinates(MTL::Device* device) {
    NS_STACK_SCOPED auto texture = createMippedTexture(device);
    NS_STACK_SCOPED auto sampler = createSampler(device);
    GuestShaderState state{};
    state.unnormalized_textures.push_back(1);
    AluState input{};
    input.regs[4] = f32Bits(2.5f);
    input.regs[5] = f32Bits(4.5f);
    // TEX.LZ 2D, RGBA: level 0 contains (1, x, y, 1).
    const u64 tex = 0xc038000000000000ull | PT_GUARD | 10 | (4ull << 8) |
                    (2ull << 28) | (15ull << 31) | (1ull << 36) | (1ull << 55);
    const auto sampled = runTextureProgram(
        device, {tex}, {input}, {}, {{"texture2d<float>", texture, sampler}},
        "Unnormalized TEX runs", state)[0];
    expect(sampled.regs[11] == f32Bits(2.0f) && sampled.regs[12] == f32Bits(4.0f),
           "Unnormalized TEX converts pixel centers to normalized coordinates");
    // Derivatives are in texels as well: dx=(2,0), dy=(0,2) selects level 1.
    input.regs[6] = f32Bits(2.0f);
    input.regs[7] = f32Bits(0.0f);
    input.regs[8] = f32Bits(0.0f);
    input.regs[9] = f32Bits(2.0f);
    const u64 txd = 0xde38000000000000ull | PT_GUARD | 10 | (4ull << 8) |
                    (6ull << 20) | (2ull << 28) | (1ull << 31) | (1ull << 36);
    const auto gradient = runTextureProgram(
        device, {txd}, {input}, {}, {{"texture2d<float>", texture, sampler}},
        "Unnormalized TXD runs", state)[0];
    expect(gradient.regs[10] == f32Bits(2.0f),
           "Unnormalized TXD scales explicit gradients in texel units");
    std::cout << "PASS: unnormalized texture coordinates and gradients\n";
}

// TXD gradients are interleaved per axis (dx.x, dy.x, dx.y, dy.y)
void testTxdGradientOrder(MTL::Device* device) {
    NS_STACK_SCOPED auto texture = createMippedTexture(device);
    NS_STACK_SCOPED auto sampler = createSampler(device);
    // TXD R10, R4 (coords), R6 (gradients), 2D, RG
    const u64 txd = 0xde38000000000000ull | 10 | (4ull << 8) | PT_GUARD |
                    (6ull << 20) | (2ull << 28) | (3ull << 31) | (1ull << 36);
    // dx = (a, 0), dy = (a, 0): LOD log2(8a) = 1.25 picks level 1. Pairing
    // the registers the other way gives |dx| = a * sqrt(2), level 2.
    const f32 a = std::exp2(1.25f) / 8.0f;
    AluState input{};
    input.regs[4] = f32Bits(0.5f);
    input.regs[5] = f32Bits(0.5f);
    input.regs[6] = f32Bits(a); // dx.x
    input.regs[7] = f32Bits(a); // dy.x
    input.regs[8] = f32Bits(0); // dx.y
    input.regs[9] = f32Bits(0); // dy.y
    const auto result = runTextureProgram(
        device, {txd}, {input}, {}, {{"texture2d<float>", texture, sampler}},
        "TXD runs")[0];
    expect(result.regs[10] == f32Bits(2.0f),
           "TXD reads the gradients interleaved per axis");
    std::cout << "PASS: TXD gradient register order\n";
}

// Third-axis derivatives must select mip levels for volumes. Cubemap
// coordinates remain directions, with the array index after all three axes.
void testTxdVolumeAndCube(MTL::Device* device) {
    NS_STACK_SCOPED auto sampler = createSampler(device);
    for (const u32 dim : {4u, 6u, 7u}) {
        NS_STACK_SCOPED auto descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setPixelFormat(MTL::PixelFormatRGBA32Float);
        descriptor->setTextureType(dim == 4 ? MTL::TextureType3D
                                   : dim == 6 ? MTL::TextureTypeCube
                                              : MTL::TextureTypeCubeArray);
        descriptor->setWidth(8);
        descriptor->setHeight(8);
        descriptor->setDepth(dim == 4 ? 8 : 1);
        descriptor->setArrayLength(dim == 7 ? 2 : 1);
        descriptor->setMipmapLevelCount(4);
        descriptor->setStorageMode(MTL::StorageModeShared);
        descriptor->setUsage(MTL::TextureUsageShaderRead);
        NS_STACK_SCOPED auto texture = device->newTexture(descriptor);
        expect(texture != nullptr, "TXD volume/cube fixture allocates");
        const u32 slices = dim == 4 ? 1 : dim == 6 ? 6 : 12;
        for (u32 level = 0; level < 4; ++level) {
            const u32 size = 8u >> level;
            const u32 depth = dim == 4 ? size : 1;
            for (u32 slice = 0; slice < slices; ++slice) {
                std::vector<f32> pixels(usize(size) * size * depth * 4);
                for (usize i = 0; i < pixels.size(); i += 4) {
                    pixels[i] = f32(level + 1);
                    pixels[i + 1] = f32(slice + 1);
                    pixels[i + 2] = 0;
                    pixels[i + 3] = 1;
                }
                texture->replaceRegion(MTL::Region::Make3D(0, 0, 0, size, size, depth),
                                       level, slice, pixels.data(), size * 16,
                                       size * size * 16);
            }
        }
        // Keep coordinate, derivative and destination registers disjoint.
        const u64 txd = 0xde38000000000000ull | PT_GUARD | 20 | (4ull << 8) |
                        (10ull << 20) | (u64(dim) << 28) | (3ull << 31) |
                        (1ull << 36);
        AluState input{};
        input.regs[4] = f32Bits(dim == 4 ? 0.5f : 1.0f);
        input.regs[5] = f32Bits(dim == 4 ? 0.5f : 0.0f);
        input.regs[6] = f32Bits(dim == 4 ? 0.5f : 0.0f);
        input.regs[7] = 1; // cube array 1, face +X = slice 6
        // Only dPdx.z is nonzero: volume LOD is log2(8 * .25) = 1.
        input.regs[14] = f32Bits(dim == 4 ? 0.25f : 0.0f);
        GuestShaderState state{};
        // Cube directions must not be divided by texture dimensions.
        state.unnormalized_textures.push_back(1);
        if (dim == 4) {
            state.unnormalized_textures.clear();
        }
        const char* declaration = dim == 4 ? "texture3d<float>"
                                 : dim == 6 ? "texturecube<float>"
                                            : "texturecube_array<float>";
        const auto result = runTextureProgram(
            device, {txd}, {input}, {}, {{declaration, texture, sampler}},
            "TXD volume/cube runs", state)[0];
        expect(result.regs[20] == f32Bits(dim == 4 ? 2.0f : 1.0f),
               "TXD volume uses third derivative and cube zero derivatives use base mip");
        expect(result.regs[21] == f32Bits(dim == 7 ? 7.0f : 1.0f),
               "TXD cube reads its face and array index after xyz");
    }
    std::cout << "PASS: TXD 3D, cube and cube-array coordinates/gradients\n";
}

// ATOMS.CAS swaps only when the word matches, under contention; byte
// stores to shared memory keep the other bytes of the word
// Global memory through the NVN storage buffer descriptor in c0[0x310]:
// {address, size} of the buffer the shader's 64-bit pointers are based on
void testGlobalMemory(MTL::Device* device) {
    constexpr u32 DESCRIPTOR_WORD = 0x310 / 4;
    constexpr u32 BASE = 0x20000;
    std::vector<u32> cbuf(DESCRIPTOR_WORD + 4);
    cbuf[DESCRIPTOR_WORD] = BASE;
    cbuf[DESCRIPTOR_WORD + 2] = 32;
    const auto pointer = [&](u32 dst, u32 src) {
        return 0x4c10000000000000ull | dst | (u64(src) << 8) | PT_GUARD |
               (u64(DESCRIPTOR_WORD) << 20);
    };
    const auto ldg = [](u32 size, u32 dst, u32 addr, u32 imm) {
        return (0xeed0000000000000ull + (u64(size) << 48)) | dst |
               (u64(addr) << 8) | PT_GUARD | (u64(imm & 0xffffff) << 20) |
               (1ull << 45);
    };
    const auto stg = [](u32 size, u32 src, u32 addr, u32 imm) {
        return (0xeed8000000000000ull + (u64(size) << 48)) | src |
               (u64(addr) << 8) | PT_GUARD | (u64(imm & 0xffffff) << 20) |
               (1ull << 45);
    };
    constexpr u32 U8 = 0, S8 = 1, U16 = 2, S16 = 3, B32 = 4, B64 = 5, B128 = 6;

    {
        std::vector<u32> storage{0x11111111, 0x22222222, 0xa5b6c7d8, 0x44444444,
                                 0x55555555, 0x66666666, 0x77777777, 0x88888888};
        storage.resize(16, 0xdeadbeef);
        AluState input;
        input.regs[0] = 0x13579bdf;
        input.regs[1] = 0x2468ace0;
        input.regs[2] = 8;
        input.regs[5] = 0x1234abcd;
        const auto results = runAlu(
            device,
            {pointer(12, 2), ldg(B32, 4, 12, 4), ldg(B64, 6, 12, 8),
             ldg(U8, 8, 12, 1), ldg(S8, 9, 12, 3), ldg(U16, 10, 12, 2),
             ldg(S16, 11, 12, 0), ldg(B32, 13, 12, 0x100),
             ldg(B128, 16, 12, 16), ldg(B128, 254, 12, 16),
             stg(B32, 5, 12, 20), stg(U8, 5, 12, 1),
             stg(B32, 5, 12, 0x100), stg(U16, 5, 12, 0)},
            {input}, cbuf, 0, "LDG/STG storage buffer", 0, &storage);
        const auto& r = results[0].regs;
        expect(r[4] == 0x44444444, "LDG.32 reads the offset from the descriptor base");
        expect(r[6] == 0x55555555 && r[7] == 0x66666666, "LDG.64 reads two words");
        expect(r[8] == 0xc7, "LDG.U8 zero extends");
        expect(r[9] == 0xffffffa5, "LDG.S8 sign extends");
        expect(r[10] == 0xa5b6, "LDG.U16 zero extends");
        expect(r[11] == 0xffffc7d8, "LDG.S16 sign extends");
        expect(r[13] == 0, "LDG beyond the buffer size reads zero");
        expect(r[16] == 0x77777777 && r[17] == 0x88888888 && r[18] == 0 &&
                   r[19] == 0,
               "LDG.128 zeroes the words beyond the buffer size");
        expect(r[0] == input.regs[0] && r[1] == input.regs[1],
               "multi-register destinations stop at RZ instead of wrapping to R0");
        expect(storage[7] == 0x1234abcd, "STG.32 stores a word");
        expect(storage[2] == 0xa5b6abcd,
               "STG.U8 and STG.U16 merge into their word");
        for (usize i = 8; i < storage.size(); ++i)
            expect(storage[i] == 0xdeadbeef, "STG beyond the buffer size is dropped");
    }

    {
        // Eight threads share one buffer
        const auto atom = [](u32 size, u32 op, u32 dst, u32 addr, u32 operand,
                             u32 imm) {
            return 0xed00000000000000ull | (u64(op) << 52) | (u64(size) << 49) |
                   (1ull << 48) | (u64(imm & 0xfffff) << 28) |
                   (u64(operand) << 20) | PT_GUARD | (u64(addr) << 8) | dst;
        };
        const auto red = [](u32 size, u32 op, u32 operand, u32 addr, u32 imm) {
            return 0xebf8000000000000ull | (1ull << 48) |
                   (u64(imm & 0xfffff) << 28) | (u64(op) << 23) |
                   (u64(size) << 20) | PT_GUARD | (u64(addr) << 8) | operand;
        };
        std::vector<u32> storage{10, 20, 100, 0, std::bit_cast<u32>(2.0f), 7, 0, 0};
        std::vector<AluState> inputs(8);
        for (u32 i = 0; i < inputs.size(); ++i) {
            inputs[i].regs[5] = 1;
            inputs[i].regs[6] = static_cast<u32>(static_cast<i32>(i) - 3);
            inputs[i].regs[7] = i + 100;
            inputs[i].regs[8] = std::bit_cast<u32>(1.5f);
        }
        const auto results = runAlu(
            device,
            {pointer(12, 2), atom(0, 0, 4, 12, 5, 0), red(0, 0, 5, 12, 4),
             atom(1, 1, 9, 12, 6, 8), atom(0, 8, 10, 12, 7, 12),
             atom(3, 0, 11, 12, 8, 16), atom(0, 3, 13, 12, 5, 20)},
            inputs, cbuf, 0, "ATOM/RED storage buffer", 0, &storage);
        std::set<u32> previous;
        for (const auto& result : results)
            previous.insert(result.regs[4]);
        expect(previous.size() == 8 && *previous.begin() == 10 &&
                   *previous.rbegin() == 17,
               "ATOM.ADD returns a distinct previous value to each thread");
        expect(storage[0] == 18, "ATOM.ADD accumulates");
        expect(storage[1] == 28, "RED.ADD accumulates");
        expect(static_cast<i32>(storage[2]) == -3, "ATOM.MIN.S32 takes the signed minimum");
        expect(storage[3] >= 100 && storage[3] < 108, "ATOM.EXCH stores one thread's value");
        expect(std::bit_cast<f32>(storage[4]) == 14.0f, "ATOM.ADD.F32 accumulates");
        expect(storage[5] == 7 && results[0].regs[13] == 7,
               "ATOM.INC leaves 32-bit memory unchanged");
    }
    {
        // ATOM.CAS: compare in Rb, new value in Rb + 1
        const auto cas = [](u32 dst, u32 addr, u32 operand, u32 imm) {
            return 0xeef0000000000000ull | dst | (u64(addr) << 8) | PT_GUARD |
                   (u64(operand) << 20) | (u64(imm & 0xfffff) << 28) |
                   (1ull << 48);
        };
        std::vector<u32> storage{5, 7, 0, 0, 0, 0, 0, 0};
        AluState input;
        input.regs[6] = 5;
        input.regs[7] = 9;
        input.regs[8] = 5;
        input.regs[9] = 11;
        const auto results = runAlu(
            device,
            {pointer(12, 2), cas(4, 12, 6, 0), cas(5, 12, 8, 0),
             cas(10, 12, 6, 4)},
            {input}, cbuf, 0, "ATOM.CAS storage buffer", 0, &storage);
        expect(results[0].regs[4] == 5 && storage[0] == 9,
               "ATOM.CAS swaps when the compare value matches");
        expect(results[0].regs[5] == 9,
               "ATOM.CAS returns the current value when the compare fails");
        expect(results[0].regs[10] == 7 && storage[1] == 7,
               "ATOM.CAS leaves memory alone on a failed compare");
    }
    std::cout << "PASS: global memory loads, stores and atomics\n";
}

// FP64 on bit patterns: DFMA/DADD/DMUL against the host's fused multiply-add
// in every rounding mode, comparisons, min/max and DSET
void testDoublePrecision(MTL::Device* device) {
    struct Rng {
        u64 state{0x9e3779b97f4a7c15ull};
        u64 next() {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        }
    } rng;
    const std::vector<u64> special{
        0x0000000000000000ull, 0x8000000000000000ull, 0x3ff0000000000000ull,
        0xbff0000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
        0x7ff8000000000000ull, 0x0000000000000001ull, 0x000fffffffffffffull,
        0x0010000000000000ull, 0x7fefffffffffffffull, 0xffefffffffffffffull,
        0x3fe0000000000000ull, 0x4008000000000000ull, 0x3ff0000000000001ull,
        0x3fefffffffffffffull, 0x8000000000000001ull, 0x4340000000000000ull,
        0x7fe0000000000000ull, 0x0020000000000000ull};
    const auto random_value = [&]() -> u64 {
        const u64 kind = rng.next() % 8;
        if (kind == 0)
            return special[rng.next() % special.size()];
        u64 sign = (rng.next() & 1) << 63;
        u64 mantissa = rng.next() & 0xfffffffffffffull;
        u64 exponent;
        if (kind <= 4)
            exponent = 0x3f0 + rng.next() % 0x20;
        else if (kind == 5)
            exponent = rng.next() % 3; // Subnormal and smallest normals
        else if (kind == 6)
            exponent = 0x7fe - rng.next() % 3; // Near overflow
        else
            exponent = rng.next() % 0x7ff;
        return sign | (exponent << 52) | mantissa;
    };
    constexpr usize COUNT = 3000;
    struct Case {
        u64 a, b, c;
    };
    std::vector<Case> cases(COUNT);
    for (usize i = 0; i < COUNT; ++i) {
        cases[i] = {random_value(), random_value(), random_value()};
        // Make cancellation likely: the addend nearly negates the product
        if (i % 4 == 0) {
            const double p = std::bit_cast<double>(cases[i].a) *
                             std::bit_cast<double>(cases[i].b);
            u64 bits = std::bit_cast<u64>(-p);
            if (std::isfinite(p))
                bits += rng.next() % 4;
            cases[i].c = bits;
        }
    }
    const auto input_for = [&](const Case& c) {
        AluState input;
        input.regs[2] = static_cast<u32>(c.a);
        input.regs[3] = static_cast<u32>(c.a >> 32);
        input.regs[4] = static_cast<u32>(c.b);
        input.regs[5] = static_cast<u32>(c.b >> 32);
        input.regs[6] = static_cast<u32>(c.c);
        input.regs[7] = static_cast<u32>(c.c >> 32);
        return input;
    };
    std::vector<AluState> inputs;
    for (const auto& c : cases)
        inputs.push_back(input_for(c));
    const auto result_of = [](const AluState& state, u32 reg) {
        return static_cast<u64>(state.regs[reg]) |
               (static_cast<u64>(state.regs[reg + 1]) << 32);
    };
    const auto same = [](u64 got, u64 want) {
        const auto is_nan = [](u64 x) {
            return (x & 0x7fffffffffffffffull) > 0x7ff0000000000000ull;
        };
        return got == want || (is_nan(got) && is_nan(want));
    };
    const int host_modes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD,
                               FE_TOWARDZERO};
    // dfma R10, R2, R4, R6
    const auto dfma = [](u32 rounding) {
        return 0x5b70000000000000ull | 10 | (2ull << 8) | PT_GUARD |
               (4ull << 20) | (6ull << 39) | (u64(rounding) << 50);
    };
    const auto dadd = [](u32 rounding) {
        return 0x5c70000000000000ull | 10 | (2ull << 8) | PT_GUARD |
               (4ull << 20) | (u64(rounding) << 39);
    };
    const auto dmul = [](u32 rounding) {
        return 0x5c80000000000000ull | 10 | (2ull << 8) | PT_GUARD |
               (4ull << 20) | (u64(rounding) << 39);
    };
    for (u32 mode = 0; mode < 4; ++mode) {
        const auto fma_results = runAlu(device, {dfma(mode)}, inputs, {}, 0, "DFMA");
        const auto add_results = runAlu(device, {dadd(mode)}, inputs, {}, 0, "DADD");
        const auto mul_results = runAlu(device, {dmul(mode)}, inputs, {}, 0, "DMUL");
        u32 fma_failures = 0, add_failures = 0, mul_failures = 0;
        for (usize i = 0; i < COUNT; ++i) {
            volatile double a = std::bit_cast<double>(cases[i].a);
            volatile double b = std::bit_cast<double>(cases[i].b);
            volatile double c = std::bit_cast<double>(cases[i].c);
            std::fesetround(host_modes[mode]);
            volatile double fma_want = std::fma(a, b, c);
            volatile double add_want = a + c;
            volatile double mul_want = a * b;
            std::fesetround(FE_TONEAREST);
            const auto check_one = [&](u64 got, double want, u32& failures,
                                       const char* name) {
                if (!same(got, std::bit_cast<u64>(want)) && failures++ < 4)
                    std::cerr << name << " mode " << mode << " a=" << std::hex
                              << cases[i].a << " b=" << cases[i].b
                              << " c=" << cases[i].c << " got=" << got
                              << " want=" << std::bit_cast<u64>(want)
                              << std::dec << '\n';
            };
            check_one(result_of(fma_results[i], 10), fma_want, fma_failures, "DFMA");
            // DADD adds its two operands: a and b (R4:R5)
            volatile double add_b = std::bit_cast<double>(cases[i].b);
            std::fesetround(host_modes[mode]);
            volatile double add_want_ab = a + add_b;
            std::fesetround(FE_TONEAREST);
            (void)add_want;
            check_one(result_of(add_results[i], 10), add_want_ab, add_failures, "DADD");
            check_one(result_of(mul_results[i], 10), mul_want, mul_failures, "DMUL");
        }
        expect(fma_failures == 0, "DFMA matches the host fused multiply-add");
        expect(add_failures == 0, "DADD matches the host addition");
        expect(mul_failures == 0, "DMUL matches the host multiplication");
    }

    // Comparisons: DSETP P1 = a OP b with dest2 = !, then DSET
    const auto dsetp = [](u32 cmp, u32 bool_op) {
        return 0x5b80000000000000ull | 2 | (1ull << 3) | (2ull << 8) |
               PT_GUARD | (4ull << 20) | (7ull << 39) | (u64(bool_op) << 45) |
               (u64(cmp) << 48);
    };
    const auto dset = [](u32 cmp, bool bf) {
        return 0x5900000000000000ull | 10 | (2ull << 8) | PT_GUARD |
               (4ull << 20) | (7ull << 39) | (u64(cmp) << 48) |
               (bf ? 1ull << 52 : 0ull);
    };
    for (u32 cmp = 0; cmp < 16; ++cmp) {
        const auto predicates = runAlu(device, {dsetp(cmp, 0)}, inputs, {}, 0, "DSETP");
        const auto masks = runAlu(device, {dset(cmp, false)}, inputs, {}, 0, "DSET");
        const auto floats = runAlu(device, {dset(cmp, true)}, inputs, {}, 0, "DSET.BF");
        u32 failures = 0;
        for (usize i = 0; i < COUNT; ++i) {
            const double a = std::bit_cast<double>(cases[i].a);
            const double b = std::bit_cast<double>(cases[i].b);
            const bool unordered = std::isnan(a) || std::isnan(b);
            bool want;
            switch (cmp) {
            case 0: want = false; break;
            case 1: want = a < b; break;
            case 2: want = a == b; break;
            case 3: want = a <= b; break;
            case 4: want = a > b; break;
            case 5: want = !unordered && a != b; break;
            case 6: want = a >= b; break;
            case 7: want = !unordered; break;
            case 8: want = unordered; break;
            case 9: want = a < b || unordered; break;
            case 10: want = a == b || unordered; break;
            case 11: want = a <= b || unordered; break;
            case 12: want = a > b || unordered; break;
            case 13: want = a != b; break;
            case 14: want = a >= b || unordered; break;
            default: want = true; break;
            }
            const bool got = (predicates[i].preds & 2) != 0;
            const bool got_inverse = (predicates[i].preds & 4) != 0;
            if (got != want || got_inverse != !want ||
                masks[i].regs[10] != (want ? 0xffffffffu : 0u) ||
                floats[i].regs[10] != (want ? 0x3f800000u : 0u))
                failures++;
        }
        expect(failures == 0, "DSETP and DSET implement every comparison");
    }

    // DMNMX R10, R2, R4, PT (minimum) and !PT (maximum)
    const auto dmnmx = [](bool maximum) {
        return 0x5c50000000000000ull | 10 | (2ull << 8) | PT_GUARD |
               (4ull << 20) | (7ull << 39) | (maximum ? 1ull << 42 : 0ull);
    };
    const auto minimums = runAlu(device, {dmnmx(false)}, inputs, {}, 0, "DMNMX min");
    const auto maximums = runAlu(device, {dmnmx(true)}, inputs, {}, 0, "DMNMX max");
    u32 minmax_failures = 0;
    for (usize i = 0; i < COUNT; ++i) {
        const double a = std::bit_cast<double>(cases[i].a);
        const double b = std::bit_cast<double>(cases[i].b);
        const auto expected = [&](bool maximum) -> double {
            if (std::isnan(a) && std::isnan(b))
                return std::numeric_limits<double>::quiet_NaN();
            if (std::isnan(a))
                return b;
            if (std::isnan(b))
                return a;
            if (a == b)
                // -0 orders below +0
                return maximum ? (std::signbit(a) ? b : a)
                               : (std::signbit(a) ? a : b);
            return maximum ? std::max(a, b) : std::min(a, b);
        };
        if (!same(result_of(minimums[i], 10), std::bit_cast<u64>(expected(false))) ||
            !same(result_of(maximums[i], 10), std::bit_cast<u64>(expected(true))))
            minmax_failures++;
    }
    expect(minmax_failures == 0, "DMNMX picks the minimum and maximum like the host");
    std::cout << "PASS: FP64 fused multiply-add, add, multiply, compare and min/max\n";
}

// Exact conversions between doubles and integers
void testDoubleConversions(MTL::Device* device) {
    const auto f2i = [](u32 dst, u32 src, u32 width, bool is_signed, u32 round,
                        bool abs_ = false, bool neg = false) -> u64 {
        return 0x5cb0000000000000ull | dst | (u64(width) << 8) |
               (3ull << 10) | (u64(is_signed) << 12) | PT_GUARD |
               (u64(src) << 20) | (u64(round) << 39) | (u64(neg) << 45) |
               (u64(abs_) << 49);
    };
    const auto f2f = [](u32 dst, u32 src, u32 round_mode) -> u64 {
        return 0x5ca8000000000000ull | dst | (3ull << 8) | (3ull << 10) |
               PT_GUARD | (u64(src) << 20) | (u64(round_mode & 3) << 39) |
               (u64(round_mode >> 2) << 42);
    };
    const auto i2f = [](u32 dst, u32 src, u32 src_width, bool src_signed,
                        bool abs_ = false, bool neg = false) -> u64 {
        return 0x5cb8000000000000ull | dst | (3ull << 8) |
               (u64(src_width) << 10) | (u64(src_signed) << 13) | PT_GUARD |
               (u64(src) << 20) | (u64(neg) << 45) | (u64(abs_) << 49);
    };
    const std::vector<double> doubles{
        0.0,         -0.0,        0.5,         -0.5,         1.5,
        2.5,         -2.5,        0.49999999999999994, 0.5000000000000001,
        1e10,        -1e10,       2147483647.5, -2147483648.5, 4294967295.5,
        65535.5,     -40000.7,    9.3e18,      -9.3e18,      1.8446744073709552e19,
        1e300,       -1e300,      4503599627370495.5, 4503599627370496.0,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(), 123456.789, -123456.789,
        32767.5,     -32768.5,    0.1,         -0.1};
    const std::vector<u64> integers{
        0ull, 1ull, 0xffffffffffffffffull, 0x8000000000000000ull,
        0x7fffffffffffffffull, 0x0020000000000001ull, 0x0020000000000003ull,
        0x00ffffffffffffffull, 0x123456789abcdef0ull, 0xfffffffffffff800ull,
        0x0000000080000000ull, 0x00000000ffffffffull};
    std::vector<AluState> inputs;
    for (usize i = 0; i < doubles.size(); ++i) {
        AluState input{};
        const u64 d = std::bit_cast<u64>(doubles[i]);
        input.regs[4] = u32(d);
        input.regs[5] = u32(d >> 32);
        const u64 n = integers[i % integers.size()];
        input.regs[6] = u32(n);
        input.regs[7] = u32(n >> 32);
        input.regs[2] = u32(n);
        inputs.push_back(input);
    }
    const auto results = runAlu(
        device,
        {f2i(10, 4, 2, true, 0), f2i(11, 4, 2, false, 3), f2i(12, 4, 1, true, 1),
         f2i(13, 4, 1, false, 2), f2i(14, 4, 3, true, 3),
         f2i(16, 4, 3, false, 0), f2i(18, 4, 2, true, 3, true, false),
         f2i(19, 4, 2, true, 3, false, true), f2f(20, 4, 4), f2f(22, 4, 5),
         f2f(24, 4, 6), f2f(26, 4, 7), i2f(28, 6, 3, true),
         i2f(30, 6, 3, false)},
        inputs, {}, 2, "double conversions");
    const auto integer_results = runAlu(
        device,
        {i2f(10, 2, 2, true), i2f(12, 2, 2, false),
         i2f(14, 6, 3, true, true, false), i2f(16, 6, 3, true, false, true)},
        inputs, {}, 2, "integer to double conversions");
    const auto same = [](u64 bits, double expected) {
        const double got = std::bit_cast<double>(bits);
        return std::isnan(expected) ? std::isnan(got)
                                    : bits == std::bit_cast<u64>(expected);
    };
    const auto pair = [](const AluState& state, u32 reg) {
        return u64(state.regs[reg]) | (u64(state.regs[reg + 1]) << 32);
    };
    for (usize i = 0; i < inputs.size(); ++i) {
        const double x = doubles[i];
        const auto& r = results[i].regs;
        const auto reference = [&](double v, u32 bits, bool is_signed) -> u64 {
            if (std::isnan(v))
                return bits == 64 ? 1ull << 63 : 0ull;
            if (is_signed) {
                const double low = -std::ldexp(1.0, int(bits) - 1);
                const double high = std::ldexp(1.0, int(bits) - 1);
                if (v >= high)
                    return bits == 64 ? u64(INT64_MAX) : u64(i64(high) - 1);
                if (v <= low)
                    return u64(bits == 64 ? INT64_MIN : i64(low));
                return u64(i64(v));
            }
            if (v <= 0.0)
                return 0ull;
            const double high = std::ldexp(1.0, int(bits));
            if (v >= high)
                return bits == 64 ? ~0ull : u64(high) - 1;
            return u64(v);
        };
        expect(r[10] == u32(reference(std::nearbyint(x), 32, true)),
               "F2I.S32 from a double rounds to even and saturates");
        expect(r[11] == u32(reference(std::trunc(x), 32, false)),
               "F2I.U32 from a double truncates and saturates");
        expect(r[12] == u32(i32(reference(std::floor(x), 16, true))),
               "F2I.S16 from a double sign extends");
        expect(r[13] == u32(reference(std::ceil(x), 16, false)),
               "F2I.U16 from a double saturates to 16 bits");
        expect(pair(results[i], 14) == reference(std::trunc(x), 64, true),
               "F2I.S64 from a double");
        expect(pair(results[i], 16) == reference(std::nearbyint(x), 64, false),
               "F2I.U64 from a double");
        expect(r[18] == u32(reference(std::trunc(std::fabs(x)), 32, true)) &&
                   r[19] == u32(reference(std::trunc(-x), 32, true)),
               "F2I from a double honours abs and neg");
        expect(same(pair(results[i], 20), std::nearbyint(x)) &&
                   same(pair(results[i], 22), std::floor(x)) &&
                   same(pair(results[i], 24), std::ceil(x)) &&
                   same(pair(results[i], 26), std::trunc(x)),
               "F2F.F64 integer rounding is exact");
        const u64 n = integers[i % integers.size()];
        expect(same(pair(results[i], 28), static_cast<double>(i64(n))) &&
                   same(pair(results[i], 30), static_cast<double>(n)),
               "I2F.F64 converts 64-bit integers with a single rounding");
        expect(same(pair(integer_results[i], 10), static_cast<double>(i32(u32(n)))) &&
                   same(pair(integer_results[i], 12), static_cast<double>(u32(n))),
               "I2F.F64 converts 32-bit integers exactly");
        const double signed_n = static_cast<double>(i64(n));
        expect(same(pair(integer_results[i], 14),
                    i64(n) == INT64_MIN ? signed_n : std::fabs(signed_n)) &&
                   same(pair(integer_results[i], 16),
                        i64(n) == INT64_MIN ? signed_n : -signed_n),
               "I2F.F64 abs and neg keep the most negative value");
    }
    std::cout << "PASS: exact double to integer and integer to double conversions\n";
}

// BRX through a table in a constant buffer, bounded by an IMNMX clamp (the
// shape of the ones in UE4 material shaders)
void testIndirectBranch(MTL::Device* device) {
    const auto word = [](u32 i) { return i + 1 + i / 3; };
    const auto mov = [](u32 dst, u32 value) {
        return 0x0100000000000000ull | PT_GUARD | dst |
               (static_cast<u64>(value) << 20);
    };
    const u64 exit = 0xe30000000007000full;
    const u64 imnmx = 0x3820038000370000ull; // IMNMX.U32 R0, R0, 3, PT
    const u64 shl = 0x3848000000270000ull;   // SHL R0, R0, 2
    // LDC R0, c[1][R0 + 0]
    const u64 ldc = 0xef94000000070000ull | (1ull << 36);
    const u64 brx = 0xe250000000070000ull | 0xf; // BRX R0
    const std::vector<u64> program{imnmx, shl, ldc, brx,
                                   mov(10, 100), exit, mov(10, 101), exit,
                                   mov(10, 102), exit, mov(10, 103), exit};
    // Offsets from the instruction after the BRX
    const u32 base = (word(3) + 1) * 8;
    std::vector<u32> cbuf{(word(4) * 8) - base, (word(6) * 8) - base,
                          (word(8) * 8) - base, (word(10) * 8) - base};
    std::vector<AluState> inputs(6);
    for (u32 i = 0; i < inputs.size(); ++i)
        inputs[i].regs[0] = i;
    const auto results = runAlu(device, program, inputs, cbuf, 1, "BRX table");
    const u32 expected[] = {100, 101, 102, 103, 103, 103};
    for (usize i = 0; i < inputs.size(); ++i)
        expect(results[i].regs[10] == expected[i],
               "BRX jumps to the table entry and clamps the index to the bound");
    std::cout << "PASS: BRX through a bounded constant buffer table\n";
}

void testSharedCompareExchange(MTL::Device* device) {
    std::vector<u64> words;
    const auto append = [&](u64 x) {
        if (words.size() % 4 == 0)
            words.push_back(0);
        words.push_back(x);
    };
    // R10 = ATOMS.CAS [R1], R2 (compare), R3 (new value)
    append(0xee40000000000000ull | 10 | (1ull << 8) | PT_GUARD | (2ull << 20));
    // STS.U8 [R4], R5: each thread stores its byte of word 1
    append(0xef58000000000000ull | 5 | (4ull << 8) | PT_GUARD | (0ull << 48));
    append(0xe30000000007000full);
    std::vector<u8> bytes(words.size() * 8);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    DecompilerContext context{};
    context.type = ShaderType::Compute;
    ir::Module module;
    ir::Builder builder(module);
    decoder::Decoder decoder({context, &stream, builder});
    decoder.decode();
    analyzer::MemoryAnalyzer memory;
    memory.analyze(module);
    GuestShaderState guest{};
    guest.shared_memory_size = 64;
    ResourceMapping mapping;
    std::vector<u8> generated;
    codegen::lang::msl::MslEmitter emitter(context, memory, guest, generated,
                                           mapping);
    emitter.emit(module);
    std::string source(generated.begin(), generated.end());
    source += runProgramSource(module);
    source += R"(
kernel void check_cas(device uint* out [[buffer(0)]],
                      uint tid [[thread_index_in_threadgroup]]) {
    threadgroup Reg words[16];
    if (tid == 0) {
        for (uint i = 0; i < 16; i++) words[i]._u32 = 0;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    State state;
    state.smem = words;
    // Thread n swaps n into word 0 when it holds n - 1: a chain of 64
    state.r[1]._u32 = 0;
    for (uint attempt = 0; attempt < 64; attempt++) {
        state.r[2]._u32 = tid;
        state.r[3]._u32 = tid + 1;
        state.r[4]._u32 = 4 + (tid & 3);
        state.r[5]._u32 = 0x10 + (tid & 3);
        func_label0x0(state);
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    out[1 + tid] = state.r[10]._u32;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (tid == 0) {
        out[0] = words[0]._u32;
        out[65] = words[1]._u32;
    }
}
)";
    NS_STACK_SCOPED auto library =
        compileLibrary(device, source, "ATOMS.CAS shader compiles");
    NS_STACK_SCOPED auto function = library->newFunction(toNsString("check_cas"));
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto pipeline =
        device->newComputePipelineState(function, &error);
    expect(pipeline != nullptr, "ATOMS.CAS pipeline compiles");
    NS_STACK_SCOPED auto output =
        device->newBuffer(66 * 4, MTL::ResourceStorageModeShared);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->computeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    encoder->setBuffer(output, 0, 0);
    encoder->dispatchThreads(MTL::Size(64, 1, 1), MTL::Size(64, 1, 1));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    expect(commands->status() == MTL::CommandBufferStatusCompleted,
           "ATOMS.CAS executes");
    const auto* result = static_cast<const u32*>(output->contents());
    expect(result[0] == 64, "Every compare and swap of the chain succeeds once");
    expect(result[65] == 0x13121110u,
           "Concurrent byte stores keep the other bytes of the word");
    std::cout << "PASS: ATOMS.CAS chain under contention and atomic shared byte "
                 "stores\n";
}

} // namespace

// Host oracle for Hydra's current conversion behavior. NVIDIA edge semantics
// still need hardware confirmation; NaN currently converts to zero.
u32 f2iReference(float x, u32 round, bool is_signed, bool is_16, bool abs_,
                 bool neg) {
    if (abs_)
        x = std::fabs(x);
    if (neg)
        x = -x;
    switch (round) {
    case 0: x = std::nearbyint(x); break;
    case 1: x = std::floor(x); break;
    case 2: x = std::ceil(x); break;
    default: x = std::trunc(x); break;
    }
    if (std::isnan(x))
        return 0;
    if (is_16)
        return is_signed ? u32(i32(std::clamp(x, -32768.0f, 32767.0f)))
                         : u32(std::clamp(x, 0.0f, 65535.0f));
    if (is_signed)
        return x >= 2147483648.0f    ? 0x7fffffffu
               : x <= -2147483648.0f ? 0x80000000u
                                     : u32(i32(x));
    return x >= 4294967296.0f ? 0xffffffffu : x <= 0.0f ? 0u : u32(x);
}

u32 halfBits(float x) {
    return std::bit_cast<u16>(static_cast<_Float16>(x));
}

float halfValue(u32 bits) {
    return static_cast<float>(std::bit_cast<_Float16>(static_cast<u16>(bits)));
}

u32 i2iReference(u32 src, u32 src_bits, bool src_signed, u32 dst_bits,
                 bool dst_signed, u32 selector, bool sat, bool abs_, bool neg) {
    u32 value = src >> (selector * 8);
    if (src_bits < 32) {
        value &= (1u << src_bits) - 1;
        if (src_signed && (value >> (src_bits - 1)) != 0)
            value |= ~((1u << src_bits) - 1);
    }
    if (abs_)
        value = u32(std::abs(i32(value)));
    if (neg)
        value = 0u - value;
    if (!sat)
        return dst_bits == 32 ? value : value & ((1u << dst_bits) - 1);
    if (dst_signed && src_signed) {
        const i32 high = i32((1u << (dst_bits - 1)) - 1);
        return u32(std::clamp(i32(value), -high - 1, high));
    }
    if (src_signed && i32(value) < 0)
        return 0;
    const u32 high = dst_signed       ? (1u << (dst_bits - 1)) - 1
                     : dst_bits == 32 ? 0xffffffffu
                                      : (1u << dst_bits) - 1;
    return std::min(value, high);
}

// F2I, F2F, I2I and I2F variants, 64-bit and double operands, and MUFU's
// double seeds, checked against host references
void testConversions(MTL::Device* device) {
    const auto f2i = [](u32 dst, u32 src, u32 width, u32 src_fmt,
                        bool is_signed, u32 round, bool sh = false,
                        bool abs_ = false, bool neg = false) -> u64 {
        return 0x5cb0000000000000ull | dst | (u64(width) << 8) |
               (u64(src_fmt) << 10) | (u64(is_signed) << 12) | PT_GUARD |
               (u64(src) << 20) | (u64(round) << 39) | (u64(sh) << 41) |
               (u64(neg) << 45) | (u64(abs_) << 49);
    };
    const auto f2f = [](u32 dst, u32 src, u32 dst_fmt, u32 src_fmt,
                        u32 round_mode, bool sh = false,
                        bool sat = false) -> u64 {
        return 0x5ca8000000000000ull | dst | (u64(dst_fmt) << 8) |
               (u64(src_fmt) << 10) | PT_GUARD | (u64(src) << 20) |
               (u64(round_mode & 3) << 39) | (u64(sh) << 41) |
               (u64(round_mode >> 2) << 42) | (u64(sat) << 50);
    };
    const auto i2i = [](u32 dst, u32 src, u32 dst_width, bool dst_signed,
                        u32 src_width, bool src_signed, u32 selector,
                        bool sat = false, bool abs_ = false,
                        bool neg = false) -> u64 {
        return 0x5ce0000000000000ull | dst | (u64(dst_width) << 8) |
               (u64(src_width) << 10) | (u64(dst_signed) << 12) |
               (u64(src_signed) << 13) | PT_GUARD | (u64(src) << 20) |
               (u64(selector) << 41) | (u64(neg) << 45) | (u64(abs_) << 49) |
               (u64(sat) << 50);
    };
    const auto i2f = [](u32 dst, u32 src, u32 dst_fmt, u32 src_width,
                        bool src_signed, u32 selector, bool abs_ = false,
                        bool neg = false) -> u64 {
        return 0x5cb8000000000000ull | dst | (u64(dst_fmt) << 8) |
               (u64(src_width) << 10) | (u64(src_signed) << 13) | PT_GUARD |
               (u64(src) << 20) | (u64(selector) << 41) | (u64(neg) << 45) |
               (u64(abs_) << 49);
    };
    const auto mufu = [](u32 dst, u32 src, u32 op) -> u64 {
        return 0x5080000000000000ull | dst | (u64(src) << 8) | PT_GUARD |
               (u64(op) << 20);
    };

    // Inputs: R1 float, R2 integer, R3 two halves, R4:R5 a double and a
    // 64-bit integer
    const float floats[] = {2.5f,      -2.5f,      3.5f,         -0.5f,
                            1e10f,     -1e10f,     NAN,          0.49999997f,
                            65535.5f,  -40000.7f,  123456.78f,   -3e9f};
    const u32 ints[] = {0x000000ffu, 0x80000000u, 0x7fffffffu, 0xfffffff0u,
                        0x00012345u, 0x0000ff80u, 0x89abcdefu, 1u,
                        0xffff8000u, 0x00800000u, 0x7f7f7f80u, 0xfedcba98u};
    const double doubles[] = {3.141592653589793, -1.0e-3,  6.5e40,
                              1.0 / 3.0,         -2.5,     1e-300,
                              65504.25,          1.5e308,  -0.0,
                              7.0,               0.1,      -123456.789};
    std::vector<AluState> inputs;
    for (usize i = 0; i < std::size(floats); i++) {
        AluState input{};
        input.regs[1] = std::bit_cast<u32>(floats[i]);
        input.regs[2] = ints[i];
        input.regs[3] = halfBits(floats[i] / 1000.0f) |
                        (halfBits(-floats[(i + 1) % std::size(floats)] /
                                  1000.0f)
                         << 16);
        const u64 double_bits = std::bit_cast<u64>(doubles[i]);
        input.regs[4] = u32(double_bits);
        input.regs[5] = u32(double_bits >> 32);
        inputs.push_back(input);
    }

    // F2I
    const auto f2i_results = runAlu(
        device,
        {f2i(10, 1, 2, 2, true, 0), f2i(11, 1, 2, 2, false, 3),
         f2i(12, 1, 1, 2, true, 1), f2i(13, 1, 1, 2, false, 2),
         f2i(14, 3, 2, 1, true, 3), f2i(15, 3, 2, 1, true, 3, true),
         f2i(16, 1, 2, 2, true, 3, false, true),
         f2i(17, 1, 2, 2, true, 3, false, false, true),
         f2i(18, 1, 3, 2, true, 3), f2i(20, 1, 3, 2, false, 0)},
        inputs, {}, 2, "F2I runs");
    for (usize i = 0; i < inputs.size(); i++) {
        const float x = floats[i];
        const auto& r = f2i_results[i].regs;
        expect(r[10] == f2iReference(x, 0, true, false, false, false),
               "F2I.S32.ROUND rounds to even and saturates");
        expect(r[11] == f2iReference(x, 3, false, false, false, false),
               "F2I.U32.TRUNC saturates to the unsigned range");
        expect(r[12] == f2iReference(x, 1, true, true, false, false),
               "F2I.S16.FLOOR writes the sign extended register");
        expect(r[13] == f2iReference(x, 2, false, true, false, false),
               "F2I.U16.CEIL saturates to 16 bits");
        expect(r[14] == f2iReference(halfValue(inputs[i].regs[3]), 3, true,
                                     false, false, false) &&
                   r[15] == f2iReference(halfValue(inputs[i].regs[3] >> 16),
                                         3, true, false, false, false),
               "F2I from F16 selects the half");
        expect(r[16] == f2iReference(x, 3, true, false, true, false) &&
                   r[17] == f2iReference(x, 3, true, false, false, true),
               "F2I abs and neg");
        const float t = std::trunc(x);
        const i64 s64 = std::isnan(x)                 ? INT64_MIN
                        : t >= 9223372036854775808.0f ? INT64_MAX
                        : t <= -9223372036854775808.0f ? INT64_MIN
                                                       : i64(t);
        expect((u64(r[19]) << 32 | r[18]) == u64(s64),
               "F2I.S64 writes a register pair");
        const float n = std::nearbyint(x);
        const u64 u64_value = std::isnan(x)                  ? 1ull << 63
                              : n >= 18446744073709551616.0f ? ~0ull
                              : n <= 0.0f                    ? 0ull
                                                             : u64(n);
        expect((u64(r[21]) << 32 | r[20]) == u64_value,
               "F2I.U64 writes a register pair");
    }

    // F2F
    const auto f2f_results = runAlu(
        device,
        {f2f(10, 1, 2, 2, 4), f2f(11, 1, 2, 2, 5), f2f(12, 1, 2, 2, 6),
         f2f(13, 1, 2, 2, 7), f2f(14, 1, 1, 2, 1), f2f(15, 3, 2, 1, 1),
         f2f(16, 3, 2, 1, 1, true), f2f(17, 1, 2, 2, 1, false, true),
         f2f(18, 4, 2, 3, 1), f2f(20, 1, 3, 2, 1)},
        inputs, {}, 2, "F2F runs");
    for (usize i = 0; i < inputs.size(); i++) {
        const float x = floats[i];
        const auto& r = f2f_results[i].regs;
        const auto same = [](u32 bits, float expected) {
            return std::isnan(expected) ? std::isnan(std::bit_cast<float>(bits))
                                        : bits == std::bit_cast<u32>(expected);
        };
        expect(same(r[10], std::nearbyint(x)) && same(r[11], std::floor(x)) &&
                   same(r[12], std::ceil(x)) && same(r[13], std::trunc(x)),
               "F2F.F32 integer rounding");
        expect(std::isnan(x) ? std::isnan(halfValue(r[14])) && r[14] < 0x10000u
                             : r[14] == halfBits(x),
               "F2F.F16 writes the low half");
        expect(same(r[15], halfValue(inputs[i].regs[3])) &&
                   same(r[16], halfValue(inputs[i].regs[3] >> 16)),
               "F2F from F16 selects the half");
        expect(r[17] == std::bit_cast<u32>(
                            std::isnan(x) ? 0.0f : std::clamp(x, 0.0f, 1.0f)),
               "F2F.SAT clamps, NaN to 0");
        expect(same(r[18], static_cast<float>(doubles[i])),
               "F2F.F32.F64 rounds the double to nearest");
        const u64 widened = std::bit_cast<u64>(static_cast<double>(x));
        expect(std::isnan(x) ? (r[21] & 0x7ff00000u) == 0x7ff00000u
                             : (u64(r[21]) << 32 | r[20]) == widened,
               "F2F.F64.F32 widens exactly");
    }

    // I2I
    const auto i2i_results = runAlu(
        device,
        {i2i(10, 2, 2, true, 0, true, 0), i2i(11, 2, 2, false, 0, false, 1),
         i2i(12, 2, 0, false, 2, true, 0),
         i2i(13, 2, 0, false, 2, true, 0, true),
         i2i(14, 2, 1, true, 2, true, 0, true),
         i2i(15, 2, 1, false, 2, true, 0, true),
         i2i(16, 2, 2, true, 1, true, 2),
         i2i(17, 2, 2, true, 2, true, 0, false, true),
         i2i(18, 2, 2, true, 2, true, 0, false, false, true),
         i2i(19, 2, 0, true, 2, false, 0, true)},
        inputs, {}, 2, "I2I runs");
    for (usize i = 0; i < inputs.size(); i++) {
        const u32 v = ints[i];
        const auto& r = i2i_results[i].regs;
        expect(r[10] == i2iReference(v, 8, true, 32, true, 0, false, false,
                                     false),
               "I2I.S32.S8 sign extends");
        expect(r[11] == i2iReference(v, 8, false, 32, false, 1, false, false,
                                     false),
               "I2I.U32.U8 selects the byte");
        expect(r[12] == i2iReference(v, 32, true, 8, false, 0, false, false,
                                     false),
               "I2I.U8 truncates to the low byte");
        expect(r[13] == i2iReference(v, 32, true, 8, false, 0, true, false,
                                     false) &&
                   r[14] == i2iReference(v, 32, true, 16, true, 0, true,
                                         false, false) &&
                   r[15] == i2iReference(v, 32, true, 16, false, 0, true,
                                         false, false) &&
                   r[19] == i2iReference(v, 32, false, 8, true, 0, true,
                                         false, false),
               "I2I.SAT saturates to the destination range");
        expect(r[16] == i2iReference(v, 16, true, 32, true, 2, false, false,
                                     false),
               "I2I.S32.S16 selects the upper half");
        expect(r[17] == i2iReference(v, 32, true, 32, true, 0, false, true,
                                     false) &&
                   r[18] == i2iReference(v, 32, true, 32, true, 0, false,
                                         false, true),
               "I2I abs and neg");
    }

    // I2F
    const auto i2f_results = runAlu(
        device,
        {i2f(10, 2, 2, 2, true, 0), i2f(11, 2, 2, 2, false, 0),
         i2f(12, 2, 2, 0, true, 1), i2f(13, 2, 2, 1, false, 2),
         i2f(14, 2, 1, 2, true, 0), i2f(15, 2, 2, 2, true, 0, false, true),
         i2f(16, 2, 2, 2, true, 0, true), i2f(17, 4, 2, 3, true, 0),
         mufu(18, 5, 6), mufu(19, 5, 7)},
        inputs, {}, 2, "I2F runs");
    for (usize i = 0; i < inputs.size(); i++) {
        const u32 v = ints[i];
        const auto& r = i2f_results[i].regs;
        expect(r[10] == std::bit_cast<u32>(float(i32(v))) &&
                   r[11] == std::bit_cast<u32>(float(v)),
               "I2F.F32 from 32-bit integers");
        expect(r[12] == std::bit_cast<u32>(float(i8(v >> 8))) &&
                   r[13] == std::bit_cast<u32>(float(u16(v >> 16))),
               "I2F selects bytes and halves");
        expect(r[14] == halfBits(float(i32(v))), "I2F.F16 writes the low half");
        const bool least = v == 0x80000000u;
        expect(r[15] == std::bit_cast<u32>(least ? float(i32(v))
                                                 : -float(i32(v))) &&
                   r[16] == std::bit_cast<u32>(
                                least ? float(i32(v)) : std::fabs(float(i32(v)))),
               "I2F neg and abs keep the most negative value");
        const u64 wide = u64(inputs[i].regs[5]) << 32 | inputs[i].regs[4];
        expect(r[17] == std::bit_cast<u32>(float(i64(wide))),
               "I2F.F32.S64 converts a register pair");

        // MUFU.RCP64H/RSQ64H seeds of the double in R4:R5 (low word 0)
        const double x = std::bit_cast<double>(u64(inputs[i].regs[5]) << 32);
        const double rcp = std::bit_cast<double>(u64(r[18]) << 32);
        const double rsq = std::bit_cast<double>(u64(r[19]) << 32);
        const bool rcp_ok =
            x == 0.0       ? std::isinf(rcp)
            : std::isinf(x) ? rcp == 0.0
            : std::fabs(1.0 / x) < 2.3e-308 ? true
                                            : std::fabs(rcp * x - 1.0) < 1e-5;
        expect(rcp_ok, "MUFU.RCP64H approximates the reciprocal");
        const bool rsq_ok =
            x == 0.0 ? std::isinf(rsq)
            : x < 0.0 ? std::isnan(rsq)
                      : std::fabs(rsq * std::sqrt(x) - 1.0) < 1e-5;
        expect(rsq_ok, "MUFU.RSQ64H approximates the reciprocal square root");
    }
    std::cout << "PASS: F2I/F2F/I2I/I2F variants, 64-bit and double "
                 "operands and MUFU double seeds\n";
}

// HADD2 reads a constant buffer operand as one F32 for both halves, RZ as two
// zero halves, and the register form negates A with bit 43
void testHalfOperands(MTL::Device* device) {
    const u64 hadd2_r = 0x5d10000000000000ull | PT_GUARD;
    const u64 hadd2_c = 0x7a80000000000000ull | PT_GUARD;
    const auto results = runAlu(
        device,
        {hadd2_c | 10 | (1ull << 8) | (2ull << 34) | (0ull << 20),
         hadd2_r | 11 | (u64(RZ) << 8) | (2ull << 20),
         hadd2_r | 12 | (1ull << 8) | (2ull << 20) | (1ull << 43)},
        {[] {
            AluState input{};
            input.regs[1] = halfBits(1.0f) | (halfBits(2.0f) << 16);
            input.regs[2] = halfBits(0.25f) | (halfBits(-4.0f) << 16);
            return input;
        }()},
        {std::bit_cast<u32>(1.5f)}, 2, "HADD2 runs");
    const auto& r = results[0].regs;
    expect(r[10] == (halfBits(2.5f) | (halfBits(3.5f) << 16)),
           "HADD2 constant buffer operand is one F32");
    expect(r[11] == (halfBits(0.25f) | (halfBits(-4.0f) << 16)),
           "HADD2 with RZ adds zero halves");
    expect(r[12] == (halfBits(-0.75f) | (halfBits(-6.0f) << 16)),
           "HADD2 register form negates A with bit 43");
    std::cout << "PASS: half operands from constant buffers, RZ and negated "
                 "registers\n";
}

// BFE.BREV reverses the source before extracting, signed or unsigned
void testBitfieldExtractReverse(MTL::Device* device) {
    const u64 bfe_r = 0x5c00000000000000ull | PT_GUARD | (1ull << 8) |
                      (2ull << 20) | (1ull << 40);
    // 0xd reversed is 0xb0000000: the top byte is the field
    const auto top = runAlu(device, {bfe_r | 10, bfe_r | 11 | (1ull << 48)},
                            {[] {
                                AluState input{};
                                input.regs[1] = 0x0000000du;
                                input.regs[2] = 24u | (8u << 8);
                                return input;
                            }()},
                            {}, 2, "BFE.BREV runs");
    expect(top[0].regs[10] == 0xb0u, "BFE.U32.BREV extracts reversed bits");
    expect(top[0].regs[11] == 0xffffffb0u,
           "BFE.S32.BREV sign extends the reversed field");
    std::cout << "PASS: BFE.BREV signed and unsigned\n";
}

void testAluEdgeCases(MTL::Device* device) {
    AluState input{};
    input.regs[1] = 0;
    input.regs[2] = std::bit_cast<u32>(std::numeric_limits<f32>::infinity());
    input.regs[3] = std::bit_cast<u32>(0.75f);
    input.regs[4] = 0x80000001u;
    input.regs[5] = 32u | (8u << 8);
    input.regs[6] = 0xffffffffu;
    input.regs[7] = 0x7c007e00u; // infinity / NaN halves
    input.regs[8] = halfBits(0.25f) | (halfBits(-0.5f) << 16);
    const auto reg = [](u64 op, u32 dst, u32 a, u32 b) {
        return op | PT_GUARD | dst | (static_cast<u64>(a) << 8) |
               (static_cast<u64>(b) << 20);
    };
    const std::vector<u64> program = {
        reg(0x5c68000000000000ull, 10, 1, 2) | (2ull << 44),
        reg(0x5980000000000000ull, 11, 1, 2) | (3ull << 39) | (2ull << 53),
        reg(0x5d08000000000000ull, 12, 1, 7) | (2ull << 39),
        reg(0x5d00000000000000ull, 13, 1, 7) | (8ull << 39) | (2ull << 37),
        reg(0x5c00000000000000ull, 14, 4, 5) | (1ull << 48),
        0x1d80000000000000ull | PT_GUARD | 15 | (6ull << 8) | (1ull << 20),
        reg(0x5cc0000000000000ull, 16, 6, 6) | (6ull << 39),
        reg(0x5c18000000000000ull, 17, 6, 6) | (1ull << 39),
        reg(0x5bd0000000000000ull, 18, 6, 6) | (1ull << 39),
    };
    const auto result = runAlu(device, program, {input}, {}, 2, "ALU edges run")[0];
    expect(result.regs[10] == 0, "FMUL.FMZ zero times infinity is positive zero");
    expect(result.regs[11] == input.regs[3], "FFMA.FMZ zero product keeps addend");
    expect(result.regs[12] == 0, "HMUL2.FMZ suppresses infinity and NaN per lane");
    expect(result.regs[13] == input.regs[8], "HFMA2.FMZ keeps both addend lanes");
    expect(result.regs[14] == 0xffffffffu, "Signed BFE beyond the word replicates sign");
    expect(result.regs[15] == 1, "IADD32I.PO decodes and adds one");
    expect(result.regs[16] == 0xfffffffdu, "IADD3 wraps three unsigned inputs");
    expect(result.regs[17] == 0xfffffffdu, "ISCADD wraps shifted addition");
    expect(result.regs[18] == 0xfffffffdu, "LEA wraps shifted addition");
    std::vector<AluState> fields;
    for (const u32 base : {0u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        for (const u32 offset : {0u, 1u, 31u, 32u, 33u, 255u}) {
            for (const u32 width : {0u, 1u, 8u, 32u, 255u}) {
                AluState field{};
                field.regs[1] = base;
                field.regs[2] = offset | (width << 8);
                fields.push_back(field);
            }
        }
    }
    const auto extracted = runAlu(
        device, {reg(0x5c00000000000000ull, 10, 1, 2) | (1ull << 48) |
                 (1ull << 47)}, fields, {}, 2, "BFE edge fields run");
    for (usize i = 0; i < fields.size(); i++) {
        const u32 base = fields[i].regs[1];
        const u32 offset = fields[i].regs[2] & 255;
        const u32 width = (fields[i].regs[2] >> 8) & 255;
        u32 expected = 0;
        if (width != 0) {
            const u32 shift = std::min(offset, 31u);
            const u32 bits = std::min(width, 32u - shift);
            const u32 low = base >> shift;
            const u32 mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
            expected = low & mask;
            if ((expected & (1u << (bits - 1))) != 0)
                expected |= ~mask;
        }
        expect(extracted[i].regs[10] == expected, "Signed BFE clamps and sign replicates");
        const u32 cc = (expected == 0 ? 1u : 0u) | ((expected >> 31) << 1);
        expect(extracted[i].cc == cc, "BFE.CC sets Z/S and clears C/O");
    }
    std::cout << "PASS: FMZ, signed BFE edges, IADD32I.PO and wrapping ALU\n";
}

// The fixed function alpha test discards fragments whose render target 0
// alpha fails the comparison
void testAlphaTest(MTL::Device* device) {
    struct Case {
        hw::tegra_x1::gpu::engines::CompareOp func;
        f32 ref;
        bool written;
    };
    const Case cases[] = {
        {hw::tegra_x1::gpu::engines::CompareOp::Greater, 0.5f, false},
        {hw::tegra_x1::gpu::engines::CompareOp::Greater, 0.1f, true},
        {hw::tegra_x1::gpu::engines::CompareOp::LessEqual, 0.25f, true},
        {hw::tegra_x1::gpu::engines::CompareOp::Never, 0.0f, false},
        {hw::tegra_x1::gpu::engines::CompareOp::Invalid, 0.0f, true},
    };
    for (const auto& test_case : cases) {
        DecompilerContext context{};
        context.type = ShaderType::Fragment;
        GuestShaderState guest{};
        guest.color_target_data_types[0] = MTL::DataTypeFloat;
        guest.alpha_test_func = test_case.func;
        guest.alpha_test_ref = test_case.ref;
        ir::Module module;
        ir::Builder builder(module);
        for (u8 i = 0; i < 4; ++i)
            builder.opCopy(ir::Value::createRegister(i, ir::ScalarType::F32),
                           ir::Value::createConstantF(i == 3 ? 0.25f : 1.0f));
        builder.opExit();
        const auto source = emitMsl(context, guest, module) + VERTEX_PASSTHROUGH;
        NS_STACK_SCOPED auto library =
            compileLibrary(device, source, "Alpha test shader compiles");
        NS_STACK_SCOPED auto vertex =
            library->newFunction(toNsString("test_vertex"));
        NS_STACK_SCOPED auto fragment =
            library->newFunction(toNsString("main_"));
        const std::array<float, 12> positions{-1, -1, 0, 1, 3, -1,
                                              0,  1,  -1, 3, 0, 1};
        RenderSetup setup{.vertex = vertex, .fragment = fragment};
        setup.draw = [&](MTL::RenderCommandEncoder* encoder) {
            encoder->setVertexBytes(positions.data(), sizeof(positions), 0);
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                    NS::UInteger(3));
        };
        const auto pixels = render(device, setup, "Alpha test draw");
        expect((pixelAt<u32>(pixels, 8, 4, 4) != 0) == test_case.written,
               "Alpha test keeps or discards the fragment");
    }
    std::cout << "PASS: fixed function alpha test functions\n";
}

// Without the viewport transform the vertex shader outputs window
// coordinates, from the top or the bottom of the surface
void testWindowSpacePositions(MTL::Device* device) {
    for (const bool lower_left : {false, true}) {
        DecompilerContext context{};
        context.type = ShaderType::Vertex;
        GuestShaderState state{};
        for (auto& attribute : state.vertex_attrib_states)
            attribute.is_fixed = true;
        state.window_space_width = 8;
        state.window_space_height = 8;
        state.y_negate = lower_left;
        ir::Module module;
        ir::Builder builder(module);
        // Triangle (0, 0), (16, 0), (0, 8) in window coordinates
        const auto vid =
            ir::Value::createAttrMemory({RZ, SV_VERTEX_ID_BASE, true});
        const auto position = [&](u32 component, u32 id, float value) {
            builder.opCopy(
                ir::Value::createAttrMemory(
                    {RZ, SV_POSITION_BASE + component * 4, false},
                    ir::ScalarType::F32),
                builder.opSelect(
                    builder.opCompareEqual(vid, ir::Value::createConstant(id)),
                    ir::Value::createConstantF(value),
                    ir::Value::createConstantF(0.0f)));
        };
        position(0, 1, 16.0f);
        position(1, 2, 8.0f);
        builder.opExit();
        auto source = emitMsl(context, state, module) + solidFragment();
        NS_STACK_SCOPED auto library =
            compileLibrary(device, source, "Window space shader compiles");
        NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("main_"));
        NS_STACK_SCOPED auto fragment =
            library->newFunction(toNsString("solid_fragment"));
        RenderSetup setup{.vertex = vertex, .fragment = fragment};
        setup.draw = [](MTL::RenderCommandEncoder* encoder) {
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                    NS::UInteger(3));
        };
        const auto pixels = render(device, setup, "Window space draw");
        const bool top_covered = pixels[(0 * 8 + 7) * 4] == 255;
        const bool bottom_covered = pixels[(7 * 8 + 7) * 4] == 255;
        expect(top_covered == !lower_left && bottom_covered == lower_left,
               "Window space rows count from the window origin");
        expect(pixels[(0 * 8 + 0) * 4] == 255 && pixels[(7 * 8 + 0) * 4] == 255,
               "Window space columns map to the surface width");
    }
    std::cout << "PASS: window space vertex positions with both window "
                 "origins\n";
}

// The second color output blends the first (SRC1 factors)
void testDualSourceBlending(MTL::Device* device) {
    DecompilerContext context{};
    context.type = ShaderType::Fragment;
    GuestShaderState guest{};
    guest.color_target_data_types[0] = MTL::DataTypeFloat;
    guest.dual_source_blending = true;
    guest.color_target_data_types[1] = MTL::DataTypeFloat;
    ir::Module module;
    ir::Builder builder(module);
    const float colors[8] = {0.25f, 0.5f, 0.75f, 1.0f, 1.0f, 0.5f, 0.0f, 1.0f};
    for (u8 i = 0; i < 8; ++i)
        builder.opCopy(ir::Value::createRegister(i, ir::ScalarType::F32),
                       ir::Value::createConstantF(colors[i]));
    builder.opExit();
    const auto source = emitMsl(context, guest, module) + VERTEX_PASSTHROUGH;
    NS_STACK_SCOPED auto library =
        compileLibrary(device, source, "Dual source shader compiles");
    NS_STACK_SCOPED auto vertex = library->newFunction(toNsString("test_vertex"));
    NS_STACK_SCOPED auto fragment = library->newFunction(toNsString("main_"));
    const std::array<float, 12> positions{-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    RenderSetup setup{.vertex = vertex, .fragment = fragment};
    setup.blend = [](MTL::RenderPipelineColorAttachmentDescriptor* attachment) {
        attachment->setBlendingEnabled(true);
        attachment->setSourceRGBBlendFactor(MTL::BlendFactorSource1Color);
        attachment->setDestinationRGBBlendFactor(MTL::BlendFactorZero);
        attachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
    };
    setup.draw = [&](MTL::RenderCommandEncoder* encoder) {
        encoder->setVertexBytes(positions.data(), sizeof(positions), 0);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                                NS::UInteger(3));
    };
    const auto pixels = render(device, setup, "Dual source draw");
    const auto channel = [&](u32 c) { return pixels[(4 * 8 + 4) * 4 + c]; };
    // Output 0 times output 1 per channel
    expect(std::abs(int(channel(0)) - 64) <= 1 &&
               std::abs(int(channel(1)) - 64) <= 1 && channel(2) == 0 &&
               channel(3) == 255,
           "The second output blends the first with SRC1 factors");
    std::cout << "PASS: dual source blending through color 0 index 1\n";
}

// CAL/RET subroutines (also nested and predicated), PRET/RET calls and a RET
// with an empty call stack
void testSubroutines(MTL::Device* device) {
    const auto mov = [](u32 dst, u32 value) -> u64 {
        return 0x0100000000000f00ull | dst | PT_GUARD | (u64(value) << 20);
    };
    const auto cal = [](u32 from_word, u32 to_word, u32 pred = 7,
                        bool pred_inv = false) -> u64 {
        const u64 imm = u64(to_word - from_word - 1) * 8;
        return 0xe260000000000000ull | 0xf | (u64(pred) << 16) |
               (u64(pred_inv) << 19) | (imm << 20);
    };
    const u64 ret = 0xe320000000000000ull | 0xf | PT_GUARD;
    const u64 exit = 0xe300000000000000ull | 0xf | PT_GUARD;
    // Words: a scheduling word precedes every 3 instructions (word 4n)
    auto word = [](u32 k) { return k + 1 + k / 3; };
    // k: 0 mov, 1 cal A, 2 mov, 3 cal B (predicated off), 4 mov, 5 exit
    //    6 A: mov, 7 cal C, 8 ret   9 C: mov, 10 ret   11 B: mov, 12 ret
    const std::vector<u64> program = {
        mov(10, 1),                  // k0
        cal(word(1), word(6)),       // k1
        mov(12, 3),                  // k2
        cal(word(3), word(11), 0),   // k3 @P0 CAL B
        mov(14, 5),                  // k4
        exit,                        // k5
        mov(11, 2),                  // k6 A
        cal(word(7), word(9)),       // k7
        ret,                         // k8
        mov(13, 4),                  // k9 C
        ret,                         // k10
        mov(15, 6),                  // k11 B
        ret,                         // k12
    };
    AluState off{};
    AluState on{};
    on.preds = 1; // P0
    const auto results = runAlu(device, program, {off, on}, {}, 2, "Calls run");
    for (u32 run = 0; run < 2; run++) {
        const auto& r = results[run].regs;
        expect(r[10] == 1 && r[11] == 2 && r[12] == 3 && r[13] == 4 &&
                   r[14] == 5,
               "Calls, nested calls and returns run in order");
        expect(r[15] == (run == 1 ? 6u : 0u),
               "A predicated call runs only when its predicate holds");
    }
    // RET without a call ends the shader
    const auto ended = runAlu(device, {mov(10, 1), ret, mov(11, 2)}, {off}, {},
                              2, "Return without call runs");
    expect(ended[0].regs[10] == 1 && ended[0].regs[11] == 0,
           "Return with an empty call stack ends the shader");
    std::cout << "PASS: CAL/RET subroutines, nesting, predicates and empty "
                 "call stacks\n";
}

void testExitTargetsAndAbsoluteJump(MTL::Device* device) {
    const auto word = [](u32 i) { return i + 1 + i / 3; };
    const auto mov = [](u32 dst, u32 value) {
        return 0x0100000000000000ull | PT_GUARD | dst |
               (static_cast<u64>(value) << 20);
    };
    const u64 exit = 0xe30000000007000full;
    const auto pexit = [&](u32 from, u32 to) {
        return 0xe230000000000000ull |
               (static_cast<u64>((word(to) - word(from) - 1) * 8) << 20);
    };
    const auto jmp = [&](u32 to, u32 pred) {
        return 0xe21000000000000full | (static_cast<u64>(pred) << 16) |
               (static_cast<u64>(word(to) * 8) << 20);
    };
    // Nested exit targets unwind once each. The conditional exit keeps its
    // target pending on the untaken path.
    const std::vector<u64> program = {
        pexit(0, 9), pexit(1, 6), mov(10, 1),
        0xe30000000000000full, mov(11, 2), exit,
        mov(12, 3), exit, mov(13, 4),
        mov(14, 5), jmp(13, 0), mov(15, 6),
        jmp(14, 7), mov(16, 7), exit,
    };
    AluState off{};
    AluState on{};
    on.preds = 1;
    const auto results = runAlu(device, program, {off, on}, {}, 2,
                                "Exit targets and absolute jumps run");
    for (usize i = 0; i < results.size(); i++) {
        const auto& r = results[i].regs;
        expect(r[10] == 1 && r[11] == (i == 0 ? 2u : 0u) && r[12] == 3 &&
                   r[13] == 0 && r[14] == 5,
               "PEXIT handles nested and conditional exits");
        expect(r[15] == (i == 0 ? 6u : 0u) && r[16] == (i == 0 ? 0u : 7u),
               "JMP uses absolute byte addresses and instruction predicates");
    }
    for (const u32 base : {0x50u, 0x40050u}) {
        auto relocated = program;
        for (auto& instruction : relocated) {
            if ((instruction & 0xfff0000000000000ull) == 0xe210000000000000ull)
                instruction += static_cast<u64>(base) << 20;
        }
        const auto moved = runAlu(device, relocated, {off, on}, {}, 2,
                                   "Relocated JMP runs", base);
        expect(moved[0].regs == results[0].regs &&
                   moved[1].regs == results[1].regs,
               "Absolute JMP accounts for the program offset and graphics header");
    }
    // A constant-buffer address participates in shader cache specialization.
    for (const u32 base : {0u, 0x40050u}) {
        const u64 jump_cbuf = 0xe21000000000002full | PT_GUARD |
                              (2ull << 36) | (16ull << 20);
        std::vector<u32> table(8);
        table[4] = base + word(3) * 8;
        const auto taken = runAlu(device,
            {jump_cbuf, mov(10, 1), exit, mov(11, 2), exit},
            {off}, table, 2, "Constant-buffer JMP runs", base)[0];
        expect(taken.regs[10] == 0 && taken.regs[11] == 2,
               "JMP resolves signed byte offset and slot from constant buffer");
        // JMX gets an absolute address from a bounded LDC jump table.
        const std::vector<u64> indirect{
            0x3820000000000000ull | PT_GUARD | 1 | (1ull << 8) |
                (1ull << 20) | (7ull << 39), // IMNMX R1, R1, 1, PT
            0x3848000000000000ull | PT_GUARD | 2 | (1ull << 8) |
                (2ull << 20),              // SHL R2, R1, 2
            0xef90000000000000ull | PT_GUARD | 3 | (2ull << 8) |
                (2ull << 36) | (4ull << 48), // LDC R3, c2[R2]
            0xe20000000000000full | PT_GUARD | (3ull << 8),
            mov(10, 3), exit, mov(11, 4), exit,
        };
        table[0] = base + word(4) * 8;
        table[1] = base + word(6) * 8;
        AluState first{}, second{}, clamped{};
        second.regs[1] = 1;
        clamped.regs[1] = 99;
        const auto branches = runAlu(device, indirect, {first, second, clamped},
                                     table, 2, "Bounded JMX runs", base);
        expect(branches[0].regs[10] == 3 && branches[0].regs[11] == 0 &&
                   branches[1].regs[10] == 0 && branches[1].regs[11] == 4 &&
                   branches[2].regs[11] == 4,
               "JMX uses absolute bounded table targets, relocation and clamping");
    }
    std::cout << "PASS: PEXIT targets and absolute JMP predicates\n";
}

void runShaderCoverageTests(MTL::Device* device) {
    testFrontFacing(device);
    testPointSizeAndCoord(device);
    testClipDistances(device);
    testViewportAndLayerOutputs(device);
    testWindowSpacePositions(device);
    testSpecialRegisters(device);
    testFlowConditions(device);
    testSetConditionFlags(device);
    testPrmt(device);
    testImul(device);
    testP2r(device);
    testHalfCompare(device);
    testLdcSmallIntegers(device);
    testXmadConstantMultiply(device);
    testTextureQuery(device);
    testMipLevelQuery(device);
    testSurfaceAccess(device);
    testTexsVariants(device);
    testTldsEncodings(device);
    testTexAoffi(device);
    testTxdGradientOrder(device);
    testTxdVolumeAndCube(device);
    testUnnormalizedTextureCoordinates(device);
    testSharedCompareExchange(device);
    testGlobalMemory(device);
    testDoublePrecision(device);
    testDoubleConversions(device);
    testIndirectBranch(device);
    testConversions(device);
    testHalfOperands(device);
    testBitfieldExtractReverse(device);
    testAluEdgeCases(device);
    testAlphaTest(device);
    testDualSourceBlending(device);
    testSubroutines(device);
    testExitTargetsAndAbsoluteJump(device);
}
