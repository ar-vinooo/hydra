#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

// Whether one Metal command buffer finished. It is shared with the work that
// depends on that command buffer.
class CompletionToken {
  public:
    // Only the first call counts
    void complete(bool succeeded) {
        u8 expected = PENDING;
        state.compare_exchange_strong(expected, succeeded ? SUCCEEDED : FAILED,
                                      std::memory_order_acq_rel,
                                      std::memory_order_relaxed);
    }
    bool done() const {
        return state.load(std::memory_order_acquire) != PENDING;
    }
    bool succeeded() const {
        return state.load(std::memory_order_acquire) == SUCCEEDED;
    }

  private:
    static constexpr u8 PENDING = 0;
    static constexpr u8 SUCCEEDED = 1;
    static constexpr u8 FAILED = 2;
    std::atomic<u8> state{PENDING};
};

// Runs work in the order it was pushed, each once the Metal command buffer it
// depends on has completed.
//
// The work runs on a thread of its own: Metal's completion threads are not
// known to the logger, and results written to guest memory must neither race
// with the GPU thread nor depend on the order in which Metal calls its
// handlers. A piece of work whose command buffer completed still waits for
// the work pushed before it.
class CompletionQueue {
  public:
    // The flag tells whether the command buffer completed successfully
    using Work = std::function<void(bool succeeded)>;

    CompletionQueue();
    ~CompletionQueue();

    CompletionQueue(const CompletionQueue&) = delete;
    CompletionQueue& operator=(const CompletionQueue&) = delete;

    // Returns the sequence number of the work. Work without a token depends
    // only on the work pushed before it. `writes` are the host ranges the
    // work writes, see lastWriteTo. `ordered` work (anything the guest can
    // observe, such as a fence) has to stay in order with later such work, see
    // hasPendingOrdered.
    u64 push(std::shared_ptr<CompletionToken> token, Work work,
             std::vector<ztd::Range<uptr>> writes = {}, bool ordered = false);
    // A token was completed
    void notify();
    // Waits until the work with that sequence number has finished. Returns
    // false on timeout.
    bool wait(u64 sequence, std::chrono::milliseconds timeout);
    // Sequence number of the last unfinished work writing to the range, 0 if
    // there is none
    u64 lastWriteTo(ztd::Range<uptr> range) const;
    // Cheap check for lastWriteTo: some unfinished work writes memory
    bool hasPendingWrites() const {
        return pending_writes.load(std::memory_order_acquire) != 0;
    }
    // Some unfinished work is observable by the guest (it writes memory or is
    // ordered). Later such work must be pushed too, or it could be observed
    // first: a fence counter only counts, a later fence satisfies an earlier
    // wait.
    bool hasPendingOrdered() const {
        return pending_ordered.load(std::memory_order_acquire) != 0;
    }
    // Number of unfinished pieces of work
    usize getPendingCount() const;

  private:
    struct Record {
        u64 sequence;
        std::shared_ptr<CompletionToken> token;
        Work work;
        std::vector<ztd::Range<uptr>> writes;
        bool ordered;
    };

    void run();

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<Record> records;
    // The work being run is not in `records`
    u64 running_sequence{0};
    std::vector<ztd::Range<uptr>> running_writes;
    bool running_ordered{false};
    u64 next_sequence{1};
    u64 finished_sequence{0};
    std::atomic<u32> pending_writes{0};
    std::atomic<u32> pending_ordered{0};
    bool stopping{false};
    std::thread thread;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
