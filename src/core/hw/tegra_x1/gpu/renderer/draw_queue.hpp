#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "core/hw/tegra_x1/gpu/command_ir/draw.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class IRenderer;
class ICommandBuffer;

// Waits until every draw queued for encoding has been encoded. Code that
// touches a command buffer, the renderer's bound state or a resource in
// place calls it first, so it sees the same order as if each draw had been
// encoded when it was sealed. Free when nothing is queued and on the
// encoding thread. Defined in the host (not inline) so live GPU modules share
// the queue state.
void syncDrawQueue();

// Encodes sealed draws on a second thread, in order, so the GPU thread can
// decode and prepare the next draws meanwhile. A sealed draw is logically
// encoded at sealing: its resources are leased and its writes issued then,
// and everything that could observe the difference syncs first (see
// syncDrawQueue). HYDRA_SYNC_ENCODE=1 encodes on the GPU thread instead.
class DrawQueue {
  public:
    explicit DrawQueue(IRenderer& renderer_);
    ~DrawQueue();
    DrawQueue(const DrawQueue&) = delete;
    DrawQueue& operator=(const DrawQueue&) = delete;

    static bool isEnabled();

    // GPU thread. The command must pass command_ir::canExecuteDraw and its
    // guest writes must already be issued.
    void push(ICommandBuffer* command_buffer, command_ir::DrawCommand&& command);
    void drain();

    // Draws encoded by the queue's thread so far
    u64 getEncodedCount() const {
        return encoded.load(std::memory_order_acquire);
    }

  private:
    struct Item {
        ICommandBuffer* command_buffer;
        command_ir::DrawCommand command;
    };

    IRenderer& renderer;
    std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable idle_cv;
    std::deque<Item> items;
    bool stopping{false};
    std::atomic<u64> encoded{0};
    std::thread worker;

    void run();
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
