#pragma once

#include "core/horizon/services/nvdrv/ioctl/const.hpp"

namespace hydra::horizon::kernel {
class Event;
}

namespace hydra::hw::tegra_x1::gpu { class GMmu; }

namespace hydra::horizon::services::nvdrv::ioctl {

class FdBase {
  public:
    virtual ~FdBase() noexcept = default;
    virtual std::string_view videoEngineName() const { return {}; }
    virtual NvResult bindAddressSpace(
        [[maybe_unused]] kernel::Process& process,
        [[maybe_unused]] std::shared_ptr<hw::tegra_x1::gpu::GMmu> space) {
        return NvResult::BadParameter;
    }

    virtual NvResult ioctl(IoctlContext& context, u32 type, u32 nr) = 0;
    virtual NvResult ioctl2([[maybe_unused]] IoctlContext& context, u32 type,
                            u32 nr) {
        LOG_WARN(Services,
                 "Ioctl2 not supported by this FD (type: {:x}, NR: {:x})", type,
                 nr);
        return NvResult::NotSupported;
    }
    virtual NvResult ioctl3([[maybe_unused]] IoctlContext& context, u32 type,
                            u32 nr) {
        LOG_WARN(Services,
                 "Ioctl2 not supported by this FD (type: {:x}, NR: {:x})", type,
                 nr);
        return NvResult::NotSupported;
    }
    virtual NvResult queryEvent(u32 event_id_u32, kernel::Event*& out_event) {
        (void)out_event;
        LOG_WARN(Services, "Unknown event id {}", event_id_u32);
        return NvResult::NotSupported;
    }
};

} // namespace hydra::horizon::services::nvdrv::ioctl
