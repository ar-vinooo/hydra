#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

// DEBUG: GPU time of every render, compute and blit pass, from timestamps the
// GPU samples at the start and end of each pass (HYDRA_PERF_STATS and
// HYDRA_GPU_PASS_TIMES). Apple GPUs sample at stage boundaries only, so a pass
// is the finest granularity. Sampling keeps passes from overlapping: times are
// relative, and their sum exceeds the frame time.
class GpuPassProfiler {
  public:
    enum class Kind : u8 { Render, Compute, Blit, Count };

    struct Pass {
        Kind kind;
        u32 sample; // Start sample, the end sample follows it
        std::string label;
        // Written into the Metal 4 counter heap instead of the sample buffer
        bool mtl4;
    };

    struct Total {
        u64 ns{0};
        u64 count{0};
    };

    // Null if statistics are off or the GPU cannot sample at stage boundaries
    static GpuPassProfiler* get(MTL::Device* device);

    MTL::CounterSampleBuffer* getSampleBuffer() const { return sample_buffer; }
    // Timestamps of Metal 4 encoders, null if they cannot be written. Indices
    // are those of the sample buffer.
    MTL4::CounterHeap* getCounterHeap() const { return counter_heap; }
    // Reserves the start and end samples of a pass, invalid<u32>() if all are
    // in flight
    u32 reserve();
    // Adds the times of the passes of a completed command buffer and frees
    // their samples
    void resolve(const std::vector<Pass>& passes, bool succeeded);

    // Per kind totals and the labels that took the most time since the last
    // call
    std::vector<std::pair<std::string, Total>> takeTop(usize count,
                                                       Total (&kinds)[3]);

  private:
    explicit GpuPassProfiler(MTL::Device* device);

    // Metal allows at most 32 KiB of samples per buffer
    static constexpr u32 SAMPLE_COUNT = 4096;

    MTL::CounterSampleBuffer* sample_buffer{nullptr};
    MTL4::CounterHeap* counter_heap{nullptr};
    // Counter heap timestamps are GPU ticks
    f64 ns_per_tick{1.0};
    std::mutex mutex;
    std::vector<u32> free_pairs;
    Total kind_totals[3];
    std::map<std::string, Total> label_totals;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
