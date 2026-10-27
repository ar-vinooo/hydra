#include "core/hw/tegra_x1/gpu/renderer/metal/completion_queue.hpp"

#include <optional>

#include "core/debugger/debugger_manager.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

namespace {

// Work still waiting for the GPU when the queue is destroyed is dropped after
// this long, as failed
constexpr auto SHUTDOWN_GRACE = std::chrono::seconds(2);

} // namespace

CompletionQueue::CompletionQueue() : thread(&CompletionQueue::run, this) {}

CompletionQueue::~CompletionQueue() {
    {
        std::scoped_lock lock(mutex);
        stopping = true;
    }
    condition.notify_all();
    thread.join();
}

u64 CompletionQueue::push(std::shared_ptr<CompletionToken> token, Work work,
                          std::vector<ztd::Range<uptr>> writes, bool ordered) {
    u64 sequence;
    {
        std::scoped_lock lock(mutex);
        sequence = next_sequence++;
        ordered = ordered || !writes.empty();
        if (!writes.empty())
            pending_writes.fetch_add(1, std::memory_order_release);
        if (ordered)
            pending_ordered.fetch_add(1, std::memory_order_release);
        records.push_back({sequence, std::move(token), std::move(work),
                           std::move(writes), ordered});
    }
    condition.notify_all();
    return sequence;
}

void CompletionQueue::notify() {
    // The empty critical section orders the token store before the worker's
    // next check, so the wake-up cannot be lost
    { std::scoped_lock lock(mutex); }
    condition.notify_all();
}

bool CompletionQueue::wait(u64 sequence, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, timeout,
                              [&] { return finished_sequence >= sequence; });
}

u64 CompletionQueue::lastWriteTo(ztd::Range<uptr> range) const {
    std::scoped_lock lock(mutex);
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        for (const auto& write : it->writes)
            if (write.intersects(range))
                return it->sequence;
    }
    for (const auto& write : running_writes)
        if (write.intersects(range))
            return running_sequence;
    return 0;
}

usize CompletionQueue::getPendingCount() const {
    std::scoped_lock lock(mutex);
    return records.size() + (running_sequence != 0 ? 1 : 0);
}

void CompletionQueue::run() {
    DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess().registerThisThread(
        "GPU completion");

    std::unique_lock lock(mutex);
    std::optional<std::chrono::steady_clock::time_point> deadline;
    while (true) {
        const bool ready = !records.empty() &&
                           (!records.front().token ||
                            records.front().token->done());
        if (!ready) {
            if (!stopping) {
                condition.wait(lock);
                continue;
            }
            if (records.empty())
                break;
            if (!deadline)
                deadline = std::chrono::steady_clock::now() + SHUTDOWN_GRACE;
            if (std::chrono::steady_clock::now() < *deadline) {
                condition.wait_until(lock, *deadline);
                continue;
            }
        }

        auto record = std::move(records.front());
        records.pop_front();
        running_sequence = record.sequence;
        running_writes = std::move(record.writes);
        running_ordered = record.ordered;
        // A token which is still pending here only happens at shutdown
        const bool succeeded = !record.token || record.token->succeeded();
        lock.unlock();

        record.work(succeeded);
        // Captured objects are released before the work counts as finished
        record.work = nullptr;

        lock.lock();
        if (!running_writes.empty())
            pending_writes.fetch_sub(1, std::memory_order_release);
        if (running_ordered)
            pending_ordered.fetch_sub(1, std::memory_order_release);
        running_writes.clear();
        running_ordered = false;
        running_sequence = 0;
        finished_sequence = record.sequence;
        condition.notify_all();
    }

    lock.unlock();
    DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess()
        .unregisterThisThread();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
