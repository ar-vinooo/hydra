#include "core/horizon/services/audio/audio_renderer.hpp"

#include "core/audio/pcm_queue.hpp"
#include "core/audio/sample_clock.hpp"
#include "core/audio/sample_decoder.hpp"
#include "core/debugger/debugger_manager.hpp"
#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/system.hpp"
#include <cmath>
#include <fstream>

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
    u8 _padding1[5];
    vaddr_t context_addr;
    u64 context_sz;
    u64 _padding2;
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
    u8 _padding3[24];
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
        auto next_tick = std::chrono::steady_clock::now();
        auto last_signal = next_tick;
        while (!token.stop_requested()) {
            const bool rendered = renderAudio();
            const auto now = std::chrono::steady_clock::now();
            if (rendered || now - last_signal >= std::chrono::milliseconds(5)) {
                event->signal();
                last_signal = now;
            }
            next_tick += std::chrono::milliseconds(2);
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
    sample_clock_origin = output_stream->getStats().sample_clock_frames;
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
    const auto& channels = render_state->channels;
    const auto& mixes = render_state->mixes;
    const auto& sinks = render_state->sinks;
    // Audio frames rendered since the last update (5ms each)
    constexpr auto FRAME_DURATION = std::chrono::milliseconds(5);
    const auto now = std::chrono::steady_clock::now();
    u32 frame_count = 0;
    if (last_update_time == std::chrono::steady_clock::time_point{}) {
        last_update_time = now;
    } else {
        frame_count = static_cast<u32>(
            std::min<i64>((now - last_update_time) / FRAME_DURATION, 20));
        last_update_time += frame_count * FRAME_DURATION;
        if (now - last_update_time > 20 * FRAME_DURATION)
            last_update_time = now;
    }

    const auto stream_stats = output_stream->getStats();
    if (running && stream_stats.has_sample_clock) {
        const u64 played =
            stream_stats.sample_clock_frames - sample_clock_origin;
        // Keep forty milliseconds ahead of the hardware clock. Wall clocks
        // and the device oscillator drift, so do not use wall time to pace PCM.
        frame_count = framesForSampleClock(played, produced_samples,
                                           params.sample_count, 1);
    }
    if (!running)
        frame_count = 0;
    // Guest clients replenish waves after each DSP event. A large hardware
    // callback must not consume multiple DSP frames while holding their update
    // lock, otherwise a single queued wave is followed by artificial silence.
    frame_count = std::min(frame_count, 1u);
    // A callback jump or a delayed worker wake must not drain the guest's
    // remaining waves in a burst. Allow a small clock correction, but leave
    // enough time between DSP frames for the client to refill a consumed wave.
    if (frame_count &&
        last_render_time != std::chrono::steady_clock::time_point{} &&
        now - last_render_time < std::chrono::microseconds(3500))
        return false;
    const usize output_frames =
        static_cast<usize>(frame_count) * params.sample_count;
    const usize mix_count = mixes.size();
    // Each mix has at most 24 channels. CTR uses only the final mix.
    std::vector<f32> mix_samples(output_frames * mix_count * 24, 0.0f);
    auto mixIndex = [&](usize mix, usize channel, usize frame) {
        return (mix * 24 + channel) * output_frames + frame;
    };
    for (u32 i = 0; i < params.voice_count; i++) {
        const auto& voice_in = render_state->voice_inputs[i];
        auto& voice = voices[i];
        auto& state = voice_states[i];
        if (voice_in.is_used &&
            voice_in.play_state == VoicePlayState::Started &&
            voice_in.channel_count > 0 && voice_in.channel_count <= 6) {
            const f64 pitch =
                std::isfinite(voice_in.pitch) && voice_in.pitch > 0.0f
                    ? static_cast<f64>(voice_in.pitch)
                    : 1.0;
            const f64 step = static_cast<f64>(voice_in.sample_rate) /
                             params.sample_rate * pitch;
            usize destination = mix_count;
            for (usize m = 0; m < mix_count; m++)
                if (mixes[m].mix_id == voice_in.dest_mix_id && mixes[m].is_used)
                    destination = m;

            const auto& coefficients = state.coefficients;

            for (usize frame = 0; frame < output_frames; frame++) {
                auto& slot = state.slots[state.crnt_slot];
                if (!slot.valid)
                    break;
                if (state.offset < slot.start || state.offset >= slot.end)
                    state.offset = slot.start;
                if (voice_in.sample_format == PcmFormat::Adpcm &&
                    !slot.decoded_ready) {
                    if (slot.has_context) {
                        state.history[0] = slot.history[0];
                        state.history[1] = slot.history[1];
                    }
                    slot.decoded =
                        decodeDspAdpcm(slot.data, coefficients, slot.end,
                                       state.history[0], state.history[1]);
                    slot.decoded_ready = true;
                }
                for (u32 channel = 0; channel < voice_in.channel_count;
                     channel++) {
                    auto sampleAt = [&](u32 offset) {
                        if (voice_in.sample_format == PcmFormat::Adpcm)
                            return offset < slot.decoded.size()
                                       ? slot.decoded[offset]
                                       : 0.0f;
                        return decodePcm(slot.data,
                                         static_cast<usize>(offset) *
                                                 voice_in.channel_count +
                                             channel,
                                         voice_in.sample_format);
                    };
                    const auto first = sampleAt(state.offset);
                    const auto second =
                        sampleAt(std::min(state.offset + 1, slot.end - 1));
                    const auto sample =
                        (first +
                         (second - first) * static_cast<f32>(state.fraction)) *
                        voice_in.volume;
                    const u32 channel_id = voice_in.channel_ids[channel];
                    if (destination < mix_count &&
                        channel_id < channels.size() && std::isfinite(sample)) {
                        for (u32 out = 0;
                             out <
                             std::min<u32>(mixes[destination].buffer_count, 24);
                             out++)
                            mix_samples[mixIndex(destination, out, frame)] +=
                                sample * channels[channel_id].mix[out];
                    }
                }
                state.fraction += std::clamp(step, 0.0, 16.0);
                u32 advance = static_cast<u32>(state.fraction);
                state.fraction -= advance;
                while (advance && state.slots[state.crnt_slot].valid) {
                    auto& current = state.slots[state.crnt_slot];
                    const u32 taken =
                        std::min(advance, current.end - state.offset);
                    state.offset += taken;
                    advance -= taken;
                    voice.played_sample_count += taken;
                    if (state.offset == current.end) {
                        if (current.is_looping) {
                            state.offset = current.start;
                        } else {
                            current.valid = false;
                            current.data.clear();
                            current.decoded.clear();
                            voice.num_wave_buffers_consumed++;
                            state.crnt_slot = (state.crnt_slot + 1) % 4;
                            state.offset = state.slots[state.crnt_slot].start;
                        }
                    }
                }
            }
        }
    }
    // Apply mix volumes and route submixes in dependency order. Cycles are
    // ignored rather than repeatedly amplifying the audio.
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
            const auto count = std::min<u32>(mixes[m].buffer_count, 24);
            for (u32 channel = 0; channel < count; channel++)
                for (usize frame = 0; frame < output_frames; frame++)
                    mix_samples[mixIndex(m, channel, frame)] *= mixes[m].volume;
            for (usize dest = 0; dest < mix_count; dest++) {
                if (dest == m || !mixes[dest].is_used ||
                    mixes[dest].mix_id != mixes[m].dest_mix_id)
                    continue;
                for (u32 src = 0; src < count; src++)
                    for (u32 out = 0;
                         out < std::min<u32>(mixes[dest].buffer_count, 24);
                         out++) {
                        const f32 gain = mixes[m].mix[src][out];
                        if (gain == 0.0f)
                            continue;
                        for (usize frame = 0; frame < output_frames; frame++)
                            mix_samples[mixIndex(dest, out, frame)] +=
                                mix_samples[mixIndex(m, src, frame)] * gain;
                    }
            }
        }
        if (!progress)
            break;
    }
    std::vector<f32> stereo(output_frames * 2, 0.0f);
    for (usize m = 0; m < mix_count; m++) {
        if (!mixes[m].is_used || mixes[m].mix_id != 0)
            continue;
        for (const auto& sink : sinks) {
            if (!sink.is_used || sink.type != 1)
                continue;
            u32 count = 0;
            std::memcpy(&count, sink.data + 256, 4);
            count = std::min<u32>(count, 6);
            for (usize frame = 0; frame < output_frames; frame++) {
                f32 inputs[6]{};
                for (u32 c = 0; c < count; c++) {
                    const u8 channel = sink.data[260 + c];
                    if (channel < std::min<u32>(mixes[m].buffer_count, 24))
                        inputs[c] = mix_samples[mixIndex(m, channel, frame)];
                }
                // Nintendo surround order: front L/R, center, LFE, rear L/R.
                stereo[frame * 2] += inputs[0] + inputs[2] * 0.70710678f +
                                     inputs[3] * 0.5f + inputs[4] * 0.70710678f;
                stereo[frame * 2 + 1] += (count == 1 ? inputs[0] : inputs[1]) +
                                         inputs[2] * 0.70710678f +
                                         inputs[3] * 0.5f +
                                         inputs[5] * 0.70710678f;
            }
        }
    }
    for (auto& sample : stereo)
        sample = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
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
    if (output_frames && queued_samples.load(std::memory_order_relaxed) <
                             params.sample_rate / 5) {
        produced_samples += output_frames;
        last_render_time = now;
        // REV5+ reports DSP frames produced, including queued frames.
        elapsed_frames.store(produced_samples / params.sample_count,
                             std::memory_order_relaxed);
        queued_samples.fetch_add(output_frames, std::memory_order_relaxed);
        output_stream->enqueueBuffer(
            output_frames, std::span(reinterpret_cast<const u8*>(stereo.data()),
                                     stereo.size() * sizeof(f32)));
    }

    if (std::getenv("HYDRA_DUMP_AUDIO") &&
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
                     "head resyncs %llu\n",
                     params.sample_rate, update_count, active, paused, stopped,
                     static_cast<double>(min_pitch),
                     static_cast<double>(max_pitch), rms,
                     static_cast<double>(peak), head_resyncs);
        u32 valid_slots = 0;
        for (const auto& state : voice_states)
            for (const auto& slot : state.slots)
                valid_slots += slot.valid ? 1u : 0u;
        std::fprintf(stderr,
                     "Audio timing: %u Hz, hardware %llu, produced %llu, "
                     "missing %llu, queued %llu, valid waves %u, voice0 "
                     "consumed %u, offset %u, slot %u\n",
                     params.sample_rate,
                     stream_stats.sample_clock_frames - sample_clock_origin,
                     produced_samples, stream_stats.missing_frames,
                     queued_samples.load(), valid_slots,
                     voices.empty() ? 0u : voices[0].num_wave_buffers_consumed,
                     voice_states.empty() ? 0u : voice_states[0].offset,
                     voice_states.empty() ? 0u : voice_states[0].crnt_slot);
    }
    return output_frames != 0;
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
        in_header.mixes_size % sizeof(MixInfoIn) ||
        in_header.sinks_size % sizeof(SinkInfoIn)) {
        ONCE(LOG_WARN(Services,
                      "Audio update rejected: need {} output bytes, got {}, "
                      "input end {}/{}",
                      output_size, out_stream->getSize(), input_end,
                      in_stream->getSize()));
        return MAKE_RESULT(Svc, kernel::Error::InvalidSize);
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
    std::vector<MixInfoIn> mixes(in_header.mixes_size / sizeof(MixInfoIn));
    std::memcpy(mixes.data(), in_stream->getPtr() + mix_offset,
                in_header.mixes_size);
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
        if (voice_in.is_new) {
            voice = {};
            state = {};
        }

        if (voice_in.is_used && voice_in.sample_format == PcmFormat::Adpcm &&
            voice_in.extra_params_sz >= 32) {
            const auto coefficients = readGuest(
                process, reinterpret_cast<uintptr_t>(voice_in.extra_params_ptr),
                32);
            if (coefficients.size() == 32)
                std::memcpy(state.coefficients.data(), coefficients.data(), 32);
        }

        if (voice_in.is_new && voice_in.wave_buffer_head >= 0)
            state.crnt_slot = static_cast<u32>(voice_in.wave_buffer_head) % 4;

        // Only buffers marked new by the client are copied. The host queue owns
        // mixed PCM, so the guest can recycle buffers as soon as they are
        // consumed.
        if (voice_in.is_used) {
            for (u32 j = 0; j < 4; j++) {
                const auto& wave = voice_in.wave_buffers[j];
                if (wave.sent_to_server || !wave.address ||
                    wave.end_sample_offset <= wave.start_sample_offset)
                    continue;
                auto& slot = state.slots[j];
                if (slot.valid && slot.address == wave.address &&
                    slot.start == wave.start_sample_offset &&
                    slot.end == wave.end_sample_offset)
                    continue;
                slot = {};
                slot.address = wave.address;
                slot.start = wave.start_sample_offset;
                slot.end = wave.end_sample_offset;
                slot.is_looping = wave.is_looping;
                slot.data = readGuest(process, wave.address, wave.size);
                if (wave.context_addr && wave.context_sz >= 6) {
                    const auto context =
                        readGuest(process, wave.context_addr, 6);
                    if (context.size() == 6) {
                        std::memcpy(slot.history, context.data() + 2, 4);
                        slot.has_context = true;
                    }
                }
                const auto bytes = pcmSampleBytes(voice_in.sample_format);
                const u64 available = voice_in.sample_format == PcmFormat::Adpcm
                                          ? (voice_in.channel_count == 1
                                                 ? slot.data.size() / 8 * 14
                                                 : 0)
                                          : (bytes && voice_in.channel_count
                                                 ? slot.data.size() / bytes /
                                                       voice_in.channel_count
                                                 : 0);
                slot.end = static_cast<u32>(std::min<u64>(slot.end, available));
                slot.valid = slot.end > slot.start;
                if (j == state.crnt_slot) {
                    state.offset = slot.start;
                    state.fraction = 0.0;
                }
            }
        }

        if (voice_in.is_used &&
            voice_in.play_state == VoicePlayState::Started) {
            const std::array<bool, 4> valid{
                state.slots[0].valid, state.slots[1].valid,
                state.slots[2].valid, state.slots[3].valid};
            const auto head = resumeWaveHead(valid, state.crnt_slot,
                                             voice_in.wave_buffer_head);
            if (head != state.crnt_slot) {
                head_resyncs++;
                state.crnt_slot = head;
                state.offset = state.slots[head].start;
            }
        }

        if (voice_in.is_used &&
            voice_in.play_state == VoicePlayState::Stopped &&
            state.previous_play_state !=
                static_cast<u8>(VoicePlayState::Stopped)) {
            for (u32 slot_index = 0; slot_index < 4; slot_index++) {
                auto& slot = state.slots[state.crnt_slot];
                if (slot.valid) {
                    voice.num_wave_buffers_consumed++;
                    state.crnt_slot = (state.crnt_slot + 1) % 4;
                } else {
                    break;
                }
                slot = {};
            }
            voice.played_sample_count = 0;
            state.offset = 0;
            state.fraction = 0.0;
            state.history[0] = state.history[1] = 0;
        }
        if (voice_in.is_used)
            state.previous_play_state = static_cast<u8>(voice_in.play_state);

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
