#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

// Writes a report once the GPU produced it: with the counter, or with nothing
// if the work failed (a failed report must not look like a result)
using ReportPublisher = std::function<void(std::optional<u64> value)>;

class ICommandBuffer {
  public:
    // Runs the callbacks once the host finished the commands, if the buffer
    // is destroyed without a host that tracks it they run right away
    virtual ~ICommandBuffer() { runCompletionCallbacks(true); }

    // The callback runs on an arbitrary thread
    void onCompleted(std::function<void()> callback) {
        completion_callbacks.push_back(
            [callback = std::move(callback)](bool) { callback(); });
    }

    // Same, for work that must not treat a failed host submission as done.
    // The callbacks of a buffer run in the order they were added, and only
    // after the reports queued through this buffer reached guest memory.
    void onCompletedChecked(std::function<void(bool succeeded)> callback) {
        completion_callbacks.push_back(std::move(callback));
    }

    // Statistics only: what the next wait for the host GPU is for
    CheckpointReason checkpoint_reason{CheckpointReason::Other};

  protected:
    void runCompletionCallbacks(bool succeeded) {
        auto callbacks = std::move(completion_callbacks);
        completion_callbacks.clear();
        for (const auto& callback : callbacks)
            callback(succeeded);
    }
    std::vector<std::function<void(bool)>> completion_callbacks;
};

// Names the waits for the host GPU done in its scope in HYDRA_PERF_STATS
class ScopedCheckpointReason {
  public:
    ScopedCheckpointReason(ICommandBuffer* command_buffer_,
                           CheckpointReason reason)
        : command_buffer{command_buffer_} {
        if (command_buffer == nullptr)
            return;
        previous = command_buffer->checkpoint_reason;
        command_buffer->checkpoint_reason = reason;
    }
    ~ScopedCheckpointReason() {
        if (command_buffer != nullptr)
            command_buffer->checkpoint_reason = previous;
    }
    ScopedCheckpointReason(const ScopedCheckpointReason&) = delete;
    ScopedCheckpointReason& operator=(const ScopedCheckpointReason&) = delete;

  private:
    ICommandBuffer* command_buffer;
    CheckpointReason previous{CheckpointReason::Other};
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
