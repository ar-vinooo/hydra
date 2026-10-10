#include "core/audio/cubeb/stream.hpp"

#include <utility>

#include "core/audio/cubeb/core.hpp"
#include "core/audio/sample_clock.hpp"

namespace hydra::audio::cubeb {

namespace {

cubeb_sample_format toCubebFormat(const PcmFormat format) {
    // TODO: more
    switch (format) {
    case PcmFormat::Int16:
        return CUBEB_SAMPLE_S16NE; // TODO: correct?
    case PcmFormat::Float:
        return CUBEB_SAMPLE_FLOAT32NE; // TODO: correct?
    default:
        LOG_NOT_IMPLEMENTED(Cubeb, "PCM format {}", format);
        return CUBEB_SAMPLE_S16NE;
    }
}

cubeb_channel_layout toCubebLayout(u16 channel_count) {
    switch (channel_count) {
    case 1:
        return CUBEB_LAYOUT_MONO;
    case 2:
        return CUBEB_LAYOUT_STEREO;
    case 6:
        return CUBEB_LAYOUT_3F2_LFE;
    default:
        return CUBEB_LAYOUT_UNDEFINED;
    }
}

} // namespace

Stream::Stream(Core& core_, PcmFormat format, u32 sample_rate,
               u16 channel_count,
               buffer_finished_callback_fn_t buffer_finished_callback)
    : IStream(format, sample_rate, channel_count,
              std::move(buffer_finished_callback)),
      core{core_} {
    if (channel_count != 1 && channel_count != 2 && channel_count != 6)
        LOG_NOT_IMPLEMENTED(Cubeb, "Channel count {}", channel_count);

    cubeb_stream_params params{};
    params.format = toCubebFormat(format);
    params.rate = sample_rate;
    params.channels = channel_count;
    params.layout = toCubebLayout(channel_count);
    params.prefs = CUBEB_STREAM_PREF_NONE;

    // TODO: device
    // TODO: buffer size
    const auto res = cubeb_stream_init(
        core.context, &stream, "Hydra stream", nullptr, nullptr, nullptr,
        &params, 512, &Stream::dataCallback, &Stream::stateCallback, this);
    // TODO: format result
    ASSERT(res == CUBEB_OK, Cubeb, "Failed to initialize cubeb stream: {}",
           res);
}

Stream::~Stream() { cubeb_stream_destroy(stream); }

void Stream::start() { cubeb_stream_start(stream); }

void Stream::stop() {
    // TODO: wait to finish?
    cubeb_stream_stop(stream);
}

void Stream::enqueueBuffer(buffer_id_t id, std::span<const u8> buffer) {
    buffer_queue.enqueue(id, buffer);
}

StreamStats Stream::getStats() const {
    std::lock_guard lock(clock_mutex);
    const auto requested = requested_frames.load(std::memory_order_relaxed);
    const auto elapsed =
        callback_time == std::chrono::steady_clock::time_point{}
            ? 0
            : std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - callback_time)
                  .count();
    last_sample_clock = std::max(
        last_sample_clock,
        interpolateSampleClock(requested, callback_frames,
                               static_cast<u64>(std::max<i64>(elapsed, 0)),
                               sample_rate));
    return {true, requested, missing_frames.load(std::memory_order_relaxed),
            last_sample_clock};
}

long Stream::dataCallback(cubeb_stream* stream, void* user_data,
                          const void* input_buffer, void* output_buffer,
                          long num_frames) {
    (void)stream;
    (void)input_buffer;

    auto self = reinterpret_cast<Stream*>(user_data);

    if (num_frames <= 0)
        return 0;
    const usize sample_bytes = self->format == PcmFormat::Float ? 4 : 2;
    const usize frame_bytes = self->channel_count * sample_bytes;
    usize bytes_read = 0;
    const auto finished = self->buffer_queue.read(
        std::span(static_cast<u8*>(output_buffer),
                  static_cast<usize>(num_frames) * frame_bytes),
        &bytes_read);
    {
        std::lock_guard lock(self->clock_mutex);
        self->requested_frames.fetch_add(static_cast<u64>(num_frames),
                                         std::memory_order_relaxed);
        self->callback_frames = static_cast<u64>(num_frames);
        self->callback_time = std::chrono::steady_clock::now();
    }
    self->missing_frames.fetch_add(static_cast<u64>(num_frames) -
                                       bytes_read / frame_bytes,
                                   std::memory_order_relaxed);
    // No queue lock is held while notifying the producer.
    if (self->buffer_finished_callback)
        for (const auto id : finished)
            self->buffer_finished_callback(id);

    return num_frames;
}

void Stream::stateCallback(cubeb_stream* stream, void* user_data,
                           cubeb_state state) {
    (void)stream;

    auto self = reinterpret_cast<Stream*>(user_data);

    switch (state) {
    case CUBEB_STATE_STARTED:
        self->state = StreamState::Started;
        break;
    case CUBEB_STATE_STOPPED:
        self->state = StreamState::Stopped;
        break;
    case CUBEB_STATE_DRAINED:
        self->state = StreamState::Drained;
        break;
    case CUBEB_STATE_ERROR:
        self->state = StreamState::Error;
        break;
    }
}

} // namespace hydra::audio::cubeb
