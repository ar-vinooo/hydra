#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "core/horizon/kernel/thread.hpp"

using namespace hydra;
using namespace hydra::horizon::kernel;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Runs one of the guest-like loops below on its own thread, like a guest
// thread between VM exits
class TestThread final : public IThread {
  public:
    enum class Mode { Spin, Sleep, TimedWait };

    TestThread(Mode mode_, i64 duration_ns_ = 0)
        : IThread(nullptr, 44, "Test thread"), mode{mode_},
          duration_ns{duration_ns_} {}
    ~TestThread() override { stopAndJoin(); }

    uptr getTlsPtr() const override { return 0; }

    std::atomic<u64> iterations{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> timed_out{false};
    std::chrono::steady_clock::time_point finished_at;

  protected:
    void run() override {
        switch (mode) {
        case Mode::Spin:
            while (!processMessagesAndCheckStop()) {
                iterations++;
                std::this_thread::sleep_for(100us);
            }
            break;
        case Mode::Sleep:
            sleep(duration_ns);
            // The VM exit after the SVC
            processMessages();
            break;
        case Mode::TimedWait:
            pause();
            timed_out = !processMessages(duration_ns);
            break;
        }
        finished_at = std::chrono::steady_clock::now();
        finished = true;
    }

    void captureContext(ThreadContext& out_context) override {
        out_context = {};
        out_context.cpu_gprs[0].x = iterations.load();
        out_context.pc.x = 0x1234;
    }

  private:
    Mode mode;
    i64 duration_ns;

    bool processMessagesAndCheckStop() {
        processMessages();
        return getState() == ThreadState::Stopping;
    }
};

void waitUntil(const std::atomic<bool>& flag, std::chrono::milliseconds limit,
               const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!flag && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    check(flag, message);
}

} // namespace

int main() {
    // A running thread stops, reports its context and stays stopped
    {
        TestThread thread(TestThread::Mode::Spin);
        thread.start();
        std::this_thread::sleep_for(20ms);
        check(thread.iterations > 0, "The thread runs");
        thread.suspendActivity();
        check(thread.isSuspendRequested(), "Suspension is requested");
        ThreadContext context;
        check(thread.waitForSuspendedContext(context, 1s),
              "A running thread reports its context");
        check(context.pc.x == 0x1234, "The context comes from the thread");
        const auto stopped_at = thread.iterations.load();
        check(context.cpu_gprs[0].x == stopped_at,
              "The context is taken where the thread stopped");
        std::this_thread::sleep_for(30ms);
        check(thread.iterations == stopped_at,
              "A suspended thread does not run");
        thread.resumeActivity();
        std::this_thread::sleep_for(30ms);
        check(thread.iterations > stopped_at, "A resumed thread runs again");
        check(!thread.waitForSuspendedContext(context, 10ms),
              "No context once resumed");
    }

    // Sleeping ends early to stop, so a process exit does not wait for it
    {
        TestThread thread(TestThread::Mode::Sleep, 10'000'000'000);
        thread.start();
        std::this_thread::sleep_for(20ms);
        const auto begin = std::chrono::steady_clock::now();
        thread.stopAndJoin();
        check(std::chrono::steady_clock::now() - begin < 1s,
              "Stopping interrupts a sleep");
    }

    // A sleeping thread can be suspended right away and does not continue
    // past its sleep until resumed
    {
        TestThread thread(TestThread::Mode::Sleep, 100'000'000);
        thread.start();
        std::this_thread::sleep_for(20ms);
        const auto begin = std::chrono::steady_clock::now();
        thread.suspendActivity();
        ThreadContext context;
        check(thread.waitForSuspendedContext(context, 1s),
              "A sleeping thread reports its context");
        check(std::chrono::steady_clock::now() - begin < 50ms,
              "A sleeping thread stops without finishing its sleep");
        std::this_thread::sleep_for(200ms);
        check(!thread.finished, "A suspended thread stays stopped after its "
                                "sleep would have ended");
        thread.resumeActivity();
        waitUntil(thread.finished, 1s, "A resumed thread finishes");
    }

    // A sleep lasts its full duration otherwise
    {
        TestThread thread(TestThread::Mode::Sleep, 50'000'000);
        const auto begin = std::chrono::steady_clock::now();
        thread.start();
        waitUntil(thread.finished, 1s, "A sleep ends");
        check(thread.finished_at - begin >= 50ms, "A sleep is not cut short");
    }

    // A timed wait that expires while suspended times out once resumed
    {
        TestThread thread(TestThread::Mode::TimedWait, 30'000'000);
        thread.start();
        std::this_thread::sleep_for(5ms);
        thread.suspendActivity();
        ThreadContext context;
        check(thread.waitForSuspendedContext(context, 1s),
              "A waiting thread reports its context");
        std::this_thread::sleep_for(100ms);
        check(!thread.finished, "The wait does not end while suspended");
        thread.resumeActivity();
        waitUntil(thread.finished, 1s, "The wait ends once resumed");
        check(thread.timed_out, "The expired wait times out");
    }

    // A wait that is not suspended still times out on time
    {
        TestThread thread(TestThread::Mode::TimedWait, 20'000'000);
        const auto begin = std::chrono::steady_clock::now();
        thread.start();
        waitUntil(thread.finished, 1s, "A timed wait ends");
        check(thread.timed_out, "The wait times out");
        check(thread.finished_at - begin < 500ms, "The wait ends on time");
    }

    std::cout << "PASS: thread suspension, context capture, interruptible "
                 "sleep and suspended timeouts\n";
    return 0;
}
