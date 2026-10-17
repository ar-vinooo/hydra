#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include <fmt/format.h>

namespace hydra::hw::tegra_x1::gpu {

// DEBUG: dumps all draws of a single frame to a text file.
// Enabled with HYDRA_TRACE_DIR. Create "<HYDRA_TRACE_DIR>/trigger" to trace
// the next frame, which is written to "<HYDRA_TRACE_DIR>/frame_<n>.txt".
// The trigger file may list dump options for that frame, overriding the
// environment: "textures", "rt=<N>", "gbuffer=<N>" and "post=<N>".
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
            if (++presents_seen < trace_presents) {
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
            readOptions(trigger);
            std::filesystem::remove(trigger, ec);
            triggered = true;
        }
        if (!triggered)
            return;

        out.open(dir / fmt::format("frame_{}.txt", frame_counter++));
        draw_index = 0;
        image_dumps = 0;
        presents_seen = 0;
        dumped.clear();
        active = true;
    }

    bool isActive() const { return active; }
    bool isDumpEnabled() const { return dump_textures; }
    // PNG encoding runs on Metal completion callbacks. Unbounded per-draw
    // dumps can block command completion long enough to time out the guest.
    bool allowImageDump() { return image_dumps.fetch_add(1) < max_image_dumps; }
    // Dump intervals from the trigger file (0 if not set)
    uint32_t getDumpRtEvery() const { return dump_rt_every; }
    uint32_t getDumpGbufferEvery() const { return dump_gbuffer_every; }
    uint32_t getDumpPostEvery() const { return dump_post_every; }
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

    void readOptions(const std::filesystem::path& path) {
        // Options belong to this capture, not every later trigger. In
        // particular, an empty trigger must return to a lightweight trace.
        dump_textures = std::getenv("HYDRA_TRACE_DUMP_TEXTURES") != nullptr;
        dump_rt_every = 0;
        dump_gbuffer_every = 0;
        dump_post_every = 0;
        trace_presents = 2;
        max_image_dumps = 24;
        std::ifstream in(path);
        std::string token;
        const auto value = [&](const char* prefix) -> std::optional<uint32_t> {
            const std::string p(prefix);
            if (token.rfind(p, 0) != 0)
                return std::nullopt;
            return static_cast<uint32_t>(
                std::max(1, std::atoi(token.c_str() + p.size())));
        };
        while (in >> token) {
            if (token == "textures")
                dump_textures = true;
            else if (const auto rt = value("rt="))
                dump_rt_every = *rt;
            else if (const auto gb = value("gbuffer="))
                dump_gbuffer_every = *gb;
            else if (const auto post = value("post="))
                dump_post_every = *post;
            else if (const auto presents = value("presents="))
                trace_presents = *presents;
            else if (const auto dumps = value("dumps="))
                max_image_dumps = std::min(*dumps, 64u);
        }
    }

    std::filesystem::path dir;
    std::mutex mutex;
    std::ofstream out;
    std::atomic<bool> active{false};
    std::atomic<uint32_t> draw_index{0};
    std::atomic<uint32_t> image_dumps{0};
    uint32_t max_image_dumps{24};
    uint32_t frame_counter{0};
    uint32_t presents_seen{0};
    // Presents a trace spans, the GPU can lag behind them
    uint32_t trace_presents{2};
    bool dump_textures{false};
    uint32_t dump_rt_every{0};
    uint32_t dump_gbuffer_every{0};
    uint32_t dump_post_every{0};
    std::set<const void*> dumped;
    std::chrono::seconds auto_interval{0};
    std::chrono::steady_clock::time_point last_auto_trace{};
};

} // namespace hydra::hw::tegra_x1::gpu
