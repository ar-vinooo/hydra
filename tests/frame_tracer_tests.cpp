#include "core/hw/tegra_x1/gpu/frame_tracer.hpp"

#include <iostream>
#include <stdexcept>
#include <unistd.h>

using hydra::hw::tegra_x1::gpu::FrameTracer;

int main() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("hydra-frame-trace-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    setenv("HYDRA_TRACE_DIR", dir.c_str(), 1);
    unsetenv("HYDRA_TRACE_DUMP_TEXTURES");
    unsetenv("HYDRA_TRACE_AUTO");
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    try {
        auto& tracer = FrameTracer::instance();
        std::ofstream(dir / "trigger")
            << "textures gbuffer=1 post=1 rt=1 presents=1 dumps=2\n";
        tracer.onPresent();
        check(tracer.isActive() && tracer.isDumpEnabled(), "capture starts");
        check(tracer.getDumpGbufferEvery() == 1, "explicit options apply");
        check(tracer.allowImageDump() && tracer.allowImageDump() &&
                  !tracer.allowImageDump(), "image budget bounds callbacks");
        tracer.onPresent();
        check(!tracer.isActive(), "capture ends");
        std::ofstream(dir / "trigger");
        tracer.onPresent();
        check(tracer.isActive() && !tracer.isDumpEnabled(),
              "empty trigger clears texture dumps");
        check(tracer.getDumpGbufferEvery() == 0 &&
                  tracer.getDumpPostEvery() == 0 &&
                  tracer.getDumpRtEvery() == 0,
              "empty trigger clears previous per-draw capture options");
        for (int i = 0; i < 24; ++i)
            check(tracer.allowImageDump(), "new capture resets budget");
        check(!tracer.allowImageDump(), "default budget is bounded");
        tracer.onPresent();
        tracer.onPresent();
        std::filesystem::remove_all(dir);
        std::cout << "PASS: capture options reset and image budgets bound dumps\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(dir);
        return 1;
    }
}
