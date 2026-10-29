#include "core/hw/tegra_x1/gpu/renderer/metal/shader.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"

#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/shader_library_cache.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <atomic>
#include <fstream>
#include <optional>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

Shader::Shader(MTL::Device* device, const ShaderDescriptor& descriptor,
               ShaderLibraryCache& library_cache)
    : ShaderBase(descriptor) {
    ScopedGpuWork work(GpuWork::Shader);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(descriptor.code.data(),
              static_cast<CC_LONG>(descriptor.code.size()), digest);
    std::string digest_hex;
    for (const auto byte : digest)
        digest_hex += fmt::format("{:02x}", byte);
    cache_key = fmt::format("{}:{}:invariant-no-fast-math:{}", descriptor.type,
                            descriptor.backend, digest_hex);
    // Library
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
        library = library_cache.get(source, &error);
        library_key = ShaderLibraryCache::keyFromDigest(digest_hex);
        if (auto& visual = VisualDiagnostics::instance(); visual.selected())
            visual.event("metal_library_result", {{"cache_key", cache_key},
                {"source_fnv1a64", VisualDiagnostics::contentId(descriptor.code)},
                {"library_available", library ? "1" : "0"},
                {"message", error ? error->localizedDescription()->utf8String() : ""},
                {"note", "library cache may satisfy this request; this does not establish semantic correctness"}});
        if (library == nullptr) {
            LOG_ERROR(MetalRenderer, "Failed to create Metal library: {}",
                      error ? error->localizedDescription()->utf8String()
                            : "unknown error");
            return;
        }
        break;
    }
    case ShaderBackend::Air: {
        std::optional<ScopedPerfTimer> compile_timer;
        if (PerfStats::isEnabled()) {
            compile_timer.emplace(PerfStats::instance().shader_compile_ns);
            PerfStats::instance().shader_compiles++;
        }
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
    if (function != nullptr && usesSamplerTable() &&
        device->argumentBuffersSupport() != MTL::ArgumentBuffersTier2)
        sampler_encoder = function->newArgumentEncoder(
            descriptor.resource_mapping.sampler_buffer_index);
    if (auto& visual = VisualDiagnostics::instance(); visual.selected())
        visual.event("metal_entry_point", {{"cache_key", cache_key}, {"main_available", function ? "1" : "0"}});
    library->release();
}

Shader::~Shader() {
    if (sampler_encoder)
        sampler_encoder->release();
    if (function)
        function->release();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
