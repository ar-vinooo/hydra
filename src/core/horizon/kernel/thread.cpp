#include "core/horizon/kernel/thread.hpp"

#include "core/debugger/debugger_manager.hpp"
#include "core/horizon/kernel/process.hpp"

#include <bit>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

namespace hydra::horizon::kernel {

namespace {

#ifdef __APPLE__
// Guest threads at this priority or lower (numerically higher) prefer the
// efficiency cores, leaving the performance cores to the game's main threads
// and the GPU thread. An M1 efficiency core is about as fast as a Tegra X1
// core, so these threads still run at least at Switch speed. nn::os maps its
// default priority 16 to 44 and its lowest, 31, to 59.
// HYDRA_EFFICIENCY_CORE_PRIORITY overrides it (64 keeps every thread on the
// performance cores).
i32 getEfficiencyCorePriority() {
    static const i32 threshold = [] {
        if (const char* env = std::getenv("HYDRA_EFFICIENCY_CORE_PRIORITY"))
            return static_cast<i32>(std::atoi(env));
        return 52;
    }();
    return threshold;
}

qos_class_t getHostQos(i32 priority) {
    if (priority <= 28)
        return QOS_CLASS_USER_INTERACTIVE;
    if (priority < getEfficiencyCorePriority())
        return QOS_CLASS_USER_INITIATED;
    return QOS_CLASS_UTILITY;
}
#endif

} // namespace

void IThread::applyHostScheduling() {
#ifdef __APPLE__
    // macOS only allows a thread to change its own QoS. Apple silicon has no
    // core affinity, so the guest core mask stays a guest-visible property.
    pthread_set_qos_class_self_np(getHostQos(getPriority()), 0);
#endif
}

u32 IThread::getCurrentCore() const {
    const auto mask = getAffinityMask();
    const auto core = getIdealCore();
    if (core >= 0 && core < CPU_CORE_COUNT && (mask & (1ull << core)) != 0)
        return static_cast<u32>(core);
    return mask != 0 ? static_cast<u32>(std::countr_zero(mask)) : 0;
}

IThread::~IThread() noexcept { stopAndJoin(); }

void IThread::stopAndJoin() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex);
    if (!thread.joinable())
        return;
    ASSERT(thread.get_id() != std::this_thread::get_id(), Kernel,
           "Cannot destroy a thread from its own callback");
    if (!isStoppingOrStopped())
        stop();
    thread.join();
}

void IThread::start() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex);
    if (process)
        process->registerThread(this);
    thread = std::jthread([&] {
        tls_current_thread = this;
        host_scheduling_dirty.store(false, std::memory_order_relaxed);
        applyHostScheduling();

        GET_CURRENT_PROCESS_DEBUGGER().registerThisThread(getDebugName());

        run();
        if (process)
            process->unregisterThread(this);

        // Signal exit
        state = ThreadState::Stopped;
        signal();

        GET_CURRENT_PROCESS_DEBUGGER().unregisterThisThread();
    });
}

bool IThread::processMessages(i64 pause_timeout_ns) {
    if (state == ThreadState::Stopping)
        return true;
    // Called on every VM exit: skip the lock while nothing is queued and the
    // thread is not paused
    if (!has_messages.load(std::memory_order_acquire) &&
        state != ThreadState::Paused)
        return true;

    const auto timeout_time = std::chrono::steady_clock::now() +
                              std::chrono::nanoseconds(pause_timeout_ns);

    std::unique_lock<std::mutex> lock(msg_mutex);
    while (true) {
        if (!processMessagesImpl())
            return true;
        if (state != ThreadState::Paused)
            return true;

        // A suspended thread stays stopped past its wait timeout; the wait
        // times out once it is resumed
        if (supervisor_pause || activity_suspended ||
            pause_timeout_ns == INFINITE_TIMEOUT) {
            msg_cv.wait(lock, [&] { return !msg_queue.empty(); });
        } else if (!msg_cv.wait_until(lock, timeout_time,
                                      [&] { return !msg_queue.empty(); })) {
            guest_pause = false;
            guest_wait_armed = false;
            state = ThreadState::Running;
            sync_info.reset();
            return false;
        }
    }
}

void IThread::sendMessage(ThreadMessage msg) {
    std::scoped_lock lock(msg_mutex);
    if (!msg.supervisor) {
        if (msg.type == ThreadMessageType::Pause) {
            guest_wait_armed = true;
        } else if (msg.type == ThreadMessageType::Resume &&
                   msg.payload.resume.signalled) {
            // Only the first signal ends a wait. Signals that arrive after the
            // wait already ended (timeout, another object, ...) would
            // otherwise wake up the next, unrelated wait.
            if (!guest_wait_armed)
                return;
            guest_wait_armed = false;
        }
    }
    msg_queue.push(msg);
    has_messages.store(true, std::memory_order_release);
    msg_cv.notify_all(); // TODO: notify one?
}

void IThread::pushCancelResumeLocked() {
    // The wait ends here; later object signals must not wake the next wait
    guest_wait_armed = false;
    msg_queue.push({.type = ThreadMessageType::Resume,
                    .supervisor = false,
                    .payload = {.resume = {.signalled = false}}});
    has_messages.store(true, std::memory_order_release);
    msg_cv.notify_all();
}

bool IThread::waitForSuspendedContext(ThreadContext& out_context,
                                      std::chrono::nanoseconds timeout) {
    std::unique_lock lock(msg_mutex);
    if (!suspended_cv.wait_for(lock, timeout, [&] {
            return suspended_context_valid || !isSuspendRequested() ||
                   isStoppingOrStopped();
        }) ||
        !suspended_context_valid)
        return false;
    out_context = suspended_context;
    return true;
}

void IThread::sleep(i64 duration_ns) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::nanoseconds(duration_ns);
    std::unique_lock lock(msg_mutex);
    while (true) {
        // Only stop and suspension messages can be queued here: signals are
        // dropped while no wait is armed
        if (!msg_queue.empty() && !processMessagesImpl())
            return;
        if (!msg_cv.wait_until(lock, deadline,
                               [&] { return !msg_queue.empty(); }))
            return;
    }
}

void IThread::cancelSync() {
    std::scoped_lock lock(msg_mutex);
    if (sync_wait_active)
        pushCancelResumeLocked();
    else
        sync_wait_cancelled = true;
}

void IThread::beginSyncWait() {
    std::scoped_lock lock(msg_mutex);
    sync_wait_active = true;
    // A cancel that arrived before this wait ends it right away. The pause
    // message is already queued, so this resume is processed after it.
    if (sync_wait_cancelled) {
        sync_wait_cancelled = false;
        pushCancelResumeLocked();
    }
}

void IThread::endSyncWait() {
    std::scoped_lock lock(msg_mutex);
    sync_wait_active = false;
}

bool IThread::processMessagesImpl() {
    has_messages.store(false, std::memory_order_relaxed);
    while (!msg_queue.empty()) {
        auto msg = msg_queue.front();
        msg_queue.pop();

        // Process the message
        switch (msg.type) {
        case ThreadMessageType::Stop:
            state = ThreadState::Stopping;
            return false;
        case ThreadMessageType::Pause: {
            state = ThreadState::Paused;
            if (msg.supervisor) {
                supervisor_pause = true;
                prepareForDebugger();
            } else {
                guest_pause = true;
                sync_info = std::nullopt;
            }
            break;
        }
        case ThreadMessageType::Resume: {
            const auto& payload = msg.payload.resume;

            if (msg.supervisor)
                supervisor_pause = false;
            else
                guest_pause = false;
            if (!supervisor_pause && !guest_pause && !activity_suspended)
                state = ThreadState::Running;

            if (!msg.supervisor && !sync_info)
                sync_info = payload;
            break;
        }
        case ThreadMessageType::Suspend: {
            if (activity_suspended)
                break;
            activity_suspended = true;
            state = ThreadState::Paused;
            // Only this thread can read its registers
            captureContext(suspended_context);
            suspended_context_valid = true;
            suspended_cv.notify_all();
            break;
        }
        case ThreadMessageType::Unsuspend: {
            activity_suspended = false;
            suspended_context_valid = false;
            if (!supervisor_pause && !guest_pause)
                state = ThreadState::Running;
            break;
        }
        }
    }

    return true;
}

void IThread::addMutexWaiter(IThread* waiter) {
    std::scoped_lock lock(mutex_wait_mutex);
    ASSERT_DEBUG(mutex_wait_list.addLast(waiter).has_value(), Kernel,
                 "Failed to add mutex waiter");
}

void IThread::removeMutexWaiter(IThread* waiter) {
    std::scoped_lock lock(mutex_wait_mutex);
    mutex_wait_list.remove(waiter);
}

IThread* IThread::relinquishMutex(uptr mutex_addr, u32& out_waiter_count) {
    std::scoped_lock lock(mutex_wait_mutex);

    // Find a new owner
    IThread* new_owner = nullptr;
    out_waiter_count = 0;
    for (auto waiter_node = mutex_wait_list.getHead();
         waiter_node.has_value();) {
        const auto waiter_node_ = waiter_node.value();
        auto waiter = waiter_node_->get();
        if (waiter->mutex_wait_addr != mutex_addr) {
            waiter_node = waiter_node_->getNext();
            continue;
        }

        waiter_node = mutex_wait_list.remove(waiter_node_);
        if (new_owner != nullptr) {
            new_owner->addMutexWaiter(waiter);
            out_waiter_count++;
        } else {
            new_owner = waiter;
            new_owner->mutex_wait_addr = 0x0;
        }
    }

    return new_owner;
}

std::optional<IThread*> getMutexOwner(Process* process, u32 mutex) {
    // HACK
    const auto thread = process->getHandle<IThread>(mutex & ~MUTEX_WAIT_MASK);
    return (thread != nullptr ? std::make_optional(thread) : std::nullopt);
}

std::optional<IThread*> getMutexOwner(Process* process, u32* mutex_ptr) {
    if (mutex_ptr == nullptr)
        return std::nullopt;
    return getMutexOwner(process, atomicLoad(mutex_ptr));
}

} // namespace hydra::horizon::kernel
