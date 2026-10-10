#pragma once

#include "core/audio/const.hpp"

namespace hydra::audio {

using buffer_id_t = u64;

using buffer_finished_callback_fn_t = std::function<void(buffer_id_t)>;

struct StreamStats {
    bool has_sample_clock{false};
    u64 requested_frames{0};
    u64 missing_frames{0};
    u64 sample_clock_frames{0};
};

class IStream {
  public:
    IStream(PcmFormat format_, u32 sample_rate_, u16 channel_count_,
            buffer_finished_callback_fn_t buffer_finished_callback_)
        : format{format_}, sample_rate{sample_rate_},
          channel_count{channel_count_},
          buffer_finished_callback{std::move(buffer_finished_callback_)} {}
    virtual ~IStream() noexcept = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    virtual void enqueueBuffer(buffer_id_t id, std::span<const u8> buffer) = 0;

    virtual StreamStats getStats() const { return {}; }

    StreamState getState() const { return state; }

  protected:
    PcmFormat format;
    u32 sample_rate;
    u16 channel_count;
    buffer_finished_callback_fn_t buffer_finished_callback;

    std::atomic<StreamState> state{StreamState::Stopped};
};

} // namespace hydra::audio
