#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace hydra::horizon::display {

// Fixed 60 Hz guest vblank, independent of the host's presentation callbacks.
// Deadlines are calculated from an origin to avoid cumulative rounding drift.
class VSyncSchedule {
  public:
    using Clock = std::chrono::steady_clock;
    explicit VSyncSchedule(Clock::time_point origin_) : origin{origin_} {}

    Clock::time_point nextDeadline() const {
        return origin + std::chrono::nanoseconds(static_cast<int64_t>(
                            (tick / 60) * 1'000'000'000ull +
                            (tick % 60) * 1'000'000'000ull / 60));
    }

    bool advance(Clock::time_point now) {
        if (now < nextDeadline())
            return false;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 now - origin).count();
        const uint64_t elapsed_ticks =
            static_cast<uint64_t>(elapsed / 1'000'000'000) * 60 +
            static_cast<uint64_t>(elapsed % 1'000'000'000) * 60 / 1'000'000'000;
        // Skip missed vblanks after suspension and never emit the same tick
        // twice when a rounded deadline falls just before an exact fraction.
        tick = std::max(tick + 1, elapsed_ticks + 1);
        return true;
    }

  private:
    Clock::time_point origin;
    uint64_t tick{1};
};

} // namespace hydra::horizon::display
