#include "core/hw/tegra_x1/gpu/renderer/metal/shader.hpp"

#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <atomic>
#include <fstream>
#include <optional>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

Shader::Shader(MTL::Device* device, const ShaderDescriptor& descriptor)
    : ShaderBase(descriptor) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(descriptor.code.data(),
              static_cast<CC_LONG>(descriptor.code.size()), digest);
    cache_key = fmt::format("{}:{}:invariant-no-fast-math:", descriptor.type,
                            descriptor.backend);
    for (const auto byte : digest)
        cache_key += fmt::format("{:02x}", byte);
    // Options
    NS_STACK_SCOPED MTL::CompileOptions* options =
        MTL::CompileOptions::alloc()->init();
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (false) // TODO: make this configurable
        options->setFastMathEnabled(true);
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (true) // TODO: make this configurable
        options->setPreserveInvariance(true);

    // Library
    std::optional<ScopedPerfTimer> compile_timer;
    if (PerfStats::isEnabled()) {
        compile_timer.emplace(PerfStats::instance().shader_compile_ns);
        PerfStats::instance().shader_compiles++;
    }
    MTL::Library* library;
    switch (descriptor.backend) {
    case ShaderBackend::Msl: {
        // TODO: doesn't this copy the contents?
        std::string source;
        source.assign(descriptor.code.begin(), descriptor.code.end());

        // DEBUG: dump shaders when HYDRA_DUMP_SHADERS is set to a directory
        if (const char* dump_dir = std::getenv("HYDRA_DUMP_SHADERS")) {
            static std::atomic<u32> dump_index{0};
            std::ofstream out(
                fmt::format("{}/{:04}_{}_{}.metal", dump_dir, dump_index++,
                            descriptor.type,
                            fmt::ptr(static_cast<ShaderBase*>(this))),
                std::ios::binary);
            out << source;
        }

        NS::Error* error = nullptr;
        library = device->newLibrary(toNsString(source), options, &error);
        if (library == nullptr) {
            LOG_ERROR(MetalRenderer, "Failed to create Metal library: {}",
                      error ? error->localizedDescription()->utf8String()
                            : "unknown error");
            return;
        }
        break;
    }
    case ShaderBackend::Air: {
        auto dispatch_data =
            dispatch_data_create(descriptor.code.data(), descriptor.code.size(),
                                 dispatch_get_global_queue(0, 0),
                                 ^{
                                 });

        NS::Error* error = nullptr;
        // TODO: options
        library = device->newLibrary(dispatch_data, &error);
        dispatch_release(dispatch_data);
        if (library == nullptr) {
            LOG_ERROR(MetalRenderer, "Failed to create Metal library: {}",
                      error ? error->localizedDescription()->utf8String()
                            : "unknown error");
            return;
        }
        break;
    }
    default:
        LOG_FATAL(MetalRenderer,
                  "Unsupported shader backend {} for the Metal renderer",
                  descriptor.backend);
        break;
    }

    // Function
    function = library->newFunction(toNsString("main_"));
    library->release();
}

Shader::~Shader() {
    if (function)
        function->release();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
