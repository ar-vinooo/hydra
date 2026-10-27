#include "core/hw/tegra_x1/gpu/optimization_policy.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2 ||
        (std::string_view(argv[1]) != "0" && std::string_view(argv[1]) != "1"))
        return 2;
    const bool expected = std::string_view(argv[1]) == "1";
    if (hydra::hw::tegra_x1::gpu::optimization::enabled() != expected) {
        std::cerr << "FAIL: optimization policy accepted/rejected the wrong "
                     "receipt\n";
        return 1;
    }
    return 0;
}
