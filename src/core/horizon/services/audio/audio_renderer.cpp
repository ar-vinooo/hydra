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

enum class EffectState : u8 {
    Enabled = 3,
    Disabled = 4,
};

struct EffectInfoOutV1 {
    EffectState state;
    u8 _reserved[15];
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
        // Stopped or paused after playing: what the voice left in the mix
        // buffers fades out (depop) instead of ending in a click
        if (state.was_playing) {
            state.was_playing = false;
            state.previous_volume = 0.0f;
            if (destination < mix_count)
                for (u32 c = 0; c < channel_count; c++) {
                    auto& last = state.channels[c].last_samples;
                    for (u32 b = 0; b < bufferCount(destination); b++)
                        depop[destination][b] += last[b];
                    last.fill(0.0f);
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
        if (destination >= mix_count)
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
            const u32 channel_id = voice_in.channel_ids[c];
            if (channel_id >= channels.size())
                continue;
            auto& previous = previous_channel_mix[channel_id];
            const auto& mix = channels[channel_id].mix;
            for (u32 b = 0; b < bufferCount(destination); b++) {
                const f32 to = std::isfinite(mix[b]) ? mix[b] : 0.0f;
                if (to == 0.0f && previous[b] == 0.0f)
                    continue;
                channel.last_samples[b] = dsp::processMixRamp(
                    buffer(destination, b), samples, previous[b], to);
            }
            for (u32 b = 0; b < dsp::MIX_BUFFER_COUNT_MAX; b++)
                previous[b] = std::isfinite(mix[b]) ? mix[b] : 0.0f;
        }
        state.previous_volume = volume;
        for (u32 f = 0; f < dsp::BIQUAD_FILTER_COUNT; f++)
            state.biquad_enabled[f] = voice_in.biquads[f].enable;
    }

    // Mixes in dependency order: depop, volume, then the routing into the
    // destination mix. Cycles are ignored rather than repeatedly amplifying
    // the audio.
    const f32 decay = dsp::depopDecay(params.sample_rate);
    std::vector<bool> processed(mix_count, false);
    for (usize pass = 0; pass < mix_count; pass++) {
        bool progress = false;
        for (usize m = 0; m < mix_count; m++) {
            if (processed[m] || !mixes[m].is_used)
                continue;
            bool waiting = false;
            for (usize source = 0; source < mix_count; source++)
                if (source != m && !processed[source] &&
                    mixes[source].is_used &&
                    mixes[source].dest_mix_id == mixes[m].mix_id)
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
            const f32 volume =
                std::isfinite(mixes[m].volume) ? mixes[m].volume : 0.0f;
            for (u32 channel = 0; channel < count; channel++)
                for (auto& sample : buffer(m, channel))
                    sample *= volume;
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
    const u64 mix_offset =
        voice_offset + in_header.voices_size + in_header.effects_size;
    const u64 sink_offset = mix_offset + in_header.mixes_size;
    const u64 input_end = sink_offset + in_header.sinks_size;
    const u32 mempool_count = params.effect_count + params.voice_count * 4;
    const bool has_render_info =
        params.revision >= makeMagic4('R', 'E', 'V', '5');
    const u64 output_size =
        sizeof(UpdateDataHeader) +
        static_cast<u64>(mempool_count) * sizeof(MemPoolInfoOut) +
        static_cast<u64>(params.voice_count) * sizeof(VoiceInfoOut) +
        static_cast<u64>(params.effect_count) * sizeof(EffectInfoOutV1) +
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

    // Effects
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (false) {
        // header->effects_size = TODO;
        // TODO
    } else {
        header->effects_size = params.effect_count * sizeof(EffectInfoOutV1);
        for (u32 i = 0; i < params.effect_count; i++) {
            out_stream->write<EffectInfoOutV1>({
                .state = EffectState::Enabled,
            });
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
