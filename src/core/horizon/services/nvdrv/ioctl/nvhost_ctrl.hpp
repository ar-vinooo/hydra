#pragma once

#include "core/horizon/services/nvdrv/ioctl/fd_base.hpp"

namespace hydra::video { class Syncpoints; }

namespace hydra::horizon::services::nvdrv::ioctl {

class NvHostEvent {
  public:
    kernel::Event* event{nullptr};

    // The owning controller serializes slot operations; callbacks use WaitState.
    u64 cancelWait(video::Syncpoints* syncpoints);
    bool armWait(video::Syncpoints& syncpoints, u32 id, u32 threshold);

  private:
    u64 wait_token{0};
    struct WaitState {
        std::mutex mutex;
        u64 generation{0};
    };
    std::shared_ptr<WaitState> wait_state{std::make_shared<WaitState>()};
};

class NvHostCtrl : public FdBase {
  public:
    ~NvHostCtrl() override;
    NvResult ioctl([[maybe_unused]] IoctlContext& context, u32 type,
                   u32 nr) override;
    NvResult queryEvent(u32 event_id_u32, kernel::Event*& out_event) override;

  private:
    constexpr static usize EVENT_COUNT = 64;

    std::mutex events_mutex;
    std::array<NvHostEvent, EVENT_COUNT> events{};
    video::Syncpoints* syncpoints{nullptr};

    NvResult syncptRead(IoctlContext* context, u32 id, u32* out_value);
    NvResult syncptSignalEvent(u32 event_id);

    // Ioctls
    NvResult getConfig(std::array<char, 0x41> name, std::array<char, 0x41> key,
                       std::array<u8, 0x101>* out_value);
    NvResult syncptWaitEvent(IoctlContext* context, u32 id, u32 threshold, i32 timeout, u32* out_value);
    NvResult syncptWaitEventEx(IoctlContext* context, u32 id, u32 threshold, i32 timeout,
                               InOutSingle<u32> out_value);
    NvResult syncptAllocEvent(u32 slot);
    NvResult syncptFreeEvent(u32 slot);
};

} // namespace hydra::horizon::services::nvdrv::ioctl
