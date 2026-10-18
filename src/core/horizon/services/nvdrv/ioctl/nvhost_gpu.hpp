#pragma once

#include "core/horizon/kernel/kernel.hpp"
#include "core/horizon/services/nvdrv/ioctl/channel_base.hpp"
#include "core/hw/tegra_x1/gpu/pfifo.hpp"

namespace hydra::horizon::services::nvdrv::ioctl {

class NvHostGpu : public ChannelBase {
  public:
    NvHostGpu();
    ~NvHostGpu() override;

    NvResult ioctl(IoctlContext& context, u32 type, u32 nr) override;
    NvResult ioctl2(IoctlContext& context, u32 type,
                    u32 nr) override;
    NvResult queryEvent(u32 event_id_u32, kernel::Event*& out_event) override;
    const auto& getChannelState() const { return decode_state; }
    NvResult bindAddressSpace(kernel::Process& process,
                               std::shared_ptr<hw::tegra_x1::gpu::GMmu> space) override;

  private:
    kernel::Event* sm_exception_bpt_int_report_event;
    kernel::Event* sm_exception_bpt_pause_report_event;
    kernel::Event* error_notifier_event;
    std::mutex address_space_mutex;
    kernel::Process* address_space_owner{nullptr};
    std::shared_ptr<hw::tegra_x1::gpu::PfifoDecodeState> decode_state{
        std::make_shared<hw::tegra_x1::gpu::PfifoDecodeState>()};

    struct ErrorNotifier {
        std::mutex mutex;
        bool enabled{false};
    };
    std::shared_ptr<ErrorNotifier> error_notifier{std::make_shared<ErrorNotifier>()};

    // Ioctls
    NvResult
    submitGpfifo(System* system, kernel::Process* process, u64 gpfifo,
                 u32 num_entries,
                 InOut<hw::tegra_x1::gpu::GpfifoFlags, u32>
                     inout_flags_and_detailed_error,
                 InOutSingle<hw::tegra_x1::gpu::Fence> inout_fence,
                 const hw::tegra_x1::gpu::GpfifoEntry* entries) override;
    NvResult allocObjCtx(u32 class_num, u32 flags, u64* out_obj_id) override;
    NvResult getSyncpoint(IoctlContext* context, u32 module_id, u32* out_value) override;
    NvResult zCullBind(gpu_vaddr_t addr, u32 mode,
                       [[maybe_unused]] u32 reserved) override;
    NvResult setErrorNotifier(u64 offset, u64 size, u32 mem,
                              u32 reserved) override;
    NvResult getErrorNotification(u64* out_timestamp, u32* out_info32,
                                  u16* out_info16, u16* out_status) override;
    NvResult
    allocGpfifoEx(IoctlContext* context, u32 num_entries, u32 num_jobs, u32 flags,
                  hw::tegra_x1::gpu::Fence* out_fence,
                  [[maybe_unused]] std::array<u32, 3> reserved) override;

    NvResult submitGpfifoChecked(IoctlContext& context, bool inline_entries);
};

} // namespace hydra::horizon::services::nvdrv::ioctl
