#include "core/horizon/display/binder.hpp"

#include <thread>

#include "common/config.hpp"
#include "core/system.hpp"

namespace hydra::horizon::display {

bool Binder::hasFreeSlot(const QueueState& state) {
    return std::any_of(state.buffers.begin(), state.buffers.end(), [](const Buffer& b) {
        return b.initialized && b.state == Buffer::State::Free;
    });
}

void Binder::updateEventLocked() {
    if (hasFreeSlot(*queue))
        event->signal();
    else
        event->clear();
}

bool Binder::addBuffer(i32 slot, const GraphicBuffer& buff) {
    if (!validSlot(slot))
        return false;
    std::scoped_lock lock(queue->mutex);
    auto& buffer = queue->buffers[static_cast<usize>(slot)];
    if (buffer.initialized && buffer.state != Buffer::State::Free)
        return false;
    if (!buffer.initialized)
        queue->buffer_count++;
    buffer.initialized = true;
    buffer.state = Buffer::State::Free;
    buffer.generation = ++queue->generation;
    buffer.buffer = buff;
    updateEventLocked();
    queue->cv.notify_all();
    return true;
}

i32 Binder::getAvailableSlot() {
    std::unique_lock lock(queue->mutex);
    queue->cv.wait(lock, [&] {
        return queue->buffer_count == 0 || hasFreeSlot(*queue);
    });
    for (usize i = 0; i < queue->buffers.size(); ++i) {
        auto& buffer = queue->buffers[i];
        if (buffer.initialized && buffer.state == Buffer::State::Free) {
            buffer.state = Buffer::State::Dequeued;
            updateEventLocked();
            return static_cast<i32>(i);
        }
    }
    return -1;
}

bool Binder::queueBuffer(System& system, i32 slot, const BqBufferInput& input) {
    // A frame rate limit above the console's 60 spaces the title's frames,
    // whether it paces itself on vsync or not
    if (const u32 limit = CONFIG_INSTANCE.getFrameRateLimit(); limit > 60) {
        const auto period =
            std::chrono::nanoseconds(static_cast<i64>(1'000'000'000 / limit));
        const auto now = clock_t::now();
        if (now < next_frame_time)
            std::this_thread::sleep_until(next_frame_time);
        // A late frame does not earn a burst of early ones
        next_frame_time = std::max(now, next_frame_time) + period;
    }

    const auto gpu_fence = system.getGpu().getPfifo().getSubmittedCount();
    if (!queueBuffer(slot, input, gpu_fence))
        return false;
    system.getGpu().getRenderer().notifyDebugFrameBoundary();
    return true;
}

bool Binder::queueBuffer(i32 slot, const BqBufferInput& input, u64 gpu_fence) {
    if (!validSlot(slot))
        return false;
    std::scoped_lock lock(queue->mutex);
    auto& buffer = queue->buffers[static_cast<usize>(slot)];
    if (!buffer.initialized || buffer.state != Buffer::State::Dequeued)
        return false;
    queue->queued_buffers.emplace(slot, input, gpu_fence);
    buffer.state = Buffer::State::Queued;
    const auto now = clock_t::now();
    queue->accumulated_dt += now - queue->last_queue_time;
    queue->last_queue_time = now;
    return true;
}

bool Binder::cancelBuffer(i32 slot) {
    if (!validSlot(slot))
        return false;
    std::scoped_lock lock(queue->mutex);
    auto& buffer = queue->buffers[static_cast<usize>(slot)];
    if (!buffer.initialized || buffer.state != Buffer::State::Dequeued)
        return false;
    buffer.state = Buffer::State::Free;
    updateEventLocked();
    queue->cv.notify_all();
    return true;
}

i32 Binder::consumeBuffer(BqBufferInput& out_input, u64& out_gpu_fence) {
    std::scoped_lock lock(queue->mutex);
    if (queue->queued_buffers.empty())
        return -1;
    const auto [slot, input, fence] = queue->queued_buffers.front();
    queue->queued_buffers.pop();
    queue->buffers[static_cast<usize>(slot)].state = Buffer::State::Acquired;
    out_input = input;
    out_gpu_fence = fence;
    return slot;
}

std::function<void()> Binder::releaseCallback(i32 slot) {
    std::scoped_lock lock(queue->mutex);
    if (!validSlot(slot) || !queue->buffers[static_cast<usize>(slot)].initialized ||
        queue->buffers[static_cast<usize>(slot)].state != Buffer::State::Acquired)
        return [] {};
    const auto generation = queue->buffers[static_cast<usize>(slot)].generation;
    // Keep the queue state alive independently of the layer/binder pool. A
    // late host completion must not free a newly connected producer's slot.
    return [state = queue, release_event = event, slot, generation] {
        std::scoped_lock release_lock(state->mutex);
        auto& buffer = state->buffers[static_cast<usize>(slot)];
        if (buffer.initialized && buffer.generation == generation &&
            buffer.state == Buffer::State::Acquired) {
            buffer.state = Buffer::State::Free;
            release_event->signal();
            state->cv.notify_all();
        }
    };
}

std::optional<GraphicBuffer> Binder::getBuffer(i32 slot) {
    std::scoped_lock lock(queue->mutex);
    if (!validSlot(slot) || !queue->buffers[static_cast<usize>(slot)].initialized)
        return std::nullopt;
    return queue->buffers[static_cast<usize>(slot)].buffer;
}

u32 Binder::getPendingBufferCount() {
    std::scoped_lock lock(queue->mutex);
    return static_cast<u32>(queue->queued_buffers.size());
}

AccumulatedTime Binder::getAccumulatedDt() {
    std::scoped_lock lock(queue->mutex);
    const auto result = queue->accumulated_dt;
    queue->accumulated_dt = {};
    return result;
}

void Binder::removeBuffer(i32 slot) {
    if (!validSlot(slot))
        return;
    std::scoped_lock lock(queue->mutex);
    auto& buffer = queue->buffers[static_cast<usize>(slot)];
    if (!buffer.initialized || buffer.state != Buffer::State::Free)
        return;
    buffer = {};
    queue->buffer_count--;
    updateEventLocked();
    queue->cv.notify_all();
}

void Binder::unqueueAllBuffers() {
    std::scoped_lock lock(queue->mutex);
    queue->queued_buffers = {};
    queue->buffers = {};
    queue->buffer_count = 0;
    queue->accumulated_dt = {};
    queue->last_queue_time = clock_t::now();
    updateEventLocked();
    queue->cv.notify_all();
}

} // namespace hydra::horizon::display
