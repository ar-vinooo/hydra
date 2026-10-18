// Guest Maxwell MOV32I -> production shader/pipeline caches -> Metal pixels.
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/shader.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/pipeline.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include <sys/mman.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <chrono>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using metal::stackAutoRelease;

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<u8> program(u32 padding, const std::array<u32, 4>& values) {
    std::vector<u64> words(10);
    words[0] = 3u << 5; // fragment stage
    words[9] = 0xf;     // RGBA output
    u32 instructions = 0;
    const auto emit = [&](u64 instruction) {
        if (instructions++ % 3 == 0) words.push_back(0); // scheduling word
        words.push_back(instruction);
    };
    for (u32 i = 0; i < padding; ++i) emit(0x50b0000000070f00ull); // NOP
    for (u32 i = 0; i < 4; ++i)
        emit(0x010000000000f000ull | i | (u64(values[i]) << 20));
    emit(0xe30000000007000full); // EXIT
    check(words.size() * 8 <= 0x10000, "Program exceeds bounded decoder window");
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
        auto* td = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatRGBA32Float, 1, 1, false);
        td->setStorageMode(MTL::StorageModeShared);
        td->setUsage(MTL::TextureUsageRenderTarget);
        NS_STACK_SCOPED auto texture = device->newTexture(td);
        std::vector<u8> a(0x10000), b(0x10000), c(0x10000);
        GMmu first(nullptr), second(nullptr);
        constexpr uptr address = 0x20000;
        const auto map = [&](GMmu& mmu, std::vector<u8>& backing) {
            mmu.unmapRange(address, 0x10000, false);
            mmu.map(address, {reinterpret_cast<uptr>(backing.data()), backing.size()});
        };
        map(first, a); map(second, c);
        tls_crnt_gmmu = &first;
        GuestShaderDescriptor descriptor{};
        descriptor.stage = engines::ShaderStage::Fragment;
        descriptor.gpu_addr = address;
        descriptor.code_ptr = reinterpret_cast<uptr>(a.data()); // deliberately stale after remap
        descriptor.state.color_target_data_types[0] = MTL::DataTypeFloat;
        const auto draw = [&](ShaderBase* fragment) {
            check(fragment != nullptr, "Missing shader");
            PipelineDescriptor pd{};
            pd.shaders[0] = vertex.get(); pd.shaders[1] = fragment;
            pd.topology_class = MTL::PrimitiveTopologyClassTriangle;
            pd.color_target_states[0].format = TextureFormat::RGBA32Float;
            pd.color_target_states[0].write_mask = engines::ColorWriteMask::All;
            auto* pipeline = static_cast<metal::Pipeline*>(renderer.getPipelineCache().find(pd));
            check(pipeline->getPipeline() != nullptr, "Pipeline compilation failed");
            auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* att = pass->colorAttachments()->object(0);
            att->setTexture(texture); att->setLoadAction(MTL::LoadActionClear);
            att->setStoreAction(MTL::StoreActionStore);
            auto* commands = queue->commandBuffer();
            auto* encoder = commands->renderCommandEncoder(pass);
            encoder->setRenderPipelineState(pipeline->getPipeline());
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
            encoder->endEncoding(); commands->commit(); commands->waitUntilCompleted();
            check(commands->status() == MTL::CommandBufferStatusCompleted, "Metal execution failed");
            std::array<u32, 4> pixels{};
            texture->getBytes(pixels.data(), 16, MTL::Region::Make2D(0, 0, 1, 1), 0);
            return pixels;
        };
        // Exact equality must also reject differing code in the same hash bucket.
        GuestShaderDescriptor x{}, y{};
        x.cache_key = y.cache_key = {1, 2};
        x.code_snapshot = std::make_shared<const std::vector<u8>>(8, 1);
        y.code_snapshot = std::make_shared<const std::vector<u8>>(8, 2);
        check(!ShaderCache::equals(x, y), "Hash collision reused different code");
        std::ifstream input(argv[1]); std::ofstream output(argv[2]);
        std::string line;
        ShaderBase* previous = nullptr;
        while (std::getline(input, line)) {
            std::istringstream row(line);
            u32 action, padding; std::array<u32, 4> values;
            row >> action >> padding;
            for (auto& value : values) row >> value;
            check(bool(row), "Malformed test case");
            const auto code = program(padding, values);
            std::copy(code.begin(), code.end(), a.begin());
            std::copy(a.begin(), a.end(), b.begin());
            std::copy(a.begin(), a.end(), c.begin());
            if (action == 5) map(*tls_crnt_gmmu, b);
            if (action == 6) {
                tls_crnt_gmmu->unmapRange(address, 0x10000, false);
                tls_crnt_gmmu->map(address, {reinterpret_cast<uptr>(a.data()), 128});
                tls_crnt_gmmu->map(address + 128, {reinterpret_cast<uptr>(b.data() + 128), b.size() - 128});
            }
            if (action == 7) tls_crnt_gmmu = tls_crnt_gmmu == &first ? &second : &first;
            if (action == 8 || action == 9) {
                tls_crnt_gmmu->unmapRange(address, 0x10000, false);
                if (action == 9)
                    tls_crnt_gmmu->map(address, {reinterpret_cast<uptr>(a.data()), 87});
                check(cache.find(descriptor) == nullptr, "Unmapped/truncated code reused cached shader");
                map(*tls_crnt_gmmu, a);
            }
            if (action >= 1 && action <= 4) {
                const auto before = cache.find(descriptor);
                const bool compute = action >= 3;
                const u32 method = action == 2 ? 0x369 : action == 4 ? 0x5a6 : 0x87;
                if (compute) gpu.channelContext().compute.method(method, 1);
                else gpu.get3DEngine().method(method, 1);
                check(cache.find(descriptor) == before,
                      "Invalidation duplicated an unchanged immutable shader");
            }
            if (action == 10) {
                const auto before = cache.find(descriptor);
                gpu.get3DEngine().method(0x87, 0x1010);
                check(cache.find(descriptor) == before, "Non-instruction bits invalidated instruction cache");
            }
            void* gpu_code = nullptr;
            if (action == 11 || action == 12) {
                check(padding == 0, "GPU patch fixture requires zero padding");
                gpu_code = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANON, -1, 0);
                check(gpu_code != MAP_FAILED, "GPU code allocation failed");
                const auto initial = program(0, {0x3e800000, 0x3e800000, 0x3e800000, 0x3f800000});
                std::memcpy(gpu_code, initial.data(), initial.size());
                tls_crnt_gmmu->unmapRange(address, 0x10000, false);
                tls_crnt_gmmu->map(address, {reinterpret_cast<uptr>(gpu_code), 0x10000});
                check(cache.find(descriptor) != nullptr, "Initial GPU-written-code shader missing");
                NS_STACK_SCOPED auto buffer = device->newBuffer(
                    gpu_code, 0x10000, MTL::ResourceStorageModeShared, nullptr);
                const char* source = R"(
#include <metal_stdlib>
using namespace metal;
kernel void patch(device ulong* code [[buffer(0)]], constant ulong* words [[buffer(1)]]) {
    code[11]=words[0]; code[12]=words[1]; code[13]=words[2]; code[15]=words[3];
})";
                NS::Error* error = nullptr;
                NS_STACK_SCOPED auto library = device->newLibrary(metal::toNsString(source), nullptr, &error);
                check(library != nullptr, "GPU patch compile failed");
                NS_STACK_SCOPED auto function = library->newFunction(metal::toNsString("patch"));
                NS_STACK_SCOPED auto pipeline = device->newComputePipelineState(function, &error);
                check(pipeline != nullptr && buffer != nullptr, "GPU patch resources failed");
                std::array<u64, 4> words{};
                for (u32 i = 0; i < 4; ++i)
                    words[i] = 0x010000000000f000ull | i | (u64(values[i]) << 20);
                std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
                auto* encoder = static_cast<metal::CommandBuffer*>(commands.get())->getComputeCommandEncoder();
                encoder->setComputePipelineState(pipeline);
                encoder->setBuffer(buffer, 0, 0);
                encoder->setBytes(words.data(), sizeof(words), 1);
                encoder->dispatchThreads(MTL::Size(1, 1, 1), MTL::Size(1, 1, 1));
                // Not committed by the test: the production invalidate handler
                // must submit/wait before the CPU translates GPU-written code.
                tls_crnt_command_buffer = commands.get();
                if (action == 11) gpu.get3DEngine().method(0x87, 1);
                else gpu.channelContext().compute.method(0x5a6, 1);
                tls_crnt_command_buffer = nullptr;
            }
            auto* fragment = cache.find(descriptor);
            check(cache.find(descriptor) == fragment, "Unchanged code failed cache reuse");
            const auto old_pixels = previous ? draw(previous) : std::array<u32, 4>{};
            const auto pixels = draw(fragment);
            for (auto word : pixels) output << word << ' ';
            for (auto word : old_pixels) output << word << ' ';
            output << '\n'; previous = fragment;
            if (gpu_code) {
                map(*tls_crnt_gmmu, a);
                munmap(gpu_code, 0x10000);
            }
        }
        const auto saved_code = a;
        const auto saved_shader = cache.find(descriptor);
        const auto alternate = program(0, {0x3f123456, 0x3f123456, 0x3f123456, 0x3f123456});
        for (auto* backing : {&a, &b, &c})
            std::copy(alternate.begin(), alternate.end(), backing->begin());
        check(cache.find(descriptor) != saved_shader, "Different code reused old identity");
        for (auto* backing : {&a, &b, &c})
            std::copy(saved_code.begin(), saved_code.end(), backing->begin());
        check(cache.find(descriptor) == saved_shader, "A -> B -> A duplicated shader artifacts");
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < 1000; ++i)
            check(cache.find(descriptor) == saved_shader, "Warm lookup changed identity");
        std::cout << "Warm shader lookup: "
                  << std::chrono::duration<double, std::micro>(
                         std::chrono::steady_clock::now() - start).count() / 1000
                  << " us/lookup (full 64 KiB validation)\n";
        tls_crnt_gmmu = nullptr;
        std::cout << "PASS: live shader identities, remaps, invalidation methods, old pipelines and Metal pixels\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
