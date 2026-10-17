#pragma once

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class ICommandBuffer {
  public:
    // Runs the callbacks once the host finished the commands, if the buffer
    // is destroyed without a host that tracks it they run right away
    virtual ~ICommandBuffer() { runCompletionCallbacks(); }

    // The callback runs on an arbitrary thread
    void onCompleted(std::function<void()> callback) {
        completion_callbacks.push_back(std::move(callback));
    }

  protected:
    void runCompletionCallbacks() {
        auto callbacks = std::move(completion_callbacks);
        completion_callbacks.clear();
        for (const auto& callback : callbacks)
            callback();
    }
    std::vector<std::function<void()>> completion_callbacks;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
