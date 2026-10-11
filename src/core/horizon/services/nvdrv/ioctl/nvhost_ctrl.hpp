#pragma once

#include "core/horizon/services/nvdrv/ioctl/fd_base.hpp"

namespace hydra::video { class Syncpoints; }

namespace hydra::horizon::services::nvdrv::ioctl {

struct NvHostEvent {
    kernel::Event* event{nullptr};
    u64 wait_token{0};
    // TODO: more
};

class NvHostCtrl : public FdBase {
  public:
    ~NvHostCtrl() override;
    NvResult ioctl([[maybe_unused]] IoctlContext& context, u32 type,
                   u32 nr) override;
    NvResult queryEvent(u32 event_id_u32, kernel::Event*& out_event) override;

  private:
    constexpr static usize EVENT_COUNT = 64;

    NvHostEvent events[EVENT_COUNT] = {{}};
    video::Syncpoints* syncpoints{nullptr};

    NvResult syncptRead(IoctlContext* context, u32 id, u32* out_value);

    // Ioctls
    NvResult getConfig(std::array<char, 0x41> name, std::array<char, 0x41> key,
                       std::array<u8, 0x101>* out_value);
    NvResult syncptWaitEvent(IoctlContext* context, u32 id, u32 tresh, i32 timeout, u32* out_value);
    NvResult syncptWaitEventEx(IoctlContext* context, u32 id, u32 tresh, i32 timeout,
                               InOutSingle<u32> out_value);
    NvResult syncptAllocEvent(u32 slot);
    NvResult syncptFreeEvent(u32 slot);
};

} // namespace hydra::horizon::services::nvdrv::ioctl
