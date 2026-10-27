#include "core/horizon/display/binder.hpp"
#include "core/horizon/display/vsync_schedule.hpp"
#include <future>
#include <iostream>
#include <stdexcept>

using namespace hydra;
using namespace hydra::horizon::display;

namespace {
void expect(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void testVSyncSchedule() {
    using Clock = VSyncSchedule::Clock;
    const Clock::time_point origin{};
    VSyncSchedule schedule(origin);
    for (u32 tick = 1; tick <= 600; ++tick) {
        const auto deadline = origin + std::chrono::nanoseconds(
            static_cast<i64>(u64(tick) * 1'000'000'000ull / 60));
        expect(schedule.nextDeadline() == deadline,
               "60 Hz deadline has no accumulated rounding error");
        expect(!schedule.advance(deadline - std::chrono::nanoseconds(1)),
               "Host callbacks before vblank do not advance the guest clock");
        expect(schedule.advance(deadline), "Vblank fires at its deadline");
        expect(!schedule.advance(deadline), "Rounded deadline fires once");
    }
    for (u32 host_hz : {60u, 120u, 144u}) {
        VSyncSchedule sampled(origin);
        u32 ticks = 0;
        for (u32 sample = 1; sample <= host_hz; ++sample)
            ticks += sampled.advance(origin + std::chrono::nanoseconds(
                static_cast<i64>(u64(sample) * 1'000'000'000ull / host_hz)));
        expect(ticks == 60, "One second contains 60 guest vblanks at any host rate");
    }
    VSyncSchedule raised(origin, 120);
    u32 raised_ticks = 0;
    for (u32 ms = 1; ms <= 1000; ++ms)
        raised_ticks += raised.advance(origin + std::chrono::milliseconds(ms));
    expect(raised_ticks == 120, "A raised frame rate limit sets the vblank rate");
    expect(schedule.advance(origin + std::chrono::hours(1)),
           "Clock resumes after host suspension");
    expect(!schedule.advance(origin + std::chrono::hours(1)),
           "Resume does not replay a burst of stale vblanks");
    std::cout << "PASS: fixed 60 Hz schedule, fractional deadlines and suspension\n";
}

void testQueue() {
    Binder binder;
    GraphicBuffer buffer{};
    buffer.header.width = 1280;
    buffer.header.height = 720;
    expect(!binder.addBuffer(-1, buffer) &&
               !binder.addBuffer(MAX_BINDER_BUFFER_COUNT, buffer),
           "Invalid preallocation slots are rejected");
    expect(!binder.getBuffer(-1) && !binder.getBuffer(64),
           "Invalid request slots have no buffer");
    expect(binder.getAvailableSlot() == -1, "Empty queue does not block forever");
    expect(binder.addBuffer(0, buffer) && binder.addBuffer(63, buffer),
           "All 64 slots are addressable");
    BqBufferInput input{};
    expect(!binder.queueBuffer(0, input, 3), "A free slot cannot be queued");
    expect(binder.getAvailableSlot() == 0 && binder.getAvailableSlot() == 63,
           "Dequeues reserve different slots");
    auto waiting = std::async(std::launch::async, [&] { return binder.getAvailableSlot(); });
    expect(waiting.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
           "No slot is handed out twice while dequeued");
    expect(binder.cancelBuffer(63), "Cancel returns a dequeued slot");
    expect(waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready &&
               waiting.get() == 63, "Cancel wakes the waiting producer");
    expect(binder.queueBuffer(0, input, 123) && binder.queueBuffer(63, input, 124),
           "Dequeued slots can be queued");
    expect(!binder.queueBuffer(0, input, 0) && !binder.cancelBuffer(0),
           "Queued slots reject duplicate queue and cancel");
    expect(binder.getPendingBufferCount() == 2, "Pending count follows queue length");
    u64 fence = 0;
    expect(binder.consumeBuffer(input, fence) == 0 && fence == 123,
           "Consumer acquires first queued slot and its fence");
    expect(binder.getPendingBufferCount() == 1, "Acquire removes one pending buffer");
    expect(!binder.addBuffer(0, buffer), "Acquired buffer cannot be replaced");
    auto release = binder.releaseCallback(0);
    waiting = std::async(std::launch::async, [&] { return binder.getAvailableSlot(); });
    expect(waiting.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
           "Acquired buffer is held until host completion");
    release();
    expect(waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready &&
               waiting.get() == 0, "Host completion returns the acquired slot");
    release();
    expect(!binder.queueBuffer(-1, input, 0) && !binder.cancelBuffer(64),
           "Invalid queue and cancel slots are rejected");
    binder.unqueueAllBuffers();
    expect(!binder.getBuffer(0) && !binder.getBuffer(63) &&
               binder.getPendingBufferCount() == 0,
           "Disconnect removes every preallocated buffer and queued entry");
    expect(binder.addBuffer(0, buffer) && binder.getAvailableSlot() == 0 &&
               binder.queueBuffer(0, input, 5), "Producer reconnects cleanly");
    expect(binder.consumeBuffer(input, fence) == 0, "New generation is acquired");
    release(); // late callback from the old connection
    expect(!binder.cancelBuffer(0), "Old completion cannot release a new generation");
    auto latest = binder.releaseCallback(0);
    latest();
    expect(binder.getAvailableSlot() == 0, "New generation completes normally");
    binder.unqueueAllBuffers();
    std::function<void()> orphaned;
    {
        Binder short_lived;
        short_lived.addBuffer(0, buffer);
        short_lived.getAvailableSlot();
        short_lived.queueBuffer(0, input, 0);
        short_lived.consumeBuffer(input, fence);
        orphaned = short_lived.releaseCallback(0);
    }
    orphaned();
    std::cout << "PASS: buffer queue slot ownership, cancel, completion, disconnect, generation and lifetime\n";
}
} // namespace
int main() {
    try {
        testQueue();
        testVSyncSchedule();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
