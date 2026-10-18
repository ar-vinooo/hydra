#pragma once

#include "core/horizon/services/nvdrv/ioctl/fd_base.hpp"
#include "core/hw/tegra_x1/gpu/const.hpp"
#include "core/video/host1x.hpp"

namespace hydra::horizon::services::nvdrv::ioctl {

struct MapCmdBufferHandle {
    Handle in_map_handle;
    u32 out_phys_addr;
};

struct UnmapCmdBufferHandle {
    Handle in_map_handle;
    u32 _reserved_x4;
};

class ChannelBase : public FdBase {
  public:
    NvResult ioctl([[maybe_unused]] IoctlContext& context, u32 type,
                   u32 nr) override;

  protected:
    u64 user_data = 0;
    std::optional<u32> video_syncpoint;
    video::Host1xState host1x_state;
    std::array<u32, 0x800> video_registers{};
    virtual NvResult executeVideo(IoctlContext&) { return NvResult::Success; }

    // Ioctls
    // TODO: args
    NvResult submit(IoctlContext* context);
    virtual NvResult getSyncpoint(IoctlContext* context, u32 module_id, u32* out_value);
    NvResult getWaitBase(u32 module_id, u32* out_value);
    NvResult setSubmitTimeout(u32 timeout);
    NvResult mapCmdBuffer(IoctlContext* context);
    NvResult unmapCmdBuffer(IoctlContext* context);
    NvResult setUserData(u64 data);
    NvResult getUserData(u64* out_data) const;
    NvResult setNvMapFd(u32 fd_id);
    NvResult setTimeout(u32 timeout);
    virtual NvResult
    submitGpfifo([[maybe_unused]] System* system,
                 [[maybe_unused]] kernel::Process* process, u64 gpfifo,
                 u32 num_entries,
                 InOut<hw::tegra_x1::gpu::GpfifoFlags, u32>
                     inout_flags_and_detailed_error,
                 InOutSingle<hw::tegra_x1::gpu::Fence> inout_fence,
                 const hw::tegra_x1::gpu::GpfifoEntry* entries) {
        std::span<const hw::tegra_x1::gpu::GpfifoEntry> entries_span(
            entries, num_entries);
        LOG_FATAL(Services,
                  "SubmitGpfifo not available (gpfifo: {}, "
                  "flags: {}, fence: {}, entries: [{}])",
                  gpfifo, inout_flags_and_detailed_error, inout_fence,
                  fmt::join(entries_span, ", "));
    }
    virtual NvResult allocObjCtx(u32 class_num, u32 flags, u64* out_obj_id) {
        (void)out_obj_id;
        LOG_FATAL(Services,
                  "AllocObjCtx not available (class_num: {}, flags: {:x})",
                  class_num, flags);
    }
    virtual NvResult zCullBind(gpu_vaddr_t addr, u32 mode,
                               [[maybe_unused]] u32 reserved) {
        LOG_FATAL(Services, "ZCullBind not available (addr: {:x}, mode: {})",
                  addr, mode);
    }
    virtual NvResult setErrorNotifier(u64 offset, u64 size, u32 mem,
                                      [[maybe_unused]] u32 reserved) {
        LOG_FATAL(Services,
                  "SetErrorNotifier not available (offset: {:x}, size: {:x}, "
                  "mem: {})",
                  offset, size, mem);
    }
    NvResult setPriority(u32 priority);
    virtual NvResult getErrorNotification(u64* out_timestamp, u32* out_info32,
                                          u16* out_info16, u16* out_status) {
        (void)out_timestamp;
        (void)out_info32;
        (void)out_info16;
        (void)out_status;
        LOG_FATAL(Services, "GetErrorNotification not available");
    }
    virtual NvResult
    allocGpfifoEx([[maybe_unused]] IoctlContext* context, u32 num_entries, u32 num_jobs, u32 flags,
                  hw::tegra_x1::gpu::Fence* out_fence,
                  [[maybe_unused]] std::array<u32, 3> reserved) {
        (void)out_fence;
        LOG_FATAL(Services,
                  "AllocGpfifoEX not available (num_entries: {}, "
                  "num_jobs: {}, flags: {:x})",
                  num_entries, num_jobs, flags);
    }
};

} // namespace hydra::horizon::services::nvdrv::ioctl

ENABLE_STRUCT_FORMATTING(
    hydra::horizon::services::nvdrv::ioctl::MapCmdBufferHandle, in_map_handle,
    "", "in map handle")

ENABLE_STRUCT_FORMATTING(
    hydra::horizon::services::nvdrv::ioctl::UnmapCmdBufferHandle, in_map_handle,
    "", "in map handle")
