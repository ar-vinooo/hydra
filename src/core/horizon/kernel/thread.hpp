#pragma once

#include "core/horizon/kernel/synchronization_object.hpp"

namespace hydra::horizon::kernel {

class Process;

constexpr u64 TLS_SIZE = 0x20000; // TODO: what should this be?
constexpr u32 MUTEX_WAIT_MASK = 0x40000000;

// Horizon thread priorities: 0 is the highest, 63 the lowest
constexpr i32 HIGHEST_THREAD_PRIORITY = 0;
constexpr i32 LOWEST_THREAD_PRIORITY = 63;
// The Tegra X1 has four cores; applications usually get cores 0-2
constexpr i32 CPU_CORE_COUNT = 4;
// Special ideal core values of CreateThread and SetThreadCoreMask
constexpr i32 IDEAL_CORE_DONT_CARE = -1;
constexpr i32 IDEAL_CORE_USE_PROCESS_VALUE = -2;
constexpr i32 IDEAL_CORE_NO_UPDATE = -3;

enum class ThreadState {
    Created,
    Running,
    Stopping,
    Stopped,
    Paused,
};

enum class ThreadMessageType {
    Stop,
    Pause,
    Resume,
    // svcSetThreadActivity
    Suspend,
    Unsuspend,
};

struct ThreadSyncInfo {
    bool signalled;
    SynchronizationObject* signalled_obj;
};

struct ThreadMessage {
    ThreadMessageType type;
    bool supervisor;
    union {
        // Resume
        ThreadSyncInfo resume;
    } payload;
};

class IThread : public SynchronizationObject {
    friend class Kernel;

  public:
    static constexpr AutoObjectTypeId TYPE_ID = AutoObjectTypeId::Thread;

    IThread(Process* process_, i32 priority_,
            std::string_view debug_name = "Thread") noexcept
        : SynchronizationObject(TYPE_ID, false, debug_name), process{process_},
          priority{priority_} {}

    // Scheduling, as seen by the guest. Hydra lets the host schedule every
    // thread on its own core; these drive the host QoS and what the guest
    // reads back.
    // Unique for the lifetime of the system, like Horizon's KThread IDs
    u64 getThreadId() const { return thread_id; }
    i32 getPriority() const { return priority.load(std::memory_order_relaxed); }
    // Applied to the host thread by the thread itself, see
    // applyHostSchedulingIfChanged
    void setPriority(i32 priority_) {
        priority.store(priority_, std::memory_order_relaxed);
        host_scheduling_dirty.store(true, std::memory_order_release);
    }
    i32 getIdealCore() const {
        return ideal_core.load(std::memory_order_relaxed);
    }
    u64 getAffinityMask() const {
        return affinity_mask.load(std::memory_order_relaxed);
    }
    void setCoreAffinity(i32 ideal_core_, u64 affinity_mask_) {
        ideal_core.store(ideal_core_, std::memory_order_relaxed);
        affinity_mask.store(affinity_mask_, std::memory_order_relaxed);
    }
    // The core the thread runs on, from the guest's point of view
    u32 getCurrentCore() const;
    // svcSetThreadActivity: the thread stops at its next VM exit or kernel
    // wait and records its context there, see waitForSuspendedContext
    bool isSuspendRequested() const {
        return suspend_requested.load(std::memory_order_acquire);
    }
    void suspendActivity() {
        suspend_requested.store(true, std::memory_order_release);
        sendMessage({.type = ThreadMessageType::Suspend});
    }
    void resumeActivity() {
        suspend_requested.store(false, std::memory_order_release);
        sendMessage({.type = ThreadMessageType::Unsuspend});
    }
    // The context recorded when the thread stopped. False on timeout.
    bool waitForSuspendedContext(ThreadContext& out_context,
                                 std::chrono::nanoseconds timeout);
    // svcSleepThread: ends early only to stop or be suspended. Must be called
    // from the thread itself.
    void sleep(i64 duration_ns);

    // Must be called from the thread itself
    void applyHostSchedulingIfChanged() {
        if (host_scheduling_dirty.exchange(false, std::memory_order_acquire))
            applyHostScheduling();
    }
    ~IThread() noexcept override;

    void start();
    void stopAndJoin();

    // Messages
    void stop() {
        sendMessage({.type = ThreadMessageType::Stop, .supervisor = false});
    }
    // TODO: SupervisorStop?
    void pause() {
        sendMessage({.type = ThreadMessageType::Pause, .supervisor = false});
    }
    void supervisorPause() {
        sendMessage({.type = ThreadMessageType::Pause, .supervisor = true});
    }
    void resume(SynchronizationObject* signalled_obj = nullptr) {
        sendMessage({.type = ThreadMessageType::Resume,
                     .supervisor = false,
                     .payload = {.resume = {.signalled = true,
                                            .signalled_obj = signalled_obj}}});
    }
    // CancelSynchronization: ends the current WaitSynchronization, or, like
    // Horizon, makes the next one return Cancelled if the thread is not
    // waiting yet. nn::os wakes MultiWait waiters this way.
    void cancelSync();
    // Unconditionally ends the current guest wait (e.g. an IPC request whose
    // server went away)
    void abortWait() {
        sendMessage({.type = ThreadMessageType::Resume,
                     .supervisor = false,
                     .payload = {.resume = {.signalled = false}}});
    }
    // Bracket a WaitSynchronization; call beginSyncWait after pause()
    void beginSyncWait();
    void endSyncWait();
    void supervisorResume() {
        sendMessage({.type = ThreadMessageType::Resume, .supervisor = true});
    }

    // Must not be called from a different thread
    bool processMessages(i64 pause_timeout_ns = INFINITE_TIMEOUT);
    bool wasSignalled() const {
        ASSERT_DEBUG(sync_info, Kernel, "No signal info present");
        const auto& sync_info_value = sync_info.value();
        ASSERT_DEBUG(!sync_info_value.signalled_obj, Kernel,
                     "Unexpected signalled object {}",
                     sync_info_value.signalled_obj->getDebugName());
        return sync_info_value.signalled;
    }
    bool consumeSignalledObject(SynchronizationObject*& out_obj) const {
        ASSERT_DEBUG(sync_info, Kernel, "No signal info present");
        const auto& sync_info_value = sync_info.value();
        if (!sync_info_value.signalled)
            return false;

        ASSERT_DEBUG(sync_info_value.signalled_obj, Kernel,
                     "Expected signalled object (self: {})", getDebugName());
        out_obj = sync_info_value.signalled_obj;
        return true;
    }

    bool isStoppingOrStopped() const {
        return state == ThreadState::Stopping || state == ThreadState::Stopped;
    }

    virtual uptr getTlsPtr() const = 0;

  protected:
    Process* process;

    virtual void run() = 0;
    // Called on the thread itself when it stops for svcSetThreadActivity
    virtual void captureContext(ThreadContext& out_context) {
        out_context = {};
    }
    // Called on the thread itself when the debugger pauses it: the debugger
    // then reads its registers from another thread
    virtual void prepareForDebugger() {}

    // Helpers
    void reset() {
        state = ThreadState::Created;
        msg_queue = {};
        mutex_wait_addr = 0x0;
        cond_var_wait_addr = 0x0;
        cond_var_wait_addr = 0x0;
        mutex_wait_list.clear();
        supervisor_pause = false;
        guest_pause = false;
    }

  private:
    static inline std::atomic<u64> next_thread_id{1};
    const u64 thread_id{next_thread_id.fetch_add(1, std::memory_order_relaxed)};
    std::atomic<i32> priority;
    std::atomic<i32> ideal_core{0};
    std::atomic<u64> affinity_mask{0x1};
    std::atomic<bool> host_scheduling_dirty{false};
    void applyHostScheduling();

    std::atomic<ThreadState> state{ThreadState::Created};

    // Serialize startup with joining a thread registered during shutdown.
    std::mutex lifecycle_mutex;
    std::mutex msg_mutex;
    // Set between a guest pause and the signal that ends that wait
    bool guest_wait_armed{false};
    // WaitSynchronization state for CancelSynchronization
    bool sync_wait_active{false};
    bool sync_wait_cancelled{false};
    void pushCancelResumeLocked();
    std::condition_variable msg_cv;
    std::queue<ThreadMessage> msg_queue;
    // Whether msg_queue may be non-empty, read without msg_mutex
    std::atomic<bool> has_messages{false};

    // svcSetThreadActivity
    std::atomic<bool> suspend_requested{false};
    bool activity_suspended{false};
    bool suspended_context_valid{false};
    ThreadContext suspended_context{};
    std::condition_variable suspended_cv;

    // Mutex and cond var
    uptr mutex_wait_addr{0x0};
    Handle self_handle_for_mutex{INVALID_HANDLE};
    uptr cond_var_wait_addr{0x0};
    std::mutex mutex_wait_mutex;
    ztd::DoublyLinkedList<IThread*> mutex_wait_list;

    // Synchronization
    bool supervisor_pause{false};
    bool guest_pause{false};
    std::optional<ThreadSyncInfo> sync_info{std::nullopt};

    std::jthread thread;

    // Helpers

    // Messages
    void sendMessage(ThreadMessage msg);
    bool processMessagesImpl();

    // Mutex
    void addMutexWaiter(IThread* waiter);
    void removeMutexWaiter(IThread* waiter);
    IThread* relinquishMutex(uptr mutex_addr, u32& out_waiter_count);

  public:
    GETTER(process, getProcess);
    ThreadState getState() const { return state.load(); }
};

inline thread_local IThread* tls_current_thread = nullptr;

std::optional<IThread*> getMutexOwner(Process* process, u32 mutex);
std::optional<IThread*> getMutexOwner(Process* process, u32* mutex_ptr);

} // namespace hydra::horizon::kernel
