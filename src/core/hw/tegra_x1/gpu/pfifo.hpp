#pragma once

#include "core/hw/tegra_x1/gpu/const.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/execution/work_manager.hpp"
#include <atomic>
#include <deque>
#include <set>
#include <unordered_map>

namespace hydra::hw::tegra_x1::cpu {
class IMmu;
}

namespace hydra::hw::tegra_x1::gpu {

class Gpu;
struct GpuChannelContext;

struct PfifoPacket {
    u32 method{0};
    u32 subchannel{0};
    u32 remaining{0};
    bool increment{false};
    bool one_increment{false};
    std::vector<std::pair<u32, u32>> macro_args;
};

struct PfifoSemaphoreWait {
    u64 address;
    u32 payload;
    u32 operation;
};

struct PfifoDecodeState : PfifoPacket {
    PfifoDecodeState();
    ~PfifoDecodeState();
    std::unique_ptr<GpuChannelContext> context;
    // Queued work retains its bound address space even after both FDs close.
    std::shared_ptr<GMmu> address_space;
    std::optional<PfifoSemaphoreWait> semaphore_wait;
    std::optional<Fence> syncpoint_wait;
    std::atomic<bool> failed{false};
    std::atomic<u32> error{0};
    std::atomic<u64> fault_time{0};
    std::function<void()> on_fault;
    void fail(u32 code);
    // Subdevice filtering (dev_pbdma.ref SUBDEVICE_STATUS): while inactive,
    // methods are consumed but not delivered and conditional segments are
    // skipped.
    bool subdevice_active{true};
    u32 stored_subdevice_mask{0};
    u32 fence_id{0};
    // Returns the fence counter once no queued work can increment it
    std::function<void()> release_fence;
    u32 fence_target{0};
    void resetPacket() { static_cast<PfifoPacket&>(*this) = {}; }
};

inline thread_local PfifoDecodeState* tls_crnt_decode_state = nullptr;

struct GpfifoEntryList {
    GMmu* gmmu;
    std::vector<GpfifoEntry> entries;
    GpfifoFlags flags;
    uint64_t correlation_id;
    // Runs when the host finished the work of the entries
    std::function<void()> on_complete;
    // With GpfifoFlags::FenceWait the GPU does not start the entries before
    // this fence is reached
    Fence wait_fence{};
    std::shared_ptr<PfifoDecodeState> decode_state;
    usize entry_index{0};
    u64 word_offset{0};
    std::shared_ptr<execution::WorkTicket> work;
};

class Pfifo;
class PfifoAdmission {
    friend class Pfifo;
  public:
    PfifoAdmission() = default;
    PfifoAdmission(PfifoAdmission&&) = default;
    PfifoAdmission& operator=(PfifoAdmission&&) = default;
    explicit operator bool() const { return work != nullptr; }
  private:
    Pfifo* owner{nullptr};
    usize bytes{0};
    std::shared_ptr<execution::WorkTicket> work;
};

class Pfifo {
  public:
    explicit Pfifo(Gpu& gpu_);
    ~Pfifo();
    void shutdown();
    void cancel(GMmu& gmmu);
    bool isStopping() const { return stop.load(std::memory_order_relaxed); }

    PfifoAdmission reserveEntries(usize count);
    bool submitEntries(GMmu& gmmu, std::span<const GpfifoEntry> entries,
                       GpfifoFlags flags,
                       std::function<void()> on_complete = {},
                       Fence wait_fence = {},
                       std::shared_ptr<PfifoDecodeState> decode_state = {},
                       PfifoAdmission admission = {});

    // Number of entry lists submitted so far
    u64 getSubmittedCount();
    execution::WorkStats getWorkStats() const { return dispatches.stats(); }
    // Waits till the first `count` entry lists have been executed
    bool waitForCompletion(u64 count);

  private:
    Gpu& gpu;
    PfifoDecodeState default_decode_state;

    std::mutex mutex;
    std::condition_variable cond_var;

    execution::WorkManager dispatches{{4096, 64 * 1024 * 1024}};
    std::deque<GpfifoEntryList> entry_lists;
    std::unordered_map<GMmu*, std::vector<std::weak_ptr<execution::WorkTicket>>>
        outstanding;
    std::atomic<bool> stop{false};
    GMmu* active_gmmu{nullptr};

    // Completion tracking
    u64 submitted_count{0};
    u64 completed_count{0};
    std::set<u64> retired_submissions;
    std::condition_variable completed_cond_var;

    std::thread thread; // TODO: jthread

    void threadFunc();

    bool ready(GpfifoEntryList& list);
    bool submitEntry(GpfifoEntry entry, PfifoDecodeState& state, u64& word_offset);
    bool submitCommand(u32 raw_header, PfifoDecodeState& state);

    // Helpers
    void processMethodArg(u32 arg, PfifoDecodeState& state);
};

} // namespace hydra::hw::tegra_x1::gpu
