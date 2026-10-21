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
    PullerSemaphore,
    ReportWrite,
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
        "puller semaphore", "report write",
    };
    return names[static_cast<usize>(reason)];
}

// What the GPU thread spends its time on, grouped by whether the work has to
// run in command stream order on that thread (sync) or only depends on data
// that could be handed to another core (async-capable)
enum class GpuWork : u8 {
    // Sync
    Decode, // Command stream, methods and macros (everything not below)
    State,  // Register state, draw state snapshots and resource tracking
    Lookup, // Cache lookups: shaders, pipelines, render targets, resources
    // Async-capable
    Encode,  // Metal command encoding of a prepared command
    Shader,  // Decompilation, library and pipeline compilation
    Texture, // Guest texture decoding and upload, GPU texture write back
    Buffer,  // Buffer snapshots
    Copy,    // DMA, 2D and inline copies done on the CPU
    // Neither: the GPU thread waits for the host GPU
    Wait,
    Count,
};

constexpr usize GPU_WORK_COUNT = static_cast<usize>(GpuWork::Count);

constexpr const char* getGpuWorkName(GpuWork work) {
    constexpr const char* names[GPU_WORK_COUNT] = {
        "decode", "state",  "lookup", "encode", "shader",
        "texture", "buffer", "copy",   "wait",
    };
    return names[static_cast<usize>(work)];
}

constexpr bool isGpuWorkAsyncCapable(GpuWork work) {
    return work >= GpuWork::Encode && work < GpuWork::Wait;
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
    std::atomic<u64> draw_prepare_ns{0};
    std::atomic<u64> draw_command_ns{0};
    std::atomic<u64> draw_pipeline_ns{0};
    std::atomic<u64> draw_targets_ns{0};
    std::atomic<u64> draw_snapshot_ns{0};
    std::atomic<u64> draw_apply_ns{0};
    std::atomic<u64> draw_resources_ns{0};
    std::atomic<u64> shader_compile_ns{0};
    std::atomic<u64> shader_compiles{0};
    std::atomic<u64> shader_library_hits{0};
    std::atomic<u64> shader_decompile_ns{0};
    std::atomic<u64> shader_decompiles{0};
    std::atomic<u64> pipeline_prewarm_hits{0};
    std::atomic<u64> pipeline_compile_ns{0};
    std::atomic<u64> pipeline_compiles{0};
    std::atomic<u64> raster_updates{0};
    std::atomic<u64> raster_skips{0};
    std::atomic<u64> texture_sync_scans{0};
    std::atomic<u64> texture_sync_skips{0};
    std::atomic<u64> register_writes{0};
    std::atomic<u64> register_write_skips{0};
    std::atomic<u64> render_encoders{0};
    // Render passes and the attachment memory they read (load) and write
    // (store), from the attachment sizes. A continuation reopens the
    // attachments of the previous render pass and loads all of them; it is
    // counted by what ended that pass: another render pass descriptor, a blit
    // or compute encoder, or a submission.
    std::atomic<u64> render_passes{0};
    std::atomic<u64> render_pass_load_bytes{0};
    std::atomic<u64> render_pass_store_bytes{0};
    std::atomic<u64> render_pass_continuations{0};
    std::array<std::atomic<u64>, 4> render_pass_continuation_causes{};
    // Passes continued in the open encoder instead of a new one
    std::atomic<u64> render_pass_merges{0};
    std::atomic<u64> blit_encoders{0};
    std::atomic<u64> buffer_snapshots{0};
    std::atomic<u64> buffer_snapshot_bytes{0};
    std::atomic<u64> checkpoint_skips{0};
    std::array<std::atomic<u64>, CHECKPOINT_REASON_COUNT> checkpoint_count{};
    std::array<std::atomic<u64>, CHECKPOINT_REASON_COUNT> checkpoint_ns{};
    // Exclusive time of the GPU thread per kind of work, see ScopedGpuWork
    std::array<std::atomic<u64>, GPU_WORK_COUNT> gpu_work_ns{};
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
    explicit ScopedPerfTimer(std::atomic<u64>& counter_, bool enabled_ = true)
        : counter{counter_}, enabled{enabled_},
          start{enabled ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{}} {}
    ~ScopedPerfTimer() {
        if (!enabled)
            return;
        counter += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
    }

  private:
    std::atomic<u64>& counter;
    bool enabled;
    std::chrono::steady_clock::time_point start;
};

// Attributes the time of the current thread to kinds of work while a
// ScopedGpuWorkRoot is active on it. Scopes nest: an inner scope's time is
// subtracted from the outer one, so the kinds add up to the root's time.
struct GpuWorkTracker {
    bool active{false};
    GpuWork work{GpuWork::Decode};
    std::chrono::steady_clock::time_point since;

    // Charge the time since the last switch to the current kind, then switch
    void switchTo(GpuWork next, std::chrono::steady_clock::time_point now) {
        PerfStats::instance().gpu_work_ns[static_cast<usize>(work)] +=
            static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - since)
                    .count());
        work = next;
        since = now;
    }
};

inline thread_local GpuWorkTracker gpu_work_tracker;

class ScopedGpuWorkRoot {
  public:
    ScopedGpuWorkRoot() : enabled{PerfStats::isEnabled() &&
                                  !gpu_work_tracker.active} {
        if (!enabled)
            return;
        gpu_work_tracker.active = true;
        gpu_work_tracker.work = GpuWork::Decode;
        gpu_work_tracker.since = std::chrono::steady_clock::now();
    }
    ~ScopedGpuWorkRoot() {
        if (!enabled)
            return;
        gpu_work_tracker.switchTo(GpuWork::Decode,
                                  std::chrono::steady_clock::now());
        gpu_work_tracker.active = false;
    }
    ScopedGpuWorkRoot(const ScopedGpuWorkRoot&) = delete;
    ScopedGpuWorkRoot& operator=(const ScopedGpuWorkRoot&) = delete;

  private:
    bool enabled;
};

class ScopedGpuWork {
  public:
    explicit ScopedGpuWork(GpuWork work) : enabled{gpu_work_tracker.active} {
        if (!enabled)
            return;
        previous = gpu_work_tracker.work;
        gpu_work_tracker.switchTo(work, std::chrono::steady_clock::now());
    }
    ~ScopedGpuWork() {
        if (enabled)
            gpu_work_tracker.switchTo(previous,
                                      std::chrono::steady_clock::now());
    }
    ScopedGpuWork(const ScopedGpuWork&) = delete;
    ScopedGpuWork& operator=(const ScopedGpuWork&) = delete;

  private:
    bool enabled;
    GpuWork previous{GpuWork::Decode};
};

} // namespace hydra::hw::tegra_x1::gpu
