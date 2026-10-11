#include "core/hw/tegra_x1/gpu/pfifo.hpp"

#include "core/debugger/debugger_manager.hpp"
#include "core/hw/tegra_x1/gpu/const.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/command_buffer.hpp"

namespace hydra::hw::tegra_x1::gpu {

namespace {

enum class SecondaryOpcode : u32 {
    Grp0UseTert = 0,
    IncMethod = 1,
    Grp2UseTert = 2,
    NonIncMethod = 3,
    ImmDataMethod = 4,
    OneInc = 5,
    Reserved = 6,
    EndPbSegment = 7,
};

// TODO: do both GRP0 and GRP2 use the same tertiary opcodes?
enum class TertiaryOpcode : u32 {
    Grp0IncMethod = 0,
    Grp2NonIncMethod = 0,
    Grp0SetSubDevMask = 1,
    Grp0StoreSubDevMask = 2,
    Grp0UseSubDevMask = 3,
};

} // namespace

} // namespace hydra::hw::tegra_x1::gpu

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::SecondaryOpcode, Grp0UseTert,
                       "GRP0 use tertiary opcode", IncMethod,
                       "incrementing method", Grp2UseTert,
                       "GRP2 use tertiary opcode", NonIncMethod,
                       "non-incremental method", ImmDataMethod,
                       "immediate data method", OneInc, "one increment",
                       Reserved, "reserved", EndPbSegment, "End PB segment")

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::TertiaryOpcode, Grp0IncMethod,
                       "GRP0 increment method / GRP2 non-increment method",
                       Grp0SetSubDevMask, "GRP0 set subdevice mask",
                       Grp0StoreSubDevMask, "GRP0 store subdevice mask",
                       Grp0UseSubDevMask, "GRP0 use subdevice mask")

namespace hydra::hw::tegra_x1::gpu {

namespace {

struct CommandHeader {
    u32 method : 12;
    u32 reserved : 1;
    u32 subchannel : 3;
    u32 arg : 13;
    SecondaryOpcode secondary_opcode : 3;
};

template <typename T>
T read(uptr& gpu_addr) {
    T word = tls_crnt_gmmu->load<T>(gpu_addr);
    gpu_addr += sizeof(T);

    return word;
}

} // namespace

Pfifo::Pfifo(Gpu& gpu_) : gpu{gpu_}, thread(&Pfifo::threadFunc, this) {}

Pfifo::~Pfifo() { shutdown(); }

void Pfifo::shutdown() {
    {
        std::scoped_lock lock(mutex);
        stop = true;
    }
    cond_var.notify_all();
    completed_cond_var.notify_all();
    if (thread.joinable())
        thread.join();
}

void Pfifo::submitEntries(GMmu& gmmu, std::span<const GpfifoEntry> entries,
                          GpfifoFlags flags) {
    LOG_DEBUG(Gpu, "Flags: {}", flags);

    {
        std::scoped_lock lock(mutex);
        entry_lists.emplace(
            gmmu, std::vector<GpfifoEntry>(entries.begin(), entries.end()),
            flags);
        submitted_count++;
    }

    cond_var.notify_all();
}

u64 Pfifo::getSubmittedCount() {
    std::scoped_lock lock(mutex);
    return submitted_count;
}

void Pfifo::waitForCompletion(u64 count) {
    ScopedPerfTimer timer(PerfStats::instance().present_wait_ns);
    std::unique_lock lock(mutex);
    // Time out to avoid hanging the presentation if the GPU gets stuck
    completed_cond_var.wait_for(lock, std::chrono::milliseconds(250), [&] {
        return stop || completed_count >= count;
    });
}

void Pfifo::threadFunc() {
    DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess().registerThisThread(
        "GPU thread");

    std::unique_lock lock(mutex);
    while (true) {
        cond_var.wait(lock, [&] { return stop || !entry_lists.empty(); });
        if (stop)
            break;

        // Process entry lists
        while (!stop && !entry_lists.empty()) {
            const auto entry_list = std::move(entry_lists.front());
            entry_lists.pop();

            lock.unlock();

            // Entries
            // TODO: flags
            {
                ScopedPerfTimer timer(PerfStats::instance().pfifo_busy_ns);
                PerfStats::instance().pfifo_lists++;
                tls_crnt_gmmu = &entry_list.gmmu;
                tls_crnt_command_buffer =
                    gpu.getRenderer().createCommandBuffer();
                for (const auto& entry : entry_list.entries)
                    submitEntry(entry);
                delete tls_crnt_command_buffer;
                tls_crnt_command_buffer = nullptr;
                tls_crnt_gmmu = nullptr;
            }

            lock.lock();
            completed_count++;
            completed_cond_var.notify_all();
        }
    }

    DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess()
        .unregisterThisThread();
}

void Pfifo::submitEntry(const GpfifoEntry entry) {
    LOG_DEBUG(
        Gpu,
        "Gpfifo entry (addr lo: {:#x}, addr hi: {:#x}, size: {:#x}, allow "
        "flush: {}, is push buffer: {}, sync: {})",
        entry.gpu_addr_lo, entry.gpu_addr_hi, entry.size, entry.allow_flush,
        entry.is_push_buffer, entry.sync);

    uptr gpu_addr = static_cast<u64>(entry.gpu_addr_lo) |
                    (static_cast<u64>(entry.gpu_addr_hi) << 32);
    gpu_addr &= ~0x3llu; // Clear the 2 lsb
    uptr end = gpu_addr + entry.size * sizeof(u32);

    while (gpu_addr < end) {
        if (!submitCommand(gpu_addr))
            break;
    }
}

bool Pfifo::submitCommand(uptr& gpu_addr) {
    const auto raw_header = read<u32>(gpu_addr);
    const auto header = std::bit_cast<CommandHeader>(raw_header);
    LOG_DEBUG(
        Gpu, "Method: {:#x}, subchannel: {}, arg: {:#x}, secondary opcode: {}",
        header.method, header.subchannel, header.arg, header.secondary_opcode);

    u32 offset = header.method;
    switch (header.secondary_opcode) {
    case SecondaryOpcode::Grp0UseTert:
    case SecondaryOpcode::Grp2UseTert: {
        const auto tert = static_cast<TertiaryOpcode>(extractBits(raw_header, 16, 2));
        if (tert == TertiaryOpcode::Grp0IncMethod) {
            // Old style method header: address in bytes and an 11-bit count
            const bool increment =
                header.secondary_opcode == SecondaryOpcode::Grp0UseTert;
            u32 method = extractBits(raw_header, 2, 11);
            const u32 count = extractBits(raw_header, 18, 11);
            for (u32 i = 0; i < count; i++)
                processMethodArg(header.subchannel, gpu_addr, method,
                                 increment);
            offset = method;
        } else {
            // Subdevice mask operations, only a single GPU is emulated
        }
        break;
    }
    case SecondaryOpcode::IncMethod:
        for (u32 i = 0; i < header.arg; i++)
            processMethodArg(header.subchannel, gpu_addr, offset, true);
        break;
    case SecondaryOpcode::NonIncMethod:
        for (u32 i = 0; i < header.arg; i++)
            processMethodArg(header.subchannel, gpu_addr, offset, false);
        break;
    case SecondaryOpcode::ImmDataMethod:
        gpu.subchannelMethod(header.subchannel, offset, header.arg);
        break;
    case SecondaryOpcode::OneInc:
        for (u32 i = 0; i < header.arg; i++)
            processMethodArg(header.subchannel, gpu_addr, offset, i == 0);
        break;
    case SecondaryOpcode::EndPbSegment:
        return false;
    default:
        LOG_NOT_IMPLEMENTED(Gpu, "Secondary opcode {}",
                            header.secondary_opcode);
        break;
    }

    // TODO: is it okay to prefetch the parameters and then execute the
    // macro?
    if (header.method >= MACRO_METHODS_REGION)
        gpu.subchannelFlushMacro(header.subchannel);

    return true;
}

void Pfifo::processMethodArg(u32 subchannel, uptr& gpu_addr, u32& method,
                             bool increment) {
    u32 arg = read<u32>(gpu_addr);
    gpu.subchannelMethod(subchannel, method, arg);
    if (increment)
        method++;
}

} // namespace hydra::hw::tegra_x1::gpu
