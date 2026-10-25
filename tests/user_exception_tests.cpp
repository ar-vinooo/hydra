// Guest exceptions enter the process at its entry point, as on Horizon: an
// undefined instruction (EL1 vector path), a BRK (host exit path), a write to
// a write tracked page made read-only by SetMemoryPermission and a read of a
// page without permissions each run the handler, which skips the faulting
// instruction and returns through ReturnFromException with the context,
// including NZCV, restored. Also checks the resource limit, event and user
// buffer IPC kernel objects.
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "core/horizon/kernel/guest_thread.hpp"
#include "core/horizon/kernel/hipc/server_session.hpp"
#include "core/horizon/kernel/kernel.hpp"
#include "core/horizon/kernel/process.hpp"
#include "core/horizon/kernel/process_manager.hpp"
#include "core/horizon/os.hpp"
#include "core/hw/tegra_x1/cpu/cpu.hpp"
#include "core/hw/tegra_x1/cpu/memory.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/cpu/thread.hpp"
#include "core/system.hpp"

using namespace hydra;
using namespace hydra::horizon;

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

constexpr u64 PAGE = 0x4000;
constexpr u64 HANDLER_OFFSET = 0x100;

// main:
//   movz x5, #0x1234; mov x13, #0; cmp xzr, xzr (NZCV 0110); udf #0
//   mrs x6, nzcv; cmn xzr, xzr (NZCV 0100); brk #0; mrs x16, nzcv
//   str x5, [x19] (read-only); ldr x7, [x20] (no access); svc #0x7f; b .
// handler (x0 = type, x1 = info):
//   [x21/x22/x23 + x13 * 8] = type/ESR/FAR, x11 = sp, x12 = x1, x13++,
//   info.pc += 4, x5 = 0, clobber NZCV; mov x0, #0; svc #0x28
constexpr u32 MAIN[] = {0xd2824685, 0xd280000d, 0xeb1f03ff, 0x00000000,
                        0xd53b4206, 0xab1f03ff, 0xd4200000, 0xd53b4210,
                        0xf9000265, 0xf9400287, 0xd4000fe1, 0x14000000};
constexpr u32 HANDLER[] = {0xb9406c2a, 0xf9403822, 0xf82d7aa0, 0xf82d7aca,
                           0xf82d7ae2, 0x910003eb, 0xaa0103ec, 0x910005ad,
                           0xf9402c22, 0x91001042, 0xf9002c22, 0xd2800005,
                           0xd2800003, 0xf100047f, 0xd2800000, 0xd4000501,
                           0x14000000};
constexpr u32 EXCEPTION_COUNT = 4;

void checkKernelObjects(System& system, kernel::Process* process) {
    auto& kernel = system.getOs().getKernel();
    const auto svc = [](kernel::Error error) {
        return MAKE_RESULT(Svc, error);
    };

    // The resource limit GetInfo hands out
    u64 handle = 0;
    check(kernel.getInfo(process, kernel::InfoType::ResourceLimit, nullptr, 0,
                         handle) == RESULT_SUCCESS,
          "GetInfo returns the resource limit");
    auto* limit =
        process->getHandle<kernel::ResourceLimit>(static_cast<u32>(handle));
    check(limit != nullptr, "The handle is a resource limit");
    i64 value = 0;
    check(kernel.getResourceLimitValue(
              limit, kernel::LimitableResource::ThreadCountMax, 0x30, value) ==
                  RESULT_SUCCESS &&
              value == 256,
          "Applications may have 256 threads");
    check(kernel.getResourceLimitValue(limit, kernel::LimitableResource::Count,
                                       0x30, value) ==
              svc(kernel::Error::InvalidEnumValue),
          "Unknown resources are refused");
    check(kernel.getResourceLimitValue(
              limit, kernel::LimitableResource::PhysicalMemoryMax, 0x31,
              value) == RESULT_SUCCESS &&
              value > 0,
          "Memory in use is reported");

    // Events count against the limit while they exist
    kernel::Event* event = nullptr;
    check(kernel.createEvent(process, event) == RESULT_SUCCESS,
          "CreateEvent succeeds");
    kernel.getResourceLimitValue(limit, kernel::LimitableResource::EventCountMax,
                                 0x31, value);
    check(value == 1, "The event is counted");
    event->release();
    kernel.getResourceLimitValue(limit, kernel::LimitableResource::EventCountMax,
                                 0x31, value);
    check(value == 0, "A destroyed event is not counted");
    kernel.getResourceLimitValue(limit, kernel::LimitableResource::EventCountMax,
                                 0x37, value);
    check(value == 1, "The peak stays");

    // An asynchronous request is answered in its user buffer and signals
    // its event
    std::vector<u8> user_buffer(0x1000, 0xaa);
    std::vector<u8> server_buffer(kernel::hipc::MSG_BUFFER_MAX_SIZE, 0x00);
    kernel::hipc::ServerSession* server = nullptr;
    kernel::hipc::ClientSession* client = nullptr;
    kernel.createSession(false, 0, server, client);
    kernel::Event* reply_event = nullptr;
    kernel.createEvent(process, reply_event);
    server->enqueueRequest(process, nullptr,
                           reinterpret_cast<uptr>(user_buffer.data()),
                           user_buffer.size(), reply_event);
    server->receive(reinterpret_cast<uptr>(server_buffer.data()),
                    server_buffer.size());
    check(server_buffer[0xfff] == 0xaa && server_buffer[0x1000] == 0x00,
          "The request is the user buffer's size");
    std::fill(server_buffer.begin(), server_buffer.end(), 0x55);
    check(!reply_event->clear(), "The event waits for the reply");
    server->reply(reinterpret_cast<uptr>(server_buffer.data()),
                  server_buffer.size());
    check(user_buffer[0] == 0x55 && user_buffer[0xfff] == 0x55,
          "The reply is in the user buffer");
    check(reply_event->clear(), "The reply signals the event");
    reply_event->release();
    client->release();
    server->release();

    // SetMemoryPermission validation
    check(kernel.checkUserBuffer(0x1001, 0x1000) ==
              svc(kernel::Error::InvalidAddress),
          "User buffers are page aligned");
    vaddr_t heap_va;
    process->createMemory(kernel::HEAP_REGION, PAGE,
                          kernel::MemoryType::Normal_4_0_0,
                          kernel::MemoryPermission::ReadWrite, heap_va);
    check(kernel.setMemoryPermission(process, heap_va + 1, 0x1000,
                                     kernel::MemoryPermission::Read) ==
              svc(kernel::Error::InvalidAddress),
          "Unaligned addresses are refused");
    check(kernel.setMemoryPermission(process, heap_va, 0x1000,
                                     kernel::MemoryPermission::ReadExecute) ==
              svc(kernel::Error::InvalidMemoryPermissions),
          "Memory cannot be made executable");
}

} // namespace

void testUserException(System& system) {
    auto* process = system.getOs().getKernel().getProcessManager().createProcess(
        "User exception test");
    auto* mmu = process->getMmu();

    vaddr_t code_va;
    const auto code = process->createMemory(
        kernel::EXECUTABLE_REGION, PAGE, kernel::MemoryType::Code,
        kernel::MemoryPermission::ReadExecute, code_va);
    std::memcpy(reinterpret_cast<void*>(code), MAIN, sizeof(MAIN));
    std::memcpy(reinterpret_cast<void*>(code + HANDLER_OFFSET), HANDLER,
                sizeof(HANDLER));
    process->setEntryPoint(code_va + HANDLER_OFFSET);

    check(system.getOs().getKernel().setMemoryPermission(
              process, code_va, PAGE, kernel::MemoryPermission::Read) ==
              MAKE_RESULT(Svc, kernel::Error::InvalidMemoryState),
          "Code cannot be reprotected");
    checkKernelObjects(system, process);

    const auto heap_page = [&] {
        vaddr_t va;
        process->createMemory(kernel::HEAP_REGION, PAGE,
                              kernel::MemoryType::Normal_4_0_0,
                              kernel::MemoryPermission::ReadWrite, va);
        return va;
    };
    const auto stack_va = heap_page();
    const auto results_va = heap_page();
    const auto results = mmu->unmapAddr(results_va);
    // Write tracked and read-only: the write is the guest's fault
    const auto read_only_va = heap_page();
    mmu->enableWriteTracking(ztd::Range<vaddr_t>::fromSize(read_only_va, PAGE));
    const auto no_access_va = heap_page();
    for (const auto& [va, perm] :
         {std::pair{read_only_va, kernel::MemoryPermission::Read},
          std::pair{no_access_va, kernel::MemoryPermission::None}})
        check(system.getOs().getKernel().setMemoryPermission(process, va, PAGE,
                                                             perm) ==
                  RESULT_SUCCESS,
              "Heap can be reprotected");
    // The kernel thread the exceptions belong to; it never runs itself
    auto* kernel_thread =
        new kernel::GuestThread(system, process, stack_va + PAGE, 44);

    vaddr_t tls_va;
    std::unique_ptr<hw::tegra_x1::cpu::IMemory> tls(
        process->createTlsMemory(tls_va));

    bool stop = false;
    bool finished = false;
    u32 returns = 0;
    std::string failure;
    std::thread runner([&] {
        try {
            std::unique_ptr<hw::tegra_x1::cpu::IThread> thread(
                system.getCpu().createThread(
                    system.getWallClock(), mmu,
                    {.svc_handler =
                         [&](hw::tegra_x1::cpu::IThread* guest, u64 id) {
                             auto& regs = guest->getState();
                             if (id == 0x28) {
                                 check(regs.r[0] == 0,
                                       "The handler reports success");
                                 check(process->leaveUserException(
                                           kernel_thread, *guest),
                                       "The handler leaves its exception");
                                 returns++;
                                 return;
                             }
                             check(id == 0x7f, "The guest makes its SVC");
                             finished = true;
                             stop = true;
                         },
                     .stop_requested = [&] { return stop; },
                     .supervisor_pause = [] {},
                     .breakpoint_hit = [] {},
                     .user_exception =
                         [&](hw::tegra_x1::cpu::IThread* guest,
                             const hw::tegra_x1::cpu::GuestException& info) {
                             return process->enterUserException(kernel_thread,
                                                                *guest, info);
                         }},
                    tls.get(), tls_va));
            auto& regs = thread->getState();
            regs.pc = code_va;
            regs.sp = stack_va + PAGE;
            regs.r[19] = read_only_va + 0x18;
            regs.r[20] = no_access_va + 0x20;
            regs.r[21] = results_va;
            regs.r[22] = results_va + 0x40;
            regs.r[23] = results_va + 0x80;
            thread->run();
            check(finished, "The guest reaches its final SVC");
            const auto info = process->getProcessLocalRegion() + 0x1c0 - 0x78;
            const auto* types = reinterpret_cast<const u64*>(results);
            const auto* esrs = types + 8;
            const auto* fars = types + 16;
            check(types[0] == 0x100 && types[1] == 0x100,
                  "UDF and BRK are instruction aborts");
            check(types[2] == 0x101 && types[3] == 0x101,
                  "Permission faults are data aborts");
            check(esrs[0] >> 26 == 0x00 && esrs[1] >> 26 == 0x3c &&
                      esrs[2] >> 26 == 0x24 && esrs[3] >> 26 == 0x24,
                  "The info has each exception's ESR");
            check((esrs[2] & (1u << 6)) != 0 && (esrs[3] & (1u << 6)) == 0,
                  "The store is a write and the load a read");
            check(fars[2] == read_only_va + 0x18 &&
                      fars[3] == no_access_va + 0x20,
                  "The info has the faulting addresses");
            check(regs.r[12] == info, "x1 is the ExceptionInfo");
            check(regs.r[11] == (info & ~u64{0xf}),
                  "The handler's stack ends below the info");
            check(regs.r[13] == EXCEPTION_COUNT && returns == EXCEPTION_COUNT,
                  "Every exception returns");
            check(*reinterpret_cast<const u64*>(mmu->unmapAddr(read_only_va) +
                                               0x18) == 0,
                  "The read-only page is not written");
            check(regs.r[5] == 0x1234, "x0-x8 are restored from the info");
            check((regs.r[6] >> 28) == 0b0110 && (regs.r[16] >> 28) == 0b0100,
                  "NZCV is restored from the info");
        } catch (const std::exception& error) {
            failure = error.what();
            stop = true;
        }
    });
    runner.join();
    if (!failure.empty())
        throw std::runtime_error(failure);
    std::cout << "PASS: UDF, BRK and permission faults enter the process "
                 "handler and ReturnFromException restores their context; "
                 "resource limits, events and user buffer replies\n";
}
