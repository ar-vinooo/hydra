// Decompiles every dumped guest shader of a directory (HYDRA_DUMP_SHADERS
// files named gpu_<address>_<stage>_<index>.bin) and compiles the generated
// MSL with the production options. Reports decompiler failures, instructions
// the decoder does not know and sources Metal rejects.
//
//   hydra-shader-corpus-tests <directory> [limit]
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include "core/debugger/debugger_manager.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.hpp"

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;
using hydra::hw::tegra_x1::gpu::renderer::metal::toNsString;

namespace {

struct Failure {
    std::string file;
    std::string reason;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: hydra-shader-corpus-tests <directory> [limit]\n";
        return 2;
    }
    const usize limit = argc > 2 ? std::stoul(argv[2]) : ~usize{0};
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
        if (entry.path().extension() == ".bin" &&
            entry.path().filename().string().starts_with("gpu_"))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.size() > limit)
        files.resize(limit);

    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    auto* device = MTL::CreateSystemDefaultDevice();

    // One thread and the name of each file before its log lines
    const bool verbose = std::getenv("HYDRA_CORPUS_VERBOSE") != nullptr;
    std::atomic<usize> next{0};
    std::atomic<usize> compiled{0};
    std::mutex mutex;
    std::vector<Failure> failures;
    const auto worker = [&] {
        NS_STACK_SCOPED auto thread_pool = NS::AutoreleasePool::alloc()->init();
        auto& debugger = DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess();
        debugger.registerThisThread("Shader corpus");
        while (true) {
            const usize index = next.fetch_add(1);
            if (index >= files.size())
                break;
            const auto& path = files[index];
            const auto name = path.filename().string();
            const auto type = name.find("_compute_") != std::string::npos
                                  ? ShaderType::Compute
                              : name.find("_vertex_") != std::string::npos
                                  ? ShaderType::Vertex
                                  : ShaderType::Fragment;
            if (verbose)
                std::cerr << "FILE " << name << '\n';
            std::ifstream in(path, std::ios::binary);
            std::vector<u8> code((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            GuestShaderState state{};
            if (type == ShaderType::Fragment) {
                state.color_target_data_types[0] = MTL::DataTypeFloat;
            }
            ShaderBackend backend{};
            std::vector<u8> out;
            ResourceMapping mapping;
            ztd::io::MemoryStream stream(code);
            shader_decomp::decompile(stream, type, state, backend, out,
                                     mapping);
            if (out.empty()) {
                std::scoped_lock lock(mutex);
                failures.push_back({name, "empty output"});
                continue;
            }
            const std::string source(out.begin(), out.end());
            if (const char* dump = std::getenv("HYDRA_CORPUS_DUMP_DIR"))
                std::ofstream(std::filesystem::path(dump) /
                              (path.stem().string() + ".metal"))
                    << source;
            NS::Error* error = nullptr;
            NS_STACK_SCOPED auto options = MTL::CompileOptions::alloc()->init();
            options->setFastMathEnabled(false);
            options->setPreserveInvariance(true);
            auto* library =
                device->newLibrary(toNsString(source), options, &error);
            if (library == nullptr) {
                std::scoped_lock lock(mutex);
                failures.push_back(
                    {name, error != nullptr
                               ? error->localizedDescription()->utf8String()
                               : "compile failed"});
            } else {
                library->release();
                compiled++;
            }
        }
        debugger.unregisterThisThread();
    };
    const usize thread_count =
        verbose ? 1
                : std::max<usize>(
                      1, std::min<usize>(4, std::thread::hardware_concurrency()));
    std::vector<std::thread> threads;
    for (usize i = 0; i < thread_count; i++)
        threads.emplace_back(worker);
    for (auto& thread : threads)
        thread.join();

    std::map<std::string, std::vector<std::string>> by_reason;
    for (const auto& failure : failures) {
        // The first error line without positions groups similar failures
        std::string reason = failure.reason.substr(0, failure.reason.find('\n'));
        const auto colon = reason.find("error:");
        if (colon != std::string::npos)
            reason = reason.substr(colon);
        by_reason[reason].push_back(failure.file);
    }
    std::cout << files.size() << " shaders, " << compiled << " compiled, "
              << failures.size() << " failed\n";
    for (const auto& [reason, names] : by_reason) {
        std::cout << "  " << names.size() << "x " << reason << "  (e.g. "
                  << names.front() << ")\n";
    }
    return failures.empty() ? 0 : 1;
}
