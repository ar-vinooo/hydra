#include "core/horizon/services/audio/audio_renderer.hpp"

#include "core/audio/pcm_queue.hpp"
#include "core/audio/renderer_dsp.hpp"
#include "core/audio/sample_clock.hpp"
#include "core/audio/sample_decoder.hpp"
#include "core/debugger/debugger_manager.hpp"
#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/system.hpp"
#include <cmath>
#include <fstream>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

namespace hydra::horizon::services::audio {

namespace {

#pragma pack(push, 1)
struct UpdateDataHeader {
    u32 revision;
    u32 behavior_size;
    u32 mempools_size;
    u32 voices_size;
    u32 channels_size;
    u32 effects_size;
    u32 mixes_size;
    u32 sinks_size;
    u32 performance_manager_size;
    u32 _unknown;
    u32 render_info_size;
    u32 _reserved[4];
    u32 total_size;
};

enum class MemPoolState : u32 {
    Invalid,
    New,
    RequestDetach,
    Detached,
    RequestAttach,
    Attached,
    Released,
};

struct MemPoolInfoIn {
    vaddr_t address;
    u64 size;
    MemPoolState state;
    u32 _padding[3];
};

struct MemPoolInfoOut {
    MemPoolState new_state;
    u32 _padding[3];
};

enum class VoicePlayState : u8 {
    Started = 0,
    Stopped = 1,
    Paused = 2,
};

struct BiquadFilter {
    bool enable;
    u8 _padding;
    i16 numerator[3];
    i16 denominator[2];
};

struct WaveBuffer {
    vaddr_t address;
    u64 size;
    u32 start_sample_offset;
    u32 end_sample_offset;
    bool is_looping;
    bool end_of_stream;
    bool sent_to_server;
    u8 _reserved;
    // Loops after the first pass, negative: forever
    i32 loop_count;
    vaddr_t context_addr;
    u64 context_sz;
    // The range of the loops, when not zero
    u32 loop_first_sample_offset;
    u32 loop_last_sample_offset;
};

struct VoiceInfoIn {
    u32 id;
    u32 node_id;
    bool is_new;
    bool is_used;
    VoicePlayState play_state;
    PcmFormat sample_format : 8;
    u32 sample_rate;
    u32 priority;
    u32 sorting_order;
    u32 channel_count;
    f32 pitch;
    f32 volume;
    BiquadFilter biquads[2];
    u32 wave_buffer_count;
    i16 wave_buffer_head;
    u16 _padding1;
    u32 _padding2;
    const void* extra_params_ptr;
    u64 extra_params_sz;
    u32 dest_mix_id;
    u32 dest_splitter_id;
    WaveBuffer wave_buffers[4];
    u32 channel_ids[6];
    bool reset_voice_drop;
    u8 flush_wave_buffer_count;
    u16 _reserved2;
    u16 decoding_flags;
    u8 src_quality;
    u32 external_context;
    u32 external_context_size;
    u8 _reserved3[9];
};

struct ChannelInfoIn {
    u32 id;
    f32 mix[24];
    bool is_used;
    u8 padding[11];
};

struct MixInfoIn {
    f32 volume;
    u32 sample_rate;
    u32 buffer_count;
    bool is_used;
    u8 padding1[3];
    u32 mix_id;
    u32 padding2;
    u32 node_id;
    u32 padding3[2];
    f32 mix[24][24];
    u32 dest_mix_id;
    u32 dest_splitter_id;
    u32 padding4;
};

struct MixUpdateHeader {
    u32 magic;
    i32 count;
    u8 reserved[24];
};
static_assert(sizeof(MixUpdateHeader) == 0x20);

struct SinkInfoIn {
    u8 type;
    bool is_used;
    u8 padding1[2];
    u32 node_id;
    u64 padding2[3];
    u8 data[288];
};

static_assert(sizeof(VoiceInfoIn) == 0x170);
static_assert(sizeof(BiquadFilter) == 0xc);

constexpr u32 UNUSED_MIX_ID = 0x7fffffff;
constexpr u32 UNUSED_SPLITTER_ID = 0xffffffff;
static_assert(sizeof(ChannelInfoIn) == 0x70);
static_assert(sizeof(MixInfoIn) == 0x930);
static_assert(sizeof(SinkInfoIn) == 0x140);

// Guest pages need not be contiguous in host memory.
std::vector<u8> readGuest(kernel::Process* process, vaddr_t address, u64 size) {
    if (!address || size > 16 * 1024 * 1024 || address + size < address)
        return {};
    std::vector<u8> bytes(static_cast<usize>(size));
    usize offset = 0;
    while (offset < bytes.size()) {
        const auto ptr = process->getMmu()->unmapAddr(address + offset);
        if (!ptr)
            return {};
        const auto count = std::min<usize>(4096 - ((address + offset) & 4095),
                                           bytes.size() - offset);
        std::memcpy(bytes.data() + offset, reinterpret_cast<const void*>(ptr),
                    count);
        offset += count;
    }
    return bytes;
}

// Guest memory of the client for the aux and capture effects, page by page
// (guest pages need not be contiguous in host memory)
class ProcessMemory final : public audio::dsp::effects::GuestMemory {
  public:
    explicit ProcessMemory(kernel::Process* process_) : process{process_} {}

    bool read(u64 address, void* data, u64 size) override {
        return access(address, size, [&](u8* host, u64 offset, u64 count) {
            std::memcpy(static_cast<u8*>(data) + offset, host, count);
        });
    }
    bool write(u64 address, const void* data, u64 size) override {
        return access(address, size, [&](u8* host, u64 offset, u64 count) {
            std::memcpy(host, static_cast<const u8*>(data) + offset, count);
        });
    }

  private:
    kernel::Process* process;

    template <typename F>
    bool access(u64 address, u64 size, F&& copy) {
        if (process == nullptr || address + size < address)
            return false;
        u64 offset = 0;
        while (offset < size) {
            const auto ptr = process->getMmu()->unmapAddr(address + offset);
            if (!ptr)
                return false;
            const u64 count =
                std::min<u64>(4096 - ((address + offset) & 4095), size - offset);
            copy(reinterpret_cast<u8*>(ptr), offset, count);
            offset += count;
        }
        return true;
    }
};

struct SinkInfoOut {
    u32 last_written_offset;
    u32 _padding;
    u64 _reserved[3];
};

struct ErrorInfo {
    result_t result;
    u32 _padding;
    u64 extra_error_info;
};

struct BehaviorInfoOut {
    ErrorInfo error_infos[10];
    u32 error_info_count;
    u32 _reserved[3];
};

struct RenderInfoOut {
    u64 elapsed_frame_count;
    u64 _reserved;
};

struct PerformanceInfoOut {
    u32 history_size;
    u32 _reserved[3];
};
#pragma pack(pop)

} // namespace

} // namespace hydra::horizon::services::audio

ENABLE_ENUM_FORMATTING(hydra::horizon::services::audio::MemPoolState, Invalid,
                       "invalid", New, "new", RequestDetach, "request detach",
                       Detached, "detached", RequestAttach, "request attach",
                       Attached, "attached", Released, "released")

namespace hydra::horizon::services::audio {

struct IAudioRenderer::RenderState {
    std::vector<VoiceInfoIn> voice_inputs;
    std::vector<ChannelInfoIn> channels;
    std::vector<MixInfoIn> mixes;
    std::vector<SinkInfoIn> sinks;
};

DEFINE_SERVICE_COMMAND_TABLE(IAudioRenderer, 4, requestUpdate, 5, start, 6,
                             stop, 7, querySystemEvent, 8,
                             setRenderingTimeLimit, 9, getRenderingTimeLimit,
                             10, requestUpdateAuto)

IAudioRenderer::IAudioRenderer(System& system,
                               const AudioRendererParameters& params_,
                               const u64 work_buffer_size_)
    : params{params_}, work_buffer_size{work_buffer_size_},
      event{new kernel::Event(false, "IAudioRenderer event")} {
    render_state = std::make_unique<RenderState>();
    render_state->voice_inputs.resize(params.voice_count);
    voices.resize(params.voice_count);
    voice_states.resize(params.voice_count);
    effects.resize(params.effect_count);
    effect_behaviour =
        audio::dsp::effects::Behaviour::fromRevision(params.revision);
    if (isAudioRendererFeatureSupported(AudioFeature::Splitter,
                                        params.revision) &&
        params.splitter_count != 0 && params.splitter_destination_count != 0) {
        splitters.resize(params.splitter_count);
        splitter_destinations.resize(params.splitter_destination_count);
    }
    LOG_INFO(Services,
             "Audio renderer: revision 0x{:08x}, rate {}, samples/frame {}, "
             "voices {}, submixes {}, mix buffers {}, sinks {}",
             params.revision, params.sample_rate, params.sample_count,
             params.voice_count, params.submix_count, params.mix_buffer_count,
             params.sink_count);

    output_stream.reset(system.getAudioCore().createStream(
        PcmFormat::Float, params.sample_rate, 2, [this](buffer_id_t samples) {
            queued_samples.fetch_sub(samples, std::memory_order_relaxed);
        }));
    signal_thread = std::jthread([this](std::stop_token token) {
#ifdef __APPLE__
        // The DSP's stand-in: late wakes underrun the device (crackles and
        // stretched sound), it must not wait behind emulation threads or run
        // on efficiency cores
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        auto next_tick = std::chrono::steady_clock::now();
        auto last_signal = next_tick;
        while (!token.stop_requested()) {
            const bool rendered = renderAudio();
            const auto now = std::chrono::steady_clock::now();
            // An event per DSP frame, and one per period while none renders
            if (rendered || now - last_signal >= std::chrono::milliseconds(5)) {
                event->signal();
                last_signal = now;
            }
            next_tick = std::max(next_tick + std::chrono::milliseconds(1), now);
            std::this_thread::sleep_until(next_tick);
        }
    });
}

IAudioRenderer::~IAudioRenderer() {
    signal_thread.request_stop();
    signal_thread.join();
    output_stream.reset();
    event->release();
}

result_t IAudioRenderer::start() {
    std::lock_guard lock(render_mutex);
    LOG_INFO(Services, "Audio renderer start: {} Hz", params.sample_rate);
    const auto stats = output_stream->getStats();
    sample_clock_origin = stats.sample_clock_frames;
    missing_frames_origin = stats.missing_frames;
    produced_samples = 0;
    last_render_time = {};
    running = true;
    last_update_time = std::chrono::steady_clock::now();
    output_stream->start();
    return RESULT_SUCCESS;
}

result_t IAudioRenderer::stop() {
    std::lock_guard lock(render_mutex);
    running = false;
    output_stream->stop();
    return RESULT_SUCCESS;
}

result_t
IAudioRenderer::requestUpdate(kernel::Process* process,
                              InBuffer<BufferAttr::MapAlias> in_buffer,
                              OutBuffer<BufferAttr::MapAlias> out_buffer,
                              OutBuffer<BufferAttr::MapAlias> out_perf_buffer) {
    return requestUpdateImpl(process, in_buffer.stream, out_buffer.stream,
                             out_perf_buffer.stream);
}

result_t
IAudioRenderer::querySystemEvent(kernel::Process* process,
                                 OutHandle<HandleAttr::Copy> out_handle) {
    out_handle = process->addHandle(event);
    return RESULT_SUCCESS;
}

result_t IAudioRenderer::setRenderingTimeLimit(u32 time_limit) {
    rendering_time_limit = time_limit;
    return RESULT_SUCCESS;
}

result_t IAudioRenderer::getRenderingTimeLimit(u32* out_time_limit) const {
    *out_time_limit = rendering_time_limit;
    return RESULT_SUCCESS;
}

result_t IAudioRenderer::requestUpdateAuto(
    kernel::Process* process, InBuffer<BufferAttr::AutoSelect> in_buffer,
    OutBuffer<BufferAttr::AutoSelect> out_buffer,
    OutBuffer<BufferAttr::AutoSelect> out_perf_buffer) {
    return requestUpdateImpl(process, in_buffer.stream, out_buffer.stream,
                             out_perf_buffer.stream);
}

IAudioRenderer::SplitterDestinationState*
IAudioRenderer::getSplitterDestination(u32 splitter, u32 index) {
    if (splitter >= splitters.size())
        return nullptr;
    const auto& destinations = splitters[splitter].destinations;
    if (index >= destinations.size() ||
        destinations[index] >= splitter_destinations.size())
        return nullptr;
    return &splitter_destinations[destinations[index]];
}

std::optional<u64> IAudioRenderer::updateSplitters(const u8* data, u64 size,
                                                   u64 offset, bool apply) {
    if (splitters.empty() || splitter_destinations.empty())
        return offset;
    const auto read = [&](u64 at, void* out, u64 bytes) {
        if (at > size || bytes > size - at)
            return false;
        std::memcpy(out, data + at, bytes);
        return true;
    };
    struct Header {
        u32 magic;
        u32 splitter_count;
        u32 destination_count;
        u32 _reserved[5];
    };
    struct SplitterIn {
        u32 magic;
        i32 id;
        u32 sample_rate;
        i32 destination_count;
    };
    struct DestinationIn {
        u32 magic;
        i32 id;
        f32 volume[24];
        i32 mix_id;
    };
    static_assert(sizeof(Header) == 0x20 && sizeof(SplitterIn) == 0x10 &&
                  sizeof(DestinationIn) == 0x6c);
    Header header;
    if (!read(offset, &header, sizeof(header)) || header.magic != 0x48444E53)
        return std::nullopt;
    u64 cursor = offset + sizeof(header);
    const u32 revision = audioRevisionNumber(params.revision);
    // Before REV5, a splitter never has more destinations than its share
    const bool trust_count = revision >= 5;
    const u32 share =
        static_cast<u32>(splitter_destinations.size() / splitters.size());
    for (u32 i = 0; i < header.splitter_count; i++) {
        SplitterIn in;
        if (!read(cursor, &in, sizeof(in)))
            return std::nullopt;
        if (in.magic != 0x49444E53)
            break;
        cursor += sizeof(in);
        const u32 listed = static_cast<u32>(std::max(in.destination_count, 0));
        const u32 used = trust_count ? listed : std::min(share, listed);
        if (static_cast<u64>(listed) * 4 > size - std::min(cursor, size))
            return std::nullopt;
        if (apply && in.id >= 0 && static_cast<usize>(in.id) < splitters.size()) {
            auto& splitter = splitters[static_cast<usize>(in.id)];
            splitter.sample_rate = in.sample_rate;
            splitter.destinations.clear();
            for (u32 d = 0; d < used; d++) {
                i32 id;
                read(cursor + d * 4ull, &id, 4);
                splitter.destinations.push_back(static_cast<u32>(id));
            }
        }
        // The destination ids, then 12 unused bytes
        cursor += listed * 4ull + 0xc;
    }
    // REV12 destinations carry two biquad filters
    const bool version2 = revision >= 12;
    const u64 destination_size = version2 ? 0x90 : 0x70;
    for (u32 i = 0; i < header.destination_count; i++) {
        DestinationIn in;
        if (!read(cursor, &in, sizeof(in)))
            return std::nullopt;
        if (in.magic != 0x44444E53)
            break;
        if (cursor + destination_size > size)
            return std::nullopt;
        if (apply && in.id >= 0 &&
            static_cast<usize>(in.id) < splitter_destinations.size()) {
            auto& destination = splitter_destinations[static_cast<usize>(in.id)];
            bool used = false;
            read(cursor + (version2 ? 0x84 : 0x6c), &used, 1);
            if (version2)
                read(cursor + 0x6c, destination.biquads.data(), 24);
            destination.mix_id = static_cast<u32>(in.mix_id);
            for (u32 b = 0; b < 24; b++)
                destination.volume[b] =
                    std::isfinite(in.volume[b]) ? in.volume[b] : 0.0f;
            // A destination taken into use starts at its volume
            if (!destination.used && used) {
                destination.previous = destination.volume;
                destination.need_update = false;
            }
            destination.used = used;
        }
        cursor += destination_size;
    }
    return (cursor + 0xf) & ~u64{0xf};
}

bool IAudioRenderer::renderAudio() {
    std::lock_guard lock(render_mutex);
    if (!running || render_state->mixes.empty())
        return false;
    // The DSP renders a frame (5 ms) per period of the audio device. Here a
    // frame is due while less than the lead (forty milliseconds) is queued
    // ahead of what the device played.
    constexpr auto FRAME_DURATION = std::chrono::milliseconds(5);
    const auto now = std::chrono::steady_clock::now();
    const u64 frame_samples = params.sample_count;
    bool due = false;
    bool behind = false;
    const auto stream_stats = output_stream->getStats();
    if (stream_stats.has_sample_clock) {
        // Keep the guest's DSP time independent from host queue accounting.
        // The initial eight-frame lead matches the renderer's startup lead.
        elapsed_frames.store(elapsedDspFrames(stream_stats.sample_clock_frames,
                                              sample_clock_origin,
                                              params.sample_count),
                             std::memory_order_relaxed);
        // Consumed PCM, not silence produced by device underruns: otherwise
        // every underrun permanently grows the actual queue
        const u64 played = consumedSampleClock(
            stream_stats.sample_clock_frames, sample_clock_origin,
            stream_stats.missing_frames, missing_frames_origin);
        due = framesForSampleClock(played, produced_samples,
                                   params.sample_count, 1) != 0;
        behind = produced_samples < played + 2 * frame_samples;
    } else {
        // Without a device clock, by wall time
        if (last_update_time == std::chrono::steady_clock::time_point{})
            last_update_time = now;
        due = now - last_update_time >= FRAME_DURATION;
        if (due) {
            last_update_time += FRAME_DURATION;
            if (now - last_update_time > 20 * FRAME_DURATION)
                last_update_time = now;
        }
    }
    if (!due)
        return false;
    // Backpressure: a full host queue must not consume guest waves and
    // silently discard their mixed output
    if (queued_samples.load(std::memory_order_relaxed) + frame_samples >
        params.sample_rate / 5)
        return false;
    // One frame at a time, each followed by its event: the client refills
    // the waves a frame consumed before the next one. Leave it time between
    // frames, unless the device is about to run dry (after a stall of this
    // thread or the client): then catch up as fast as the client allows.
    const auto spacing = behind ? std::chrono::microseconds(1000)
                                : std::chrono::microseconds(3500);
    if (last_render_time != std::chrono::steady_clock::time_point{} &&
        now - last_render_time < spacing)
        return false;

    std::vector<f32> stereo;
    renderFrame(stereo);
    stats_frames++;
    for (auto& sample : stereo) {
        if (!std::isfinite(sample) || std::abs(sample) > 1.0f)
            stats_clipped++;
        sample = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
    }
    if (!logged_output && !stereo.empty()) {
        f32 peak = 0.0f;
        for (const auto sample : stereo)
            peak = std::max(peak, std::abs(sample));
        if (peak > 0.00001f) {
            logged_output = true;
            std::fprintf(stderr,
                         "Audio output: %u Hz stereo float, peak %.6f, queued "
                         "%llu samples\n",
                         params.sample_rate, static_cast<double>(peak),
                         queued_samples.load());
        }
    }
    const usize output_frames = params.sample_count;
    produced_samples += output_frames;
    last_render_time = now;
    if (!stream_stats.has_sample_clock)
        elapsed_frames.store(produced_samples / params.sample_count,
                             std::memory_order_relaxed);
    queued_samples.fetch_add(output_frames, std::memory_order_relaxed);
    output_stream->enqueueBuffer(
        output_frames, std::span(reinterpret_cast<const u8*>(stereo.data()),
                                 stereo.size() * sizeof(f32)));

    if ((std::getenv("HYDRA_DUMP_AUDIO") || std::getenv("HYDRA_AUDIO_STATS")) &&
        now - last_stats_time >= std::chrono::seconds(5)) {
        last_stats_time = now;
        u32 active = 0, paused = 0, stopped = 0;
        f32 min_pitch = 1000.0f, max_pitch = 0.0f;
        for (const auto& input : render_state->voice_inputs) {
            if (!input.is_used)
                continue;
            if (input.play_state == VoicePlayState::Paused)
                paused++;
            if (input.play_state == VoicePlayState::Stopped)
                stopped++;
            if (input.play_state == VoicePlayState::Started) {
                active++;
                min_pitch = std::min(min_pitch, input.pitch);
                max_pitch = std::max(max_pitch, input.pitch);
            }
        }
        f64 square_sum = 0.0;
        f32 peak = 0.0f;
        for (const auto sample : stereo) {
            square_sum += static_cast<f64>(sample) * static_cast<f64>(sample);
            peak = std::max(peak, std::abs(sample));
        }
        const f64 rms =
            stereo.empty()
                ? 0.0
                : std::sqrt(square_sum / static_cast<f64>(stereo.size()));
        std::fprintf(stderr,
                     "Audio sources: %u Hz, updates %llu, active %u, paused "
                     "%u, stopped %u, pitch %.4f..%.4f, RMS %.6f, peak %.6f, "
                     "head resyncs %llu | frames %llu, starved voice frames "
                     "%llu, biquad voice frames %llu, clipped samples %llu\n",
                     params.sample_rate, update_count, active, paused, stopped,
                     static_cast<double>(min_pitch),
                     static_cast<double>(max_pitch), rms,
                     static_cast<double>(peak), head_resyncs, stats_frames,
                     stats_starved, stats_biquad, stats_clipped);
        stats_frames = stats_starved = stats_biquad = stats_clipped = 0;
        u32 valid_slots = 0;
        for (const auto& state : voice_states)
            for (const auto& buffer : state.source.buffers)
                valid_slots += buffer.valid ? 1u : 0u;
        std::fprintf(stderr,
                     "Audio timing: %u Hz, hardware %llu, produced %llu, "
                     "missing %llu, queued %llu, consumed clock %llu, valid "
                     "DSP frames %llu, waves %u, voice0 "
                     "consumed %u, offset %u, slot %u\n",
                     params.sample_rate,
                     stream_stats.sample_clock_frames - sample_clock_origin,
                     produced_samples, stream_stats.missing_frames,
                     queued_samples.load(),
                     consumedSampleClock(
                         stream_stats.sample_clock_frames, sample_clock_origin,
                         stream_stats.missing_frames, missing_frames_origin),
                     elapsed_frames.load(std::memory_order_relaxed),
                     valid_slots,
                     voices.empty() ? 0u : voices[0].num_wave_buffers_consumed,
                     voice_states.empty() ? 0u : voice_states[0].source.offset,
                     voice_states.empty() ? 0u : voice_states[0].source.index);
    }
    return true;
}

void IAudioRenderer::renderFrame(std::vector<f32>& stereo) {
    namespace dsp = audio::dsp;
    const auto& channels = render_state->channels;
    const auto& mixes = render_state->mixes;
    const auto& sinks = render_state->sinks;
    const usize sample_count = params.sample_count;
    const usize mix_count = mixes.size();
    std::vector<f32> mix_samples(sample_count * mix_count *
                                     dsp::MIX_BUFFER_COUNT_MAX,
                                 0.0f);
    const auto buffer = [&](usize mix, usize channel) {
        return std::span(mix_samples)
            .subspan((mix * dsp::MIX_BUFFER_COUNT_MAX + channel) * sample_count,
                     sample_count);
    };
    const auto bufferCount = [&](usize mix) {
        return std::min<u32>(mixes[mix].buffer_count, dsp::MIX_BUFFER_COUNT_MAX);
    };
    depop.resize(mix_count);
    if (previous_channel_mix.size() < channels.size())
        previous_channel_mix.resize(channels.size());
    const auto findMix = [&](u32 mix_id) {
        for (usize m = 0; m < mix_count; m++)
            if (mixes[m].is_used && mixes[m].mix_id == mix_id)
                return m;
        return mix_count;
    };

    const auto finite = [](f32 value) {
        return std::isfinite(value) ? value : 0.0f;
    };
    // Mix ramp of a voice channel into the buffers of a mix, keeping the
    // last values for the depop
    const auto mixRamp = [&](std::span<const f32> samples,
                             dsp::VoiceChannelState& channel, usize mix,
                             std::span<const f32> volumes,
                             std::span<const f32> previous) {
        for (u32 b = 0; b < bufferCount(mix); b++) {
            const f32 to = finite(volumes[b]);
            if (to == 0.0f && previous[b] == 0.0f)
                continue;
            channel.last_samples[b] = dsp::processMixRamp(
                buffer(mix, b), samples, previous[b], to);
        }
    };
    // A REV12 splitter destination's biquad filters, run once per frame.
    // A filter turned on starts from rest (the first filter's previous state
    // decides for both, as on hardware).
    const auto filterSplitterDestination =
        [&](SplitterDestinationState& splitter_destination,
            std::span<const f32> samples, std::vector<f32>& filtered) {
            bool any = false;
            for (u32 f = 0; f < dsp::BIQUAD_FILTER_COUNT; f++)
                any |= splitter_destination.biquads[f].enable;
            if (any) {
                filtered.assign(samples.begin(), samples.end());
                const bool initialize = !splitter_destination.biquad_enabled[0];
                for (u32 f = 0; f < dsp::BIQUAD_FILTER_COUNT; f++) {
                    const auto& filter = splitter_destination.biquads[f];
                    if (!filter.enable)
                        continue;
                    if (initialize)
                        splitter_destination.biquad_state[f] = {};
                    dsp::processBiquad(filtered, filter.numerator,
                                       filter.denominator,
                                       splitter_destination.biquad_state[f]);
                }
            }
            for (u32 f = 0; f < dsp::BIQUAD_FILTER_COUNT; f++)
                splitter_destination.biquad_enabled[f] =
                    splitter_destination.biquads[f].enable;
            return any;
        };

    // Voices: data source, biquad filters, volume ramp, mix ramp
    std::array<std::vector<f32>, dsp::VOICE_CHANNEL_COUNT_MAX> voice_samples;
    for (auto& samples : voice_samples)
        samples.resize(sample_count);
    for (u32 i = 0; i < params.voice_count; i++) {
        const auto& voice_in = render_state->voice_inputs[i];
        auto& state = voice_states[i];
        const u32 channel_count =
            std::min<u32>(voice_in.channel_count, dsp::VOICE_CHANNEL_COUNT_MAX);
        if (!voice_in.is_used || channel_count == 0)
            continue;
        const usize destination = findMix(voice_in.dest_mix_id);
        const bool has_splitter = destination >= mix_count &&
                                  voice_in.dest_splitter_id != UNUSED_SPLITTER_ID &&
                                  getSplitterDestination(voice_in.dest_splitter_id, 0);
        // Stopped or paused after playing: what the voice left in the mix
        // buffers fades out (depop) instead of ending in a click
        if (state.was_playing) {
            state.was_playing = false;
            state.previous_volume = 0.0f;
            for (u32 c = 0; c < channel_count; c++) {
                auto& last = state.channels[c].last_samples;
                const auto prepare = [&](usize mix) {
                    for (u32 b = 0; b < bufferCount(mix); b++)
                        depop[mix][b] += last[b];
                    last.fill(0.0f);
                };
                if (destination < mix_count) {
                    prepare(destination);
                    continue;
                }
                // Splitter: every destination is offered the channel's last
                // samples, the first configured one takes them
                for (u32 index = 0; has_splitter; index++) {
                    auto* splitter_destination = getSplitterDestination(
                        voice_in.dest_splitter_id, index);
                    if (splitter_destination == nullptr)
                        break;
                    if (!splitter_destination->isConfigured())
                        continue;
                    if (const usize mix = findMix(splitter_destination->mix_id);
                        mix < mix_count)
                        prepare(mix);
                    splitter_destination->need_update = true;
                }
            }
            continue;
        }
        if (voice_in.play_state != VoicePlayState::Started ||
            !state.source.hasValidBuffer())
            continue;

        const dsp::SourceFormat source{
            .format = voice_in.sample_format,
            .channel_count = channel_count,
            .adpcm_coefficients = state.coefficients,
            .decoding_flags = state.decoding_flags,
        };
        const f32 pitch = std::isfinite(voice_in.pitch) && voice_in.pitch > 0.0f
                              ? voice_in.pitch
                              : 1.0f;
        const f32 ratio = static_cast<f32>(voice_in.sample_rate) /
                          static_cast<f32>(params.sample_rate) * pitch;
        if (!dsp::processDataSource(state.source, source, state.channels,
                                    ratio, state.src_quality,
                                    static_cast<u32>(sample_count),
                                    voice_samples))
            stats_starved++;
        voices[i].played_sample_count = state.source.played;
        voices[i].num_wave_buffers_consumed = state.source.consumed;
        if (destination >= mix_count && !has_splitter)
            continue;

        const f32 volume =
            std::isfinite(voice_in.volume) ? voice_in.volume : 0.0f;
        for (u32 c = 0; c < channel_count; c++) {
            auto samples = std::span(voice_samples[c]).first(sample_count);
            auto& channel = state.channels[c];
            for (u32 f = 0; f < dsp::BIQUAD_FILTER_COUNT; f++) {
                const auto& filter = voice_in.biquads[f];
                if (!filter.enable)
                    continue;
                if (c == 0)
                    stats_biquad++;
                if (!logged_biquad) {
                    logged_biquad = true;
                    LOG_INFO(Services,
                             "Audio voice biquad: {} {} {} / {} {}",
                             filter.numerator[0], filter.numerator[1],
                             filter.numerator[2], filter.denominator[0],
                             filter.denominator[1]);
                }
                // A filter enabled now starts from rest
                if (!state.biquad_enabled[f])
                    channel.biquad[f] = {};
                dsp::processBiquad(samples, filter.numerator,
                                   filter.denominator, channel.biquad[f]);
            }
            dsp::processVolumeRamp(samples, state.previous_volume, volume);
            if (destination < mix_count) {
                const u32 channel_id = voice_in.channel_ids[c];
                if (channel_id >= channels.size())
                    continue;
                auto& previous = previous_channel_mix[channel_id];
                mixRamp(samples, channel, destination,
                        channels[channel_id].mix, previous);
                for (u32 b = 0; b < dsp::MIX_BUFFER_COUNT_MAX; b++)
                    previous[b] = finite(channels[channel_id].mix[b]);
                continue;
            }
            // Splitter: the destinations of channel c are c, c + channel
            // count, ...
            for (u32 index = c;; index += channel_count) {
                auto* splitter_destination =
                    getSplitterDestination(voice_in.dest_splitter_id, index);
                if (splitter_destination == nullptr)
                    break;
                if (!splitter_destination->isConfigured())
                    continue;
                const usize mix = findMix(splitter_destination->mix_id);
                if (mix >= mix_count)
                    continue;
                std::span<const f32> input = samples;
                std::vector<f32> filtered;
                if (filterSplitterDestination(*splitter_destination, samples,
                                              filtered))
                    input = filtered;
                mixRamp(input, channel, mix, splitter_destination->volume,
                        splitter_destination->previous);
                splitter_destination->need_update = true;
            }
        }
        state.previous_volume = volume;
        for (u32 f = 0; f < dsp::BIQUAD_FILTER_COUNT; f++)
            state.biquad_enabled[f] = voice_in.biquads[f].enable;
    }

    // The splitters' previous volumes catch up with what the voices mixed
    for (auto& splitter_destination : splitter_destinations) {
        if (splitter_destination.used && splitter_destination.need_update)
            splitter_destination.previous = splitter_destination.volume;
        splitter_destination.need_update = false;
    }

    // Where a mix sends its buffers: its destination mix or the mixes of its
    // splitter's destinations
    const auto routesTo = [&](usize source, usize target) {
        const auto& mix = mixes[source];
        if (mix.dest_mix_id != UNUSED_MIX_ID)
            return findMix(mix.dest_mix_id) == target;
        for (u32 index = 0;; index++) {
            const auto* splitter_destination =
                getSplitterDestination(mix.dest_splitter_id, index);
            if (splitter_destination == nullptr)
                return false;
            if (splitter_destination->isConfigured() &&
                findMix(splitter_destination->mix_id) == target)
                return true;
        }
    };

    // Mixes in dependency order: depop, volume, then the routing into the
    // destination mix. Cycles are ignored rather than repeatedly amplifying
    // the audio.
    const f32 decay = dsp::depopDecay(params.sample_rate);
    // The mix's effects, in its processing order (slot n holds the effect
    // with order n, the first empty slot ends the list)
    ProcessMemory guest_memory(guest_process);
    const auto processEffects = [&](usize m, u32 count) {
        if (effects.empty())
            return;
        std::vector<i32> order(effects.size(), -1);
        for (usize e = 0; e < effects.size(); e++) {
            const auto& effect = effects[e];
            if (effect.type() == dsp::effects::EffectType::Invalid ||
                effect.mixId() != static_cast<i32>(mixes[m].mix_id) ||
                effect.processingOrder() >= order.size())
                continue;
            order[effect.processingOrder()] = static_cast<i32>(e);
        }
        std::array<std::span<f32>, dsp::MIX_BUFFER_COUNT_MAX> spans;
        for (u32 b = 0; b < count; b++)
            spans[b] = buffer(m, b);
        for (const i32 e : order) {
            if (e < 0)
                break;
            auto& effect = effects[static_cast<usize>(e)];
            if (effect.shouldSkip())
                continue;
            effect.process(std::span(spans).first(count),
                           static_cast<u32>(sample_count), params.sample_rate,
                           &guest_memory, effect_behaviour);
        }
    };
    std::vector<bool> processed(mix_count, false);
    for (usize pass = 0; pass < mix_count; pass++) {
        bool progress = false;
        for (usize m = 0; m < mix_count; m++) {
            if (processed[m] || !mixes[m].is_used)
                continue;
            bool waiting = false;
            for (usize source = 0; source < mix_count; source++)
                if (source != m && !processed[source] &&
                    mixes[source].is_used && routesTo(source, m))
                    waiting = true;
            if (waiting)
                continue;
            processed[m] = true;
            progress = true;
            const u32 count = bufferCount(m);
            for (u32 channel = 0; channel < count; channel++)
                if (depop[m][channel] != 0.0f)
                    depop[m][channel] = dsp::processDepop(
                        buffer(m, channel), depop[m][channel], decay);
            processEffects(m, count);
            const f32 volume =
                std::isfinite(mixes[m].volume) ? mixes[m].volume : 0.0f;
            for (u32 channel = 0; channel < count; channel++)
                for (auto& sample : buffer(m, channel))
                    sample *= volume;
            if (mixes[m].dest_mix_id == UNUSED_MIX_ID) {
                // Splitter: destination i takes buffer i (modulo the count)
                // to every buffer of its mix
                for (u32 index = 0; count != 0; index++) {
                    auto* splitter_destination =
                        getSplitterDestination(mixes[m].dest_splitter_id, index);
                    if (splitter_destination == nullptr)
                        break;
                    if (!splitter_destination->isConfigured())
                        continue;
                    const usize dest = findMix(splitter_destination->mix_id);
                    if (dest >= mix_count || dest == m)
                        continue;
                    std::span<const f32> from = buffer(m, index % count);
                    std::vector<f32> filtered;
                    if (filterSplitterDestination(*splitter_destination, from,
                                                  filtered))
                        from = filtered;
                    for (u32 out = 0; out < bufferCount(dest); out++) {
                        const f32 gain = finite(splitter_destination->volume[out]);
                        if (gain == 0.0f)
                            continue;
                        auto to = buffer(dest, out);
                        for (usize frame = 0; frame < sample_count; frame++)
                            to[frame] += from[frame] * gain;
                    }
                }
                continue;
            }
            for (usize dest = 0; dest < mix_count; dest++) {
                if (dest == m || !mixes[dest].is_used ||
                    mixes[dest].mix_id != mixes[m].dest_mix_id)
                    continue;
                for (u32 src = 0; src < count; src++)
                    for (u32 out = 0; out < bufferCount(dest); out++) {
                        const f32 gain = mixes[m].mix[src][out];
                        if (gain == 0.0f || !std::isfinite(gain))
                            continue;
                        auto from = buffer(m, src);
                        auto to = buffer(dest, out);
                        for (usize frame = 0; frame < sample_count; frame++)
                            to[frame] += from[frame] * gain;
                    }
            }
        }
        if (!progress)
            break;
    }

    // Device sinks of the final mix, downmixed to the host's stereo
    stereo.assign(sample_count * 2, 0.0f);
    const usize final_mix = findMix(0);
    if (final_mix >= mix_count)
        return;
    for (const auto& sink : sinks) {
        if (!sink.is_used || sink.type != 1)
            continue;
        // DeviceParameter: name (0xff), padding, input count, inputs[6],
        // reserved, downmix enabled, downmix coefficients[4]
        u32 input_count = 0;
        std::memcpy(&input_count, sink.data + 256, 4);
        input_count = std::min<u32>(input_count, dsp::VOICE_CHANNEL_COUNT_MAX);
        const bool custom_downmix = sink.data[267] != 0;
        auto coefficients = dsp::DEFAULT_SURROUND_TO_STEREO;
        if (custom_downmix)
            std::memcpy(coefficients.data(), sink.data + 268, 16);
        const auto input = [&](u32 index, usize frame) {
            const u8 channel = sink.data[260 + index];
            return channel < bufferCount(final_mix)
                       ? buffer(final_mix, channel)[frame]
                       : 0.0f;
        };
        for (usize frame = 0; frame < sample_count; frame++) {
            f32 left, right;
            if (custom_downmix || input_count == 6) {
                left = dsp::downmixSurroundToStereo(
                    coefficients, input(0, frame), input(2, frame),
                    input(3, frame), input(4, frame));
                right = dsp::downmixSurroundToStereo(
                    coefficients, input(1, frame), input(2, frame),
                    input(3, frame), input(5, frame));
            } else if (input_count == 1) {
                left = right = input(0, frame);
            } else if (input_count != 0) {
                left = input(0, frame);
                right = input(1, frame);
            } else {
                continue;
            }
            stereo[frame * 2] += left;
            stereo[frame * 2 + 1] += right;
        }
    }
}

result_t IAudioRenderer::requestUpdateImpl(
    kernel::Process* process, std::optional<ztd::io::MemoryStream> in_stream,
    std::optional<ztd::io::MemoryStream> out_stream,
    std::optional<ztd::io::MemoryStream> out_perf_stream) {
    std::lock_guard lock(render_mutex);
    if (!in_stream || !out_stream ||
        in_stream->getSize() < sizeof(UpdateDataHeader))
        return MAKE_RESULT(Svc, kernel::Error::InvalidSize);

    // Header
    const auto in_header = in_stream->read<UpdateDataHeader>();
    if (!logged_header) {
        logged_header = true;
        LOG_INFO(
            Services,
            "Audio update sections: behavior {}, pools {}, voices {}, channels "
            "{}, effects {}, mixes {}, sinks {}, render info {}, total {}",
            in_header.behavior_size, in_header.mempools_size,
            in_header.voices_size, in_header.channels_size,
            in_header.effects_size, in_header.mixes_size, in_header.sinks_size,
            in_header.render_info_size, in_header.total_size);
    }

    // The input order is pools, channels, voices, effects, mixes, sinks.
    // Header fields list voices before channels, but their payload order
    // differs.
    const u64 channel_offset = sizeof(UpdateDataHeader) +
                               static_cast<u64>(in_header.behavior_size) +
                               in_header.mempools_size;
    const u64 voice_offset = channel_offset + in_header.channels_size;
    const u64 effect_offset = voice_offset + in_header.voices_size;
    // Splitters (no size in the header) follow the effects
    const auto splitter_end = updateSplitters(
        in_stream->getPtr(), in_stream->getSize(),
        effect_offset + in_header.effects_size, false);
    if (!splitter_end) {
        ONCE(LOG_WARN(Services, "Audio update rejected: invalid splitters"));
        return MAKE_RESULT(Svc, kernel::Error::InvalidSize);
    }
    const u64 mix_offset = *splitter_end;
    const u64 sink_offset = mix_offset + in_header.mixes_size;
    const u64 input_end = sink_offset + in_header.sinks_size;
    const u32 mempool_count = params.effect_count + params.voice_count * 4;
    // REV9 effect statuses carry a result state (limiter statistics)
    const u64 effect_status_size = effect_behaviour.effect_info_v2
                                       ? sizeof(audio::dsp::effects::EffectOutStatusV2)
                                       : sizeof(audio::dsp::effects::EffectOutStatusV1);
    const bool has_render_info =
        params.revision >= makeMagic4('R', 'E', 'V', '5');
    const u64 output_size =
        sizeof(UpdateDataHeader) +
        static_cast<u64>(mempool_count) * sizeof(MemPoolInfoOut) +
        static_cast<u64>(params.voice_count) * sizeof(VoiceInfoOut) +
        static_cast<u64>(params.effect_count) * effect_status_size +
        static_cast<u64>(params.sink_count) * sizeof(SinkInfoOut) +
        sizeof(BehaviorInfoOut) + sizeof(PerformanceInfoOut) +
        (has_render_info ? sizeof(RenderInfoOut) : 0);
    if (input_end > in_stream->getSize() ||
        output_size > out_stream->getSize() ||
        in_header.mempools_size < mempool_count * sizeof(MemPoolInfoIn) ||
        in_header.voices_size < params.voice_count * sizeof(VoiceInfoIn) ||
        in_header.channels_size % sizeof(ChannelInfoIn) ||
        in_header.sinks_size % sizeof(SinkInfoIn)) {
        ONCE(LOG_WARN(Services,
                      "Audio update rejected: need {} output bytes, got {}, "
                      "input end {}/{}",
                      output_size, out_stream->getSize(), input_end,
                      in_stream->getSize()));
        return MAKE_RESULT(Svc, kernel::Error::InvalidSize);
    }
    updateSplitters(in_stream->getPtr(), in_stream->getSize(),
                    effect_offset + in_header.effects_size, true);

    // REV7 sends only changed mixes, prefixed by a count header. Preserve
    // previous mixes when subsequent updates contain no changed entries.
    const bool sparse_mixes = params.revision >= makeMagic4('R', 'E', 'V', '7');
    u64 mix_data_offset = mix_offset;
    u32 mix_count = in_header.mixes_size / sizeof(MixInfoIn);
    if (sparse_mixes) {
        if (in_header.mixes_size < sizeof(MixUpdateHeader))
            return MAKE_RESULT(Svc, kernel::Error::InvalidSize);
        MixUpdateHeader update;
        std::memcpy(&update, in_stream->getPtr() + mix_offset, sizeof(update));
        if (update.count < 0 || static_cast<u32>(update.count) > params.submix_count + 1)
            return MAKE_RESULT(Svc, kernel::Error::InvalidSize);
        mix_count = static_cast<u32>(update.count);
        mix_data_offset += sizeof(update);
    }
    const u64 expected_mix_size = (sparse_mixes ? sizeof(MixUpdateHeader) : 0) +
                                 static_cast<u64>(mix_count) * sizeof(MixInfoIn);
    if (expected_mix_size != in_header.mixes_size || mix_count > params.submix_count + 1)
        return MAKE_RESULT(Svc, kernel::Error::InvalidSize);
    auto mixes = sparse_mixes ? render_state->mixes : std::vector<MixInfoIn>{};
    mixes.resize(static_cast<usize>(params.submix_count) + 1);
    std::vector<bool> updated(mixes.size());
    for (u32 index = 0; index < mix_count; ++index) {
        MixInfoIn mix;
        std::memcpy(&mix, in_stream->getPtr() + mix_data_offset +
                              static_cast<u64>(index) * sizeof(mix), sizeof(mix));
        const u32 id = sparse_mixes ? mix.mix_id : index;
        if (id >= mixes.size() || updated[id] || mix.buffer_count > 24)
            return MAKE_RESULT(Svc, kernel::Error::InvalidSize);
        updated[id] = true;
        mixes[id] = mix;
    }

    update_count++;
    if (const auto dir = std::getenv("HYDRA_DUMP_AUDIO")) {
        if (update_count == 1 || update_count % 1000 == 0) {
            std::filesystem::create_directories(dir);
            std::ofstream packet(fmt::format("{}/{}_{}_update.bin", dir,
                                             params.sample_rate, update_count),
                                 std::ios::binary);
            packet.write(reinterpret_cast<const char*>(in_stream->getPtr()),
                         static_cast<std::streamsize>(in_stream->getSize()));
            LOG_INFO(Services,
                     "Audio update #{}: {} Hz, running {}, elapsed frames {}, "
                     "output bytes {}",
                     update_count, params.sample_rate, running.load(),
                     elapsed_frames.load(), out_stream->getSize());
        }
    }

    std::vector<ChannelInfoIn> channels(in_header.channels_size /
                                        sizeof(ChannelInfoIn));
    std::memcpy(channels.data(), in_stream->getPtr() + channel_offset,
                in_header.channels_size);
    std::vector<SinkInfoIn> sinks(in_header.sinks_size / sizeof(SinkInfoIn));
    std::memcpy(sinks.data(), in_stream->getPtr() + sink_offset,
                in_header.sinks_size);

    auto header = out_stream->writeReturningPtr<UpdateDataHeader>();
    *header = {};
    header->revision = in_header.revision; // makeMagic4('R', 'E', 'V', '4');
    header->total_size = sizeof(UpdateDataHeader);

    in_stream->seekBy(in_header.behavior_size);

    // Mempools

    header->mempools_size = mempool_count * sizeof(MemPoolInfoOut);
    header->total_size += header->mempools_size;
    for (u32 i = 0; i < mempool_count; i++) {
        const auto mempool_in = in_stream->read<MemPoolInfoIn>();

        MemPoolInfoOut mempool{};
        switch (mempool_in.state) {
        case MemPoolState::RequestAttach:
            mempool.new_state = MemPoolState::Attached;
            break;
        case MemPoolState::RequestDetach:
            mempool.new_state = MemPoolState::Detached;
            break;
        default:
            // Other states don't change
            mempool.new_state = mempool_in.state;
            break;
        }
        out_stream->write(mempool);
    }

    render_state->channels = std::move(channels);
    render_state->mixes = std::move(mixes);
    render_state->sinks = std::move(sinks);
    in_stream->seekTo(voice_offset);

    // Voices
    header->voices_size = params.voice_count * sizeof(VoiceInfoOut);
    header->total_size += header->voices_size;
    for (u32 i = 0; i < params.voice_count; i++) {
        const auto voice_in = in_stream->read<VoiceInfoIn>();
        render_state->voice_inputs[i] = voice_in;
        if (voice_in.is_used &&
            voice_in.play_state == VoicePlayState::Started) {
            const auto format = static_cast<u32>(voice_in.sample_format);
            if (format < 32 && !(observed_formats & (1u << format))) {
                observed_formats |= 1u << format;
                LOG_INFO(Services,
                         "Audio voice: format {}, rate {}, channels {}, pitch "
                         "{}, volume {}, mix {}, splitter {}, wave head {}",
                         static_cast<PcmFormat>(voice_in.sample_format),
                         voice_in.sample_rate, voice_in.channel_count,
                         voice_in.pitch, voice_in.volume, voice_in.dest_mix_id,
                         voice_in.dest_splitter_id, voice_in.wave_buffer_head);
                if (const auto dir = std::getenv("HYDRA_DUMP_AUDIO")) {
                    std::filesystem::create_directories(dir);
                    std::ofstream file(
                        fmt::format("{}/{}_{}.bin", dir,
                                    reinterpret_cast<uintptr_t>(this), format),
                        std::ios::binary);
                    file.write(
                        reinterpret_cast<const char*>(in_stream->getPtr()),
                        static_cast<std::streamsize>(in_stream->getSize()));
                }
            }
        }

        VoiceInfoOut& voice = voices[i];
        VoiceState& state = voice_states[i];
        auto& source = state.source;
        if (voice_in.is_new) {
            voice = {};
            state = {};
            state.previous_volume = voice_in.volume;
        }

        if (voice_in.is_used && voice_in.sample_format == PcmFormat::Adpcm &&
            voice_in.extra_params_sz >= 32) {
            const auto coefficients = readGuest(
                process, reinterpret_cast<uintptr_t>(voice_in.extra_params_ptr),
                32);
            if (coefficients.size() == 32)
                std::memcpy(state.coefficients.data(), coefficients.data(), 32);
        }
        // REV5: decoding behaviour and wave buffer flushes, REV8: sample rate
        // conversion quality
        if (voice_in.is_used) {
            if (params.revision >= makeMagic4('R', 'E', 'V', '5')) {
                std::memcpy(&state.decoding_flags, &voice_in.decoding_flags,
                            sizeof(state.decoding_flags));
                state.flush_count += voice_in.flush_wave_buffer_count;
            }
            if (params.revision >= makeMagic4('R', 'E', 'V', '8') &&
                voice_in.src_quality <= 2)
                state.src_quality =
                    static_cast<audio::dsp::SrcQuality>(voice_in.src_quality);
        }

        if (voice_in.is_new && voice_in.wave_buffer_head >= 0)
            source.index = static_cast<u32>(voice_in.wave_buffer_head) %
                           audio::dsp::WAVE_BUFFER_COUNT;

        // Only buffers marked new by the client are copied. The host queue owns
        // mixed PCM, so the guest can recycle buffers as soon as they are
        // consumed.
        if (voice_in.is_used) {
            for (u32 j = 0; j < audio::dsp::WAVE_BUFFER_COUNT; j++) {
                const auto& wave = voice_in.wave_buffers[j];
                if (wave.sent_to_server || !wave.address ||
                    wave.end_sample_offset <= wave.start_sample_offset)
                    continue;
                auto& buffer = source.buffers[j];
                if (buffer.valid && buffer.address == wave.address &&
                    buffer.start == wave.start_sample_offset &&
                    buffer.end == wave.end_sample_offset)
                    continue;
                const bool was_current = j == source.index && !buffer.valid;
                buffer = {};
                buffer.address = wave.address;
                buffer.start = wave.start_sample_offset;
                buffer.end = wave.end_sample_offset;
                buffer.looping = wave.is_looping;
                buffer.end_of_stream = wave.end_of_stream;
                buffer.loop_count = wave.loop_count;
                buffer.loop_start = wave.loop_first_sample_offset;
                buffer.loop_end = wave.loop_last_sample_offset;
                buffer.data = readGuest(process, wave.address, wave.size);
                if (wave.context_addr && wave.context_sz >= 6) {
                    const auto context =
                        readGuest(process, wave.context_addr, 6);
                    if (context.size() == 6) {
                        std::memcpy(buffer.context, context.data() + 2, 4);
                        buffer.has_context = true;
                    }
                }
                const auto bytes = pcmSampleBytes(voice_in.sample_format);
                const u64 available = voice_in.sample_format == PcmFormat::Adpcm
                                          ? (voice_in.channel_count == 1
                                                 ? buffer.data.size() / 8 * 14
                                                 : 0)
                                          : (bytes && voice_in.channel_count
                                                 ? buffer.data.size() / bytes /
                                                       voice_in.channel_count
                                                 : 0);
                buffer.end = static_cast<u32>(std::min<u64>(buffer.end, available));
                buffer.loop_end =
                    static_cast<u32>(std::min<u64>(buffer.loop_end, available));
                buffer.valid = buffer.end > buffer.start;
                // A buffer queued after the voice ran dry starts at its
                // beginning
                if (was_current) {
                    source.offset = 0;
                    source.loop_counter = 0;
                }
            }
        }

        if (state.flush_count != 0) {
            source.flush(state.flush_count);
            state.flush_count = 0;
        }

        if (voice_in.is_used &&
            voice_in.play_state == VoicePlayState::Started) {
            std::array<bool, audio::dsp::WAVE_BUFFER_COUNT> valid{};
            for (u32 j = 0; j < audio::dsp::WAVE_BUFFER_COUNT; j++)
                valid[j] = source.buffers[j].valid;
            const auto head = resumeWaveHead(valid, source.index,
                                             voice_in.wave_buffer_head);
            if (head != source.index) {
                head_resyncs++;
                source.index = head;
                source.offset = 0;
                source.loop_counter = 0;
            }
        }

        const bool was_started = state.previous_play_state ==
                                 static_cast<u8>(VoicePlayState::Started);
        if (voice_in.is_used &&
            voice_in.play_state == VoicePlayState::Stopped &&
            state.previous_play_state !=
                static_cast<u8>(VoicePlayState::Stopped)) {
            source.stop();
            // The resampler and filter history reset. What the voice last
            // added to the mix buffers is kept for the depop.
            for (auto& channel : state.channels) {
                channel.pitch.fill(0.0f);
                channel.biquad = {};
            }
            state.was_playing = was_started;
        }
        if (voice_in.is_used &&
            voice_in.play_state == VoicePlayState::Paused && was_started)
            state.was_playing = true;
        if (voice_in.is_used)
            state.previous_play_state = static_cast<u8>(voice_in.play_state);
        voice.played_sample_count = source.played;
        voice.num_wave_buffers_consumed = source.consumed;

        out_stream->write(voice);
    }

    // Channels
    header->channels_size = 0x0;
    header->total_size += header->channels_size;
    // TODO

    // Effects: the client's parameters, then each slot's status
    guest_process = process;
    {
        // Behaviour flags: bit 0 maps memory pools without a check
        u64 behaviour_flags = 0;
        if (in_header.behavior_size >= 16)
            std::memcpy(&behaviour_flags,
                        in_stream->getPtr() + sizeof(UpdateDataHeader) + 8, 8);
        effect_behaviour.memory_pool_force_mapping = (behaviour_flags & 1) != 0;
    }
    if (in_header.effects_size >=
        static_cast<u64>(params.effect_count) *
            sizeof(audio::dsp::effects::EffectInParameter)) {
        for (u32 i = 0; i < params.effect_count; i++) {
            audio::dsp::effects::EffectInParameter in;
            std::memcpy(&in,
                        in_stream->getPtr() + effect_offset +
                            static_cast<u64>(i) * sizeof(in),
                        sizeof(in));
            effects[i].update(in, running.load(), effect_behaviour);
        }
    }
    header->effects_size = static_cast<u32>(params.effect_count * effect_status_size);
    for (const auto& effect : effects) {
        if (effect_behaviour.effect_info_v2) {
            audio::dsp::effects::EffectOutStatusV2 status{};
            effect.writeStatus(&status, true);
            out_stream->write(status);
        } else {
            audio::dsp::effects::EffectOutStatusV1 status{};
            effect.writeStatus(&status, false);
            out_stream->write(status);
        }
    }
    header->total_size += header->effects_size;

    // Sinks
    header->sinks_size = params.sink_count * sizeof(SinkInfoOut);
    header->total_size += header->sinks_size;
    for (u32 i = 0; i < params.sink_count; i++) {
        out_stream->write<SinkInfoOut>({
            .last_written_offset = 0,
        });
    }

    // Output payload order is sinks, performance, behavior, renderer info.
    header->performance_manager_size = sizeof(PerformanceInfoOut);
    header->total_size += header->performance_manager_size;
    out_stream->write<PerformanceInfoOut>({});
    header->behavior_size = sizeof(BehaviorInfoOut);
    header->total_size += header->behavior_size;
    out_stream->write<BehaviorInfoOut>({});
    if (has_render_info) {
        header->render_info_size = sizeof(RenderInfoOut);
        header->total_size += header->render_info_size;
        out_stream->write<RenderInfoOut>(
            {.elapsed_frame_count =
                 elapsed_frames.load(std::memory_order_relaxed)});
    }
    if (out_perf_stream &&
        out_perf_stream->getSize() >= sizeof(PerformanceInfoOut))
        out_perf_stream->write<PerformanceInfoOut>({});

    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::audio
