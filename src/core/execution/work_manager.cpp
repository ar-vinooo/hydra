#include "core/execution/work_manager.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace hydra::execution {
struct WorkManagerState {
    explicit WorkManagerState(WorkLimits limits_) : limits{limits_} {}
    const WorkLimits limits;
    mutable std::mutex mutex;
    std::condition_variable condition;
    WorkStats stats;
    std::unordered_map<WorkTicket*, std::weak_ptr<WorkTicket>> tickets;
    bool stopping{false};
};
namespace {
bool terminal(WorkStatus status) {
    return status == WorkStatus::Succeeded || status == WorkStatus::Failed ||
           status == WorkStatus::Cancelled;
}
bool available(const WorkManagerState& state, std::size_t bytes) {
    return state.stats.pending < state.limits.count &&
           bytes <= state.limits.bytes - state.stats.bytes;
}
} // namespace

WorkTicket::WorkTicket(ConstructionKey, std::shared_ptr<WorkManagerState> state_,
                       std::size_t bytes_,
                       std::vector<std::shared_ptr<WorkTicket>> dependencies_)
    : state{std::move(state_)}, bytes{bytes_},
      dependencies{std::move(dependencies_)} {}
WorkTicket::~WorkTicket() {
    if (registered)
        cancel();
}
WorkStatus WorkTicket::status() const {
    std::scoped_lock lock(state->mutex);
    return current;
}
bool WorkTicket::start() {
    std::vector<std::shared_ptr<WorkTicket>> required;
    bool stopping;
    {
        std::scoped_lock lock(state->mutex);
        if (current != WorkStatus::Queued)
            return false;
        stopping = state->stopping;
        required = dependencies;
    }
    if (stopping) {
        cancel();
        return false;
    }
    // Dependency locks are never nested with our manager lock.
    bool failed = false;
    bool blocked = false;
    for (const auto& dependency : required) {
        const auto value = dependency->status();
        failed |= value == WorkStatus::Failed || value == WorkStatus::Cancelled;
        blocked |= value != WorkStatus::Succeeded;
    }
    if (failed) {
        terminate(WorkStatus::Failed);
        return false;
    }
    if (blocked)
        return false;
    std::scoped_lock lock(state->mutex);
    if (current != WorkStatus::Queued || state->stopping)
        return false;
    current = WorkStatus::Running;
    return true;
}
void WorkTicket::terminate(WorkStatus outcome) {
    std::vector<std::shared_ptr<WorkTicket>> released;
    {
        std::scoped_lock lock(state->mutex);
        if (terminal(current))
            return;
        if (outcome == WorkStatus::Succeeded && current != WorkStatus::Running)
            outcome = WorkStatus::Failed;
        current = outcome;
        released = std::move(dependencies);
        state->tickets.erase(this);
        --state->stats.pending;
        state->stats.bytes -= bytes;
        if (outcome == WorkStatus::Succeeded)
            ++state->stats.succeeded;
        else if (outcome == WorkStatus::Failed)
            ++state->stats.failed;
        else
            ++state->stats.cancelled;
    }
    state->condition.notify_all();
}
void WorkTicket::finish(bool succeeded) {
    terminate(succeeded ? WorkStatus::Succeeded : WorkStatus::Failed);
}
void WorkTicket::cancel() { terminate(WorkStatus::Cancelled); }
bool WorkTicket::wait(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(state->mutex);
    return state->condition.wait_for(lock, timeout,
                                     [&] { return terminal(current); });
}
WorkManager::WorkManager(WorkLimits limits)
    : state{std::make_shared<WorkManagerState>(limits)} {
    if (limits.count == 0)
        throw std::invalid_argument("Work capacity must be positive");
}
WorkManager::~WorkManager() { stop(); }
std::shared_ptr<WorkTicket> WorkManager::reserve(
    const std::shared_ptr<WorkTicket>& ticket) {
    const auto bytes = ticket->bytes;
    state->tickets.emplace(ticket.get(), ticket);
    ticket->registered = true;
    ++state->stats.pending;
    state->stats.bytes += bytes;
    ++state->stats.admitted;
    state->stats.peak_pending =
        std::max(state->stats.peak_pending, state->stats.pending);
    state->stats.peak_bytes = std::max(state->stats.peak_bytes, state->stats.bytes);
    return ticket;
}
std::shared_ptr<WorkTicket> WorkManager::tryAdmit(
    std::size_t bytes, std::vector<std::shared_ptr<WorkTicket>> dependencies) {
    if (std::any_of(dependencies.begin(), dependencies.end(),
                    [](const auto& dependency) { return !dependency; })) {
        std::scoped_lock lock(state->mutex);
        ++state->stats.rejected;
        return {};
    }
    // Allocate before locking: allocation failure can release dependencies,
    // whose last ticket destructor may need this same mutex.
    auto ticket = std::make_shared<WorkTicket>(
        WorkTicket::ConstructionKey{}, state, bytes, std::move(dependencies));
    std::unique_lock lock(state->mutex);
    if (state->stopping || !available(*state, bytes)) {
        ++state->stats.rejected;
        return {};
    }
    return reserve(ticket);
}
std::shared_ptr<WorkTicket> WorkManager::admit(std::size_t bytes) {
    auto ticket = std::make_shared<WorkTicket>(
        WorkTicket::ConstructionKey{}, state, bytes,
        std::vector<std::shared_ptr<WorkTicket>>{});
    std::unique_lock lock(state->mutex);
    if (bytes <= state->limits.bytes)
        state->condition.wait(lock, [&] {
            return state->stopping || available(*state, bytes);
        });
    if (state->stopping || !available(*state, bytes)) {
        ++state->stats.rejected;
        return {};
    }
    return reserve(ticket);
}
void WorkManager::stop() {
    decltype(state->tickets) tickets;
    {
        std::scoped_lock lock(state->mutex);
        state->stopping = true;
        tickets.swap(state->tickets);
    }
    state->condition.notify_all();
    // Running host work keeps its budget until real completion. Never
    // destroy a last ticket reference while holding its manager mutex.
    for (const auto& [address, weak] : tickets)
        if (auto ticket = weak.lock())
            if (ticket->status() == WorkStatus::Queued)
                ticket->cancel();
}
WorkStats WorkManager::stats() const {
    std::scoped_lock lock(state->mutex);
    return state->stats;
}
} // namespace hydra::execution
