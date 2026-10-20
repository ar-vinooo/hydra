#pragma once

#include <string>
#include <vector>

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp {

// Collects the guest features a decompile skipped, so a shader that compiles
// but computes something different can be traced to the instruction family.
// decompile() installs one per thread; without one the report is dropped.
class UnsupportedCollector {
  public:
    UnsupportedCollector() : previous{current} { current = this; }
    ~UnsupportedCollector() { current = previous; }
    UnsupportedCollector(const UnsupportedCollector&) = delete;
    UnsupportedCollector& operator=(const UnsupportedCollector&) = delete;

    static void report(std::string feature) {
        if (!current)
            return;
        for (const auto& known : current->features)
            if (known == feature)
                return;
        current->features.push_back(std::move(feature));
    }

    // Sorted by first occurrence; each feature once
    std::vector<std::string> features;

  private:
    inline static thread_local UnsupportedCollector* current{nullptr};
    UnsupportedCollector* previous;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp

// Logs like LOG_NOT_IMPLEMENTED and records the feature for the shader
#define SHADER_UNSUPPORTED(f, ...)                                             \
    do {                                                                       \
        LOG_NOT_IMPLEMENTED(ShaderDecompiler, f, ##__VA_ARGS__);               \
        ::hydra::hw::tegra_x1::gpu::renderer::shader_decomp::                  \
            UnsupportedCollector::report(fmt::format(f, ##__VA_ARGS__));       \
    } while (false)
// The log is printed once per process, the record for every shader
#define SHADER_UNSUPPORTED_ONCE(f, ...)                                        \
    do {                                                                       \
        ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler, f, ##__VA_ARGS__));         \
        ::hydra::hw::tegra_x1::gpu::renderer::shader_decomp::                  \
            UnsupportedCollector::report(fmt::format(f, ##__VA_ARGS__));       \
    } while (false)
