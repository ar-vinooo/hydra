#include "core/hw/tegra_x1/gpu/command_ir/compute_limits.hpp"

#include <fstream>
#include <iostream>

using namespace hydra::hw::tegra_x1::gpu::command_ir;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: compute-limits-tests input.txt output.txt\n";
        return 2;
    }
    std::ifstream input(argv[1]);
    std::ofstream output(argv[2]);
    if (!input || !output)
        return 2;
    std::uint64_t x, y, z, maximum;
    while (input >> x) {
        if (!(input >> y >> z >> maximum))
            return 2;
        output << validComputeBlock(x, y, z, maximum) << ' '
               << validComputeGrid(x, y, z) << ' ' << emptyComputeGrid(x, y, z)
               << '\n';
    }
    if (!input.eof() || !output)
        return 2;
}
