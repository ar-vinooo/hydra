#include "core/horizon/kernel/kernel.hpp"

#include <set>

#include "core/debugger/debugger_manager.hpp"
#include "core/horizon/kernel/code_memory.hpp"
#include "core/horizon/kernel/hipc/client_session.hpp"
#include "core/horizon/kernel/hipc/server_port.hpp"
#include "core/horizon/kernel/hipc/server_session.hpp"
#include "core/horizon/kernel/hipc/session.hpp"
#include "core/horizon/kernel/process.hpp"
#include "core/horizon/kernel/synchronization_object.hpp"
#include "core/horizon/kernel/thread.hpp"
#include "core/hw/tegra_x1/cpu/cpu.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/cpu/thread.hpp"
#include "core/system.hpp"

namespace hydra::horizon::kernel {

Kernel::Kernel(System& system_) : system{system_}, process_manager(system) {
    free_applet_resource_user_ids.fill(true);
}

void Kernel::supervisorCall(Process* crnt_process, IThread* crnt_thread,
                            hw::tegra_x1::cpu::IThread* guest_thread, u64 id) {
    auto& state = guest_thread->getState();
    // SetThreadPriority can only change the host QoS from the thread itself
    if (crnt_thread)
        crnt_thread->applyHostSchedulingIfChanged();

    i32 tmp_i32;
    u32 tmp_u32;
    u64 tmp_u64;
    uptr tmp_uptr;
    // DEBUG: HYDRA_TRACE_SVC traces memory SVCs, waits and failed SVCs
    static const bool trace_svc = std::getenv("HYDRA_TRACE_SVC") != nullptr;
    static const bool trace_condvar =
        trace_svc || std::getenv("HYDRA_TRACE_CONDVAR") != nullptr;
    const std::array<u64, 4> trace_args = {state.r[0], state.r[1], state.r[2],
                                           state.r[3]};
    switch (id) {
    case 0x1:
        state.r[0] = setHeapSize(crnt_process, state.r[1], tmp_uptr);
        state.r[1] = tmp_uptr;
        break;
    case 0x2:
        state.r[0] = setMemoryPermission(
            state.r[0], state.r[1], static_cast<MemoryPermission>(state.r[2]));
        break;
    case 0x3:
        state.r[0] =
            setMemoryAttribute(crnt_process, state.r[0], state.r[1],
                               static_cast<MemoryAttribute>(state.r[2]),
                               static_cast<MemoryAttribute>(state.r[3]));
        break;
    case 0x4:
        state.r[0] =
            mapMemory(crnt_process, state.r[0], state.r[1], state.r[2]);
        break;
    case 0x5:
        state.r[0] =
            unmapMemory(crnt_process, state.r[0], state.r[1], state.r[2]);
        break;
    case 0x6:
        state.r[0] =
            queryMemory(crnt_process, state.r[2],
                        *reinterpret_cast<MemoryInfo*>(
                            crnt_process->getMmu()->unmapAddr(state.r[0])),
                        tmp_u32);
        state.r[1] = tmp_u32;
        break;
    case 0x7:
        exitProcess(crnt_process);
        break;
    case 0x8: {
        IThread* thread = nullptr;
        state.r[0] = createThread(
            crnt_process, state.r[1], state.r[2], state.r[3],
            std::bit_cast<i32>(static_cast<u32>(state.r[4])),
            std::bit_cast<i32>(static_cast<u32>(state.r[5])), thread);
        if (state.r[0] == RESULT_SUCCESS)
            state.r[1] = crnt_process->addHandleNoRetain(thread).getRaw();
        break;
    }
    case 0x9:
        state.r[0] = startThread(crnt_process->getHandle<IThread>(
            Handle(static_cast<u32>(state.r[0]))));
        break;
    case 0xa:
        exitThread(crnt_thread);
        break;
    case 0xb: {
        // Optional polling diagnostics; avoid atomics and stack walks in normal play.
        static const bool trace_sleep =
            std::getenv("HYDRA_TRACE_SLEEP") != nullptr;
        static std::atomic<u64> sleep_calls{0};
        if (trace_sleep && sleep_calls.fetch_add(1) % 500 == 0) {
            std::string frames = fmt::format("{:#x}", state.lr);
            vaddr_t fp = state.fp;
            for (u32 i = 0; i < 6 && fp != 0; i++) {
                u64 next_fp = 0, ret = 0;
                if (!crnt_process->getMmu()->tryRead(fp, next_fp) ||
                    !crnt_process->getMmu()->tryRead(fp + 8, ret))
                    break;
                frames += fmt::format(" {:#x}", ret);
                fp = next_fp;
            }
            LOG_INFO(Kernel, "SleepThread({}) from {}",
                     std::bit_cast<i64>(state.r[0]), frames);
        }
        sleepThread(crnt_thread, std::bit_cast<i64>(state.r[0]));
        break;
    }
    case 0xc:
        state.r[0] = getThreadPriority(
            crnt_process->getHandle<IThread>(static_cast<u32>(state.r[1])),
            tmp_i32);
        state.r[1] = std::bit_cast<u32>(tmp_i32);
        break;
    case 0xd:
        state.r[0] = setThreadPriority(
            crnt_process->getHandle<IThread>(static_cast<u32>(state.r[0])),
            std::bit_cast<i32>(static_cast<u32>(state.r[1])));
        break;
    case 0xe:
        // The thread handle is passed in w2; x0/x1 hold userland out pointers
        state.r[0] = getThreadCoreMask(
            crnt_process->getHandle<IThread>(static_cast<u32>(state.r[2])),
            tmp_i32, tmp_u64);
        state.r[1] = std::bit_cast<u32>(tmp_i32);
        state.r[2] = tmp_u64;
        break;
    case 0xf:
        state.r[0] = setThreadCoreMask(
            crnt_process->getHandle<IThread>(static_cast<u32>(state.r[0])),
            std::bit_cast<i32>(static_cast<u32>(state.r[1])), state.r[2]);
        break;
    case 0x10:
        getCurrentProcessorNumber(crnt_thread, tmp_u32);
        state.r[0] = tmp_u32;
        break;
    case 0x11:
        state.r[0] = signalEvent(
            crnt_process->getHandle<Event>(static_cast<u32>(state.r[0])));
        break;
    case 0x12:
        state.r[0] = clearEvent(
            crnt_process->getHandle<Event>(static_cast<u32>(state.r[0])));
        break;
    case 0x13:
        state.r[0] = mapSharedMemory(
            crnt_process,
            crnt_process->getHandle<SharedMemory>(static_cast<u32>(state.r[0])),
            state.r[1], state.r[2], static_cast<MemoryPermission>(state.r[3]));
        break;
    case 0x14:
        state.r[0] = unmapSharedMemory(
            crnt_process,
            crnt_process->getHandle<SharedMemory>(static_cast<u32>(state.r[0])),
            state.r[1], state.r[2]);
        break;
    case 0x15: {
        TransferMemory* tmem = nullptr;
        state.r[0] = createTransferMemory(
            state.r[1], state.r[2], static_cast<MemoryPermission>(state.r[3]),
            tmem);
        state.r[1] = crnt_process->addHandleNoRetain(tmem).getRaw();
        break;
    }
    case 0x16:
        state.r[0] = closeHandle(crnt_process, static_cast<u32>(state.r[0]));
        break;
    case 0x17:
        state.r[0] = resetSignal(crnt_process->getHandle<SynchronizationObject>(
            static_cast<u32>(state.r[0])));
        break;
    case 0x18: {
        const auto handles = reinterpret_cast<Handle*>(
            crnt_process->getMmu()->unmapAddr(state.r[1]));
        const auto num_handles = std::bit_cast<i64>(state.r[2]);
        SynchronizationObject* sync_objs[num_handles];
        for (auto i = 0; i < num_handles; i++)
            sync_objs[i] =
                crnt_process->getHandle<SynchronizationObject>(handles[i]);

        // An infinite timeout is normal for vsync/event waits, not a stall.
        if (trace_svc && std::bit_cast<i64>(state.r[3]) < 0) {
            std::string names;
            for (auto i = 0; i < num_handles; i++)
                names += fmt::format(
                    "{}{}", i ? ", " : "",
                    sync_objs[i] ? sync_objs[i]->getDebugName() : "null");
            LOG_INFO(Kernel, "Infinite wait on [{}]", names);
        }
        const auto wait_start = std::chrono::steady_clock::now();
        state.r[0] = waitSynchronization(
            crnt_thread, std::span(sync_objs, static_cast<usize>(num_handles)),
            std::bit_cast<i64>(state.r[3]), tmp_u32);
        if (state.r[0] != MAKE_RESULT(Svc, Error::Cancelled) &&
            std::chrono::steady_clock::now() - wait_start >
                std::chrono::seconds(2)) {
            std::string names;
            for (auto i = 0; i < num_handles; i++)
                names += fmt::format(
                    "{}{}", i ? ", " : "",
                    sync_objs[i] ? sync_objs[i]->getDebugName() : "null");
            LOG_WARN(Kernel, "Long wait on [{}] (timeout: {}, result: {:#x})",
                     names, std::bit_cast<i64>(trace_args[3]),
                     static_cast<u32>(state.r[0]));
        }
        state.r[1] = tmp_u32;
        break;
    }
    case 0x19:
        state.r[0] = cancelSynchronization(
            crnt_process->getHandle<IThread>(static_cast<u32>(state.r[0])));
        break;
    case 0x1a:
        state.r[0] = arbitrateLock(
            crnt_thread,
            crnt_process->getHandle<IThread>(static_cast<u32>(state.r[0])),
            crnt_process->getMmu()->unmapAddr(state.r[1]),
            static_cast<u32>(state.r[2]), static_cast<u32>(state.r[0]));
        break;
    case 0x1b:
        state.r[0] = arbitrateUnlock(
            crnt_thread, crnt_process->getMmu()->unmapAddr(state.r[0]));
        break;
    case 0x1c:
        // Stack walks, formatting and the shared call-site lock are diagnostic
        // work: keep them off the normal guest synchronization path.
        if (trace_condvar && std::bit_cast<i64>(state.r[3]) < 0) {
            static std::mutex sites_mutex;
            static std::set<u64> sites;
            std::string frames = fmt::format("{:#x}", state.lr);
            u64 key = state.lr;
            vaddr_t fp = state.fp;
            for (u32 i = 0; i < 8 && fp != 0; i++) {
                u64 next_fp = 0, ret = 0;
                if (!crnt_process->getMmu()->tryRead(fp, next_fp) ||
                    !crnt_process->getMmu()->tryRead(fp + 8, ret))
                    break;
                frames += fmt::format(" {:#x}", ret);
                key = key * 31 + ret;
                fp = next_fp;
            }
            std::scoped_lock lock(sites_mutex);
            if (sites.insert(key).second)
                LOG_INFO(Kernel, "CondVar wait (cv: {:#x}) from {}", state.r[1],
                         frames);
        }
        {
            const i64 wait_timeout = std::bit_cast<i64>(state.r[3]);
            state.r[0] = waitProcessWideKeyAtomic(
                crnt_process, crnt_thread,
                crnt_process->getMmu()->unmapAddr(state.r[0]),
                crnt_process->getMmu()->unmapAddr(state.r[1]),
                static_cast<u32>(state.r[2]), wait_timeout);
        }
        break;
    case 0x1d: {
        const u64 guest_cv = state.r[0];
        const auto signal_count = std::bit_cast<i32>(static_cast<u32>(state.r[1]));
        state.r[0] = signalProcessWideKey(
            crnt_process, crnt_process->getMmu()->unmapAddr(guest_cv),
            signal_count);
        break;
    }
    case 0x1e:
        getSystemTick(tmp_u64);
        state.r[0] = tmp_u64;
        break;
    case 0x1f: {
        hipc::ClientSession* client_session = nullptr;
        state.r[0] = connectToNamedPort(
            reinterpret_cast<const char*>(
                crnt_process->getMmu()->unmapAddr(state.r[1])),
            client_session);
        state.r[1] = crnt_process->addHandleNoRetain(client_session).getRaw();
        break;
    }
    case 0x21:
        state.r[0] =
            sendSyncRequest(crnt_process, crnt_thread,
                            crnt_process->getHandle<hipc::ClientSession>(
                                static_cast<u32>(state.r[0])));
        break;
    case 0x24:
        state.r[0] = getProcessId(
            crnt_process->getHandle<AutoObject>(static_cast<u32>(state.r[1])),
            tmp_u64);
        state.r[1] = tmp_u64;
        break;
    case 0x25:
        state.r[0] = getThreadId(
            crnt_process->getHandle<IThread>(static_cast<u32>(state.r[1])),
            tmp_u64);
        state.r[1] = tmp_u64;
        break;
    case 0x26: {
        LOG_ERROR(Kernel, "Guest break at PC 0x{:x}, LR 0x{:x}", state.pc,
                  state.lr);
        guest_thread->getStackTrace([&](vaddr_t address) {
            LOG_ERROR(Kernel, "Guest stack frame 0x{:x}", address);
        });
        const vaddr_t addr = state.r[1];
        state.r[0] = break_(
            BreakReason(state.r[0]),
            (addr != 0x0 ? crnt_process->getMmu()->unmapAddr(addr) : 0x0),
            state.r[2]);
        break;
    }
    case 0x27:
        state.r[0] = outputDebugString(
            reinterpret_cast<const char*>(
                crnt_process->getMmu()->unmapAddr(state.r[0])),
            state.r[1]);
        break;
    case 0x2a: // FlushEntireDataCache
    case 0x2b: // FlushDataCache
        // CPU writes reach the GPU through write tracking, not through
        // the data cache: nothing to write back
        state.r[0] = RESULT_SUCCESS;
        break;
    case 0x3c: // KernelDebug
    case 0x3d: // ChangeKernelTraceState
        // No-ops on retail kernels
        state.r[0] = RESULT_SUCCESS;
        break;
    case 0x29:
        state.r[0] = getInfo(
            crnt_process, static_cast<InfoType>(state.r[1]),
            crnt_process->getHandle<AutoObject>(static_cast<u32>(state.r[2])),
            state.r[3], tmp_u64);
        state.r[1] = tmp_u64;
        break;
    case 0x2c:
        state.r[0] = mapPhysicalMemory(crnt_process, state.r[0], state.r[1]);
        break;
    case 0x2d:
        state.r[0] =
            unmapPhysicalMemory(crnt_process, state.r[0], state.r[1]);
        break;
    case 0x32:
        state.r[0] = setThreadActivity(
            crnt_thread,
            crnt_process->getHandle<IThread>(static_cast<u32>(state.r[0])),
            static_cast<ThreadActivity>(state.r[1]));
        break;
    case 0x33: {
        auto* context = reinterpret_cast<ThreadContext*>(
            crnt_process->getMmu()->unmapAddr(state.r[0]));
        state.r[0] =
            context == nullptr
                ? MAKE_RESULT(Svc, Error::InvalidUserBuffer)
                : getThreadContext3(crnt_thread,
                                    crnt_process->getHandle<IThread>(
                                        static_cast<u32>(state.r[1])),
                                    *context);
        break;
    }
    case 0x34:
        state.r[0] = waitForAddress(
            crnt_thread, crnt_process->getMmu()->unmapAddr(state.r[0]),
            static_cast<ArbitrationType>(state.r[1]),
            static_cast<u32>(state.r[2]), std::bit_cast<i64>(state.r[3]));
        break;
    case 0x35:
        state.r[0] = signalToAddress(
            crnt_process->getMmu()->unmapAddr(state.r[0]),
            static_cast<SignalType>(state.r[1]), static_cast<u32>(state.r[2]),
            static_cast<u32>(state.r[3]));
        break;
    case 0x36:
        synchronizePreemptionState(crnt_thread);
        break;
    case 0x40: {
        hipc::ServerSession* server_session = nullptr;
        hipc::ClientSession* client_session = nullptr;
        state.r[0] = createSession(state.r[2] != 0, state.r[3], server_session,
                                   client_session);
        state.r[1] = crnt_process->addHandleNoRetain(server_session).getRaw();
        state.r[2] = crnt_process->addHandleNoRetain(client_session).getRaw();
        break;
    }
    case 0x41: {
        hipc::ServerSession* server_session = nullptr;
        state.r[0] = acceptSession(crnt_process->getHandle<hipc::ServerPort>(
                                       static_cast<u32>(state.r[1])),
                                   server_session);
        state.r[1] = crnt_process->addHandleNoRetain(server_session).getRaw();
        break;
    }
    case 0x43: {
        const auto handles = reinterpret_cast<Handle*>(
            crnt_process->getMmu()->unmapAddr(state.r[1]));
        const auto num_handles = std::bit_cast<i64>(state.r[2]);
        SynchronizationObject* sync_objs[num_handles];
        for (auto i = 0; i < num_handles; i++)
            sync_objs[i] =
                crnt_process->getHandle<SynchronizationObject>(handles[i]);

        state.r[0] = replyAndReceive(
            crnt_thread, std::span(sync_objs, static_cast<usize>(num_handles)),
            crnt_process->getHandle<hipc::ServerSession>(
                static_cast<u32>(state.r[3])),
            std::bit_cast<i64>(state.r[4]), tmp_u32);
        state.r[1] = tmp_u32;
        break;
    }
    case 0x4b: {
        CodeMemory* code_mem = nullptr;
        state.r[0] = createCodeMemory(state.r[1], state.r[2], code_mem);
        state.r[1] = crnt_process->addHandleNoRetain(code_mem).getRaw();
        break;
    }
    case 0x4c:
        state.r[0] = controlCodeMemory(
            crnt_process->getHandle<CodeMemory>(static_cast<u32>(state.r[0])),
            CodeMemoryOperation(state.r[1]), state.r[2], state.r[3],
            MemoryPermission(state.r[4]));
        break;
    case 0x65:
        state.r[0] =
            getProcessList(reinterpret_cast<u64*>(
                               crnt_process->getMmu()->unmapAddr(state.r[1])),
                           static_cast<u32>(state.r[2]), tmp_u32);
        state.r[1] = tmp_u32;
        break;
    case 0x73:
        state.r[0] = setProcessMemoryPermission(
            crnt_process->getHandle<Process>(static_cast<u32>(state.r[0])),
            state.r[1], state.r[2], MemoryPermission(state.r[3]));
        break;
    case 0x74:
        state.r[0] = mapProcessMemory(
            crnt_process, state.r[0],
            crnt_process->getHandle<Process>(static_cast<u32>(state.r[1])),
            state.r[2], state.r[3]);
        break;
    case 0x77:
        state.r[0] = mapProcessCodeMemory(
            crnt_process->getHandle<Process>(static_cast<u32>(state.r[0])),
            state.r[1], state.r[2], state.r[3]);
        break;
    case 0x78:
        state.r[0] = unmapProcessCodeMemory(
            crnt_process->getHandle<Process>(static_cast<u32>(state.r[0])),
            state.r[1], state.r[2], state.r[3]);
        break;
    default:
        LOG_NOT_IMPLEMENTED(Kernel, "SVC {:#x}", id);
        state.r[0] = MAKE_RESULT(Svc, Error::NotImplemented);
        break;
    }

    if (trace_svc) {
        const bool memory_svc = id <= 0x6 || id == 0x29 || id == 0x2c ||
                                id == 0x2d || id == 0x13 || id == 0x14 ||
                                id == 0x15;
        const bool failed = static_cast<u32>(state.r[0]) != RESULT_SUCCESS &&
                            id != 0x18 && id != 0x34 && id != 0x1a;
        if (memory_svc || failed)
            LOG_INFO(Kernel,
                     "SVC {:#x}({:#x}, {:#x}, {:#x}, {:#x}) -> {:#x} [r1={:#x}]",
                     id, trace_args[0], trace_args[1], trace_args[2],
                     trace_args[3], static_cast<u32>(state.r[0]), state.r[1]);
    }
}

result_t Kernel::setHeapSize(Process* crnt_process, u64 size, uptr& out_base) {
    LOG_DEBUG(Kernel, "SetHeapSize called (size: 0x{:08x})", size);

    // Switchbrew SVC: a multiple of 0x200000 and at most 4GB (0xCA01)
    if ((size % HEAP_MEM_ALIGNMENT) != 0 || size > 0x100000000ull)
        return MAKE_RESULT(Svc, Error::InvalidSize);

    crnt_process->resizeHeap(size);

    out_base = HEAP_REGION.getBegin();
    return RESULT_SUCCESS;
}

result_t Kernel::setMemoryPermission(uptr addr, u64 size,
                                     MemoryPermission perm) {
    LOG_DEBUG(
        Kernel,
        "SetMemoryPermission called (addr: 0x{:08x}, size: 0x{:08x}, perm: "
        "{})",
        addr, size, perm);

    // TODO: implement
    LOG_FUNC_WITH_ARGS_STUBBED(
        Kernel, "addr: 0x{:08x}, size: 0x{:08x}, perm: {}", addr, size, perm);

    return RESULT_SUCCESS;
}

result_t Kernel::setMemoryAttribute(Process* crnt_process, vaddr_t addr,
                                    u64 size, MemoryAttribute mask,
                                    MemoryAttribute value) {
    LOG_DEBUG(
        Kernel,
        "SetMemoryAttribute called (addr: 0x{:08x}, size: 0x{:08x}, mask: "
        "{}, value: {})",
        addr, size, mask, value);

    crnt_process->getMmu()->setMemoryAttribute(
        ztd::Range<vaddr_t>::fromSize(addr, size), mask, value);

    return RESULT_SUCCESS;
}

result_t Kernel::mapMemory(Process* crnt_process, uptr dst_addr, uptr src_addr,
                           u64 size) {
    LOG_DEBUG(Kernel,
              "MapMemory called (dst_addr: 0x{:08x}, src_addr: 0x{:08x}, size: "
              "0x{:08x})",
              dst_addr, src_addr, size);

    crnt_process->getMmu()->map(dst_addr,
                                ztd::Range<vaddr_t>::fromSize(src_addr, size));

    return RESULT_SUCCESS;
}

result_t Kernel::unmapMemory(Process* crnt_process, uptr dst_addr,
                             uptr src_addr, u64 size) {
    LOG_DEBUG(
        Kernel,
        "UnmapMemory called (dst_addr: 0x{:08x}, src_addr: 0x{:08x}, size: "
        "0x{:08x})",
        dst_addr, src_addr, size);

    // TODO: verify that src_addr is the same as the one used in MapMemory?
    (void)src_addr;

    crnt_process->getMmu()->unmap(
        ztd::Range<vaddr_t>::fromSize(dst_addr, size));

    return RESULT_SUCCESS;
}

result_t Kernel::queryMemory(Process* crnt_process, uptr addr,
                             MemoryInfo& out_mem_info, u32& out_page_info) {
    LOG_DEBUG(Kernel, "QueryMemory called (addr: 0x{:08x})", addr);

    out_mem_info = crnt_process->getMmu()->queryMemory(addr);

    // TODO: what is this?
    out_page_info = 0;

    return RESULT_SUCCESS;
}

void Kernel::exitProcess(Process* crnt_process) {
    LOG_DEBUG(Kernel, "ExitProcess called");

    crnt_process->stop();
}

result_t Kernel::createThread(Process* crnt_process, vaddr_t entry_point,
                              vaddr_t args_addr, vaddr_t stack_top_addr,
                              i32 priority, i32 processor_id,
                              IThread*& out_thread) {
    LOG_DEBUG(Kernel,
              "CreateThread called (entry_point: 0x{:08x}, args_addr: "
              "0x{:08x}, stack_top_addr: 0x{:08x}, priority: {}, "
              "processor_id: {})",
              entry_point, args_addr, stack_top_addr, priority, processor_id);

    // Validated like Horizon: the core and priority must be allowed by the
    // process capabilities
    if (processor_id == IDEAL_CORE_USE_PROCESS_VALUE)
        processor_id = crnt_process->getIdealCore();
    if (processor_id < 0 || processor_id >= CPU_CORE_COUNT ||
        (crnt_process->getCoreMask() & (1ull << processor_id)) == 0) {
        LOG_WARN(Kernel, "CreateThread: core {} not in core mask {:#x}",
                 processor_id, crnt_process->getCoreMask());
        return MAKE_RESULT(Svc, Error::InvalidCoreId);
    }
    if (priority < HIGHEST_THREAD_PRIORITY ||
        priority > LOWEST_THREAD_PRIORITY ||
        !crnt_process->isValidThreadPriority(priority)) {
        LOG_WARN(Kernel, "CreateThread: priority {} outside {}-{}", priority,
                 crnt_process->getHighestThreadPriority(),
                 crnt_process->getLowestThreadPriority());
        return MAKE_RESULT(Svc, Error::InvalidPriority);
    }

    auto thread =
        new GuestThread(system, crnt_process, stack_top_addr, priority);
    thread->setCoreAffinity(processor_id, 1ull << processor_id);
    thread->setEntryPoint(entry_point);
    thread->setArg(0, args_addr);

    out_thread = thread;

    return RESULT_SUCCESS;
}

result_t Kernel::startThread(IThread* thread) {
    LOG_DEBUG(Kernel, "StartThread called (thread: {})",
              thread->getDebugName());

    thread->start();

    return RESULT_SUCCESS;
}

void Kernel::exitThread(IThread* crnt_thread) {
    LOG_DEBUG(Kernel, "ExitThread called");

    crnt_thread->stop();
}

void Kernel::sleepThread(IThread* crnt_thread, i64 nano) {
    LOG_DEBUG(Kernel, "SleepThread called (nano: {})", nano);

    // 0, -1 and -2 yield (to the same priority, without and with core
    // migration); the host scheduler decides for every core here
    if (nano == 0 || nano == -1 || nano == -2)
        std::this_thread::yield();
    else if (nano > 0)
        crnt_thread->sleep(nano);
}

result_t Kernel::getThreadPriority(IThread* thread, i32& out_priority) {
    if (thread == nullptr)
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    LOG_DEBUG(Kernel, "GetThreadPriority called (thread: {})",
              thread->getDebugName());

    out_priority = thread->getPriority();
    return RESULT_SUCCESS;
}

result_t Kernel::setThreadPriority(IThread* thread, i32 priority) {
    if (thread == nullptr)
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    LOG_DEBUG(Kernel, "SetThreadPriority called (thread: {}, priority: 0x{:x})",
              thread->getDebugName(), priority);

    if (priority < HIGHEST_THREAD_PRIORITY ||
        priority > LOWEST_THREAD_PRIORITY)
        return MAKE_RESULT(Svc, Error::InvalidPriority);
    if (auto process = thread->getProcess();
        process && !process->isValidThreadPriority(priority))
        return MAKE_RESULT(Svc, Error::InvalidPriority);

    thread->setPriority(priority);
    return RESULT_SUCCESS;
}

result_t Kernel::getThreadCoreMask(IThread* thread, i32& out_core_mask0,
                                   u64& out_core_mask1) {
    if (thread == nullptr)
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    LOG_DEBUG(Kernel, "GetThreadCoreMask called (thread: {})",
              thread->getDebugName());

    out_core_mask0 = thread->getIdealCore();
    out_core_mask1 = thread->getAffinityMask();
    return RESULT_SUCCESS;
}

result_t Kernel::setThreadCoreMask(IThread* thread, i32 core_mask0,
                                   u64 core_mask1) {
    if (thread == nullptr)
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    LOG_DEBUG(Kernel,
              "SetThreadCoreMask called (thread: {}, core mask 0: "
              "{:#x}, core mask 1: {:#x})",
              thread->getDebugName(), core_mask0, core_mask1);

    // Validation follows Atmosphere's mesosphere (svc_thread.cpp,
    // kern_k_thread.cpp)
    auto ideal_core = core_mask0;
    auto affinity_mask = core_mask1;
    const auto process = thread->getProcess();
    const u64 process_mask = process ? process->getCoreMask() : 0xf;
    const auto valid_core = [](i32 core) {
        return core >= 0 && core < CPU_CORE_COUNT;
    };
    if (ideal_core == IDEAL_CORE_USE_PROCESS_VALUE) {
        ideal_core = process ? process->getIdealCore() : 0;
        affinity_mask = 1ull << ideal_core;
    } else {
        if ((affinity_mask | process_mask) != process_mask)
            return MAKE_RESULT(Svc, Error::InvalidCoreId);
        if (affinity_mask == 0)
            return MAKE_RESULT(Svc, Error::InvalidCombination);
        if (valid_core(ideal_core)) {
            if ((affinity_mask & (1ull << ideal_core)) == 0)
                return MAKE_RESULT(Svc, Error::InvalidCombination);
        } else if (ideal_core != IDEAL_CORE_NO_UPDATE &&
                   ideal_core != IDEAL_CORE_DONT_CARE) {
            return MAKE_RESULT(Svc, Error::InvalidCoreId);
        }
    }
    if (ideal_core == IDEAL_CORE_NO_UPDATE) {
        ideal_core = thread->getIdealCore();
        if (ideal_core != IDEAL_CORE_DONT_CARE &&
            (affinity_mask & (1ull << ideal_core)) == 0)
            return MAKE_RESULT(Svc, Error::InvalidCombination);
    }

    thread->setCoreAffinity(ideal_core, affinity_mask);
    return RESULT_SUCCESS;
}

void Kernel::getCurrentProcessorNumber(IThread* crnt_thread, u32& out_number) {
    LOG_DEBUG(Kernel, "GetCurrentProcessorNumber called");

    // Hydra does not pin guest threads to cores: the thread reports the core
    // the guest put it on, so per-core guest data stays consistent
    out_number = crnt_thread ? crnt_thread->getCurrentCore() : 0;
}

result_t Kernel::signalEvent(Event* event) {
    LOG_DEBUG(Kernel, "SignalEvent called (event: {})", event->getDebugName());

    event->signal();
    return RESULT_SUCCESS;
}

result_t Kernel::clearEvent(Event* event) {
    LOG_DEBUG(Kernel, "ClearEvent called (event: {})", event->getDebugName());

    event->clear();
    return RESULT_SUCCESS;
}

result_t Kernel::mapSharedMemory(Process* crnt_process, SharedMemory* shmem,
                                 uptr addr, u64 size, MemoryPermission perm) {
    if (!crnt_process || !shmem)
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    LOG_DEBUG(Kernel,
              "MapSharedMemory called (handle: {}, addr: 0x{:08x}, size: "
              "0x{:08x}, perm: {})",
              shmem->getDebugName(), addr, size, perm);

    return crnt_process->mapSharedMemory(shmem, addr, size, perm);
}

result_t Kernel::unmapSharedMemory(Process* crnt_process, SharedMemory* shmem,
                                   uptr addr, u64 size) {
    if (!crnt_process || !shmem)
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    LOG_DEBUG(Kernel,
              "UnmapSharedMemory called (shmem: {}, addr: 0x{:08x}, size: "
              "0x{:08x})",
              shmem->getDebugName(), addr, size);

    return crnt_process->unmapSharedMemory(shmem, addr, size);
}

result_t Kernel::createTransferMemory(uptr addr, u64 size,
                                      MemoryPermission perm,
                                      TransferMemory*& out_tmem) {
    LOG_DEBUG(Kernel,
              "CreateTransferMemory called (address: 0x{:08x}, size: 0x{:08x}, "
              "perm: {})",
              addr, size, perm);

    out_tmem = new TransferMemory(addr, size, perm);

    return RESULT_SUCCESS;
}

result_t Kernel::closeHandle(Process* crnt_process, Handle handle) {
    LOG_DEBUG(Kernel, "CloseHandle called (handle: {})", handle);

    if (crnt_process->freeHandle(handle)) {
        return RESULT_SUCCESS;
    } else {
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    }
}

// TODO: can only be ReadableEvent or Process?
result_t Kernel::resetSignal(SynchronizationObject* sync_obj) {
    if (sync_obj == nullptr) {
        LOG_WARN(Kernel, "ResetSignal called (INVALID_HANDLE)");
        // HACK
        return RESULT_SUCCESS; // MAKE_RESULT(Svc, Error::InvalidHandle);
    }

    LOG_DEBUG(Kernel, "ResetSignal called (sync_obj: {})",
              sync_obj->getDebugName());

    if (!sync_obj->clear())
        return MAKE_RESULT(Svc, Error::InvalidState);

    return RESULT_SUCCESS;
}

result_t
Kernel::waitSynchronization(IThread* crnt_thread,
                            std::span<SynchronizationObject*> sync_objs,
                            i64 timeout, u32& out_signalled_index) {
    LOG_DEBUG(Kernel,
              "WaitSynchronization called (count: {}, timeout: "
              "{})",
              sync_objs.size(), timeout);

    for (auto& sync_obj : sync_objs) {
        if (sync_obj == nullptr) {
            LOG_WARN(Kernel, "Invalid sync object");
            // HACK: Celeste gets stuck in an infinite WaitSynchronization
            // loop if an error is returned
            return RESULT_SUCCESS; // MAKE_RESULT(Svc,
                                   // Error::InvalidHandle);
        }
    }

    crnt_thread->pause();
    crnt_thread->beginSyncWait();

    // Add waiting thread
    for (auto sync_obj : sync_objs) {
        // LOG_DEBUG(Kernel, "Synchronizing with {}",
        //           sync_obj->GetDebugName());

        sync_obj->addWaitingThread(crnt_thread);
    }

    SynchronizationObject* signalled_obj = nullptr;
    result_t res = RESULT_SUCCESS;
    if (crnt_thread->processMessages(timeout)) {
        if (crnt_thread->getState() == ThreadState::Stopping) {
            // TODO: is this necessary?
            res = MAKE_RESULT(Svc, Error::Cancelled);
        } else {
            if (!crnt_thread->consumeSignalledObject(signalled_obj))
                res = MAKE_RESULT(Svc, Error::Cancelled);
        }
    } else {
        res = MAKE_RESULT(Svc, Error::TimedOut);
    }

    crnt_thread->endSyncWait();

    // Remove the thread from the waiting list
    for (auto sync_obj : sync_objs) {
        if (sync_obj != signalled_obj)
            sync_obj->removeWaitingThread(crnt_thread);
    }

    // Find the handle index
    out_signalled_index = 0;
    if (signalled_obj != nullptr) {
        for (u32 i = 0; i < sync_objs.size(); i++) {
            if (sync_objs[i] == signalled_obj) {
                out_signalled_index = i;
                break;
            }
        }
    }

    return res;
}

result_t Kernel::cancelSynchronization(IThread* thread) {
    LOG_DEBUG(Kernel, "CancelSynchronization called (thread: {})",
              thread->getDebugName());

    thread->cancelSync();

    return RESULT_SUCCESS;
}

result_t Kernel::arbitrateLock(IThread* crnt_thread, IThread* owner_thread,
                               uptr mutex_addr, Handle self_handle,
                               Handle owner_handle) {
    LOG_DEBUG(Kernel,
              "ArbitrateLock called (owner: {}, mutex: {:#x}, self: "
              "{})",
              owner_thread->getDebugName(), mutex_addr, self_handle);

    crnt_thread->self_handle_for_mutex = self_handle;
    owner_thread->self_handle_for_mutex = owner_handle;

    {
        CriticalSectionLock cs_lock(*this);

        if (atomicLoad(reinterpret_cast<u32*>(mutex_addr)) !=
            (owner_thread->self_handle_for_mutex.getRaw() | MUTEX_WAIT_MASK))
            return RESULT_SUCCESS;

        crnt_thread->mutex_wait_addr = mutex_addr;

        crnt_thread->pause();
        owner_thread->addMutexWaiter(crnt_thread);
    }

    ASSERT_DEBUG(crnt_thread->processMessages(), Kernel,
                 "ArbitrateLock timed out");

    result_t res = RESULT_SUCCESS;
    if (crnt_thread->getState() == ThreadState::Stopping) {
        // TODO: is this necessary?
        res = MAKE_RESULT(Svc, Error::Cancelled);
    } else {
        if (!crnt_thread->wasSignalled())
            res = MAKE_RESULT(Svc, Error::Cancelled);
    }

    return res;
}

result_t Kernel::arbitrateUnlock(IThread* crnt_thread, uptr mutex_addr) {
    LOG_DEBUG(Kernel, "ArbitrateUnlock called (mutex: 0x{:08x})", mutex_addr);

    {
        CriticalSectionLock cs_lock(*this);
        unlockMutex(crnt_thread, mutex_addr);
    }

    return RESULT_SUCCESS;
}

result_t Kernel::waitProcessWideKeyAtomic(Process* crnt_process,
                                          IThread* crnt_thread, uptr mutex_addr,
                                          uptr var_addr, Handle self_handle,
                                          i64 timeout) {
    LOG_DEBUG(Kernel,
              "WaitProcessWideKeyAtomic called (mutex: {:#x}, var: {:#x}, "
              "self: {}, timeout: {})",
              mutex_addr, var_addr, self_handle, timeout);

    crnt_thread->self_handle_for_mutex = self_handle;
    crnt_thread->mutex_wait_addr = mutex_addr;
    crnt_thread->cond_var_wait_addr = var_addr;

    crnt_thread->pause();

    {
        CriticalSectionLock cs_lock(*this);
        ASSERT_DEBUG(cond_var_waiters.addLast(crnt_thread).has_value(), Kernel,
                     "Failed to add cond var waiter");
        // Horizon publishes this flag before releasing the user's mutex.
        // SDK condition-variable signals skip the SVC when the flag is zero.
        atomicStore(reinterpret_cast<u32*>(var_addr), 1u);
        unlockMutex(crnt_thread, mutex_addr);
    }

    result_t res = RESULT_SUCCESS;
    if (crnt_thread->processMessages(timeout)) {
        if (crnt_thread->getState() == ThreadState::Stopping) {
            // TODO: is this necessary?
            res = MAKE_RESULT(Svc, Error::Cancelled);
        } else {
            if (!crnt_thread->wasSignalled())
                res = MAKE_RESULT(Svc, Error::Cancelled);
        }
    } else {
        // A signal may have raced with the timeout. If it already took this
        // thread off the condition variable, the mutex is (or will be) handed
        // to it, so this is a regular wakeup. Returning TimedOut would make
        // userland lock a mutex it already owns and deadlock.
        bool signalled;
        {
            CriticalSectionLock cs_lock(*this);
            signalled = crnt_thread->cond_var_wait_addr == 0x0;
            if (signalled && crnt_thread->mutex_wait_addr != 0x0) {
                // Still queued on the owner; pause before releasing the lock
                // so the owner's resume cannot be lost
                crnt_thread->pause();
            }
        }

        if (signalled) {
            // Consume the resume from the mutex hand-off
            crnt_thread->processMessages();
            if (crnt_thread->getState() == ThreadState::Stopping)
                res = MAKE_RESULT(Svc, Error::Cancelled);
        } else {
            res = MAKE_RESULT(Svc, Error::TimedOut);
        }
    }

    // Remove this thread from the wait list
    {
        CriticalSectionLock cs_lock(*this);

        // Cond var
        cond_var_waiters.remove(crnt_thread);

        // Mutex
        auto owner = getMutexOwner(
            crnt_process, reinterpret_cast<u32*>(crnt_thread->mutex_wait_addr));
        if (owner.has_value())
            owner.value()->removeMutexWaiter(crnt_thread);
    }

    return res;
}

result_t Kernel::signalProcessWideKey(Process* crnt_process, uptr addr,
                                      i32 count, u64* out_woken) {
    LOG_DEBUG(Kernel, "SignalProcessWideKey called (addr: 0x{:08x}, count: {})",
              addr, count);

    CriticalSectionLock cs_lock(*this);

    // A non-positive count wakes every waiter
    if (count <= 0)
        count = static_cast<i32>(cond_var_waiters.getSize());

    // TODO: sort by priority
    for (auto thread_node = cond_var_waiters.getHead();
         thread_node.has_value() && count > 0;) {
        const auto thread_node_ = thread_node.value();
        const auto thread = thread_node_->get();
        if (thread->cond_var_wait_addr == addr) {
            thread->cond_var_wait_addr = 0x0;
            tryAcquireMutex(crnt_process, thread);
            thread_node = cond_var_waiters.remove(thread_node_);
            count--;
            if (out_woken)
                (*out_woken)++;
        } else {
            thread_node = thread_node_->getNext();
        }
    }

    bool has_waiter = false;
    for (auto node = cond_var_waiters.getHead(); node; node = (*node)->getNext())
        if ((*node)->get()->cond_var_wait_addr == addr) {
            has_waiter = true;
            break;
        }
    if (!has_waiter)
        atomicStore(reinterpret_cast<u32*>(addr), 0u);
    return RESULT_SUCCESS;
}

void Kernel::getSystemTick(u64& out_tick) {
    LOG_DEBUG(Kernel, "GetSystemTick called");

    out_tick = system.getWallClock().getCntpct(); // TODO: correct?
}

result_t Kernel::connectToNamedPort(const std::string_view name,
                                    hipc::ClientSession*& out_client_session) {
    LOG_DEBUG(Kernel, "ConnectToNamedPort called (name: {})", name);

    auto port = service_manager.getPort(std::string(name));
    if (port == nullptr) {
        LOG_ERROR(Kernel, "Failed to connect to port \"{}\"", name);
        return MAKE_RESULT(Svc, Error::NotFound);
    }

    out_client_session = port->connect();

    return RESULT_SUCCESS;
}

result_t Kernel::sendSyncRequest(Process* crnt_process, IThread* crnt_thread,
                                 hipc::ClientSession* client_session) {
    if (client_session == nullptr) {
        LOG_WARN(Kernel, "SendSyncRequest called (INVALID_HANDLE)");
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    }

    LOG_DEBUG(Kernel, "SendSyncRequest called (session: {})",
              client_session->getDebugName());

    // Pause the thread
    crnt_thread->pause();

    // Send request
    client_session->getParent()->getServerSide()->enqueueRequest(
        crnt_process, crnt_thread, crnt_thread->getTlsPtr());

    // Wait for response
    ASSERT_DEBUG(crnt_thread->processMessages(), Kernel,
                 "SendSyncRequest timed out");

    result_t res = RESULT_SUCCESS;
    if (crnt_thread->getState() == ThreadState::Stopping) {
        // TODO: is this necessary?
        res = MAKE_RESULT(Svc, Error::Cancelled);
    } else {
        if (!crnt_thread->wasSignalled())
            res = MAKE_RESULT(Svc, Error::Cancelled);
    }

    return res;
}

result_t Kernel::getProcessId(AutoObject* obj, u64& out_process_id) {
    // A thread handle gives the ID of its process
    Process* process = nullptr;
    if (obj != nullptr) {
        if (auto thread = obj->as<IThread>())
            process = thread->getProcess();
        else
            process = obj->as<Process>();
    }
    if (process == nullptr)
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    LOG_DEBUG(Kernel, "GetProcessId called (process: {})",
              process->getDebugName());

    out_process_id = process->getProcessId();
    return RESULT_SUCCESS;
}

result_t Kernel::getThreadId(IThread* thread, u64& out_thread_id) {
    if (thread == nullptr)
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    LOG_DEBUG(Kernel, "GetThreadId called (thread: {})",
              thread->getDebugName());

    out_thread_id = thread->getThreadId();
    return RESULT_SUCCESS;
}

result_t Kernel::break_(BreakReason reason, uptr buffer_ptr, u64 buffer_size) {
    LOG_DEBUG(Kernel,
              "Break called (reason: {}, buffer ptr: 0x{:08x}, buffer "
              "size: 0x{:08x})",
              reason.type, buffer_ptr, buffer_size);

    // TODO: this should be sent to the debugger instead of being logged
    if (buffer_ptr != 0u) {
        if (buffer_size == sizeof(u32)) {
            const u32 result = *reinterpret_cast<u32*>(buffer_ptr);
            const auto module = GET_RESULT_MODULE(result);
            const auto description = GET_RESULT_DESCRIPTION(result);
            LOG_INFO(Kernel, "Module: {}, description: {}", module,
                     description);
        } else {
            constexpr u32 MAX_DATA_COUNT = 16;
            const auto data_count = static_cast<u32>(buffer_size / sizeof(u32));
            for (u32 i = 0; i < std::min(data_count, MAX_DATA_COUNT); i++) {
                const u32 value = reinterpret_cast<u32*>(buffer_ptr)[i];
                LOG_INFO(Kernel, "0x{:08x}", value);
            }
            if (data_count > MAX_DATA_COUNT)
                LOG_INFO(Kernel, "...");
        }
    }

    if (!reason.notification_only)
        GET_CURRENT_PROCESS_DEBUGGER().breakOnThisThread("Break");

    return RESULT_SUCCESS;
}

result_t Kernel::outputDebugString(const std::string_view str, u64 len) {
    LOG_DEBUG(Kernel, "OutputDebugString called");
    if (len != 0) {
        // TODO: handle differently
        LOG_INFO(Kernel, "{}", str);
    }

    return RESULT_SUCCESS;
}

// TODO: object
result_t Kernel::getInfo(Process* crnt_process, InfoType info_type,
                         AutoObject* obj, u64 info_sub_type, u64& out_info) {
    (void)obj;

    LOG_DEBUG(Kernel, "GetInfo called (type: {}, object: {}, subtype: {})",
              info_type, (obj != nullptr ? obj->getDebugName() : "null"),
              info_sub_type);

    switch (info_type) {
    case InfoType::CoreMask:
        // From the process capabilities, cores 0-2 for most games
        out_info = crnt_process->getCoreMask();
        return RESULT_SUCCESS;
    case InfoType::PriorityMask:
        out_info = crnt_process->getPriorityMask();
        return RESULT_SUCCESS;
    case InfoType::AliasRegionAddress:
        out_info = ALIAS_REGION.getBegin();
        return RESULT_SUCCESS;
    case InfoType::AliasRegionSize:
        out_info = ALIAS_REGION.getSize();
        return RESULT_SUCCESS;
    case InfoType::HeapRegionAddress:
        out_info = HEAP_REGION.getBegin();
        return RESULT_SUCCESS;
    case InfoType::HeapRegionSize:
        out_info = HEAP_REGION.getSize();
        return RESULT_SUCCESS;
    case InfoType::TotalMemorySize:
        // TODO: what should this be?
        out_info = 3ull * 1024ull * 1024ull * 1024ull;
        return RESULT_SUCCESS;
    case InfoType::UsedMemorySize:
        out_info =
            crnt_process->getUsedUserMemorySize() +
            std::min<u64>(crnt_process->getSystemResourceSize(), 64ull * 1024);
        return RESULT_SUCCESS;
    case InfoType::DebuggerAttached:
        // Retail consoles report no debugger, so games take their normal
        // paths; with the GDB server one is attached
        out_info = static_cast<u64>(CONFIG_INSTANCE.getGdbEnabled());
        return RESULT_SUCCESS;
    case InfoType::RandomEntropy:
        ASSERT_DEBUG(info_sub_type < crnt_process->getRandomEntropy().size(),
                     Kernel, "Invalid random entropy index {}", info_sub_type);
        out_info = crnt_process->getRandomEntropy()[info_sub_type];
        return RESULT_SUCCESS;
    case InfoType::AslrRegionAddress:
        out_info = ADDRESS_SPACE.getBegin();
        return RESULT_SUCCESS;
    case InfoType::AslrRegionSize:
        out_info = ADDRESS_SPACE.getSize();
        return RESULT_SUCCESS;
    case InfoType::StackRegionAddress:
        out_info = STACK_REGION.getBegin();
        return RESULT_SUCCESS;
    case InfoType::StackRegionSize:
        out_info = STACK_REGION.getSize();
        return RESULT_SUCCESS;
    case InfoType::TotalSystemResourceSize: {
        out_info = crnt_process->getSystemResourceSize();
        LOG_DEBUG(Kernel, "Total system resource size: 0x{:x}", out_info);
        return RESULT_SUCCESS;
    }
    case InfoType::UsedSystemResourceSize:
        out_info =
            std::min<u64>(crnt_process->getSystemResourceSize(), 64ull * 1024);
        return RESULT_SUCCESS;
    case InfoType::ProgramId:
        out_info = crnt_process->getTitleId();
        return RESULT_SUCCESS;
    case InfoType::UserExceptionContextAddress:
        LOG_NOT_IMPLEMENTED(Kernel, "UserExceptionContextAddress");
        // HACK
        out_info = 0;
        return RESULT_SUCCESS;
    case InfoType::TotalNonSystemMemorySize:
        out_info = 3ull * 1024ull * 1024ull * 1024ull -
                   crnt_process->getSystemResourceSize();
        return RESULT_SUCCESS;
    case InfoType::UsedNonSystemMemorySize:
        out_info = crnt_process->getUsedUserMemorySize();
        return RESULT_SUCCESS;
    case InfoType::IsApplication:
        // TODO: don't always return true
        out_info = static_cast<u64>(true);
        return RESULT_SUCCESS;
    case InfoType::AliasRegionExtraSize:
        LOG_NOT_IMPLEMENTED(Kernel, "AliasRegionExtraSize");
        // HACK
        out_info = 0;
        return RESULT_SUCCESS;
    default:
        LOG_WARN(Kernel, "Unknown info type {}", info_type);
        return MAKE_RESULT(Svc, 0x78);
    }
}

result_t Kernel::mapPhysicalMemory(Process* crnt_process, vaddr_t addr,
                                   u64 size) {
    LOG_DEBUG(Kernel,
              "MapPhysicalMemory called (addr: 0x{:08x}, size: 0x{:08x})", addr,
              size);

    if (const auto res = checkPhysicalMemoryRange(addr, size);
        res != RESULT_SUCCESS)
        return res;

    return crnt_process->mapPhysicalMemory(addr, size);
}

result_t Kernel::unmapPhysicalMemory(Process* crnt_process, vaddr_t addr,
                                     u64 size) {
    LOG_DEBUG(Kernel,
              "UnmapPhysicalMemory called (addr: 0x{:08x}, size: 0x{:08x})",
              addr, size);

    if (const auto res = checkPhysicalMemoryRange(addr, size);
        res != RESULT_SUCCESS)
        return res;

    return crnt_process->unmapPhysicalMemory(addr, size);
}

result_t Kernel::checkPhysicalMemoryRange(vaddr_t addr, u64 size) {
    if (!isAligned(addr, hw::tegra_x1::cpu::GUEST_PAGE_SIZE))
        return MAKE_RESULT(Svc, Error::InvalidAddress);

    if (size == 0 || !isAligned(size, hw::tegra_x1::cpu::GUEST_PAGE_SIZE))
        return MAKE_RESULT(Svc, Error::InvalidSize);

    if (addr + size <= addr)
        return MAKE_RESULT(Svc, Error::InvalidMemoryRange);

    if (!ALIAS_REGION.contains(ztd::Range<vaddr_t>::fromSize(addr, size)))
        return MAKE_RESULT(Svc, Error::InvalidMemoryRange);

    return RESULT_SUCCESS;
}

result_t Kernel::setThreadActivity(IThread* crnt_thread, IThread* thread,
                                   ThreadActivity activity) {
    // Validation follows mesosphere (svc_thread.cpp, KThread::SetActivity)
    if (activity != ThreadActivity::Runnable &&
        activity != ThreadActivity::Paused)
        return MAKE_RESULT(Svc, Error::InvalidEnumValue);
    if (thread == nullptr || thread->getProcess() != crnt_thread->getProcess())
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    if (thread == crnt_thread)
        return MAKE_RESULT(Svc, Error::AlreadyExists); // Busy
    LOG_DEBUG(Kernel, "SetThreadActivity called (thread: {}, activity: {})",
              thread->getDebugName(), activity);

    if (activity == ThreadActivity::Paused) {
        if (thread->isSuspendRequested())
            return MAKE_RESULT(Svc, Error::InvalidState);
        thread->suspendActivity();
    } else {
        if (!thread->isSuspendRequested())
            return MAKE_RESULT(Svc, Error::InvalidState);
        thread->resumeActivity();
    }
    return RESULT_SUCCESS;
}

result_t Kernel::getThreadContext3(IThread* crnt_thread, IThread* thread,
                                   ThreadContext& out_thread_context) {
    if (thread == nullptr || thread->getProcess() != crnt_thread->getProcess())
        return MAKE_RESULT(Svc, Error::InvalidHandle);
    if (thread == crnt_thread)
        return MAKE_RESULT(Svc, Error::AlreadyExists); // Busy
    LOG_DEBUG(Kernel, "GetThreadContext3 called (thread: {})",
              thread->getDebugName());

    if (!thread->isSuspendRequested())
        return MAKE_RESULT(Svc, Error::InvalidState);
    // Like Horizon, wait until the thread actually stopped. A running thread
    // stops at its next VM exit, within one timer interrupt.
    constexpr auto STOP_TIMEOUT = std::chrono::seconds(2);
    if (!thread->waitForSuspendedContext(out_thread_context, STOP_TIMEOUT)) {
        LOG_WARN(Kernel, "GetThreadContext3: {} did not stop",
                 thread->getDebugName());
        return MAKE_RESULT(Svc, Error::InvalidState);
    }
    return RESULT_SUCCESS;
}

result_t Kernel::waitForAddress(IThread* crnt_thread, uptr addr,
                                ArbitrationType arbitration_type, u32 value,
                                i64 timeout) {
    LOG_DEBUG(Kernel,
              "WaitForAddress called (addr: 0x{:08x}, type: {}, value: "
              "0x{:x}, timeout: 0x{:08x})",
              addr, arbitration_type, value, timeout);

    bool wait = false;
    {
        CriticalSectionLock cs_lock(*this);

        auto value_ptr = reinterpret_cast<u32*>(addr);
        u32 current_value;
        switch (arbitration_type) {
        case ArbitrationType::WaitIfLessThan:
            current_value = atomicLoad(value_ptr);
            wait = (static_cast<i32>(current_value) < static_cast<i32>(value));
            break;
        case ArbitrationType::DecrementAndWaitIfLessThan:
            current_value = atomicLoad(value_ptr);
            while (static_cast<i32>(current_value) < static_cast<i32>(value)) {
                if (atomicCompareExchangeWeak(value_ptr, current_value,
                                              current_value - 1u)) {
                    wait = true;
                    break;
                }
            }
            break;
        case ArbitrationType::WaitIfEqual:
            current_value = atomicLoad(value_ptr);
            wait = (current_value == value);
            break;
        default:
            return MAKE_RESULT(Svc, Error::InvalidEnumValue);
        }

        if (wait) {
            crnt_thread->pause();

            crnt_thread->mutex_wait_addr = addr;
            ASSERT_DEBUG(arbiters.addLast(crnt_thread).has_value(), Kernel,
                         "Failed to add arbiter");
        }
    }

    if (wait) {
        result_t res = RESULT_SUCCESS;
        if (crnt_thread->processMessages(timeout)) {
            if (crnt_thread->getState() == ThreadState::Stopping) {
                // TODO: is this necessary?
                res = MAKE_RESULT(Svc, Error::Cancelled);
            } else {
                if (!crnt_thread->wasSignalled())
                    res = MAKE_RESULT(Svc, Error::Cancelled);
            }
        } else {
            res = MAKE_RESULT(Svc, Error::TimedOut);
        }

        // Signalled waiters are removed by signalToAddress. Timed out or
        // cancelled waiters must also leave the list before returning.
        {
            CriticalSectionLock cs_lock(*this);
            arbiters.remove(crnt_thread);
        }

        return res;
    }

    return MAKE_RESULT(Svc, Error::InvalidState);
}

result_t Kernel::signalToAddress(uptr addr, SignalType signal_type, u32 value,
                                 u32 count) {
    LOG_DEBUG(Kernel,
              "SignalToAddress called (addr: 0x{:08x}, "
              "signal_type: {}, value: 0x{:08x}, count: {})",
              addr, signal_type, value, count);

    CriticalSectionLock cs_lock(*this);
    usize waiting_count = 0;
    for (auto node = arbiters.getHead(); node; node = (*node)->getNext()) {
        if ((*node)->get()->mutex_wait_addr == addr)
            ++waiting_count;
    }
    const auto signed_count = static_cast<i32>(count);
    if (signal_type != SignalType::Signal) {
        u32 updated_value;
        switch (signal_type) {
        case SignalType::SignalAndIncrementIfEqual:
            updated_value = value + 1u;
            break;
        case SignalType::SignalAndModifyBasedOnWaitingThreadCountIfEqual:
            updated_value =
                waiting_count == 0 ? value + 1u
                : (signed_count <= 0 ||
                   waiting_count <= static_cast<usize>(signed_count))
                    ? value - 1u
                    : value;
            break;
        default:
            return MAKE_RESULT(Svc, Error::InvalidEnumValue);
        }
        auto expected = value;
        if (!atomicCompareExchangeStrong(reinterpret_cast<u32*>(addr), expected,
                                         updated_value))
            return MAKE_RESULT(Svc, Error::InvalidState);
    }
    usize signalled_count = 0;
    for (auto waiter_node = arbiters.getHead(); waiter_node.has_value();) {
        if (signed_count > 0 &&
            signalled_count >= static_cast<usize>(signed_count))
            break;
        const auto waiter_node_ = waiter_node.value();
        auto waiter = waiter_node_->get();
        if (waiter->mutex_wait_addr != addr) {
            waiter_node = waiter_node_->getNext();
            continue;
        }

        waiter->resume();
        waiter_node = arbiters.remove(waiter_node_);
        ++signalled_count;
    }

    return RESULT_SUCCESS;
}

void Kernel::synchronizePreemptionState(IThread* crnt_thread) {
    (void)crnt_thread;

    LOG_FUNC_STUBBED(Kernel);
}

result_t Kernel::createSession(bool is_light, u64 name,
                               hipc::ServerSession*& out_server_session,
                               hipc::ClientSession*& out_client_session) {
    LOG_DEBUG(Kernel, "CreateSession called (is_light: {}, name: 0x{:08x})",
              is_light, name);

    // TODO: what are light sessions?
    (void)is_light;

    // TODO: what's the purpose of the name?
    (void)name;

    out_server_session = new hipc::ServerSession();
    out_client_session = new hipc::ClientSession();
    // TODO: is it fine to just instantiate it like this?
    new hipc::Session(out_server_session, out_client_session);

    return RESULT_SUCCESS;
}

result_t Kernel::acceptSession(hipc::ServerPort* server_port,
                               hipc::ServerSession*& out_server_session) {
    LOG_DEBUG(Kernel, "AcceptSession called (port: {})",
              server_port->getDebugName());

    out_server_session = server_port->acceptSession();

    return RESULT_SUCCESS;
}

result_t Kernel::replyAndReceive(IThread* crnt_thread,
                                 std::span<SynchronizationObject*> sync_objs,
                                 hipc::ServerSession* reply_target_session,
                                 i64 timeout, u32& out_signalled_index) {
    LOG_DEBUG(Kernel, "ReplyAndReceive called (count: {}, timeout: {})",
              sync_objs.size(), timeout);

    if (reply_target_session != nullptr) {
        // Reply
        reply_target_session->reply(crnt_thread->getTlsPtr());
    }

    // Wait
    const auto res = waitSynchronization(crnt_thread, sync_objs, timeout,
                                         out_signalled_index);
    if (res != RESULT_SUCCESS)
        return res;

    auto sync_obj = sync_objs[static_cast<usize>(out_signalled_index)];
    if (sync_obj->isOfType<hipc::ServerSession>()) {
        auto server_session = static_cast<hipc::ServerSession*>(sync_obj);
        if (server_session->isClientOpen()) {
            // Receive
            server_session->receive(crnt_thread);
            return RESULT_SUCCESS;
        } else {
            return MAKE_RESULT(Svc, 123); // SessionClosed
        }
    } else {
        return RESULT_SUCCESS;
    }
}

result_t Kernel::createCodeMemory(vaddr_t addr, u64 size,
                                  CodeMemory*& out_code_memory) {
    LOG_DEBUG(Kernel, "CreateCodeMemory called (addr: 0x{:08x}, size: {})",
              addr, size);

    out_code_memory = new CodeMemory(addr, size);

    return RESULT_SUCCESS;
}

result_t Kernel::controlCodeMemory(CodeMemory* code_memory,
                                   CodeMemoryOperation op, vaddr_t addr,
                                   u64 size, MemoryPermission perm) {
    LOG_DEBUG(Kernel,
              "ControlCodeMemory called (code memory: {}, op: {}, addr: "
              "0x{:08x}, size: {}, perm: {})",
              code_memory->getDebugName(), op, addr, size, perm);

    // TODO: implement
    LOG_FUNC_WITH_ARGS_STUBBED(
        Kernel,
        "code memory: {}, op: {}, addr: 0x{:08x}, size: 0x{:08x}, perm: {}",
        code_memory->getDebugName(), op, addr, size, perm);

    return RESULT_SUCCESS;
}

result_t Kernel::getProcessList(u64* process_id_buffer,
                                u32 process_id_buffer_size, u32& out_count) {
    LOG_DEBUG(Kernel,
              "GetProcessList called (process ID buffer: {}, "
              "process ID buffer size: {})",
              reinterpret_cast<void*>(process_id_buffer),
              process_id_buffer_size);

    // The size is the maximum number of IDs (svcGetProcessList's
    // max_out_count), not bytes
    out_count = 0;
    for (auto it = process_manager.begin();
         it != process_manager.end() && out_count < process_id_buffer_size;
         it++)
        process_id_buffer[out_count++] = (*it)->getProcessId();

    return RESULT_SUCCESS;
}

result_t Kernel::setProcessMemoryPermission(Process* process, vaddr_t addr,
                                            u64 size, MemoryPermission perm) {
    LOG_DEBUG(Kernel,
              "SetProcessMemoryPermission called (process: {}, addr: 0x{:08x}, "
              "size: {}, perm: {})",
              process->getDebugName(), addr, size, perm);

    // TODO: implement
    (void)process;
    LOG_FUNC_WITH_ARGS_STUBBED(
        Kernel, "addr: 0x{:08x}, size: 0x{:08x}, perm: {}", addr, size, perm);

    return RESULT_SUCCESS;
}

result_t Kernel::mapProcessMemory(Process* crnt_process, vaddr_t dst_addr,
                                  Process* process, vaddr_t src_addr,
                                  u64 size) {
    LOG_DEBUG(Kernel,
              "MapProcessMemory called (crnt_process: {}, dst_addr: 0x{:08x}, "
              "process: {}, src_addr: 0x{:08x}, size: {})",
              crnt_process->getDebugName(), dst_addr, process->getDebugName(),
              src_addr, size);

    // TODO: correct?
    const auto ptr = process->getMmu()->unmapAddr(src_addr);
    crnt_process->getMmu()->map(dst_addr, ztd::Range<uptr>::fromSize(ptr, size),
                                {}); // TODO: state

    return RESULT_SUCCESS;
}

result_t Kernel::mapProcessCodeMemory(Process* process, vaddr_t dst_addr,
                                      vaddr_t src_addr, u64 size) {
    LOG_DEBUG(Kernel,
              "MapProcessCodeMemory called (process: {}, dst_addr: 0x{:08x}, "
              "src_addr: 0x{:08x}, size: {})",
              process->getDebugName(), dst_addr, src_addr, size);

    process->getMmu()->map(dst_addr,
                           ztd::Range<vaddr_t>::fromSize(src_addr, size));

    return RESULT_SUCCESS;
}

result_t Kernel::unmapProcessCodeMemory(Process* process, vaddr_t dst_addr,
                                        vaddr_t src_addr, u64 size) {
    LOG_DEBUG(Kernel,
              "UnmapProcessCodeMemory called (process: {}, dst_addr: 0x{:08x}, "
              "src_addr: 0x{:08x}, size: {})",
              process->getDebugName(), dst_addr, src_addr, size);

    // TODO: verify that src_addr is the same as the one used in MapMemory?
    (void)src_addr;

    process->getMmu()->unmap(ztd::Range<vaddr_t>::fromSize(dst_addr, size));

    return RESULT_SUCCESS;
}

void Kernel::tryAcquireMutex(Process* crnt_process, IThread* thread) {
    auto mutex = reinterpret_cast<u32*>(thread->mutex_wait_addr);

    u32 value = *mutex;
    u32 new_value;
    do {
        if (value == 0) {
            // Register this thread as the owner
            new_value = thread->self_handle_for_mutex.getRaw();
        } else {
            // Register this thread as a waiter
            new_value = value | MUTEX_WAIT_MASK;
        }
    } while (!atomicCompareExchangeWeak(mutex, value, new_value));

    if (value == 0) {
        // Mutex acquired
        thread->mutex_wait_addr = 0x0;
        thread->resume();
        return;
    }

    // Register this thread as a waiter by the owner
    auto owner = getMutexOwner(crnt_process, value).value();
    owner->addMutexWaiter(thread);
}

void Kernel::unlockMutex(IThread* thread, uptr mutex_addr) {
    auto mutex = reinterpret_cast<u32*>(mutex_addr);

    u32 waiter_count;
    auto new_owner = thread->relinquishMutex(mutex_addr, waiter_count);
    if (new_owner == nullptr) {
        atomicStore(mutex, 0u);
        return;
    }

    u32 value = new_owner->self_handle_for_mutex.getRaw();
    if (waiter_count > 0)
        value |= MUTEX_WAIT_MASK;

    atomicStore(mutex, value);

    // Resume the owner
    new_owner->resume();
}

} // namespace hydra::horizon::kernel
