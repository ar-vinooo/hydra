#include "core/hot/table.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace hydra;
namespace fs = std::filesystem;
using namespace hw::tegra_x1::gpu::renderer;
using namespace hw::tegra_x1::gpu::renderer::metal;

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void waitRevision(u64 revision) {
    const auto limit =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (hot::generation() < revision &&
           std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(hot::generation() == revision, "Live revision did not arrive");
}
void load(const fs::path& fixture, const fs::path& module) {
    fs::copy_file(fixture, module, fs::copy_options::overwrite_existing);
    // Explicit reload works even if the fixture has the same timestamp.
    std::ofstream(module.string() + ".reload").put('\n');
}
u8 draw(MTL::Device* device) {
    using namespace shader_decomp;
    // A real guest fragment shader: MOV32I R0=.25, R1=0, R2=0, R3=1, EXIT.
    std::vector<u64> words(10, 0);
    words[0] = 3u << 5;
    words[9] = 0xf;
    const std::array<u32, 4> values{0x3e800000, 0, 0, 0x3f800000};
    for (u64 i = 0; i < 4; ++i) {
        if (i % 3 == 0)
            words.push_back(0);
        words.push_back(0x010000000000f000ull | i | (u64(values[i]) << 20));
    }
    words.push_back(0xe30000000007000full);
    words.push_back(0x50b0000000070f00ull);
    std::vector<u8> bytes(words.size() * 8);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    ztd::io::MemoryStream stream(bytes);
    GuestShaderState guest{};
    guest.color_target_data_types[0] = MTL::DataTypeFloat;
    ShaderBackend backend;
    std::vector<u8> code;
    ResourceMapping mapping;
    decompile(stream, ShaderType::Fragment, guest, backend, code, mapping);
    std::string source(code.begin(), code.end());
    source += "\nvertex float4 live_vertex(uint id [[vertex_id]]) { return "
              "float4(id==1?3.0:-1.0,id==2?3.0:-1.0,0,1); }\n";
    NS::Error* error = nullptr;
    NS_STACK_SCOPED auto library =
        device->newLibrary(toNsString(source), nullptr, &error);
    if (!library && error)
        std::cerr << error->localizedDescription()->utf8String() << '\n';
    check(library != nullptr, "Live shader compile");
    NS_STACK_SCOPED auto vertex =
        library->newFunction(toNsString("live_vertex"));
    NS_STACK_SCOPED auto fragment = library->newFunction(toNsString("main_"));
    NS_STACK_SCOPED auto pd = MTL::RenderPipelineDescriptor::alloc()->init();
    pd->setVertexFunction(vertex);
    pd->setFragmentFunction(fragment);
    pd->colorAttachments()->object(0)->setPixelFormat(
        MTL::PixelFormatRGBA8Unorm);
    NS_STACK_SCOPED auto pipeline = device->newRenderPipelineState(pd, &error);
    check(pipeline != nullptr, "Live pipeline compile");
    auto* td = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatRGBA8Unorm, 4, 4, false);
    td->setStorageMode(MTL::StorageModeShared);
    td->setUsage(MTL::TextureUsageRenderTarget);
    NS_STACK_SCOPED auto texture = device->newTexture(td);
    auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    pass->colorAttachments()->object(0)->setTexture(texture);
    pass->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionClear);
    pass->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);
    NS_STACK_SCOPED auto queue = device->newCommandQueue();
    auto* commands = queue->commandBuffer();
    auto* encoder = commands->renderCommandEncoder(pass);
    encoder->setRenderPipelineState(pipeline);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                            NS::UInteger(3));
    encoder->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    check(commands->status() == MTL::CommandBufferStatusCompleted,
          "Live draw completes");
    std::array<u8, 64> pixels{};
    texture->getBytes(pixels.data(), 16, MTL::Region::Make2D(0, 0, 4, 4), 0);
    for (usize i = 0; i < 16; ++i)
        check(pixels[i * 4] == pixels[0], "Live pixels consistent");
    return pixels[0];
}
int main(int argc, char** argv) {
    try {
        check(argc == 5, "Usage: hot-tests production-module fixture-A "
                         "fixture-B rejected-fixture");
        const auto dir = fs::temp_directory_path() /
                         fmt::format("hydra-hot-test-{}", getpid());
        fs::create_directories(dir);
        const auto module = dir / "live.so";
        setenv("HYDRA_HOT_MODULE", module.c_str(), 1);
        // Cache warmup may be the first hot caller. Debugger log callbacks
        // require guest thread context, so runtime initialization must not
        // invoke them from this background thread.
        const auto main_thread = std::this_thread::get_id();
        LOGGER_INSTANCE.installCallback([main_thread](const LogMessage&) {
            check(std::this_thread::get_id() == main_thread,
                  "Hot startup called a guest logger from a worker thread");
        });
        std::exception_ptr startup_error;
        std::thread startup([&] {
            try {
                (void)hot::generation();
            } catch (...) {
                startup_error = std::current_exception();
            }
        });
        startup.join();
        LOGGER_INSTANCE.uninstallCallback();
        if (startup_error)
            std::rethrow_exception(startup_error);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        NS_STACK_SCOPED auto device = MTL::CreateSystemDefaultDevice();
        const auto builtin = hot::getTable().shader_decompile;
        check(draw(device) == 64, "Built-in guest shader output");
        load(argv[1], module);
        waitRevision(1);
        check(hot::getTable().shader_decompile != builtin,
              "Production module replaces host entry point");
        check(draw(device) == 64, "Production module preserves GPU output");
        metal::DepthStencilStateDescriptor depth{};
        const auto hash1 = metal::DepthStencilStateCache::hash(depth);
        load(argv[2], module);
        waitRevision(2);
        check(draw(device) == 64, "First fixture GPU output");
        check(metal::DepthStencilStateCache::hash(depth) != hash1,
              "GPU caches change identity after reload");
        {
            hot::Scope pinned;
            load(argv[3], module);
            std::thread observer([] { waitRevision(3); });
            observer.join();
            check(hot::generation() == 2,
                  "Nested calls retain their original image");
            check(draw(device) == 64, "In-flight draw keeps old shader");
        }
        check(hot::generation() == 3 && draw(device) == 191,
              "Next draw sees new GPU result without restart");
        load(argv[4], module);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        check(hot::generation() == 3 && draw(device) == 191,
              "Rejected module keeps last working code");
        std::ofstream(module.string() + ".rollback").put('\n');
        waitRevision(5);
        check(draw(device) == 64, "Rollback restores previous GPU result");
        std::cout
            << "PASS: production dlopen, live Metal pixels, pinned calls, "
               "cache revisions, ABI rejection and rollback in PID "
            << getpid() << '\n';
        fs::remove_all(dir);
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
