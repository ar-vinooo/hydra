#include "core/hw/tegra_x1/gpu/optimization_policy.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace hydra::hw::tegra_x1::gpu::optimization {
bool enabled() {
#if defined(HYDRA_GPU_OPTIMIZATION_FINGERPRINT) &&                             \
    defined(HYDRA_GPU_OPTIMIZATION_BUILD_ALLOWED)
#if HYDRA_GPU_OPTIMIZATION_BUILD_ALLOWED
    static const bool verified = [] {
        const auto* requested = std::getenv("HYDRA_GPU_OPTIMIZATIONS");
        if (!requested || std::string_view(requested) != "1")
            return false;
        const auto* path = std::getenv("HYDRA_GPU_OPTIMIZATION_RECEIPT");
        if (!path || !*path)
            return false;
        std::ifstream input(path);
        std::string schema, fingerprint, extra;
        return static_cast<bool>(std::getline(input, schema)) &&
               static_cast<bool>(std::getline(input, fingerprint)) &&
               schema == "HYDRA_GPU_P0_V1" &&
               fingerprint == HYDRA_GPU_OPTIMIZATION_FINGERPRINT &&
               !std::getline(input, extra);
    }();
    return verified;
#endif
#endif
    return false;
}
} // namespace hydra::hw::tegra_x1::gpu::optimization
