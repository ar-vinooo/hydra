#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace hydra::execution {

enum class WorkStatus { Queued, Running, Succeeded, Failed, Cancelled };
struct WorkLimits {
    std::size_t count;
    std::size_t bytes;
};
struct WorkStats {
    std::size_t pending{0};
    std::size_t bytes{0};
    std::size_t peak_pending{0};
    std::size_t peak_bytes{0};
    std::uint64_t admitted{0};
    std::uint64_t succeeded{0};
    std::uint64_t failed{0};
    std::uint64_t cancelled{0};
    std::uint64_t rejected{0};
};
struct WorkManagerState;

// A reservation owns its budget until a terminal outcome. Abandoned work is
// cancelled, never counted as success. Tickets can outlive their manager.
class WorkTicket {
  public:
    ~WorkTicket();
    WorkTicket(const WorkTicket&) = delete;
    WorkTicket& operator=(const WorkTicket&) = delete;
    WorkStatus status() const;
    // False means blocked or terminal. Failed dependencies fail this work.
    bool start();
    void finish(bool succeeded);
    void cancel();
    bool wait(std::chrono::milliseconds timeout) const;

    // Internal construction key keeps admission under manager control.
    struct ConstructionKey {
      private:
        friend class WorkManager;
        ConstructionKey() = default;
    };
    WorkTicket(ConstructionKey, std::shared_ptr<WorkManagerState> state_,
               std::size_t bytes_,
               std::vector<std::shared_ptr<WorkTicket>> dependencies_);
  private:
    friend class WorkManager;
    bool registered{false};
    void terminate(WorkStatus outcome);
    std::shared_ptr<WorkManagerState> state;
    const std::size_t bytes;
    std::vector<std::shared_ptr<WorkTicket>> dependencies;
    WorkStatus current{WorkStatus::Queued};
};

class WorkManager {
  public:
    explicit WorkManager(WorkLimits limits);
    ~WorkManager();
    WorkManager(const WorkManager&) = delete;
    WorkManager& operator=(const WorkManager&) = delete;
    // Nonblocking: a blocked guest channel must not prevent another channel
    // from submitting the work which satisfies its fence.
    std::shared_ptr<WorkTicket> tryAdmit(
        std::size_t bytes = 0,
        std::vector<std::shared_ptr<WorkTicket>> dependencies = {});
    // For host submissions already encoded and guaranteed to make progress.
    std::shared_ptr<WorkTicket> admit(std::size_t bytes = 0);
    void stop();
    WorkStats stats() const;

  private:
    std::shared_ptr<WorkTicket> reserve(
        const std::shared_ptr<WorkTicket>& ticket);
    std::shared_ptr<WorkManagerState> state;
};

} // namespace hydra::execution
