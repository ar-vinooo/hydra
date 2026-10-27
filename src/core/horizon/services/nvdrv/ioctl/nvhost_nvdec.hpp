#pragma once

#include "core/horizon/services/nvdrv/ioctl/channel_base.hpp"

namespace hydra::horizon::services::nvdrv::ioctl {

class NvHostNvDec : public ChannelBase {
  public:
    NvHostNvDec();
    ~NvHostNvDec() override;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    NvResult executeVideo(IoctlContext& context) override;
    std::string_view videoEngineName() const override { return "nvdec"; }
    // Ioctls
};

} // namespace hydra::horizon::services::nvdrv::ioctl
