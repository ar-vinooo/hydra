#include "core/hw/tegra_x1/gpu/pfifo.hpp"
#include "core/hw/tegra_x1/gpu/capture/guest_events.hpp"
#include "core/hw/tegra_x1/gpu/optimization_policy.hpp"

#include <limits>
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

// The Tegra channel is a single subdevice. gk20a programs its PBDMA with
// SUBDEVICE_ID 1 and channel DMA filtering enabled, so a mask selects it when
// bit 0 is set.
constexpr u32 SUBDEVICE_ID = 1;

struct DecodeTrace {
    struct Word {
        uptr address;
        u32 raw;
        u32 remaining;
        u32 method;
        u32 subchannel;
    };
    const PfifoDecodeState* owner{nullptr};
    std::array<Word, 64> words{};
    u64 count{0};
    uptr entry_begin{0};
    uptr entry_end{0};

    void record(const PfifoDecodeState& state, uptr address, u32 raw,
                uptr begin, uptr end) {
        if (owner != &state) {
            owner = &state;
            count = 0;
        }
        words[count++ % words.size()] = {
            address, raw, state.remaining, state.method, state.subchannel};
        entry_begin = begin;
        entry_end = end;
    }

    void dump(GMmu& gmmu) const {
        LOG_ERROR(Gpu, "PFIFO fault history: entry [{:#x}, {:#x}), {} recorded words",
                  entry_begin, entry_end, std::min<u64>(count, words.size()));
        const auto start = count > words.size() ? count - words.size() : 0;
        for (auto i = start; i < count; ++i) {
            const auto& word = words[i % words.size()];
            const auto bytes = gmmu.readMappedBytes(word.address, sizeof(u32));
            u32 current = 0;
            if (bytes.size() == sizeof(current))
                std::memcpy(&current, bytes.data(), sizeof(current));
            LOG_ERROR(Gpu, "PFIFO word {:#x}: read {:#010x}, current {:#010x}, mapped {}, "
                           "pending {}, method {:#x}, subchannel {}",
                      word.address, word.raw, current, bytes.size() == sizeof(current),
                      word.remaining, word.method, word.subchannel);
        }
    }
};

bool decodeTraceEnabled() {
    static const bool enabled = std::getenv("HYDRA_PFIFO_TRACE") != nullptr;
    return enabled;
}

thread_local DecodeTrace decode_trace;

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

PfifoDecodeState::PfifoDecodeState() : channel_id{[] {
    static std::atomic<u64> next_channel{1};
    return next_channel.fetch_add(1, std::memory_order_relaxed);
}()} {}
PfifoDecodeState::~PfifoDecodeState() {
    capture::GuestEvents::instance().event("channel_destroy",
                                           {channel_id, 0, 0});
    if (release_fence)
        release_fence();
}

void PfifoDecodeState::fail(u32 code) {
    u32 expected = 0;
    if (!error.compare_exchange_strong(expected, code))
        return;
    fault_time = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    failed.store(true, std::memory_order_release);
    capture::GuestEvents::instance().event("channel_fault", {channel_id, 0, 0},
                                           0, 0, 0, code);
    if (on_fault)
        on_fault();
}

Pfifo::Pfifo(Gpu& gpu_) : gpu{gpu_}, thread(&Pfifo::threadFunc, this) {}

Pfifo::~Pfifo() { shutdown(); }

void Pfifo::cancel(GMmu& gmmu) {
    gmmu.cancelGpuWork();
    {
        std::unique_lock lock(mutex);
        if (auto found = outstanding.find(&gmmu); found != outstanding.end()) {
            for (const auto& weak : found->second)
                if (auto work = weak.lock())
                    work->cancel();
            outstanding.erase(found);
        }
        for (auto it = entry_lists.begin(); it != entry_lists.end();) {
            if (it->gmmu == &gmmu) {
                it->work->cancel();
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
    // Reports of the process write to its memory once the GPU produced them:
    // none may be left when the allocations are freed (they are dropped, the
    // address space is cancelled)
    gpu.getRenderer().drainReports();
}

void Pfifo::shutdown() {
    {
        std::scoped_lock lock(mutex);
        stop = true;
        dispatches.stop();
        for (const auto& [space, tickets] : outstanding)
            for (const auto& weak : tickets)
                if (auto work = weak.lock())
                    work->cancel();
        outstanding.clear();
    }
    cond_var.notify_all();
    completed_cond_var.notify_all();
    if (thread.joinable())
        thread.join();
    capture::GuestEvents::instance().finish();
    std::scoped_lock lock(mutex);
    entry_lists.clear();
}

PfifoAdmission Pfifo::reserveEntries(usize count) {
    PfifoAdmission admission;
    if (count > (64 * 1024 * 1024) / sizeof(GpfifoEntry))
        return admission;
    admission.owner = this;
    admission.bytes = count * sizeof(GpfifoEntry);
    admission.work = dispatches.tryAdmit(admission.bytes);
    return admission;
}

bool Pfifo::submitEntries(GMmu& gmmu, std::span<const GpfifoEntry> entries,
                          GpfifoFlags flags,
                          std::function<void()> on_complete,
                          Fence wait_fence,
                          std::shared_ptr<PfifoDecodeState> decode_state,
                          PfifoAdmission admission) {
    LOG_DEBUG(Gpu, "Flags: {}", flags);

    {
        std::scoped_lock lock(mutex);
        if (stop || gmmu.gpuWorkCancelled())
            return false;
        if (!admission)
            admission = reserveEntries(entries.size());
        if (!admission || admission.owner != this ||
            admission.bytes != entries.size_bytes() ||
            admission.work->status() != execution::WorkStatus::Queued)
            return false;
        auto& tickets = outstanding[&gmmu];
        std::erase_if(tickets, [](const auto& weak) {
            const auto work = weak.lock();
            if (!work)
                return true;
            const auto status = work->status();
            return status != execution::WorkStatus::Queued &&
                   status != execution::WorkStatus::Running;
        });
        tickets.push_back(admission.work);
        entry_lists.emplace_back(
            &gmmu, std::vector<GpfifoEntry>(entries.begin(), entries.end()),
            flags, submitted_count + 1, std::move(on_complete), wait_fence,
            std::move(decode_state), 0, 0, std::move(admission.work));
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
    const bool was_waiting = state.semaphore_wait || state.syncpoint_wait;
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
        capture::GuestEvents::instance().event("syncpoint_resume",
                                               {state.channel_id,
                                                state.active_sequence,
                                                state.dispatch_source_address},
                                               wait.id, 0, wait.value, *value);
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
        auto* word_ptr = resolveSemaphoreWord(*list.gmmu, wait.address);
        if (!word_ptr) {
            LOG_ERROR(Gpu, "Pending semaphore is unmapped at {:#x}", wait.address);
            state.fail(31);
            return true;
        }
        const u32 word = std::atomic_ref<u32>(*word_ptr).load(
            std::memory_order_acquire);
        const bool satisfied = wait.operation == 1 ? word == wait.payload
            : wait.operation == 4 ? static_cast<i32>(word - wait.payload) >= 0
            : (word & wait.payload) != 0;
        if (!satisfied)
            return false;
        capture::GuestEvents::instance().event(
            "semaphore_resume",
            {state.channel_id, state.active_sequence,
             state.dispatch_source_address},
            wait.address, 4, wait.payload, word, wait.operation);
        state.semaphore_wait.reset();
    }
    if (was_waiting && VisualDiagnostics::instance().enabled())
        VisualDiagnostics::instance().event(
            "pfifo_resumed",
            {{"channel", std::to_string(state.channel_id)},
             {"sequence", std::to_string(state.active_sequence)}});
    return true;
}

void Pfifo::threadFunc() {
    DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess().registerThisThread(
        "GPU thread");

    std::unique_lock lock(mutex);
    std::unique_ptr<renderer::ICommandBuffer> batch_commands;
    GMmu* batch_space = nullptr;
    PfifoDecodeState* batch_state = nullptr;
    unsigned batch_lists = 0;
    u64 batch_words = 0;
    std::vector<u64> batch_retirements;
    // Publish decode retirement only after the native batch was committed;
    // presentation may submit a queue drain as soon as this counter advances.
    const auto publish_retirements = [&] {
        for (const auto id : batch_retirements)
            retired_submissions.insert(id);
        batch_retirements.clear();
        while (retired_submissions.erase(completed_count + 1) != 0)
            ++completed_count;
    };
    // Caller releases mutex: completion callbacks may re-enter admission.
    const auto flush_batch = [&] {
        tls_crnt_gmmu = batch_space;
        tls_crnt_decode_state = batch_state;
        tls_crnt_command_buffer = batch_commands.get();
        batch_commands.reset();
        tls_crnt_command_buffer = nullptr;
        tls_crnt_decode_state = nullptr;
        tls_crnt_gmmu = nullptr;
        batch_space = nullptr;
        batch_state = nullptr;
        batch_lists = 0;
        batch_words = 0;
    };
    while (true) {
        if (batch_commands &&
            (stop || entry_lists.empty() || batch_space->gpuWorkCancelled())) {
            lock.unlock();
            flush_batch();
            lock.lock();
            publish_retirements();
            active_gmmu = nullptr;
            completed_cond_var.notify_all();
        }
        if (!batch_commands)
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
            if (batch_commands) {
                lock.unlock();
                flush_batch();
                lock.lock();
                publish_retirements();
                active_gmmu = nullptr;
                completed_cond_var.notify_all();
                continue;
            }
            cond_var.wait_for(lock, std::chrono::milliseconds(1));
            continue;
        }
        const auto* selected_state = selected->decode_state
                                         ? selected->decode_state.get()
                                         : &default_decode_state;
        if (batch_commands &&
            (selected->gmmu != batch_space || selected_state != batch_state)) {
            lock.unlock();
            flush_batch();
            lock.lock();
            publish_retirements();
            active_gmmu = nullptr;
            completed_cond_var.notify_all();
            continue;
        }
        auto entry_list = std::move(*selected);
        entry_lists.erase(selected);
        auto& decode = entry_list.decode_state
            ? *entry_list.decode_state : default_decode_state;
        active_gmmu = entry_list.gmmu;
        lock.unlock();
        bool yielded = false;
        const auto status = entry_list.work->status();
        if (status == execution::WorkStatus::Cancelled)
            decode.failed = true;
        else if (status == execution::WorkStatus::Queued && !entry_list.work->start())
            decode.fail(25);
        if (!decode.failed) {
            // Entries
            // TODO: flags
            {
                ScopedPerfTimer timer(PerfStats::instance().pfifo_busy_ns);
                PerfStats::instance().pfifo_lists++;
                VisualDiagnostics::instance().beginSubmission(entry_list.correlation_id);
                tls_crnt_gmmu = entry_list.gmmu;
                gpu.getRenderer().applyCaptureRequests();
                if (!batch_commands) {
                    batch_commands.reset(
                        gpu.getRenderer().createCommandBuffer());
                    batch_space = entry_list.gmmu;
                    batch_state = &decode;
                }
                ++batch_lists;
                for (const auto& entry : entry_list.entries)
                    batch_words = std::min<u64>(optimization::MAX_BATCH_WORDS,
                                                batch_words + entry.size);
                tls_crnt_command_buffer = batch_commands.get();
                // Retain the AS through host completion, including yielded/faulted work.
                if (entry_list.decode_state)
                    tls_crnt_command_buffer->onCompleted(
                        [state = entry_list.decode_state] {});
                if (entry_list.decode_state && !decode.context)
                    decode.context = std::make_unique<GpuChannelContext>(gpu);
                tls_crnt_decode_state = &decode;
                capture::GuestEvents::instance().channel(
                    *entry_list.gmmu,
                    {decode.channel_id, decode.active_sequence,
                     decode.dispatch_source_address});
                while (entry_list.entry_index < entry_list.entries.size()) {
                    if (!submitEntry(entry_list.entries[entry_list.entry_index],
                                     decode, entry_list.word_offset))
                        break;
                    ++entry_list.entry_index;
                    entry_list.word_offset = 0;
                }
                yielded = (decode.semaphore_wait || decode.syncpoint_wait) && !decode.failed;
                if (yielded) {
                    const auto origin = capture::currentGuestEventOrigin();
                    if (decode.semaphore_wait) {
                        const auto& wait = *decode.semaphore_wait;
                        capture::GuestEvents::instance().event(
                            "semaphore_wait", origin, wait.address, 4, 0,
                            wait.payload, wait.operation);
                    } else {
                        const auto& wait = *decode.syncpoint_wait;
                        capture::GuestEvents::instance().event(
                            "syncpoint_wait", origin, wait.id, 0, 0,
                            wait.value);
                    }
                }
                if (yielded && VisualDiagnostics::instance().enabled())
                    VisualDiagnostics::instance().event(
                        "pfifo_blocked",
                        {{"channel", std::to_string(decode.channel_id)},
                         {"sequence", std::to_string(decode.active_sequence)},
                         {"kind",
                          decode.semaphore_wait ? "semaphore" : "syncpoint"}});
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
                    // Runs after the reports queued by this submission reached
                    // guest memory, the guest reads them once the fence is
                    // reached
                    tls_crnt_command_buffer->onCompletedChecked(
                        [complete = entry_list.on_complete,
                         state = entry_list.decode_state,
                         work = entry_list.work](bool succeeded) {
                            // The claim keeps a concurrent cancel from
                            // winning after the fence was published
                            if (succeeded && (!state || !state->failed) &&
                                work->claimCompletion()) {
                                if (complete)
                                    complete();
                                work->finish(true);
                            } else {
                                work->finish(false);
                                if (state && work->status() !=
                                                 execution::WorkStatus::Cancelled)
                                    state->fail(25);
                            }
                        });
                }
#ifdef ZTD_PLATFORM_APPLE
                // Batching native submissions must not silently erase an
                // encoder transition that separated the original lists. Keep
                // this boundary even when the wrapper is reused. Intra-list
                // encoder reuse remains the existing backend's responsibility.
                if (optimization::enabled() &&
                    CONFIG_INSTANCE.getGpuRenderer() == GpuRenderer::Metal)
                    static_cast<renderer::metal::CommandBuffer*>(
                        tls_crnt_command_buffer)
                        ->endEncoding();
#endif
                tls_crnt_command_buffer = nullptr;
                tls_crnt_gmmu = nullptr;
                tls_crnt_decode_state = nullptr;
            }
        }

        lock.lock();
        if (yielded && !stop && !entry_list.gmmu->gpuWorkCancelled()) {
            entry_lists.push_front(std::move(entry_list));
        } else {
            if (stop || entry_list.gmmu->gpuWorkCancelled())
                entry_list.work->cancel();
            else if (decode.failed)
                entry_list.work->finish(false);
            batch_retirements.push_back(entry_list.correlation_id);
        }
        bool extend =
            batch_commands && optimization::enabled() && !stop && !yielded &&
            !decode.failed && !batch_space->gpuWorkCancelled() &&
            entry_list.flags == GpfifoFlags::None &&
            batch_lists < optimization::MAX_BATCH_LISTS && !entry_lists.empty();
        if (extend) {
            const auto& following = entry_lists.front();
            const auto* following_state = following.decode_state
                                              ? following.decode_state.get()
                                              : &default_decode_state;
            u64 following_words = 0;
            for (const auto& entry : following.entries)
                following_words = std::min<u64>(optimization::MAX_BATCH_WORDS,
                                                following_words + entry.size);
            extend =
                following.gmmu == batch_space &&
                following_state == batch_state &&
                following.flags == GpfifoFlags::None &&
                !following_state->failed && !following_state->semaphore_wait &&
                !following_state->syncpoint_wait &&
                following_words <= optimization::MAX_BATCH_WORDS - batch_words;
        }
        if (batch_commands && !extend) {
            lock.unlock();
            flush_batch();
            lock.lock();
        }
        // A process cancel must also wait for an uncommitted batch before
        // freeing its guest allocations. Clear this only after submission.
        active_gmmu = batch_commands ? batch_space : nullptr;
        if (!batch_commands)
            publish_retirements();
        completed_cond_var.notify_all();
    }

    lock.unlock();
    flush_batch();
    lock.lock();
    publish_retirements();
    active_gmmu = nullptr;
    completed_cond_var.notify_all();
    lock.unlock();

    DEBUGGER_MANAGER_INSTANCE.getDebuggerForCurrentProcess()
        .unregisterThisThread();
}

bool Pfifo::submitEntry(GpfifoEntry entry, PfifoDecodeState& state,
                        u64& word_offset) {
    // LEVEL controls USERD progress reporting, not packet execution or a call stack.
    // USERD TOP_LEVEL_GET is not exposed by Hydra yet; both levels execute here.
    // A conditional segment is only fetched while the subdevice is active;
    // otherwise it behaves as a NOP entry (dev_pbdma.ref, GP_ENTRY0_FETCH).
    if ((entry.gpu_addr_lo & 1u) != 0 && !state.subdevice_active)
        return true;
    // CPU writes queued by write tracking must reach the caches first, and so
    // must the guest reports written by the renderer's completion thread
    tls_crnt_gmmu->getMmu()->applyPendingInvalidations();
    gpu.getRenderer().applyQueuedMemoryInvalidations();
    LOG_DEBUG(
        Gpu,
        "Gpfifo entry (addr lo: {:#x}, addr hi: {:#x}, size: {:#x}, allow "
        "flush: {}, subroutine level: {}, sync: {})",
        entry.gpu_addr_lo, entry.gpu_addr_hi, entry.size, entry.allow_flush,
        entry.level_subroutine, entry.sync);

    uptr gpu_addr = static_cast<u64>(entry.gpu_addr_lo) |
                    (static_cast<u64>(entry.gpu_addr_hi) << 32);
    gpu_addr &= ~0x3llu; // Clear the 2 lsb
    uptr end = gpu_addr + entry.size * sizeof(u32);
    capture::GuestEvents::instance().event(
        "pfifo_entry", {state.channel_id, state.active_sequence, gpu_addr},
        gpu_addr, entry.size * sizeof(u32), 0, std::bit_cast<u64>(entry),
        word_offset);

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
        capture::GuestEvents::instance().event(
            "pfifo_word",
            {state.channel_id, state.active_sequence, command_address},
            command_address, sizeof(u32), 0, *word, state.remaining);
        state.source_address = command_address;
        if (decodeTraceEnabled())
            decode_trace.record(state, command_address, *word, gpu_addr, end);
        if (state.remaining != 0) {
            processMethodArg(*word, state);
        } else {
            VisualDiagnostics::instance().command(command_address, *word);
            if (!submitCommand(*word, state))
                break;
        }
        if (state.failed && decodeTraceEnabled())
            decode_trace.dump(*tls_crnt_gmmu);
        if (state.semaphore_wait || state.syncpoint_wait || state.failed)
            return false;
    }
    return true;
}

bool Pfifo::replayWord(u32 word, uptr source, PfifoDecodeState& state) {
    if (tls_crnt_decode_state != &state || tls_crnt_gmmu == nullptr ||
        tls_crnt_command_buffer == nullptr || !state.context || state.failed ||
        state.semaphore_wait || state.syncpoint_wait)
        return false;
    gpu.getRenderer().applyQueuedMemoryInvalidations();
    state.source_address = source;
    if (state.remaining != 0) {
        processMethodArg(word, state);
        return !state.failed;
    }
    return submitCommand(word, state);
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
        } else if (header.secondary_opcode == SecondaryOpcode::Grp0UseTert &&
                   tert == TertiaryOpcode::Grp0SetSubDevMask) {
            state.subdevice_active = (extractBits(raw_header, 4, 12) & SUBDEVICE_ID) != 0;
        } else if (header.secondary_opcode == SecondaryOpcode::Grp0UseTert &&
                   tert == TertiaryOpcode::Grp0StoreSubDevMask) {
            state.stored_subdevice_mask = extractBits(raw_header, 4, 12);
        } else if (header.secondary_opcode == SecondaryOpcode::Grp0UseTert &&
                   tert == TertiaryOpcode::Grp0UseSubDevMask) {
            state.subdevice_active = (state.stored_subdevice_mask & SUBDEVICE_ID) != 0;
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
    // Fault rather than wrap and make a later command look older.
    if (state.next_sequence == std::numeric_limits<u64>::max()) {
        state.fail(25);
        return;
    }
    const auto sequence = state.next_sequence++;
    // Collect macro arguments until the packet is complete, even when it is
    // split across submissions. Incomplete packets must not execute a macro.
    if (!state.subdevice_active) {
        // Consumed without delivery; the packet layout still advances
        if (state.increment)
            ++state.method;
        if (state.one_increment)
            state.increment = false;
        --state.remaining;
        return;
    }
    if (state.method >= MACRO_METHODS_REGION)
        state.macro_args.push_back(
            {state.method, arg, sequence, state.source_address});
    else
        dispatchMethod(state.method, arg, sequence, state.source_address, state);
    if (state.increment)
        ++state.method;
    if (state.one_increment)
        state.increment = false;
    if (--state.remaining == 0 && !state.macro_args.empty()) {
        for (const auto& argument : state.macro_args) {
            if (state.failed)
                break;
            dispatchMethod(argument.method, argument.value, argument.sequence,
                           argument.address, state);
        }
        if (!state.failed) {
            capture::GuestEvents::instance().event(
                "macro_flush", capture::currentGuestEventOrigin(), 0, 0, 0,
                state.subchannel);
            gpu.subchannelFlushMacro(state.subchannel);
        }
        state.macro_args.clear();
    }
}

void Pfifo::dispatchMethod(u32 method, u32 arg, u64 sequence, uptr address,
                           PfifoDecodeState& state) {
    ASSERT_DEBUG(sequence > state.last_dispatched_sequence, Gpu,
                 "Channel {} reordered method sequence {} after {}",
                 state.channel_id, sequence, state.last_dispatched_sequence);
    state.last_dispatched_sequence = sequence;
    state.active_sequence = sequence;
    state.dispatch_source_address = address;
    if (state.replay_method_observer)
        state.replay_method_observer(method, arg, sequence, address,
                                     state.subchannel);
    auto& events = capture::GuestEvents::instance();
    if (events.enabled()) {
        const auto origin = capture::currentGuestEventOrigin();
        events.channel(*tls_crnt_gmmu, origin, false);
        for (u32 id = 1; id < 192; ++id) {
            const auto value = gpu.getVideoSyncpoints().read(id);
            if (!value)
                break;
            events.initialSyncpoint(id, *value);
        }
        events.event("method", origin, method, 0, 0, arg, state.subchannel);
    }
    auto& diagnostics = VisualDiagnostics::instance();
    if (diagnostics.enabled())
        diagnostics.event("pfifo_method", {
            {"channel", std::to_string(state.channel_id)},
            {"sequence", std::to_string(sequence)},
            {"source_address", std::to_string(address)},
            {"subchannel", std::to_string(state.subchannel)},
            {"method", std::to_string(method)},
            {"argument", std::to_string(arg)}});
    gpu.subchannelMethod(state.subchannel, method, arg);
}

} // namespace hydra::hw::tegra_x1::gpu
