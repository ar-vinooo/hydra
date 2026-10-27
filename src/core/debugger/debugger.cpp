#include "core/debugger/debugger.hpp"

#include "core/hw/tegra_x1/cpu/mmu.hpp"

#include "core/debugger/gdb_server.hpp"
#include "core/horizon/kernel/guest_thread.hpp"
#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/cpu/thread.hpp"

#define GET_THIS_THREAD()                                                      \
    std::unique_lock lock(mutex);                                              \
    const auto thread_id = std::this_thread::get_id();                         \
    auto it = threads.find(thread_id);                                         \
    ASSERT_DEBUG(it != threads.end(), Debugger,                                \
                 "Thread {:016x} not registered",                              \
                 std::bit_cast<u64>(thread_id));                               \
    [[maybe_unused]] auto& thread = it->second;

namespace hydra::debugger {

ResolvedStackFrame StackFrame::resolve() const {
    switch (type) {
    case StackFrameType::Host:
        // TODO
        return {.module = "libhydra.dylib", .function = "", .addr = addr};
    case StackFrameType::Guest: {
        const auto& module = debugger->getModuleTable().findSymbol(addr);
        const auto& function = debugger->getFunctionTable().findSymbol(addr);
        return {.module = module, .function = function, .addr = addr};
    }
    }
}

Thread::Thread(const std::string_view name_) : name{name_} {
    // TODO: make this configurable
    messages.resize(256);
}

void Thread::log(const Message& msg) {
    std::unique_lock lock(msg_mutex);
    messages[(msg_tail + msg_count) % messages.size()] = msg;
    if (msg_count < messages.size())
        msg_count++;
    else
        msg_tail = (msg_tail + 1) % messages.size();
}

void Debugger::registerThisThread(const std::string_view thread_name) {
    std::unique_lock lock(mutex);
    ASSERT(threads.try_emplace(std::this_thread::get_id(), thread_name).second,
           Debugger, "Failed to register thread");
}

void Debugger::unregisterThisThread() {
    GET_THIS_THREAD();
    threads.erase(it);
}

void Debugger::registerGuestThreadForThisThread(
    horizon::kernel::GuestThread* guest_thread) {
    GET_THIS_THREAD();
    thread.guest_thread = guest_thread;

    if (gdb_server.has_value())
        gdb_server->registerThread(thread);
}

void Debugger::unregisterGuestThreadForThisThread() {
    GET_THIS_THREAD();
    thread.guest_thread = nullptr;
}

void Debugger::activateGdbServer(System& system) {
    gdb_server.emplace(system, *this);
}

void Debugger::notifySupervisorPaused(horizon::kernel::GuestThread* thread,
                                      Signal signal) {
    if (gdb_server.has_value())
        gdb_server->notifySupervisorPaused(thread, signal);
}

void Debugger::breakpointHit(horizon::kernel::GuestThread* thread) {
    if (gdb_server.has_value())
        gdb_server->breakpointHit(thread);
}

void Debugger::logOnThisThread(const LogMessage& msg) {
    std::unique_lock lock(mutex);
    const auto it = threads.find(std::this_thread::get_id());
    // Host callbacks and background workers may log without a debugger entry.
    // The logger has already written their message to its normal output.
    if (it == threads.end())
        return;
    auto& thread = it->second;
    lock.unlock();
    auto stack_trace = getStackTrace(thread);
    lock.lock();
    thread.log({.log = msg, .stack_trace = stack_trace});
}

void Debugger::breakOnThisThreadImpl(const std::string_view reason) {
    LOG_ERROR(Debugger, "BREAK ({})", reason);

    horizon::kernel::GuestThread* guest_thread;
    {
        GET_THIS_THREAD();
        thread.status = ThreadStatus::Break;
        thread.break_reason = reason;
        guest_thread = thread.guest_thread;
    }

    if (guest_thread && guest_thread->getThread()) {
        auto* cpu_thread = guest_thread->getThread();
        // This is the guest thread: it may read its stale registers
        cpu_thread->completeState();
        const auto& state = cpu_thread->getState();
        LOG_ERROR(Debugger,
                  "Guest registers x0=0x{:x}, x1=0x{:x}, x19=0x{:x}, "
                  "x20=0x{:x}, LR=0x{:x}",
                  state.r[0], state.r[1], state.r[19], state.r[20], state.lr);
        // DEBUG: full register file and the stack around sp
        for (u32 i = 0; i < 29; i += 4)
            LOG_ERROR(Debugger, "x{}..: 0x{:x} 0x{:x} 0x{:x} 0x{:x}", i,
                      state.r[i], i + 1 < 29 ? state.r[i + 1] : 0,
                      i + 2 < 29 ? state.r[i + 2] : 0,
                      i + 3 < 29 ? state.r[i + 3] : 0);
        LOG_ERROR(Debugger, "fp=0x{:x} sp=0x{:x}", state.fp, state.sp);
        auto* mmu = process->getMmu();
        for (u64 off = 0; off < 0x320; off += 0x20) {
            u64 v[4] = {};
            for (u32 j = 0; j < 4; j++)
                mmu->tryRead(state.sp + off + j * 8, v[j]);
            LOG_ERROR(Debugger, "[sp+0x{:03x}] {:016x} {:016x} {:016x} {:016x}",
                      off, v[0], v[1], v[2], v[3]);
        }
        cpu_thread->getStackTrace([&](vaddr_t address) {
            LOG_ERROR(Debugger, "Guest frame 0x{:x}: {} {}", address,
                      module_table.findSymbol(address),
                      function_table.findSymbol(address));
        });
    }
    process->supervisorPause();
    notifySupervisorPaused(
        guest_thread,
        Signal::SigHup); // TODO: make the signal configurable
}

StackTrace Debugger::getStackTrace(Thread& thread) {
    StackTrace stack_trace;

    // Host
    // TODO

    // Guest
    if (auto guest_thread = thread.guest_thread) {
        guest_thread->getThread()->getStackTrace([&](vaddr_t addr) {
            stack_trace.frames.emplace_back(this, StackFrameType::Guest, addr);
        });
    }

    return stack_trace;
}

} // namespace hydra::debugger
