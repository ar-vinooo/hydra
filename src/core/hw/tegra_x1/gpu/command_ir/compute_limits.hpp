#pragma once

#include <cstdint>
#include <limits>

namespace hydra::hw::tegra_x1::gpu::command_ir {

inline bool emptyComputeGrid(std::uint64_t x, std::uint64_t y,
                             std::uint64_t z) {
    return x == 0 || y == 0 || z == 0;
}

// Checked separately from Metal so malformed dimensions cannot wrap into a
// small, apparently legal threadgroup before the backend validates them.
inline bool validComputeBlock(std::uint64_t x, std::uint64_t y, std::uint64_t z,
                              std::uint64_t maximum) {
    return x != 0 && y != 0 && z != 0 && x <= maximum && y <= maximum / x &&
           z <= maximum / (x * y);
}

inline bool validComputeGrid(std::uint64_t x, std::uint64_t y,
                             std::uint64_t z) {
    return validComputeBlock(x, y, z,
                             std::numeric_limits<std::uint64_t>::max());
}

} // namespace hydra::hw::tegra_x1::gpu::command_ir
