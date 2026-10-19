#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>

namespace hydra::hw::tegra_x1::gpu {

// Why the GPU thread waited for the host GPU, see ScopedCheckpointReason
enum class CheckpointReason : u8 {
    Other,
    ShaderInvalidate,
    SyncPoint,
    ReportRelease,
    ReportFlush,
    ConditionalRender,
    SemaphoreAcquire,
    PullerAcquire,
    PullerOther,
    TextureReadback,
    StorageWrite,
    Count,
};

constexpr usize CHECKPOINT_REASON_COUNT =
    static_cast<usize>(CheckpointReason::Count);

constexpr const char* getCheckpointReasonName(CheckpointReason reason) {
    constexpr const char* names[CHECKPOINT_REASON_COUNT] = {
        "other",          "shader invalidate", "syncpoint",
        "report release", "report flush",      "conditional render",
        "acquire",        "puller acquire",    "puller other",
        "texture readback", "storage write",
    };
    return names[static_cast<usize>(reason)];
}

// DEBUG: frame timing statistics, logged when HYDRA_PERF_STATS is set
struct PerfStats {
    std::atomic<u64> pfifo_busy_ns{0};
    std::atomic<u64> pfifo_lists{0};
    std::atomic<u64> present_wait_ns{0};
    std::atomic<u64> gpu_ns{0};
    std::atomic<u64> command_buffers{0};
    std::atomic<u64> draws{0};
    std::atomic<u64> compute_dispatches{0};
    std::atomic<u64> compute_threads{0};
    std::atomic<u64> frames{0};
    std::atomic<u64> texture_upload_ns{0};
    std::atomic<u64> texture_upload_bytes{0};
    std::atomic<u64> upload_buffers_created{0};
    std::atomic<u64> upload_buffers_reused{0};
    std::atomic<u64> draw_state_ns{0};
    std::atomic<u64> shader_compile_ns{0};
    std::atomic<u64> shader_compiles{0};
    std::atomic<u64> shader_library_hits{0};
    std::atomic<u64> pipeline_compile_ns{0};
    std::atomic<u64> pipeline_compiles{0};
    std::atomic<u64> raster_updates{0};
    std::atomic<u64> raster_skips{0};
    std::atomic<u64> texture_sync_scans{0};
    std::atomic<u64> texture_sync_skips{0};
    std::atomic<u64> register_writes{0};
    std::atomic<u64> register_write_skips{0};
    std::atomic<u64> render_encoders{0};
    std::atomic<u64> blit_encoders{0};
    std::atomic<u64> buffer_snapshots{0};
    std::atomic<u64> buffer_snapshot_bytes{0};
    std::atomic<u64> checkpoint_skips{0};
    std::array<std::atomic<u64>, CHECKPOINT_REASON_COUNT> checkpoint_count{};
    std::array<std::atomic<u64>, CHECKPOINT_REASON_COUNT> checkpoint_ns{};
    // Occlusion reports queued, and how often a consumer had to wait for them
    std::atomic<u64> reports_queued{0};
    std::atomic<u64> report_waits{0};
    // Draws issued with state Hydra does not implement yet (MSAA, alpha-to-coverage)
    std::atomic<u64> msaa_draws{0};
    std::atomic<u64> alpha_to_coverage_draws{0};
    std::atomic<u64> alpha_to_one_draws{0};

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
