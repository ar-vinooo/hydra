#include "core/horizon/services/nvdrv/ioctl/nvhost_gpu.hpp"

#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/gpu/const.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/system.hpp"

namespace hydra::horizon::services::nvdrv::ioctl {

namespace {

enum class EventId {
    SmExceptionBptIntReport = 1,
    SmExceptionBptPauseReport,
    ErrorNotifierEvent,
};

}

NvResult NvHostGpu::ioctl(IoctlContext& context, u32 type, u32 nr) {
    if (type == 0x48 && (nr == 0x08 || nr == 0x1b))
        return submitGpfifoChecked(context, false);
    if (type == 0x48 && nr == 0x17) {
        if (!context.out_stream)
            return NvResult::BadParameter;
        if (context.out_stream->getSeek() > context.out_stream->getSize() ||
            context.out_stream->getSize() - context.out_stream->getSeek() < 16)
            return NvResult::InvalidSize;
    }
    if (type == 0x48 && (nr == 0x18 || nr == 0x1a)) {
        if (!context.in_stream || !context.out_stream)
            return NvResult::BadParameter;
        if (context.in_stream->getSeek() > context.in_stream->getSize() ||
            context.in_stream->getSize() - context.in_stream->getSeek() < 32 ||
            context.out_stream->getSeek() > context.out_stream->getSize() ||
            context.out_stream->getSize() - context.out_stream->getSeek() < 32)
            return NvResult::InvalidSize;
    }
    if (type == 0 && nr == 2) {
        if (!context.in_stream || !context.out_stream)
            return NvResult::BadParameter;
        if (context.in_stream->getSeek() > context.in_stream->getSize() ||
            context.in_stream->getSize() - context.in_stream->getSeek() < 4 ||
            context.out_stream->getSeek() > context.out_stream->getSize() ||
            context.out_stream->getSize() - context.out_stream->getSeek() < 8)
            return NvResult::InvalidSize;
    }
    return ChannelBase::ioctl(context, type, nr);
}

NvResult NvHostGpu::ioctl2(IoctlContext& context, u32 type, u32 nr) {
    if (type == 0x48 && nr == 0x1b)
        return submitGpfifoChecked(context, true);
    return NvResult::NotImplemented;
}

NvResult NvHostGpu::submitGpfifoChecked(IoctlContext& context,
                                       bool inline_entries) {
    using namespace hw::tegra_x1::gpu;
    constexpr usize HEADER_SIZE = 24;
    const auto has_bytes = [](const auto& stream, u64 size) {
        return stream && stream->getSeek() <= stream->getSize() &&
               size <= stream->getSize() - stream->getSeek();
    };
    if (!context.in_stream || !context.out_stream)
        return NvResult::BadParameter;
    if (!has_bytes(context.in_stream, HEADER_SIZE) ||
        !has_bytes(context.out_stream, HEADER_SIZE))
        return NvResult::InvalidSize;

    const auto* input = context.in_stream->getPtr() + context.in_stream->getSeek();
    u64 gpfifo;
    u32 count;
    GpfifoFlags flags;
    Fence fence;
    std::memcpy(&gpfifo, input, 8);
    std::memcpy(&count, input + 8, 4);
    std::memcpy(&flags, input + 12, 4);
    std::memcpy(&fence, input + 16, 8);
    const u64 entry_bytes = static_cast<u64>(count) * sizeof(GpfifoEntry);
    if (inline_entries) {
        if (count != 0 && !context.in_buffer_stream)
            return NvResult::BadParameter;
        if (context.in_buffer_stream &&
            !has_bytes(context.in_buffer_stream, entry_bytes))
            return NvResult::InvalidSize;
    } else if (gpfifo == 0 && !has_bytes(context.in_stream, HEADER_SIZE + entry_bytes)) {
        return NvResult::InvalidSize;
    }
    if (!context.process)
        return NvResult::BadParameter;

    // NVIDIA's ioctl1 copies from args.gpfifo in the process address space;
    // ioctl2 supplies the entries inline. Keep the existing null-pointer
    // appended-array path for internal callers.
    std::vector<ztd::Range<uptr>> guest_pieces;
    if (!inline_entries && gpfifo != 0 && count != 0) {
        if (!kernel::ADDRESS_SPACE.contains(gpfifo) ||
            entry_bytes > kernel::ADDRESS_SPACE.getEnd() - gpfifo)
            return NvResult::InvalidAddress;
        u64 copied = 0;
        auto* mmu = context.process->getMmu();
        while (copied < entry_bytes) {
            const auto address = gpfifo + copied;
            const auto region = mmu->queryRegion(address);
            if (region.state.type == kernel::MemoryType::Free ||
                !any(region.state.perm & kernel::MemoryPermission::Read))
                return NvResult::InvalidAddress;
            const auto host = mmu->unmapAddr(address);
            if (host == 0)
                return NvResult::InvalidAddress;
            const u64 amount = std::min(entry_bytes - copied,
                hw::tegra_x1::cpu::GUEST_PAGE_SIZE -
                    (address % hw::tegra_x1::cpu::GUEST_PAGE_SIZE));
            guest_pieces.push_back(ztd::Range<uptr>::fromSize(host, amount));
            copied += amount;
        }
    }
    // Copy only after validating the whole array; IPC buffers can be unaligned.
    std::vector<GpfifoEntry> entries;
    try {
        entries.resize(count);
    } catch (const std::bad_alloc&) {
        return NvResult::InsufficientMemory;
    }
    if (count != 0) {
        if (!guest_pieces.empty()) {
            auto* destination = reinterpret_cast<u8*>(entries.data());
            for (const auto& piece : guest_pieces) {
                std::memcpy(destination, reinterpret_cast<const void*>(piece.getBegin()),
                            piece.getSize());
                destination += piece.getSize();
            }
        } else {
            const auto* source = inline_entries
                ? context.in_buffer_stream->getPtr() + context.in_buffer_stream->getSeek()
                : input + HEADER_SIZE;
            std::memcpy(entries.data(), source, static_cast<usize>(entry_bytes));
        }
    }
    u32 detailed_error = 0;
    const auto result = submitGpfifo(&context.system, context.process,
        gpfifo, count, {flags, &detailed_error}, {&fence}, entries.data());
    if (result != NvResult::Success)
        return result;

    std::array<u8, HEADER_SIZE> response;
    std::memcpy(response.data(), input, HEADER_SIZE);
    std::memcpy(response.data() + 12, &detailed_error, 4);
    std::memcpy(response.data() + 16, &fence, 8);
    std::memcpy(context.out_stream->getPtr() + context.out_stream->getSeek(),
                response.data(), response.size());
    context.in_stream->seekBy(HEADER_SIZE + (inline_entries || gpfifo != 0 ? 0 : entry_bytes));
    if (inline_entries && context.in_buffer_stream)
        context.in_buffer_stream->seekBy(entry_bytes);
    context.out_stream->seekBy(HEADER_SIZE);
    return result;
}

NvResult NvHostGpu::queryEvent(u32 event_id_u32, kernel::Event*& out_event) {
    auto event_id = static_cast<EventId>(event_id_u32);
    switch (event_id) {
    case EventId::SmExceptionBptIntReport:
        out_event = sm_exception_bpt_int_report_event;
        break;
    case EventId::SmExceptionBptPauseReport:
        out_event = sm_exception_bpt_pause_report_event;
        break;
    case EventId::ErrorNotifierEvent:
        out_event = error_notifier_event;
        break;
    default:
        return NvResult::NotSupported;
    }

    return NvResult::Success;
}

// TODO: gpfifo
NvResult NvHostGpu::submitGpfifo(
    System* system, kernel::Process* process, u64 gpfifo, u32 num_entries,
    InOut<hw::tegra_x1::gpu::GpfifoFlags, u32> inout_flags_and_detailed_error,
    InOutSingle<hw::tegra_x1::gpu::Fence> inout_fence,
    const hw::tegra_x1::gpu::GpfifoEntry* entries) {
    (void)gpfifo;

    auto& gpu = system->getGpu();
    if (decode_state->failed || gpu.getPfifo().isStopping() ||
        process->getGMmu().gpuWorkCancelled())
        return NvResult::InvalidState;
    const hw::tegra_x1::gpu::GpfifoFlags flags =
        inout_flags_and_detailed_error;
    constexpr auto supported = hw::tegra_x1::gpu::GpfifoFlags::FenceWait |
        hw::tegra_x1::gpu::GpfifoFlags::FenceGet |
        hw::tegra_x1::gpu::GpfifoFlags::SuppressWfi;
    if (any(flags & ~supported))
        return NvResult::NotSupported;
    // FENCE_WAIT: the fence the submission has to wait for comes in
    const hw::tegra_x1::gpu::Fence wait_fence = inout_fence.get();
    if (any(flags & hw::tegra_x1::gpu::GpfifoFlags::FenceWait) &&
        wait_fence.id != 0 && !gpu.getVideoSyncpoints().read(wait_fence.id))
        return NvResult::BadValue;
    // A fence is reached when the host finished the submission
    hw::tegra_x1::gpu::Fence fence{};
    std::function<void()> on_complete;
    if (any(flags & hw::tegra_x1::gpu::GpfifoFlags::FenceGet)) {
        if (!gpu.reserveFence(fence, decode_state.get()))
            return NvResult::InsufficientMemory;
        on_complete = gpu.makeFenceSignal(fence);
    }

    if (!gpu.getPfifo().submitEntries(
        process->getGMmu(),
        std::span<const hw::tegra_x1::gpu::GpfifoEntry>(entries, num_entries),
        flags, std::move(on_complete), wait_fence, decode_state))
        return NvResult::InvalidState;

    // No error is reported
    inout_flags_and_detailed_error = 0;
    inout_fence = fence;
    return NvResult::Success;
}

NvResult NvHostGpu::allocObjCtx(u32 class_num, u32 flags, u64* out_obj_id) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "class number: {}, flags: {:#x}",
                               class_num, flags);

    // HACK
    *out_obj_id = 0;
    return NvResult::Success;
}

NvResult NvHostGpu::zCullBind(gpu_vaddr_t addr, u32 mode,
                              [[maybe_unused]] u32 reserved) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "address: {:#x}, mode: {}", addr,
                               mode);
    return NvResult::Success;
}

NvResult NvHostGpu::setErrorNotifier(u64 offset, u64 size, u32 mem,
                                     [[maybe_unused]] u32 reserved) {
    LOG_FUNC_WITH_ARGS_STUBBED(
        Services, "offset: {:#x}, size: {:#x}, memory: {}", offset, size, mem);
    return NvResult::NotSupported;
}

NvResult NvHostGpu::getErrorNotification(u64* out_timestamp, u32* out_info32,
                                         u16* out_info16, u16* out_status) {
    const bool failed = decode_state->failed.load(std::memory_order_acquire);
    *out_timestamp = failed ? decode_state->fault_time.load() : 0;
    *out_info32 = failed ? decode_state->error.load() : 0;
    *out_info16 = 0;
    *out_status = failed ? 0xffff : 0;

    return NvResult::Success;
}

NvResult
NvHostGpu::allocGpfifoEx(IoctlContext* context, u32 num_entries, u32 num_jobs, u32 flags,
                         hw::tegra_x1::gpu::Fence* out_fence,
                         [[maybe_unused]] std::array<u32, 3> reserved) {
    (void)num_entries;
    (void)num_jobs;
    (void)flags;
    LOG_DEBUG(
        Services, "number of entries: {}, num_jobs: {}, flags: {:#x}",
        num_entries, num_jobs, flags);

    if (decode_state->failed || context->system.getGpu().getPfifo().isStopping())
        return NvResult::InvalidState;
    return context->system.getGpu().reserveFence(*out_fence, decode_state.get(), 0)
        ? NvResult::Success : NvResult::InsufficientMemory;
}

NvResult NvHostGpu::getSyncpoint(IoctlContext* context, u32 module_id, u32* out_value) {
    (void)module_id;
    hw::tegra_x1::gpu::Fence fence{};
    if (!context->system.getGpu().reserveFence(fence, decode_state.get(), 0))
        return NvResult::InsufficientMemory;
    *out_value = fence.id;
    return NvResult::Success;
}

} // namespace hydra::horizon::services::nvdrv::ioctl
