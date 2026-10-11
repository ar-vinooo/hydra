#include "core/horizon/kernel/thread.hpp"

#include "core/debugger/debugger_manager.hpp"
#include "core/horizon/kernel/process.hpp"

namespace hydra::horizon::kernel {

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

    const auto timeout_time = std::chrono::steady_clock::now() +
                              std::chrono::nanoseconds(pause_timeout_ns);

    std::unique_lock<std::mutex> lock(msg_mutex);
    while (true) {
        if (!processMessagesImpl())
            return true;
        if (state != ThreadState::Paused)
            return true;

        if (supervisor_pause || pause_timeout_ns == INFINITE_TIMEOUT) {
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
    msg_cv.notify_all(); // TODO: notify one?
}

void IThread::pushCancelResumeLocked() {
    // The wait ends here; later object signals must not wake the next wait
    guest_wait_armed = false;
    msg_queue.push({.type = ThreadMessageType::Resume,
                    .supervisor = false,
                    .payload = {.resume = {.signalled = false}}});
    msg_cv.notify_all();
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
            if (!supervisor_pause && !guest_pause)
                state = ThreadState::Running;

            if (!msg.supervisor && !sync_info)
                sync_info = payload;
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
