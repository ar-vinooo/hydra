#include "core/execution/work_manager.hpp"

#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace hydra::execution;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
}
int main() {
    try {
        WorkManager manager{{2, 8}};
        auto first = manager.tryAdmit(5);
        check(first && !manager.tryAdmit(4), "Byte budget bounds admission");
        auto second = manager.tryAdmit(3, {first});
        check(second && !manager.tryAdmit(), "Count budget bounds admission");
        check(!second->start(), "Dependency blocks execution");
        check(first->start(), "Ready work starts");
        first->finish(true);
        first->finish(false);
        check(first->status() == WorkStatus::Succeeded && second->start(),
              "First terminal result wins and unblocks dependency");
        second->finish(false);
        check(manager.stats().pending == 0 && manager.stats().bytes == 0,
              "Completion releases both budgets");
        auto failed_child = manager.tryAdmit(0, {second});
        check(!failed_child->start() && failed_child->status() == WorkStatus::Failed,
              "Failure propagates without executing dependent work");
        auto abandoned = manager.tryAdmit(8);
        abandoned.reset();
        check(manager.stats().pending == 0 && manager.stats().cancelled == 1,
              "Abandonment cancels and releases budget");
        auto never_started = manager.tryAdmit();
        never_started->finish(true);
        check(never_started->status() == WorkStatus::Failed,
              "Unexecuted work cannot report success");
        check(!manager.tryAdmit(9) && !manager.tryAdmit(0, {nullptr}),
              "Oversized and null dependencies are rejected");

        WorkManager bounded{{1, 0}};
        auto active = bounded.admit();
        check(active->start(), "Admitted work starts");
        std::promise<void> waiting;
        auto blocked = std::async(std::launch::async, [&] {
            waiting.set_value();
            return bounded.admit();
        });
        waiting.get_future().wait();
        check(blocked.wait_for(20ms) == std::future_status::timeout,
              "Host submission backpressure waits for a free slot");
        active->finish(true);
        auto next = blocked.get();
        check(next && next->start(), "Completion wakes blocked submitter");
        auto stopping = std::async(std::launch::async, [&] { return bounded.admit(); });
        bounded.stop();
        check(!stopping.get() && !bounded.tryAdmit(), "Stop wakes and rejects submitters");
        next->finish(false);

        std::shared_ptr<WorkTicket> survivor;
        {
            WorkManager temporary{{1, 0}};
            survivor = temporary.tryAdmit();
        }
        check(survivor->status() == WorkStatus::Cancelled && !survivor->start(),
              "Manager teardown cancels surviving queued tickets");

        std::shared_ptr<WorkTicket> running_survivor;
        {
            WorkManager temporary{{1, 4}};
            running_survivor = temporary.tryAdmit(4);
            check(running_survivor->start(), "Surviving host work starts");
        }
        check(running_survivor->status() == WorkStatus::Running,
              "Manager teardown leaves running host work tracked");
        running_survivor->finish(true);
        check(running_survivor->wait(10ms), "Surviving running work completes safely");

        WorkManager concurrent{{8, 32}};
        std::vector<std::thread> workers;
        for (int index = 0; index < 8; ++index)
            workers.emplace_back([&] {
                for (int iteration = 0; iteration < 1000; ++iteration) {
                    auto work = concurrent.admit(4);
                    if (!work || !work->start())
                        std::terminate();
                    work->finish(true);
                }
            });
        for (auto& worker : workers)
            worker.join();
        const auto stats = concurrent.stats();
        check(stats.admitted == 8000 && stats.succeeded == 8000 &&
                  stats.pending == 0 && stats.bytes == 0 &&
                  stats.peak_pending <= 8 && stats.peak_bytes <= 32,
              "Concurrent admission never exceeds budgets or loses retirement");
        std::cout << "PASS: managed work budgets, dependencies, failure, cancellation, backpressure and concurrency\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
