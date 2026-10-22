#include <iostream>
#include <stdexcept>
#include <thread>

#include "common/log.hpp"
#include "core/debugger/debugger_manager.hpp"

using namespace hydra;

int main() {
    auto& debugger = GET_CURRENT_PROCESS_DEBUGGER();
    const auto initial_count = debugger.getThreadCount();
    bool recorded = false;
    std::thread worker([&] {
        // This used to dereference threads.end() in Release and abort.
        LOG_WARN(Services, "Unregistered online worker");
        debugger.registerThisThread("Online nickname regression");
        LOG_INFO(Services, "Registered online worker");
        {
            std::lock_guard lock(debugger);
            for (u32 i = 0; i < debugger.getThreadCount(); ++i) {
                auto& thread = debugger.getThread(i);
                if (thread.getName() == "Online nickname regression") {
                    std::lock_guard message_lock(thread);
                    recorded = thread.getMessageCount() == 1 &&
                               thread.getMessage(0).log.str ==
                                   "Registered online worker";
                }
            }
        }
        debugger.unregisterThisThread();
        LOG_WARN(Services, "Worker after unregister");
    });
    worker.join();
    if (!recorded || debugger.getThreadCount() != initial_count)
        throw std::runtime_error("Worker logging or debugger cleanup failed");
    std::cout << "PASS: unregistered worker logging, registered messages and cleanup\n";
}
