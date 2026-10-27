#include "common/config.hpp"
#include "common/frame_generation_timing.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void near(double actual, double expected, const char* message) {
    check(std::abs(actual - expected) < 1e-5, message);
}
} // namespace

int main() {
    using namespace hydra;
    try {
        Config config;
        config.getFrameRateLimit() = 60;
        config.getFrameGeneration() = FrameGeneration::Interpolate;
        config.getFrameGenerationRateMode() = FrameGenerationRateMode::Multiple;
        config.getFrameGenerationMultiplier() = 2;
        const auto output = [&](double native_fps) {
            return frameGenerationOutputRate(
                config.getFrameGenerationRateMode() ==
                    FrameGenerationRateMode::Multiple,
                config.getFrameGenerationTargetFps(),
                config.getFrameGenerationMultiplier(), 1.0 / native_fps);
        };
        near(output(60), 120, "60 native with 2x must target 120");
        near(output(45), 90,
             "Multiple must follow actual FPS, not the native limit");
        check(config.getFrameRateLimit() == 60,
              "FG must preserve the native rendering limit");
        config.getFrameGenerationMultiplier() = 4;
        near(output(60), 240, "4x must not be capped to display refresh");
        config.getFrameGenerationMultiplier() = 16;
        near(output(30), 480, "Multipliers above 8x must work");
        for (int tick = 0; tick < 16; ++tick) {
            near(frameGenerationFraction(static_cast<double>(tick) / 480.0,
                                         1.0 / 30.0, 480),
                 static_cast<double>(tick + 1) / 16.0,
                 "Every 16x output step must progress across the native "
                 "interval");
        }
        near(frameGenerationFraction(0, 1.0 / 60.0, 120), 0.5,
             "2x must produce the exact MetalFX midpoint");
        near(frameGenerationFraction(0.1, 1.0 / 60.0, 120), 1,
             "A missed deadline must hold the current frame instead of "
             "extrapolating");
        near(frameGenerationFraction(0, 1.0 / 60.0, 60), 1,
             "Output equal to native rate needs no interpolation");
        config.getFrameGenerationRateMode() = FrameGenerationRateMode::Variable;
        config.getFrameGenerationTargetFps() = 237;
        near(output(30), 237, "Variable must use the exact requested target");
        near(output(60), 237, "Variable must be independent of native FPS");
        config.getFrameGenerationMultiplier() = 99;
        near(output(60), 237, "Variable must ignore the multiplier");
        near(frameGenerationOutputRate(true, 120, 2, 0), 120,
             "Startup needs a finite rate before native FPS is measured");
        near(frameGenerationOutputRate(false, 30, 2, 1.0 / 60.0), 60,
             "A low FG target must not throttle native rendering");
        near(
            frameGenerationOutputRate(false, 0, 2, 0), 60,
            "A zero target must not divide by zero in presentation scheduling");
        std::cout << "Frame generation timing tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
