#pragma once

#include "core/horizon/kernel/kernel.hpp"
#include "core/horizon/services/nvdrv/ioctl/channel_base.hpp"

namespace hydra::horizon::services::nvdrv::ioctl {

class NvHostVic : public ChannelBase {
  private:
    std::string_view videoEngineName() const override { return "vic"; }
    NvResult executeVideo(IoctlContext& context) override;
};

} // namespace hydra::horizon::services::nvdrv::ioctl
