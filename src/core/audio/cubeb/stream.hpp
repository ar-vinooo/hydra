#pragma once

#include "core/audio/cubeb/const.hpp"
#include "core/audio/pcm_queue.hpp"
#include "core/audio/stream.hpp"

struct cubeb_stream;

namespace hydra::audio::cubeb {

class Core;

class Stream final : public IStream {
  public:
    Stream(Core& core_, PcmFormat format, u32 sample_rate, u16 channel_count,
           buffer_finished_callback_fn_t buffer_finished_callback);
    ~Stream() override;

    void start() override;
    void stop() override;

    void enqueueBuffer(buffer_id_t id, std::span<const u8> buffer) override;

    StreamStats getStats() const override;

  private:
    Core& core;

    cubeb_stream* stream;

    PcmQueue buffer_queue;
    std::atomic<u64> requested_frames{0};
    std::atomic<u64> missing_frames{0};
    mutable std::mutex clock_mutex;
    std::chrono::steady_clock::time_point callback_time{};
    u64 callback_frames{0};
    mutable u64 last_sample_clock{0};

    static long dataCallback(cubeb_stream* stream, void* user_data,
                             const void* input_buffer, void* output_buffer,
                             long num_frames);
    static void stateCallback(cubeb_stream* stream, void* user_data,
                              cubeb_state state);
};

} // namespace hydra::audio::cubeb
