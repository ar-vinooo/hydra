#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"

#include <iostream>
#include <stdexcept>
#include <unistd.h>

using hydra::hw::tegra_x1::gpu::VisualDiagnostics;

int main() {
    const auto dir = std::filesystem::temp_directory_path() /
        ("hydra-visual-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    setenv("HYDRA_VISUAL_DIR", dir.c_str(), 1);
    const auto check = [](bool ok, const char* message) {
        if (!ok) throw std::runtime_error(message);
    };
    try {
        auto& visual = VisualDiagnostics::instance();
        std::ofstream(dir / "request") << "action=skip shader=0x100 images=1 shaders=1 frames=1";
        visual.submissionQueued(1, 100, 2);
        visual.beginSubmission(1);
        visual.beginDraw(0x100, 0x200);
        check(visual.selected() && visual.skipDraw(), "selected shader is isolated");
        check(!visual.imagePath("before-rt0").empty(), "selected target readback path exists");
        check(visual.wantsShader("key") && !visual.wantsShader("key"), "shader deduplication");
        visual.endDraw();
        visual.beginDraw(0x300, 0x400);
        check(!visual.selected() && !visual.skipDraw(), "unselected draws keep guest state");
        check(visual.imagePath("after-rt0").empty(), "unselected resources are not dumped");
        visual.onPresent();
        visual.beginDraw(0x100, 0x200);
        check(!visual.active() && !visual.skipDraw(), "expiry restores guest drawing");
        check(visual.options().output == VisualDiagnostics::Output::Guest, "expiry clears output overrides");
        check(VisualDiagnostics::quote("\"\\\n") == "\"\\\"\\\\\\u000a\"", "JSON control characters escape");
        std::filesystem::path capture;
        for (const auto& entry : std::filesystem::directory_iterator(dir))
            if (entry.is_directory()) capture = entry.path();
        std::ifstream input(capture / "events.jsonl");
        const std::string events((std::istreambuf_iterator<char>(input)), {});
        check(events.find("cpu_submit") != std::string::npos &&
              events.find("gpu_submit") != std::string::npos &&
              events.find("capture_end") != std::string::npos, "correlated bounded capture persists");
        std::filesystem::remove_all(dir);
        std::cout << "PASS: selector isolation, expiry, deduplication and capture lifecycle\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(dir);
        return 1;
    }
}
