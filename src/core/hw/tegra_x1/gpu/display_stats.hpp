#pragma once

#include <atomic>

namespace hydra::hw::tegra_x1::gpu {

// What the display really showed, counted when a drawable's presentation is
// reported (its presentedTime): a game frame shown for the first time, a
// generated frame, or an image shown before. Dropped drawables never showed.
// The counts only grow; readers keep the totals they saw last.
struct DisplayStats {
    std::atomic<u64> game_frames{0};
    std::atomic<u64> generated_frames{0};
    std::atomic<u64> repeated_frames{0};
    std::atomic<u64> dropped{0};

    static DisplayStats& instance() {
        static DisplayStats stats;
        return stats;
    }
};

} // namespace hydra::hw::tegra_x1::gpu
