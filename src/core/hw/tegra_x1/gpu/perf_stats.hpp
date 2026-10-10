#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>

namespace hydra::hw::tegra_x1::gpu {

// DEBUG: frame timing statistics, logged when HYDRA_PERF_STATS is set
struct PerfStats {
    std::atomic<u64> pfifo_busy_ns{0};
    std::atomic<u64> pfifo_lists{0};
    std::atomic<u64> present_wait_ns{0};
    std::atomic<u64> gpu_ns{0};
    std::atomic<u64> command_buffers{0};
    std::atomic<u64> draws{0};
    std::atomic<u64> frames{0};

    static PerfStats& instance() {
        static PerfStats stats;
        return stats;
    }

    static bool isEnabled() {
        static const bool enabled = std::getenv("HYDRA_PERF_STATS") != nullptr;
        return enabled;
    }
};

class ScopedPerfTimer {
  public:
    explicit ScopedPerfTimer(std::atomic<u64>& counter_)
        : counter{counter_}, start{std::chrono::steady_clock::now()} {}
    ~ScopedPerfTimer() {
        counter += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
    }

  private:
    std::atomic<u64>& counter;
    std::chrono::steady_clock::time_point start;
};

} // namespace hydra::hw::tegra_x1::gpu
