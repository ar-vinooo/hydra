#include "core/hw/tegra_x1/gpu/pfifo.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/visual_diagnostics.hpp"
#ifdef ZTD_PLATFORM_APPLE
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#endif

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

// Keep the fast contiguous path local to one entry and one GPU instance.
class PushBufferReader {
  public:
    PushBufferReader(GMmu& gmmu_, uptr begin, uptr end_)
        : address{begin}, gmmu{gmmu_}, end{end_} {}

    std::optional<u32> read() {
        if (address > end || end - address < sizeof(u32))
            return std::nullopt;
        if (address < window_begin || address >= window_end ||
            window_end - address < sizeof(u32)) {
            uptr base;
            const auto as = gmmu.findAddr(address, base);
            if (!as || as->ptr == 0)
                return std::nullopt;
            window_begin = base;
            window_end = base + as->size;
            host = as->ptr;
        }
        u32 word;
        if (window_end - address >= sizeof(word)) {
            std::memcpy(&word, reinterpret_cast<const void*>(
                host + (address - window_begin)), sizeof(word));
        } else {
            const auto bytes = gmmu.readMappedBytes(address, sizeof(word));
            if (bytes.size() != sizeof(word))
                return std::nullopt;
            std::memcpy(&word, bytes.data(), sizeof(word));
        }
        address += sizeof(word);
        return word;
    }

    uptr address;

  private:
    GMmu& gmmu;
    uptr end;
    uptr window_begin{0};
    uptr window_end{0};
    uptr host{0};
};

} // namespace

PfifoDecodeState::PfifoDecodeState() = default;
PfifoDecodeState::~PfifoDecodeState() = default;

void PfifoDecodeState::fail(u32 code) {
    u32 expected = 0;
    if (!error.compare_exchange_strong(expected, code))
        return;
    fault_time = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    failed.store(true, std::memory_order_release);
    if (on_fault)
        on_fault();
}

Pfifo::Pfifo(Gpu& gpu_) : gpu{gpu_}, thread(&Pfifo::threadFunc, this) {}

Pfifo::~Pfifo() { shutdown(); }

void Pfifo::cancel(GMmu& gmmu) {
    gmmu.cancelGpuWork();
    {
        std::unique_lock lock(mutex);
        for (auto it = entry_lists.begin(); it != entry_lists.end();) {
            if (it->gmmu == &gmmu) {
                retired_submissions.insert(it->correlation_id);
                it = entry_lists.erase(it);
            } else {
                ++it;
            }
        }
        cond_var.notify_all();
        completed_cond_var.wait(lock, [&] { return active_gmmu != &gmmu; });
        while (retired_submissions.erase(completed_count + 1) != 0)
            ++completed_count;
        completed_cond_var.notify_all();
    }
    // An empty submission on the same Metal queue drains already committed
    // work before the process frees its guest allocations.
    std::unique_ptr<renderer::ICommandBuffer> commands(gpu.getRenderer().createCommandBuffer());
    gpu.getRenderer().debugSyncCommandBuffer(commands.get());
}

void Pfifo::shutdown() {
    {
        std::scoped_lock lock(mutex);
        stop = true;
    }
    cond_var.notify_all();
    completed_cond_var.notify_all();
    if (thread.joinable())
        thread.join();
    std::scoped_lock lock(mutex);
    entry_lists.clear();
}

bool Pfifo::submitEntries(GMmu& gmmu, std::span<const GpfifoEntry> entries,
                          GpfifoFlags flags,
                          std::function<void()> on_complete,
                          Fence wait_fence,
                          std::shared_ptr<PfifoDecodeState> decode_state) {
    LOG_DEBUG(Gpu, "Flags: {}", flags);

    {
        std::scoped_lock lock(mutex);
        if (stop || gmmu.gpuWorkCancelled())
            return false;
        entry_lists.emplace_back(
            &gmmu, std::vector<GpfifoEntry>(entries.begin(), entries.end()),
            flags, submitted_count + 1, std::move(on_complete), wait_fence,
            std::move(decode_state));
        submitted_count++;
        VisualDiagnostics::instance().submissionQueued(
            submitted_count, reinterpret_cast<uint64_t>(&gmmu), entries.size());
    }

    cond_var.notify_all();
    return true;
}

u64 Pfifo::getSubmittedCount() {
    std::scoped_lock lock(mutex);
    return submitted_count;
}

bool Pfifo::waitForCompletion(u64 count) {
    ScopedPerfTimer timer(PerfStats::instance().present_wait_ns);
    std::unique_lock lock(mutex);
    // Time out to avoid hanging the presentation if the GPU gets stuck
    const bool reached = completed_cond_var.wait_for(lock, std::chrono::milliseconds(250), [&] {
        return stop || completed_count >= count;
    });
    return reached && completed_count >= count;
}

bool Pfifo::ready(GpfifoEntryList& list) {
    auto& state = list.decode_state ? *list.decode_state : default_decode_state;
    if (list.gmmu->gpuWorkCancelled())
        state.failed = true;
    if (state.failed)
        return true;
    if (state.syncpoint_wait) {
        const auto wait = *state.syncpoint_wait;
        const auto value = gpu.getVideoSyncpoints().read(wait.id);
        if (!value) {
            state.fail(25);
            return true;
        }
        if (!video::Syncpoints::reached(*value, wait.value))
            return false;
        state.syncpoint_wait.reset();
    }
    if (any(list.flags & GpfifoFlags::FenceWait) && list.wait_fence.id != 0) {
        const auto value = gpu.getVideoSyncpoints().read(list.wait_fence.id);
        if (!value) {
            LOG_ERROR(Gpu, "Invalid submission fence {}:{}", list.wait_fence.id,
                      list.wait_fence.value);
            state.fail(25);
            return true;
        }
        if (!video::Syncpoints::reached(*value, list.wait_fence.value))
            return false;
    }
    if (state.semaphore_wait) {
        const auto wait = *state.semaphore_wait;
        uptr base;
        const auto mapping = list.gmmu->findAddr(wait.address, base);
        if (!mapping || mapping->ptr == 0 ||
            mapping->size - (wait.address - base) < sizeof(u32) ||
            (mapping->ptr + wait.address - base) %
                std::atomic_ref<u32>::required_alignment != 0) {
            LOG_ERROR(Gpu, "Pending semaphore is unmapped at {:#x}", wait.address);
            state.fail(31);
            return true;
        }
        const u32 word = std::atomic_ref<u32>(*reinterpret_cast<u32*>(
            mapping->ptr + wait.address - base)).load(std::memory_order_acquire);
        const bool satisfied = wait.operation == 1 ? word == wait.payload
            : wait.operation == 4 ? static_cast<i32>(word - wait.payload) >= 0
            : (word & wait.payload) != 0;
        if (!satisfied)
            return false;
        state.semaphore_wait.reset();
    }
    return true;
}

void Pfifo::threadFunc() {
    DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess().registerThisThread(
        "GPU thread");

    std::unique_lock lock(mutex);
    while (true) {
        cond_var.wait(lock, [&] { return stop || !entry_lists.empty(); });
        if (stop)
            break;

        // A blocked channel keeps its own FIFO order, but must not prevent
        // another channel from producing the value it is waiting for.
        std::set<PfifoDecodeState*> blocked;
        auto selected = entry_lists.end();
        for (auto it = entry_lists.begin(); it != entry_lists.end(); ++it) {
            auto* state = it->decode_state ? it->decode_state.get() : &default_decode_state;
            if (blocked.contains(state))
                continue;
            if (ready(*it)) {
                selected = it;
                break;
            }
            blocked.insert(state);
        }
        if (selected == entry_lists.end()) {
            cond_var.wait_for(lock, std::chrono::milliseconds(1));
            continue;
        }
        auto entry_list = std::move(*selected);
        entry_lists.erase(selected);
        auto& decode = entry_list.decode_state
            ? *entry_list.decode_state : default_decode_state;
        active_gmmu = entry_list.gmmu;
        lock.unlock();
        bool yielded = false;
        if (!decode.failed) {
            // Entries
            // TODO: flags
            {
                ScopedPerfTimer timer(PerfStats::instance().pfifo_busy_ns);
                PerfStats::instance().pfifo_lists++;
                VisualDiagnostics::instance().beginSubmission(entry_list.correlation_id);
                tls_crnt_gmmu = entry_list.gmmu;
                gpu.getRenderer().applyCaptureRequests();
                tls_crnt_command_buffer =
                    gpu.getRenderer().createCommandBuffer();
                if (entry_list.decode_state && !decode.context)
                    decode.context = std::make_unique<GpuChannelContext>(gpu);
                tls_crnt_decode_state = &decode;
                while (entry_list.entry_index < entry_list.entries.size()) {
                    if (!submitEntry(entry_list.entries[entry_list.entry_index],
                                     decode, entry_list.word_offset))
                        break;
                    ++entry_list.entry_index;
                    entry_list.word_offset = 0;
                }
                yielded = (decode.semaphore_wait || decode.syncpoint_wait) && !decode.failed;
#ifdef ZTD_PLATFORM_APPLE
                if (VisualDiagnostics::instance().enabled() &&
                    CONFIG_INSTANCE.getGpuRenderer() == GpuRenderer::Metal) {
                    const auto id = entry_list.correlation_id;
                    const auto epoch = VisualDiagnostics::instance().frameId();
                    static_cast<renderer::metal::CommandBuffer*>(tls_crnt_command_buffer)
                        ->getCommandBuffer()->addCompletedHandler([id, epoch](MTL::CommandBuffer* completed) {
                            const auto* error = completed->error();
                            VisualDiagnostics::instance().completed(id, epoch,
                                completed->status() == MTL::CommandBufferStatusCompleted ? "completed" : "failed",
                                error ? error->localizedDescription()->utf8String() : "",
                                completed->GPUEndTime() - completed->GPUStartTime());
                        });
                }
#endif
                if (!stop && !entry_list.gmmu->gpuWorkCancelled() &&
                    !yielded && !decode.failed) {
#ifdef ZTD_PLATFORM_APPLE
                    if (CONFIG_INSTANCE.getGpuRenderer() == GpuRenderer::Metal) {
                        static_cast<renderer::metal::CommandBuffer*>(tls_crnt_command_buffer)
                            ->getCommandBuffer()->addCompletedHandler(
                                [complete = entry_list.on_complete,
                                 state = entry_list.decode_state](MTL::CommandBuffer* completed) {
                                    if (completed->status() == MTL::CommandBufferStatusCompleted &&
                                        (!state || !state->failed)) {
                                        if (complete)
                                            complete();
                                    } else if (state)
                                        state->fail(25);
                                });
                    } else
#endif
                        if (entry_list.on_complete)
                            tls_crnt_command_buffer->onCompleted(entry_list.on_complete);
                }
                delete tls_crnt_command_buffer;
                tls_crnt_command_buffer = nullptr;
                tls_crnt_gmmu = nullptr;
                tls_crnt_decode_state = nullptr;
            }
        }

        lock.lock();
        active_gmmu = nullptr;
        if (yielded && !stop && !entry_list.gmmu->gpuWorkCancelled()) {
            entry_lists.push_front(std::move(entry_list));
        } else {
            retired_submissions.insert(entry_list.correlation_id);
            while (retired_submissions.erase(completed_count + 1) != 0)
                ++completed_count;
            completed_cond_var.notify_all();
        }
        completed_cond_var.notify_all();
    }

    DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess()
        .unregisterThisThread();
}

bool Pfifo::submitEntry(GpfifoEntry entry, PfifoDecodeState& state,
                        u64& word_offset) {
    // clb06f.h: conditional fetch and LEVEL_SUBROUTINE need distinct
    // execution rules. Never execute these as an unconditional main segment.
    if ((entry.gpu_addr_lo & 1u) != 0 || entry.is_push_buffer != 0) {
        LOG_ERROR(Gpu, "Unsupported conditional/subroutine GPFIFO entry");
        state.fail(25);
        return false;
    }
    // CPU writes queued by write tracking must reach the caches first
    tls_crnt_gmmu->getMmu()->applyPendingInvalidations();
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

    PushBufferReader reader(*tls_crnt_gmmu, gpu_addr + word_offset * sizeof(u32), end);
    while (reader.address < end) {
        if (stop || state.failed || tls_crnt_gmmu->gpuWorkCancelled())
            return false;
        const auto command_address = reader.address;
        const auto word = reader.read();
        if (!word) {
            LOG_ERROR(Gpu, "Unmapped pushbuffer word at {:#x}", command_address);
            state.resetPacket();
            state.fail(31);
            return false;
        }
        ++word_offset;
        if (state.remaining != 0) {
            processMethodArg(*word, state);
        } else {
            VisualDiagnostics::instance().command(command_address, *word);
            if (!submitCommand(*word, state))
                break;
        }
        if (state.semaphore_wait || state.syncpoint_wait || state.failed)
            return false;
    }
    return true;
}

bool Pfifo::submitCommand(u32 raw_header, PfifoDecodeState& state) {
    const auto header = std::bit_cast<CommandHeader>(raw_header);
    LOG_DEBUG(
        Gpu, "Method: {:#x}, subchannel: {}, arg: {:#x}, secondary opcode: {}",
        header.method, header.subchannel, header.arg, header.secondary_opcode);

    // NVIDIA clb06f.h defines the method formats. PB_COUNT persists across
    // segments (dev_pbdma.ref, PB_COUNT); entry boundaries are not packet ends.
    state.resetPacket();
    state.method = header.method;
    state.subchannel = header.subchannel;
    switch (header.secondary_opcode) {
    case SecondaryOpcode::Grp0UseTert:
    case SecondaryOpcode::Grp2UseTert: {
        const auto tert = static_cast<TertiaryOpcode>(extractBits(raw_header, 16, 2));
        if (tert == TertiaryOpcode::Grp0IncMethod) {
            state.increment =
                header.secondary_opcode == SecondaryOpcode::Grp0UseTert;
            state.method = extractBits(raw_header, 2, 11);
            state.remaining = extractBits(raw_header, 18, 11);
        } else {
            LOG_ERROR(Gpu, "Unsupported PFIFO subdevice operation {}", tert);
            state.fail(25);
            return false;
        }
        break;
    }
    case SecondaryOpcode::IncMethod:
        state.increment = true;
        state.remaining = header.arg;
        break;
    case SecondaryOpcode::NonIncMethod:
        state.remaining = header.arg;
        break;
    case SecondaryOpcode::ImmDataMethod:
        state.remaining = 1;
        processMethodArg(header.arg, state);
        break;
    case SecondaryOpcode::OneInc:
        state.increment = true;
        state.one_increment = true;
        state.remaining = header.arg;
        break;
    case SecondaryOpcode::EndPbSegment:
        return false;
    default:
        LOG_NOT_IMPLEMENTED(Gpu, "Secondary opcode {}",
                            header.secondary_opcode);
        state.fail(25);
        return false;
    }
    return true;
}

void Pfifo::processMethodArg(u32 arg, PfifoDecodeState& state) {
    // Collect macro arguments until the packet is complete, even when it is
    // split across submissions. Incomplete packets must not execute a macro.
    if (state.method >= MACRO_METHODS_REGION)
        state.macro_args.emplace_back(state.method, arg);
    else
        gpu.subchannelMethod(state.subchannel, state.method, arg);
    if (state.increment)
        ++state.method;
    if (state.one_increment)
        state.increment = false;
    if (--state.remaining == 0 && !state.macro_args.empty()) {
        for (const auto& [method, value] : state.macro_args)
            gpu.subchannelMethod(state.subchannel, method, value);
        gpu.subchannelFlushMacro(state.subchannel);
        state.macro_args.clear();
    }
}

} // namespace hydra::hw::tegra_x1::gpu
