#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <utility>
#include <vector>

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

// See draw_queue.hpp
void syncDrawQueue();

// Writes a report once the GPU produced it: with the counter, or with nothing
// if the work failed (a failed report must not look like a result)
using ReportPublisher = std::function<void(std::optional<u64> value)>;

class ICommandBuffer {
  public:
    // Runs the callbacks once the host finished the commands, if the buffer
    // is destroyed without a host that tracks it they run right away
    virtual ~ICommandBuffer() {
        runGpuCompletionCallbacks(true);
        runCompletionCallbacks(true);
    }

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

    // Completion of the work encoded so far, including when a backend
    // checkpoints and continues encoding in a new native command buffer.
    // These callbacks do not wait for guest report publication.
    void onGpuCompletedChecked(std::function<void(bool succeeded)> callback) {
        gpu_completion_callbacks.push_back(std::move(callback));
    }

    // Any thread, also while the draw queue encodes
    void retainResource(const std::shared_ptr<void>& owner) {
        if (!owner)
            return;
        std::scoped_lock lock(retained_mutex);
        // Consecutive draws retain mostly the same owners: a direct mapped
        // filter of recent ones skips the hash lookup. An owner in it is in
        // the map, which keeps it alive, so its address cannot be reused.
        auto& recent = recent_retained[(reinterpret_cast<uptr>(owner.get()) >> 4) %
                                       recent_retained.size()];
        if (recent == owner.get())
            return;
        recent = owner.get();
        retained_resources.try_emplace(owner.get(), owner);
    }

    // Statistics only: what the next wait for the host GPU is for
    CheckpointReason checkpoint_reason{CheckpointReason::Other};

  protected:
    void runGpuCompletionCallbacks(bool succeeded) {
        auto callbacks = std::move(gpu_completion_callbacks);
        gpu_completion_callbacks.clear();
        for (const auto& callback : callbacks)
            callback(succeeded);
    }
    void runCompletionCallbacks(bool succeeded) {
        auto callbacks = std::move(completion_callbacks);
        completion_callbacks.clear();
        for (const auto& callback : callbacks)
            callback(succeeded);
    }
    std::vector<std::function<void(bool)>> completion_callbacks;
    std::vector<std::function<void(bool)>> gpu_completion_callbacks;
    // Hands the retained owners over (to the submission that keeps them
    // until the GPU is done)
    std::unordered_map<const void*, std::shared_ptr<void>>
    takeRetainedResources() {
        std::scoped_lock lock(retained_mutex);
        recent_retained.fill(nullptr);
        return std::exchange(retained_resources, {});
    }
    std::mutex retained_mutex;
    std::unordered_map<const void*, std::shared_ptr<void>> retained_resources;
    std::array<const void*, 64> recent_retained{};
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
