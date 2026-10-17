#include "core/horizon/services/nvdrv/ioctl/channel_base.hpp"
#include "core/video/submit_trace.hpp"
#include "core/system.hpp"
#include "core/video/submit.hpp"
#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"

namespace hydra::horizon::services::nvdrv::ioctl {

DEFINE_IOCTL_TABLE(
    ChannelBase,
    DEFINE_IOCTL_TABLE_ENTRY(ChannelBase, 0x00, 0x01, submit, 0x02,
                             getSyncpoint, 0x03, getWaitBase, 0x07,
                             setSubmitTimeout, 0x09, mapCmdBuffer, 0x0a,
                             unmapCmdBuffer)
        DEFINE_IOCTL_TABLE_ENTRY(ChannelBase, 0x47, 0x14, setUserData, 0x15,
                                 getUserData)
            DEFINE_IOCTL_TABLE_ENTRY(ChannelBase, 0x48, 0x01, setNvMapFd, 0x03,
                                     setTimeout, 0x08, submitGpfifo, 0x09,
                                     allocObjCtx, 0x0b, zCullBind, 0x0c,
                                     setErrorNotifier, 0x0d, setPriority, 0x17,
                                     getErrorNotification, 0x18, allocGpfifoEx,
                                     0x1a, allocGpfifoEx, 0x1b, submitGpfifo))

NvResult ChannelBase::submit(IoctlContext* context) {
    video::traceSubmission(videoEngineName(), *context);
    const auto result = [&]() -> NvResult {
    if (!videoEngineName().empty()) {
        if (!context->in_stream || !context->process)
            return NvResult::BadParameter;
        auto& stream = *context->in_stream;
        if (stream.getSeek() > stream.getSize())
            return NvResult::InvalidSize;
        const auto parsed = video::parseSubmission({stream.getPtr() + stream.getSeek(),
            static_cast<usize>(stream.getSize() - stream.getSeek())});
        if (!parsed)
            return NvResult::InvalidSize;
        if (parsed->relocation_count)
            return NvResult::NotSupported;
        const auto input = std::span(stream.getPtr() + stream.getSeek(),
            static_cast<usize>(stream.getSize() - stream.getSeek()));
        const auto sync_offset = usize(16) + parsed->commands.size() * 12;
        const auto fence_offset = sync_offset + usize(parsed->syncpoint_count) * 20;
        if (parsed->syncpoint_count != parsed->fence_count ||
            (parsed->fence_count && (!context->out_stream ||
                context->out_stream->getSeek() > context->out_stream->getSize() ||
                context->out_stream->getSize() - context->out_stream->getSeek() < input.size())))
            return NvResult::InvalidSize;
        auto word = [&](usize offset) {
            u32 value;
            std::memcpy(&value, input.data() + offset, 4);
            return value;
        };
        for (u32 index = 0; index < parsed->syncpoint_count; ++index)
            if (!video_syncpoint || word(sync_offset + usize(index) * 20) != *video_syncpoint)
                return NvResult::BadValue;
        usize budget = 4 * 1024 * 1024;
        for (const auto& command : parsed->commands) {
            const auto map = context->system.getGpu().getMap(Handle(command.handle));
            const auto size = usize(command.word_count) * 4;
            if (size > budget)
                return NvResult::InvalidSize;
            budget -= size;
            if (!map || !(*map)->addr || command.offset > (*map)->size ||
                size > (*map)->size - command.offset)
                return NvResult::InvalidAddress;
            std::vector<u32> words(command.word_count);
            usize copied = 0;
            while (copied < size) {
                const auto address = (*map)->addr + command.offset + copied;
                const auto host = context->process->getMmu()->unmapAddr(address);
                if (!host)
                    return NvResult::InvalidAddress;
                const auto amount = std::min(size - copied, usize(0x1000 - (address & 0xfff)));
                std::memcpy(reinterpret_cast<u8*>(words.data()) + copied,
                            reinterpret_cast<const void*>(host), amount);
                copied += amount;
            }
            NvResult execution = NvResult::Success;
            const auto interpreted = video::interpretHost1x(words, host1x_state,
                [&](u32 method, u32 value) {
                    if (execution != NvResult::Success)
                        return;
                    if (method < video_registers.size()) {
                        video_registers[method] = value;
                        if (method == 0xc0) {
                            video::traceEngineState(videoEngineName(), *context, video_registers);
                            execution = executeVideo(*context);
                        }
                    } else {
                        execution = NvResult::BadParameter;
                    }
                });
            if (execution != NvResult::Success)
                return execution;
            if (!interpreted) {
                LOG_WARN(Services, "{}: {}", videoEngineName(), interpreted.error());
                return NvResult::NotSupported;
            }
        }
        if (parsed->fence_count) {
            std::vector<u8> response(input.begin(), input.end());
            for (u32 index = 0; index < parsed->syncpoint_count; ++index) {
                const auto value = context->system.getGpu().getVideoSyncpoints().complete(
                    *video_syncpoint, word(sync_offset + usize(index) * 20 + 4));
                if (!value) return NvResult::InvalidState;
                std::memcpy(response.data() + fence_offset + usize(index) * 4, &*value, 4);
            }
            std::memcpy(context->out_stream->getPtr() + context->out_stream->getSeek(),
                response.data(), response.size());
        }
    }
    if (videoEngineName().empty())
        LOG_FUNC_STUBBED(Services);
    return NvResult::Success;
    }();
    // Failed video submissions are otherwise silent: the guest just stops
    // receiving frames.
    if (result != NvResult::Success && !videoEngineName().empty())
        LOG_WARN(Services, "{} submit failed: {}", videoEngineName(),
                 static_cast<u32>(result));
    return result;
}

NvResult ChannelBase::getSyncpoint(IoctlContext* context, u32 module_id, u32* out_value) {
    if (videoEngineName().empty()) {
        LOG_FUNC_WITH_ARGS_STUBBED(Services, "module ID: 0x{:x}", module_id);
        *out_value = 0;
        return NvResult::Success;
    }
    if (!video_syncpoint)
        video_syncpoint = context->system.getGpu().getVideoSyncpoints().allocate();
    if (!video_syncpoint) return NvResult::InsufficientMemory;
    *out_value = *video_syncpoint;
    return NvResult::Success;
}

NvResult ChannelBase::getWaitBase(u32 module_id, u32* out_value) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "module ID: 0x{:x}", module_id);

    // HACK
    *out_value = 0;
    return NvResult::Success;
}

NvResult ChannelBase::setSubmitTimeout(u32 timeout) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "timeout: {}", timeout);
    return NvResult::Success;
}

namespace {
NvResult mapBuffers(IoctlContext& context, bool map) {
    if (!context.in_stream || !context.out_stream)
        return NvResult::BadParameter;
    auto& input = *context.in_stream;
    auto& output = *context.out_stream;
    if (input.getSeek() > input.getSize() || input.getSize() - input.getSeek() < 12)
        return NvResult::InvalidSize;
    const auto* bytes = input.getPtr() + input.getSeek();
    u32 count;
    std::memcpy(&count, bytes, sizeof(count));
    if (count > 1024)
        return NvResult::InvalidSize;
    const auto size = usize(12) + usize(count) * 8;
    if (input.getSize() - input.getSeek() < size ||
        output.getSeek() > output.getSize() ||
        output.getSize() - output.getSeek() < size)
        return NvResult::InvalidSize;
    std::vector<u8> response(bytes, bytes + size);
    // Validate every handle before modifying the output or assigning addresses.
    for (usize i = 0; i < count; ++i) {
        u32 handle;
        std::memcpy(&handle, response.data() + 12 + i * 8, sizeof(handle));
        const auto backing = context.system.getGpu().getMap(Handle(handle));
        if (!backing || !(*backing)->addr)
            return NvResult::InvalidAddress;
    }
    for (usize i = 0; i < count; ++i) {
        u32 handle;
        std::memcpy(&handle, response.data() + 12 + i * 8, sizeof(handle));
        u32 address = 0;
        if (map) {
            const auto assigned = context.system.getGpu().mapHost1x(Handle(handle));
            if (!assigned)
                return NvResult::InsufficientMemory;
            address = *assigned;
        }
        std::memcpy(response.data() + 16 + i * 8, &address, sizeof(address));
    }
    std::memcpy(output.getPtr() + output.getSeek(), response.data(), size);
    return NvResult::Success;
}
} // namespace

NvResult ChannelBase::mapCmdBuffer(IoctlContext* context) {
    return mapBuffers(*context, true);
}

NvResult ChannelBase::unmapCmdBuffer(IoctlContext* context) {
    // No asynchronous Host1x worker yet; preserve the stable address assignment.
    return mapBuffers(*context, false);
}

NvResult ChannelBase::setUserData(u64 data) {
    user_data = data;
    return NvResult::Success;
}

NvResult ChannelBase::getUserData(u64* out_data) const {
    *out_data = user_data;
    return NvResult::Success;
}

NvResult ChannelBase::setNvMapFd(u32 fd_id) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "FD: {}", fd_id);
    return NvResult::Success;
}

NvResult ChannelBase::setTimeout(u32 timeout) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "timeout: {}", timeout);
    return NvResult::Success;
}

NvResult ChannelBase::setPriority(u32 priority) {
    LOG_FUNC_WITH_ARGS_STUBBED(Services, "priority: {}", priority);
    return NvResult::Success;
}

} // namespace hydra::horizon::services::nvdrv::ioctl
