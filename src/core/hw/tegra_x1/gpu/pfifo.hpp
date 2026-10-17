#pragma once

#include "core/hw/tegra_x1/gpu/const.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"

namespace hydra::hw::tegra_x1::cpu {
class IMmu;
}

namespace hydra::hw::tegra_x1::gpu {

class Gpu;

struct GpfifoEntryList {
    GMmu& gmmu;
    std::vector<GpfifoEntry> entries;
    GpfifoFlags flags;
    uint64_t correlation_id;
    // Runs when the host finished the work of the entries
    std::function<void()> on_complete;
};

class Pfifo {
  public:
    explicit Pfifo(Gpu& gpu_);
    ~Pfifo();
    void shutdown();

    void submitEntries(GMmu& gmmu, std::span<const GpfifoEntry> entries,
                       GpfifoFlags flags,
                       std::function<void()> on_complete = {});

    // Number of entry lists submitted so far
    u64 getSubmittedCount();
    // Waits till the first `count` entry lists have been executed
    void waitForCompletion(u64 count);

  private:
    Gpu& gpu;

    std::mutex mutex;
    std::condition_variable cond_var;

    std::queue<GpfifoEntryList> entry_lists;
    bool stop{false};

    // Completion tracking
    u64 submitted_count{0};
    u64 completed_count{0};
    std::condition_variable completed_cond_var;

    std::thread thread; // TODO: jthread

    void threadFunc();

    void submitEntry(const GpfifoEntry entry);
    bool submitCommand(uptr& gpu_addr); // TODO: return void

    // Helpers
    void processMethodArg(u32 subchannel, uptr& gpu_addr, u32& method,
                          bool increment);
};

} // namespace hydra::hw::tegra_x1::gpu
