#pragma once

#include "core/audio/resampler_tables.hpp"
#include "core/audio/sample_decoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

// The audio renderer's DSP commands, as the Switch runs them for each frame:
// data source (wave buffers, loops, sample rate conversion), biquad filters,
// volume ramp, mix ramp into the mix buffers and depop. Behavior follows the
// renderer's command list (Ryujinx.Audio/Renderer, MIT license, used as the
// reference: DataSourceHelper, ResamplerHelper, BiquadFilterHelper,
// VolumeRampCommand, MixRampCommand, DepopPrepareCommand,
// DepopForMixBuffersCommand, DownMixSurroundToStereoCommand). Samples are
// floats in [-1, 1] instead of the DSP's 16-bit scale.
namespace hydra::audio::dsp {

constexpr u32 WAVE_BUFFER_COUNT = 4;
constexpr u32 MIX_BUFFER_COUNT_MAX = 24;
constexpr u32 VOICE_CHANNEL_COUNT_MAX = 6;
constexpr u32 BIQUAD_FILTER_COUNT = 2;
constexpr u32 RESAMPLER_TAPS_MAX = 8;
// Source samples one output sample may advance by
constexpr f32 RATIO_MAX = 16.0f;

enum class SrcQuality : u8 {
    Default = 0,
    High = 1,
    Low = 2,
};

// VoiceInParameter DecodingBehaviourFlags
namespace decoding {
constexpr u16 PLAYED_SAMPLE_COUNT_RESET_WHEN_LOOPING = 1;
constexpr u16 SKIP_PITCH_AND_SAMPLE_RATE_CONVERSION = 2;
} // namespace decoding

// Taps of the sample rate conversion, and the pitch history kept for it
inline u32 resamplerTaps(SrcQuality quality) {
    return quality == SrcQuality::High ? 8 : 4;
}

// A wave buffer the server took from the client. Its bytes are copied at
// submission, the samples decoded when it is first played.
struct WaveBuffer {
    bool valid{false};
    u64 address{0};
    u32 start{0};
    u32 end{0};
    bool looping{false};
    bool end_of_stream{false};
    // Loops after the first pass (negative: forever), and the range they
    // play when set
    i32 loop_count{0};
    u32 loop_start{0};
    u32 loop_end{0};
    std::vector<u8> data;
    // ADPCM decoder history at the start of the buffer (context), if given
    bool has_context{false};
    i16 context[2]{};
    // Samples interleaved by channel, from sample 0 to the end
    std::vector<f32> decoded;
    bool decoded_ready{false};
};

struct SourceFormat {
    PcmFormat format{PcmFormat::Int16};
    u32 channel_count{1};
    std::span<const i16, 16> adpcm_coefficients;
    u16 decoding_flags{0};
};

// Position in the wave buffers, shared by the channels of a voice
struct VoiceSource {
    std::array<WaveBuffer, WAVE_BUFFER_COUNT> buffers{};
    u32 index{0};
    // Within the range the current pass plays
    u32 offset{0};
    i32 loop_counter{0};
    u64 played{0};
    u32 consumed{0};
    f32 fraction{0.0f};
    // ADPCM decoder history after the last decoded buffer
    i16 adpcm_history[2]{};

    bool hasValidBuffer() const {
        return std::any_of(buffers.begin(), buffers.end(),
                           [](const auto& buffer) { return buffer.valid; });
    }
    // The current pass ends the buffer
    void endBuffer() {
        auto& buffer = buffers[index];
        buffer.valid = false;
        buffer.data.clear();
        buffer.decoded.clear();
        loop_counter = 0;
        consumed++;
        index = (index + 1) % WAVE_BUFFER_COUNT;
        if (buffer.end_of_stream)
            played = 0;
    }
    // The client flushed `count` buffers from the current one: skipped and
    // reported consumed
    void flush(u32 count) {
        for (u32 i = 0; i < count; i++) {
            auto& buffer = buffers[index];
            buffer.valid = false;
            buffer.data.clear();
            buffer.decoded.clear();
            consumed++;
            index = (index + 1) % WAVE_BUFFER_COUNT;
        }
        offset = 0;
        loop_counter = 0;
    }
    // Stopping: the buffers are consumed, the position resets
    void stop() {
        for (auto& buffer : buffers) {
            if (buffer.valid) {
                index = (index + 1) % WAVE_BUFFER_COUNT;
                consumed++;
            }
            buffer = {};
        }
        offset = 0;
        played = 0;
        fraction = 0.0f;
        loop_counter = 0;
        adpcm_history[0] = adpcm_history[1] = 0;
    }
};

inline void decodeWaveBuffer(WaveBuffer& buffer, const SourceFormat& source,
                             i16 (&adpcm_history)[2]) {
    if (buffer.decoded_ready)
        return;
    buffer.decoded_ready = true;
    const u32 channels = std::max(source.channel_count, 1u);
    if (source.format == PcmFormat::Adpcm) {
        if (buffer.has_context) {
            adpcm_history[0] = buffer.context[0];
            adpcm_history[1] = buffer.context[1];
        }
        buffer.decoded =
            decodeDspAdpcm(buffer.data, source.adpcm_coefficients, buffer.end,
                           adpcm_history[0], adpcm_history[1]);
        return;
    }
    const usize samples = static_cast<usize>(buffer.end) * channels;
    buffer.decoded.resize(samples);
    for (usize i = 0; i < samples; i++)
        buffer.decoded[i] = decodePcm(buffer.data, i, source.format);
}

// The range a pass over the buffer plays: the loop range once it looped
inline std::pair<u32, u32> passRange(const WaveBuffer& buffer,
                                     i32 loop_counter) {
    if (loop_counter > 0 && buffer.loop_start != 0 && buffer.loop_end != 0 &&
        buffer.loop_start <= buffer.loop_end)
        return {buffer.loop_start, buffer.loop_end};
    return {buffer.start, buffer.end};
}

// Reads `count` sample frames (all channels) into `out`, advancing through the
// wave buffers. Returns the frames read: fewer when the voice starves, the
// rest of `out` is silence.
inline u32 readSource(VoiceSource& voice, const SourceFormat& source,
                      u32 count, std::span<f32> out) {
    const u32 channels = std::max(source.channel_count, 1u);
    std::fill(out.begin(), out.end(), 0.0f);
    u32 read = 0;
    while (read < count) {
        auto& buffer = voice.buffers[voice.index];
        if (!buffer.valid)
            break;
        decodeWaveBuffer(buffer, source, voice.adpcm_history);
        const auto [first, last] = passRange(buffer, voice.loop_counter);
        const u32 length = last > first ? last - first : 0;
        const usize available = buffer.decoded.size() / channels;
        u32 taken = 0;
        while (read < count && voice.offset < length) {
            const usize position = static_cast<usize>(first) + voice.offset;
            if (position >= available)
                break;
            for (u32 c = 0; c < channels; c++)
                out[static_cast<usize>(read) * channels + c] =
                    buffer.decoded[position * channels + c];
            read++;
            voice.offset++;
            taken++;
        }
        voice.played += taken;
        if (voice.offset < length && taken != 0)
            continue;
        // The pass is over (or nothing could be read: a broken buffer)
        voice.offset = 0;
        if (buffer.looping) {
            voice.loop_counter++;
            if (buffer.loop_count >= 0 &&
                (taken == 0 || voice.loop_counter > buffer.loop_count))
                voice.endBuffer();
            if (taken == 0)
                break;
            if (source.decoding_flags &
                decoding::PLAYED_SAMPLE_COUNT_RESET_WHEN_LOOPING)
                voice.played = 0;
        } else {
            voice.endBuffer();
        }
    }
    return read;
}

// The coefficients for the ratio: 128 phases of `taps` taps
inline std::span<const i16> resamplerCurve(SrcQuality quality, f32 ratio) {
    if (quality == SrcQuality::High) {
        if (ratio <= 1.0f)
            return HIGH_QUALITY_UP;
        if (ratio > 1.333313f)
            return HIGH_QUALITY_DOWN;
        return HIGH_QUALITY_MID;
    }
    if (ratio <= 1.0f)
        return DEFAULT_QUALITY_UP;
    if (ratio > 1.333313f)
        return DEFAULT_QUALITY_DOWN;
    return DEFAULT_QUALITY_MID;
}

// Per channel DSP state of a voice
struct VoiceChannelState {
    // The last source samples, the start of the next frame's input
    std::array<f32, RESAMPLER_TAPS_MAX> pitch{};
    std::array<std::array<f32, 2>, BIQUAD_FILTER_COUNT> biquad{};
    // What the last mix ramp added to each mix buffer, for depop
    std::array<f32, MIX_BUFFER_COUNT_MAX> last_samples{};
};

// Resamples `input` (pitch history, then the frame's source samples, then
// zero padding of `taps`) to `output`. `fraction` carries over frames.
inline void resample(std::span<f32> output, std::span<const f32> input,
                     f32 ratio, f32& fraction, SrcQuality quality) {
    usize index = 0;
    if (quality == SrcQuality::Low) {
        for (auto& sample : output) {
            sample = fraction > 1.0f ? input[index + 1] : input[index];
            fraction += ratio;
            index += static_cast<usize>(fraction);
            fraction -= std::trunc(fraction);
        }
        return;
    }
    const u32 taps = resamplerTaps(quality);
    const auto curve = resamplerCurve(quality, ratio);
    for (auto& sample : output) {
        const usize phase =
            std::min<usize>(static_cast<usize>(fraction * 128.0f), 127) * taps;
        f32 sum = 0.0f;
        for (u32 t = 0; t < taps; t++)
            sum += input[index + t] * (static_cast<f32>(curve[phase + t]) /
                                       32768.0f);
        sample = sum;
        fraction += ratio;
        index += static_cast<usize>(fraction);
        fraction -= std::trunc(fraction);
    }
}

// Data source command: `outputs[c]` gets `sample_count` samples of channel c.
// Returns false if the voice ran out of wave buffers during the frame.
inline bool processDataSource(VoiceSource& voice, const SourceFormat& source,
                              std::span<VoiceChannelState> channels,
                              f32 ratio, SrcQuality quality, u32 sample_count,
                              std::span<std::vector<f32>> outputs) {
    const u32 channel_count = std::max(source.channel_count, 1u);
    ratio = std::clamp(std::isfinite(ratio) ? ratio : 1.0f, 0.0f, RATIO_MAX);
    if (source.decoding_flags & decoding::SKIP_PITCH_AND_SAMPLE_RATE_CONVERSION) {
        std::vector<f32> frames(static_cast<usize>(sample_count) * channel_count);
        const u32 read = readSource(voice, source, sample_count, frames);
        for (u32 c = 0; c < channel_count; c++)
            for (u32 i = 0; i < sample_count; i++)
                outputs[c][i] = frames[static_cast<usize>(i) * channel_count + c];
        return read == sample_count;
    }
    const u32 taps = resamplerTaps(quality);
    const u32 needed =
        static_cast<u32>(std::trunc(voice.fraction + ratio * static_cast<f32>(sample_count)));
    std::vector<f32> frames(static_cast<usize>(needed) * channel_count);
    const u32 read = readSource(voice, source, needed, frames);
    std::vector<f32> input(taps + needed + taps + 1);
    f32 fraction = voice.fraction;
    for (u32 c = 0; c < channel_count; c++) {
        auto& state = channels[c];
        std::fill(input.begin(), input.end(), 0.0f);
        std::copy_n(state.pitch.begin(), taps, input.begin());
        for (u32 i = 0; i < needed; i++)
            input[taps + i] = frames[static_cast<usize>(i) * channel_count + c];
        fraction = voice.fraction;
        resample(std::span(outputs[c]).first(sample_count), input, ratio,
                 fraction, quality);
        // The window of the next frame starts after the samples consumed
        std::copy_n(input.begin() + needed, taps, state.pitch.begin());
    }
    voice.fraction = fraction;
    return read == needed;
}

// Biquad filter (transposed direct form 2), Q14 coefficients. The
// denominators are stored negated.
inline void processBiquad(std::span<f32> samples, const i16 (&numerator)[3],
                          const i16 (&denominator)[2],
                          std::array<f32, 2>& state) {
    const f32 b0 = static_cast<f32>(numerator[0]) / 16384.0f;
    const f32 b1 = static_cast<f32>(numerator[1]) / 16384.0f;
    const f32 b2 = static_cast<f32>(numerator[2]) / 16384.0f;
    const f32 a1 = static_cast<f32>(denominator[0]) / 16384.0f;
    const f32 a2 = static_cast<f32>(denominator[1]) / 16384.0f;
    for (auto& sample : samples) {
        const f32 input = sample;
        const f32 output = input * b0 + state[0];
        state[0] = input * b1 + output * a1 + state[1];
        state[1] = input * b2 + output * a2;
        sample = output;
    }
    // A filter blowing up must not poison later frames
    for (auto& value : state)
        if (!std::isfinite(value))
            value = 0.0f;
}

// Volume ramp: from `from` to `to` over the frame
inline void processVolumeRamp(std::span<f32> samples, f32 from, f32 to) {
    const f32 step = (to - from) / static_cast<f32>(samples.size());
    f32 volume = from;
    for (auto& sample : samples) {
        sample *= volume;
        volume += step;
    }
}

// Mix ramp: adds `input` with a gain ramping from `from` to `to`, returns the
// last value added (depop)
inline f32 processMixRamp(std::span<f32> output, std::span<const f32> input,
                          f32 from, f32 to) {
    const f32 step = (to - from) / static_cast<f32>(output.size());
    f32 volume = from;
    f32 last = 0.0f;
    for (usize i = 0; i < output.size(); i++) {
        last = input[i] * volume;
        output[i] += last;
        volume += step;
    }
    return last;
}

// Depop decay per sample at the mix's sample rate
inline f32 depopDecay(u32 sample_rate) {
    return sample_rate == 48000 ? 0.962189f : 0.943695f;
}

// Fades the value a stopped voice left in a mix buffer out, returns what is
// left for the next frame
inline f32 processDepop(std::span<f32> buffer, f32 depop, f32 decay) {
    for (auto& sample : buffer) {
        depop *= decay;
        sample += depop;
    }
    return std::abs(depop) < 1e-9f ? 0.0f : depop;
}

// Device sink downmix of 5.1 (front L/R, center, LFE, back L/R) to stereo
constexpr std::array<f32, 4> DEFAULT_SURROUND_TO_STEREO{1.0f, 0.707f, 0.251f,
                                                        0.707f};
inline f32 downmixSurroundToStereo(const std::array<f32, 4>& coefficients,
                                   f32 front, f32 center, f32 lfe, f32 back) {
    return coefficients[0] * front + coefficients[1] * center +
           coefficients[2] * lfe + coefficients[3] * back;
}

} // namespace hydra::audio::dsp
