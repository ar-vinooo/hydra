#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hydra {

// Neither rate calculation nor interpolation depends on the host display.
inline double frameGenerationOutputRate(bool multiple, std::uint32_t target,
                                        std::uint32_t multiplier,
                                        double game_frame_seconds) {
    const double game_fps =
        std::isfinite(game_frame_seconds) && game_frame_seconds > 0.0
            ? 1.0 / game_frame_seconds
            : 60.0;
    // A target below native FPS adds no frames; FG never throttles the game.
    if (!multiple)
        return std::max(static_cast<double>(std::max(target, 1u)), game_fps);
    return game_fps * std::max(multiplier, 1u);
}

// Snap clock jitter to output-frame steps. In particular, 2x produces the
// exact midpoint MetalFX requires. The step count has no display or 8x cap.
inline double frameGenerationFraction(double elapsed, double game_frame_seconds,
                                      double output_fps) {
    if (!(game_frame_seconds > 0.0) || !(output_fps > 0.0))
        return 1.0;
    const double steps =
        std::max(std::round(game_frame_seconds * output_fps), 1.0);
    const double step =
        std::floor(std::max(elapsed, 0.0) * output_fps + 0.5) + 1.0;
    return std::clamp(step / steps, 0.0, 1.0);
}

} // namespace hydra
