#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>

#include <fmt/format.h>

namespace hydra::hw::tegra_x1::gpu {

// DEBUG: dumps all draws of a single frame to a text file.
// Enabled with HYDRA_TRACE_DIR. Create "<HYDRA_TRACE_DIR>/trigger" to trace
// the next frame, which is written to "<HYDRA_TRACE_DIR>/frame_<n>.txt".
class FrameTracer {
  public:
    static FrameTracer& instance() {
        static FrameTracer tracer;
        return tracer;
    }

    // Called once per presented frame
    void onPresent() {
        if (dir.empty())
            return;

        std::scoped_lock lock(mutex);
        if (active) {
            // Trace across 2 presents to always capture a whole frame
            if (++presents_seen < 2) {
                out << "=== present ===\n";
                return;
            }
            out << fmt::format("=== end of frame ({} draws) ===\n", draw_index.load());
            out.close();
            active = false;
            return;
        }

        // Automatic mode: trace a frame every HYDRA_TRACE_AUTO seconds
        bool triggered = false;
        if (auto_interval.count() > 0 && frame_counter < 40) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_auto_trace >= auto_interval) {
                last_auto_trace = now;
                triggered = true;
            }
        }

        const auto trigger = dir / "trigger";
        std::error_code ec;
        if (std::filesystem::exists(trigger, ec)) {
            std::filesystem::remove(trigger, ec);
            triggered = true;
        }
        if (!triggered)
            return;

        out.open(dir / fmt::format("frame_{}.txt", frame_counter++));
        draw_index = 0;
        presents_seen = 0;
        dumped.clear();
        active = true;
    }

    bool isActive() const { return active; }
    bool isDumpEnabled() const { return dump_textures; }
    const std::filesystem::path& getDir() const { return dir; }
    uint32_t getFrameIndex() const { return frame_counter - 1; }

    // Returns true the first time a texture is seen in the current frame
    bool markDumped(const void* texture) {
        std::scoped_lock lock(mutex);
        return dumped.insert(texture).second;
    }

    uint32_t nextDrawIndex() { return draw_index++; }

    void write(const std::string& str) {
        std::scoped_lock lock(mutex);
        if (active)
            out << str;
    }

  private:
    FrameTracer() {
        if (const char* env = std::getenv("HYDRA_TRACE_DIR"))
            dir = env;
        dump_textures = std::getenv("HYDRA_TRACE_DUMP_TEXTURES") != nullptr;
        if (const char* env = std::getenv("HYDRA_TRACE_AUTO"))
            auto_interval = std::chrono::seconds(std::atoi(env));
    }

    std::filesystem::path dir;
    std::mutex mutex;
    std::ofstream out;
    std::atomic<bool> active{false};
    std::atomic<uint32_t> draw_index{0};
    uint32_t frame_counter{0};
    uint32_t presents_seen{0};
    bool dump_textures{false};
    std::set<const void*> dumped;
    std::chrono::seconds auto_interval{0};
    std::chrono::steady_clock::time_point last_auto_trace{};
};

} // namespace hydra::hw::tegra_x1::gpu
