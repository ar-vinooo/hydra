#pragma once

#include "common/types.hpp"
#include "core/audio/renderer_dsp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

// The audio renderer's effects, as the Switch runs them for each frame:
// buffer mix, aux and capture buffers (ring buffers shared with the game),
// delay, reverb, 3D reverb, biquad filter, limiter and compressor, plus the
// server side update/status rules of an effect slot. Behavior follows the
// renderer (Ryujinx.Audio/Renderer, MIT license, used as the reference:
// Server/Effect/*, StateUpdater.UpdateEffects*, CommandGenerator.GenerateEffect*,
// Dsp/Command/{AuxiliaryBuffer,CaptureBuffer,Delay,Reverb,Reverb3d,
// BiquadFilter,LimiterVersion1/2,Compressor,Mix}Command, Dsp/State/*,
// Dsp/Effect/*). Samples are floats in [-1, 1] instead of the DSP's 16-bit
// scale; the few places that depend on the absolute scale convert (see aux,
// capture and limiter).
namespace hydra::audio::dsp::effects {

constexpr u32 CHANNEL_COUNT_MAX = 6;
constexpr usize SPECIFIC_DATA_SIZE = 0xA0;
constexpr usize RESULT_STATE_SIZE = 0x80;
constexpr i32 UNUSED_MIX_ID = std::numeric_limits<i32>::max();

enum class EffectType : u8 {
    Invalid = 0,
    BufferMix = 1,
    AuxiliaryBuffer = 2,
    Delay = 3,
    Reverb = 4,
    Reverb3d = 5,
    BiquadFilter = 6,
    Limiter = 7,
    CaptureBuffer = 8,
    Compressor = 9,
};

// State reported to the client in EffectOutStatus
enum class EffectState : u8 {
    Enabled = 3,
    Disabled = 4,
};

// Server usage state, also the type of the parameters' Status fields (the
// client marks Invalid/New, the server sets Enabled once commands were made)
enum class UsageState : u8 {
    Invalid = 0,
    New = 1,
    Enabled = 2,
    Disabled = 3,
};

inline bool isChannelCountValid(u32 count) {
    return count == 1 || count == 2 || count == 4 || count == 6;
}

#pragma pack(push, 1)

struct EffectInParameter {
    EffectType type;
    u8 is_new;
    u8 is_enabled;
    u8 reserved0;
    i32 mix_id;
    u64 buffer_base;
    u64 buffer_size;
    u32 processing_order;
    u32 reserved1;
    u8 specific[SPECIFIC_DATA_SIZE];
};
static_assert(sizeof(EffectInParameter) == 0xC0);
static_assert(offsetof(EffectInParameter, mix_id) == 0x4);
static_assert(offsetof(EffectInParameter, buffer_base) == 0x8);
static_assert(offsetof(EffectInParameter, processing_order) == 0x18);
static_assert(offsetof(EffectInParameter, specific) == 0x20);

struct EffectOutStatusV1 {
    EffectState state;
    u8 reserved[15];
};
static_assert(sizeof(EffectOutStatusV1) == 0x10);

struct EffectOutStatusV2 {
    EffectState state;
    u8 reserved[15];
    u8 result_state[RESULT_STATE_SIZE];
};
static_assert(sizeof(EffectOutStatusV2) == 0x90);

struct BufferMixParameter {
    u8 input[MIX_BUFFER_COUNT_MAX];
    u8 output[MIX_BUFFER_COUNT_MAX];
    f32 volumes[MIX_BUFFER_COUNT_MAX];
    u32 mix_count;
};
static_assert(sizeof(BufferMixParameter) == 0x94);

// Aux and capture. The info addresses point at an AuxiliaryBufferHeader (CPU
// info at +0, DSP info at +0x40), the samples follow at +0x80.
struct AuxiliaryBufferParameter {
    u8 input[MIX_BUFFER_COUNT_MAX];
    u8 output[MIX_BUFFER_COUNT_MAX];
    u32 channel_count;
    u32 sample_rate;
    u32 buffer_storage_size;
    u32 channel_count_max;
    u64 send_buffer_info_address;
    u64 send_buffer_storage_address;
    u64 return_buffer_info_address;
    u64 return_buffer_storage_address;
    u32 mix_buffer_sample_size;
    u32 total_sample_count;
    u32 mix_buffer_sample_count;
};
static_assert(sizeof(AuxiliaryBufferParameter) == 0x6C);
static_assert(offsetof(AuxiliaryBufferParameter, send_buffer_info_address) == 0x40);

// Gains are Q14, sample_rate is Hz in Q14, times in milliseconds
struct DelayParameter {
    u8 input[CHANNEL_COUNT_MAX];
    u8 output[CHANNEL_COUNT_MAX];
    u16 channel_count_max;
    u16 channel_count;
    u32 delay_time_max;
    u32 delay_time;
    u32 sample_rate;
    u32 in_gain;
    u32 feedback_gain;
    u32 out_gain;
    u32 dry_gain;
    u32 channel_spread;
    u32 low_pass_amount;
    UsageState status;
};
static_assert(sizeof(DelayParameter) == 0x35);
static_assert(offsetof(DelayParameter, delay_time_max) == 0x10);
static_assert(offsetof(DelayParameter, status) == 0x34);

// Gains and times are Q14, sample_rate is kHz in Q14
struct ReverbParameter {
    u8 input[CHANNEL_COUNT_MAX];
    u8 output[CHANNEL_COUNT_MAX];
    u16 channel_count_max;
    u16 channel_count;
    i32 sample_rate;
    u32 early_mode;
    i32 early_gain;
    i32 pre_delay_time;
    u32 late_mode;
    i32 late_gain;
    i32 decay_time;
    i32 high_frequency_decay_ratio;
    i32 coloration;
    i32 reverb_gain;
    i32 out_gain;
    i32 dry_gain;
    UsageState status;
};
static_assert(sizeof(ReverbParameter) == 0x41);
static_assert(offsetof(ReverbParameter, sample_rate) == 0x10);
static_assert(offsetof(ReverbParameter, status) == 0x40);

// I3DL2 parameters (gains in millibels, times in seconds), sample_rate in Hz
struct Reverb3dParameter {
    u8 input[CHANNEL_COUNT_MAX];
    u8 output[CHANNEL_COUNT_MAX];
    u16 channel_count_max;
    u16 channel_count;
    u32 reserved;
    u32 sample_rate;
    f32 room_hf;
    f32 hf_reference;
    f32 decay_time;
    f32 hf_decay_ratio;
    f32 room_gain;
    f32 reflections_gain;
    f32 reverb_gain;
    f32 diffusion;
    f32 reflection_delay;
    f32 reverb_delay_time;
    f32 density;
    f32 dry_gain;
    UsageState parameter_status;
};
static_assert(sizeof(Reverb3dParameter) == 0x49);
static_assert(offsetof(Reverb3dParameter, sample_rate) == 0x14);
static_assert(offsetof(Reverb3dParameter, parameter_status) == 0x48);

struct BiquadFilterEffectParameter {
    u8 input[CHANNEL_COUNT_MAX];
    u8 output[CHANNEL_COUNT_MAX];
    i16 numerator[3];
    i16 denominator[2];
    u8 channel_count;
    UsageState status;
};
static_assert(sizeof(BiquadFilterEffectParameter) == 0x18);
static_assert(offsetof(BiquadFilterEffectParameter, numerator) == 0xC);

// Threshold and gains are linear, relative to full scale (32767)
struct LimiterParameter {
    u8 input[CHANNEL_COUNT_MAX];
    u8 output[CHANNEL_COUNT_MAX];
    u16 channel_count_max;
    u16 channel_count;
    i32 sample_rate;
    i32 look_ahead_time_max;
    i32 attack_time;
    i32 release_time;
    i32 look_ahead_time;
    f32 attack_coefficient;
    f32 release_coefficient;
    f32 threshold;
    f32 input_gain;
    f32 output_gain;
    i32 delay_buffer_sample_count_min;
    i32 delay_buffer_sample_count_max;
    UsageState status;
    u8 statistics_enabled;
    u8 statistics_reset;
    u8 reserved;
};
static_assert(sizeof(LimiterParameter) == 0x44);
static_assert(offsetof(LimiterParameter, threshold) == 0x2C);
static_assert(offsetof(LimiterParameter, status) == 0x40);

// V2 result state of a limiter
struct LimiterStatistics {
    f32 input_max[CHANNEL_COUNT_MAX];
    f32 compression_gain_min[CHANNEL_COUNT_MAX];

    void reset() {
        std::fill(std::begin(input_max), std::end(input_max), 0.0f);
        std::fill(std::begin(compression_gain_min),
                  std::end(compression_gain_min), 1.0f);
    }
};
static_assert(sizeof(LimiterStatistics) == 0x30);

// Threshold, output gain in dB (full scale = 32768); input_gain is the
// detector's averaging factor
struct CompressorParameter {
    u8 input[CHANNEL_COUNT_MAX];
    u8 output[CHANNEL_COUNT_MAX];
    u16 channel_count_max;
    u16 channel_count;
    i32 sample_rate;
    f32 threshold;
    f32 ratio;
    i32 attack_time;
    i32 release_time;
    f32 input_gain;
    f32 attack_coefficient;
    f32 release_coefficient;
    f32 output_gain;
    UsageState status;
    u8 makeup_gain_enabled;
    u8 reserved[2];
};
static_assert(sizeof(CompressorParameter) == 0x38);
static_assert(offsetof(CompressorParameter, status) == 0x34);

#pragma pack(pop)

static_assert(sizeof(BufferMixParameter) <= SPECIFIC_DATA_SIZE &&
              sizeof(LimiterStatistics) <= RESULT_STATE_SIZE);

// Revision gates of the effects (BehaviourContext)
struct Behaviour {
    // REV3: reverb pre-delay of up to 350 ms instead of 150 ms
    bool long_size_pre_delay{true};
    // REV6: a biquad effect marked New keeps its filter state
    bool biquad_filter_effect_state_clear_fix{true};
    // REV9: EffectInParameterVersion2/OutStatusVersion2, limiter statistics,
    // compressor parameters
    bool effect_info_v2{true};
    // REV11: 5.1 channel order of delay/reverb/reverb 3D (FL FR FC LFE BL BR)
    bool new_effect_channel_mapping{true};
    // Behaviour flag bit 0: work buffers count as mapped even without a pool
    bool memory_pool_force_mapping{false};

    static constexpr i32 BASE_REVISION_MAGIC =
        ('R' << 0) | ('E' << 8) | ('V' << 16) | ('0' << 24);
    static constexpr i32 LAST_REVISION = 12 << 24;

    static i32 revisionNumber(i32 revision) {
        return (revision - BASE_REVISION_MAGIC) >> 24;
    }
    // CheckFeatureSupported, including its comparison of the number with the
    // shifted last revision
    static bool supported(i32 revision, i32 supported_revision) {
        i32 a = revisionNumber(revision);
        i32 b = revisionNumber(supported_revision);
        if (a > LAST_REVISION)
            a = 1;
        if (b > LAST_REVISION)
            b = 1;
        return a >= b;
    }
    static i32 revisionMagic(u32 n) {
        return static_cast<i32>(static_cast<u32>(BASE_REVISION_MAGIC) + (n << 24));
    }
    // `revision_magic` is the user revision ('REVn' + further revisions added
    // in the top byte)
    static Behaviour fromRevision(u32 revision_magic) {
        const i32 revision = static_cast<i32>(revision_magic);
        Behaviour behaviour;
        behaviour.long_size_pre_delay = supported(revision, revisionMagic(3));
        behaviour.biquad_filter_effect_state_clear_fix =
            supported(revision, revisionMagic(6));
        behaviour.effect_info_v2 = supported(revision, revisionMagic(9));
        behaviour.new_effect_channel_mapping =
            supported(revision, revisionMagic(11));
        return behaviour;
    }
};

// Guest memory the aux and capture ring buffers live in (guest virtual
// addresses from the parameters)
struct GuestMemory {
    virtual ~GuestMemory() = default;
    virtual bool read(u64 address, void* data, u64 size) = 0;
    virtual bool write(u64 address, const void* data, u64 size) = 0;
};

// Q14 fixed point to float
inline f32 q14(i64 value) {
    return static_cast<f32>(value) / 16384.0f;
}

// Pow10 and Cos/Sin in degrees of the DSP (Pow10 saturates like the
// hardware's table)
inline f32 pow10Dsp(f32 x) {
    if (x >= 0.0f)
        return 1.0f;
    if (x <= -5.3f)
        return 0.0f;
    return std::pow(10.0f, x);
}
inline f32 cosDegrees(f32 degrees) {
    return std::cos(degrees * std::numbers::pi_v<f32> / 180.0f);
}
inline f32 sinDegrees(f32 degrees) {
    return std::sin(degrees * std::numbers::pi_v<f32> / 180.0f);
}
inline f32 decibelToLinear(f32 db) {
    return std::pow(10.0f, db / 20.0f);
}
inline f32 finiteOr(f32 value, f32 fallback) {
    return std::isfinite(value) ? value : fallback;
}

// Samples for a time at a rate (per the time's unit), rounded to nearest even
inline u32 delaySampleCount(u32 sample_rate, f32 time) {
    const f32 samples = static_cast<f32>(sample_rate) * time;
    if (!(samples > 0.0f))
        return 0;
    return static_cast<u32>(std::min(std::nearbyint(samples), 16777216.0f));
}

// Tap `index` samples back from `base` in a ring of `count` samples; out of
// range taps wrap instead of faulting
inline f32 tapRing(const std::vector<f32>& buffer, i64 base, i64 index,
                   i64 count) {
    i64 target = base - index;
    if (target < 0)
        target += count;
    const i64 size = static_cast<i64>(buffer.size());
    if (size == 0)
        return 0.0f;
    if (target < 0 || target >= size)
        target = ((target % size) + size) % size;
    return buffer[static_cast<usize>(target)];
}

// Delay line of the delay and reverb effects. Update() wraps at the last
// index, so it delays by one sample less than configured (as the DSP does).
struct DelayLine {
    std::vector<f32> buffer;
    u32 sample_rate{0};
    u32 current_index{0};
    u32 last_index{0};
    u32 current_count{0};
    u32 count_max{0};

    void init(u32 rate, f32 delay_time_max) {
        sample_rate = rate;
        count_max = delaySampleCount(rate, delay_time_max);
        buffer.assign(static_cast<usize>(count_max) + 1, 0.0f);
        setDelay(delay_time_max);
    }
    void setDelay(f32 delay_time) {
        current_count = std::min(count_max, delaySampleCount(sample_rate, delay_time));
        current_index = 0;
        last_index = current_count == 0 ? 0 : current_count - 1;
    }
    f32 read() const { return buffer[current_index]; }
    f32 update(f32 value) {
        const f32 output = read();
        buffer[current_index++] = value;
        if (current_index >= last_index)
            current_index = 0;
        return output;
    }
    f32 tapUnsafe(u32 index, i32 offset) const {
        return tapRing(buffer, current_index, static_cast<i64>(index) + offset,
                       current_count);
    }
    f32 tap(u32 index) const {
        if (current_count == 0)
            return read();
        return tapUnsafe(std::min(index, current_count - 1), -1);
    }
};

// Delay line of the 3D reverb: writes `current_count` samples ahead of the
// read position. The reference wraps the positions at count_max but taps a
// ring of count_max + 1, so the first sample written stays in the last slot
// and is tapped forever; the positions wrap at the ring size here.
struct DelayLine3d {
    std::vector<f32> buffer;
    u32 sample_rate{0};
    u32 current_index{0};
    u32 last_index{0};
    u32 current_count{0};
    u32 count_max{0};

    void init(u32 rate, f32 delay_time_max) {
        sample_rate = rate;
        count_max = delaySampleCount(rate, delay_time_max);
        buffer.assign(static_cast<usize>(count_max) + 1, 0.0f);
        current_index = 0;
        last_index = 0;
        current_count = 0;
        setDelay(delay_time_max);
    }
    void setDelay(f32 delay_time) {
        const u32 target = delaySampleCount(sample_rate, delay_time);
        if (count_max >= target) {
            current_count = target;
            last_index = (current_index + target) % (count_max + 1);
        }
    }
    f32 read() const { return buffer[current_index]; }
    f32 update(f32 value) {
        buffer[last_index++] = value;
        const f32 output = read();
        current_index++;
        if (current_index > count_max)
            current_index = 0;
        if (last_index > count_max)
            last_index = 0;
        return output;
    }
    f32 tapUnsafe(u32 index, i32 offset) const {
        return tapRing(buffer, last_index, static_cast<i64>(index) + offset,
                       static_cast<i64>(count_max) + 1);
    }
    f32 tap(u32 index) const { return tapUnsafe(index, -1); }
};

// All-pass style decay around a delay line
template <typename Line>
struct DecayDelay {
    Line line;
    f32 decay_rate{0.0f};

    f32 update(f32 value) {
        const f32 processed = value - decay_rate * line.read();
        return line.update(processed) + processed * decay_rate;
    }
};

// Buffers of an effect's channels (the reference's buffer indices)
using ChannelBuffers = std::array<std::span<f32>, CHANNEL_COUNT_MAX>;

// ---------------------------------------------------------------- Delay

struct DelayState {
    bool initialized{false};
    u32 channel_count{0};
    std::array<DelayLine, CHANNEL_COUNT_MAX> lines{};
    std::array<f32, CHANNEL_COUNT_MAX> low_pass_z{};
    f32 feedback_gain{0.0f};
    f32 feedback_base_gain{0.0f};
    f32 feedback_cross_gain{0.0f};
    f32 low_pass_feedback_gain{0.0f};
    f32 low_pass_base_gain{0.0f};

    void init(const DelayParameter& parameter) {
        *this = {};
        initialized = true;
        channel_count = std::min<u32>(parameter.channel_count, CHANNEL_COUNT_MAX);
        // Hz in Q14 to samples per millisecond
        const u32 rate = (parameter.sample_rate >> 14) / 1000;
        for (u32 c = 0; c < channel_count; c++) {
            lines[c].init(rate, static_cast<f32>(parameter.delay_time_max));
            lines[c].setDelay(static_cast<f32>(parameter.delay_time));
        }
        updateParameter(parameter);
    }
    void updateParameter(const DelayParameter& parameter) {
        feedback_gain = q14(parameter.feedback_gain) * 0.98f;
        const f32 spread = q14(parameter.channel_spread);
        feedback_base_gain = (1.0f - spread) * feedback_gain;
        if (parameter.channel_count == 4 || parameter.channel_count == 6)
            feedback_cross_gain = spread * 0.5f * feedback_gain;
        else
            feedback_cross_gain = spread * feedback_gain;
        low_pass_feedback_gain = 0.95f * q14(parameter.low_pass_amount);
        low_pass_base_gain = 1.0f - low_pass_feedback_gain;
    }
};

// Feedback matrix between the channels' delay lines
inline std::array<std::array<f32, CHANNEL_COUNT_MAX>, CHANNEL_COUNT_MAX>
delayFeedbackMatrix(const DelayState& state, const DelayParameter& parameter,
                    u32 channel_count) {
    std::array<std::array<f32, CHANNEL_COUNT_MAX>, CHANNEL_COUNT_MAX> m{};
    const f32 b = state.feedback_base_gain;
    const f32 x = state.feedback_cross_gain;
    switch (channel_count) {
    case 1:
        // Mono uses the raw feedback gain (no 0.98 factor)
        m[0][0] = q14(parameter.feedback_gain);
        break;
    case 2:
        m[0] = {b, x};
        m[1] = {x, b};
        break;
    case 4:
        m[0] = {b, x, x, 0.0f};
        m[1] = {x, b, 0.0f, x};
        m[2] = {x, 0.0f, b, x};
        m[3] = {0.0f, x, x, b};
        break;
    case 6:
        m[0] = {b, 0.0f, x, 0.0f, x, 0.0f};
        m[1] = {0.0f, b, x, 0.0f, 0.0f, x};
        m[2] = {x, x, b, 0.0f, 0.0f, 0.0f};
        // LFE only feeds back into itself, with the raw gain
        m[3] = {0.0f, 0.0f, 0.0f, q14(parameter.feedback_gain), 0.0f, 0.0f};
        m[4] = {x, 0.0f, 0.0f, 0.0f, b, x};
        m[5] = {0.0f, x, 0.0f, 0.0f, x, b};
        break;
    default:
        break;
    }
    return m;
}

// Delay command (enabled). The DSP scales by 64 around the lines, which a
// linear float path does not need.
inline void processDelay(DelayState& state, const DelayParameter& parameter,
                         const ChannelBuffers& inputs,
                         const ChannelBuffers& outputs, u32 sample_count) {
    const u32 channels = std::min(state.channel_count,
                                  static_cast<u32>(parameter.channel_count));
    const f32 in_gain = q14(parameter.in_gain);
    const f32 dry_gain = q14(parameter.dry_gain);
    const f32 out_gain = q14(parameter.out_gain);
    const auto m = delayFeedbackMatrix(state, parameter, channels);
    std::array<f32, CHANNEL_COUNT_MAX> input{};
    std::array<f32, CHANNEL_COUNT_MAX> delayed{};
    for (u32 i = 0; i < sample_count; i++) {
        for (u32 c = 0; c < channels; c++) {
            input[c] = inputs[c][i];
            delayed[c] = state.lines[c].read();
        }
        for (u32 c = 0; c < channels; c++) {
            f32 feedback = 0.0f;
            for (u32 k = 0; k < channels; k++)
                feedback += m[c][k] * delayed[k];
            const f32 value = feedback + input[c] * in_gain;
            const f32 low_pass = state.low_pass_feedback_gain * state.low_pass_z[c] +
                                 value * state.low_pass_base_gain;
            state.low_pass_z[c] = low_pass;
            state.lines[c].update(low_pass);
        }
        for (u32 c = 0; c < channels; c++)
            outputs[c][i] = input[c] * dry_gain + delayed[c] * out_gain;
    }
}

// --------------------------------------------------------------- Reverb

namespace reverb_tables {
// Per late mode (Room, Hall, Plate, Cathedral, Max), milliseconds
constexpr std::array<f32, 20> FDN_DELAY_TIMES{
    53.953247f, 79.192566f, 116.238770f, 130.615295f,
    53.953247f, 79.192566f, 116.238770f, 170.615295f,
    5.0f,       10.0f,      5.0f,        10.0f,
    47.03f,     71.0f,      103.0f,      170.0f,
    53.953247f, 79.192566f, 116.238770f, 170.615295f,
};
constexpr std::array<f32, 20> DECAY_DELAY_TIMES{
    7.0f, 9.0f, 13.0f, 17.0f, 7.0f, 9.0f, 13.0f, 17.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 7.0f,  7.0f,  13.0f, 9.0f, 7.0f, 9.0f, 13.0f, 17.0f,
};
// Per early mode (Room, Chamber, Hall, Cathedral, Disabled)
constexpr std::array<f32, 50> EARLY_DELAY_TIMES{
    0.0f,  3.5f,  2.8f,  3.9f,  2.7f,  13.4f, 7.9f,  8.4f,  9.9f,  12.0f,
    0.0f,  11.8f, 5.5f,  11.2f, 10.4f, 38.1f, 22.2f, 29.6f, 21.2f, 24.8f,
    0.0f,  41.5f, 20.5f, 41.3f, 0.0f,  29.5f, 33.8f, 45.2f, 46.8f, 0.0f,
    33.1f, 43.3f, 22.8f, 37.9f, 14.9f, 35.3f, 17.9f, 34.2f, 0.0f,  43.3f,
    0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,
};
constexpr std::array<f32, 50> EARLY_GAIN_BASE{
    0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.68f, 0.68f,
    0.70f, 0.68f, 0.70f, 0.68f, 0.70f, 0.68f, 0.68f, 0.68f, 0.68f, 0.68f,
    0.50f, 0.70f, 0.70f, 0.68f, 0.50f, 0.68f, 0.68f, 0.70f, 0.68f, 0.00f,
    0.93f, 0.92f, 0.87f, 0.86f, 0.94f, 0.81f, 0.80f, 0.77f, 0.76f, 0.65f,
    0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f,
};
constexpr std::array<f32, 5> PRE_DELAY_TIMES{12.5f, 40.0f, 50.0f, 50.0f, 0.0f};
constexpr u32 MODE_COUNT = 5;
constexpr u32 LATE_MODE_MAX = 4;
} // namespace reverb_tables

struct ReverbState {
    static constexpr u32 EARLY_COUNT = 10;
    bool initialized{false};
    std::array<DelayLine, 4> fdn{};
    std::array<DecayDelay<DelayLine>, 4> decay{};
    DelayLine pre_delay{};
    DelayLine front_center{};
    std::array<u32, EARLY_COUNT> early_delay_time{};
    std::array<f32, EARLY_COUNT> early_gain{};
    u32 pre_delay_time{0};
    std::array<f32, 4> hf_decay_direct_gain{};
    std::array<f32, 4> hf_decay_previous_gain{};
    std::array<f32, 4> previous_feedback_output{};

    // Samples per millisecond
    static u32 rate(const ReverbParameter& parameter) {
        return static_cast<u32>(q14(static_cast<u32>(parameter.sample_rate)));
    }

    void init(const ReverbParameter& parameter, bool long_size_pre_delay) {
        *this = {};
        initialized = true;
        const u32 sample_rate = rate(parameter);
        // Sized for the longest mode
        for (u32 i = 0; i < 4; i++) {
            fdn[i].init(sample_rate, reverb_tables::FDN_DELAY_TIMES[16 + i]);
            decay[i].line.init(sample_rate, reverb_tables::DECAY_DELAY_TIMES[16 + i]);
        }
        pre_delay.init(sample_rate, long_size_pre_delay ? 350.0f : 150.0f);
        front_center.init(sample_rate, 5.0f);
        updateParameter(parameter);
    }

    void updateParameter(const ReverbParameter& parameter) {
        const u32 sample_rate = rate(parameter);
        const f32 pre_delay_ms = q14(parameter.pre_delay_time);
        const f32 early_gain_value = q14(parameter.early_gain);
        const f32 coloration = q14(parameter.coloration);
        const f32 decay_time = q14(parameter.decay_time);
        const u32 early_mode =
            std::min(parameter.early_mode, reverb_tables::MODE_COUNT - 1);
        const u32 late_mode =
            std::min(parameter.late_mode, reverb_tables::LATE_MODE_MAX);
        for (u32 i = 0; i < EARLY_COUNT; i++) {
            const u32 entry = early_mode * EARLY_COUNT + i;
            early_delay_time[i] =
                std::min(delaySampleCount(sample_rate,
                                          reverb_tables::EARLY_DELAY_TIMES[entry] +
                                              pre_delay_ms),
                         pre_delay.count_max) +
                1;
            early_gain[i] = reverb_tables::EARLY_GAIN_BASE[entry] * early_gain_value;
        }
        if (parameter.channel_count == 2) {
            early_gain[4] *= 0.5f;
            early_gain[5] *= 0.5f;
        }
        pre_delay_time = std::min(
            delaySampleCount(sample_rate, reverb_tables::PRE_DELAY_TIMES[early_mode] +
                                              pre_delay_ms),
            pre_delay.count_max);

        const f32 hf_decay_ratio = q14(parameter.high_frequency_decay_ratio);
        const f32 rate_f = static_cast<f32>(sample_rate);
        const f32 hf_cos = sample_rate != 0 ? cosDegrees(1280.0f / rate_f) : 1.0f;
        for (u32 i = 0; i < 4; i++) {
            fdn[i].setDelay(reverb_tables::FDN_DELAY_TIMES[late_mode * 4 + i]);
            decay[i].line.setDelay(reverb_tables::DECAY_DELAY_TIMES[late_mode * 4 + i]);
            const f32 a = static_cast<f32>(
                -3 * static_cast<i64>(decay[i].line.current_count + fdn[i].current_count));
            const f32 b = a / (decay_time * rate_f);
            f32 c;
            f32 d;
            if (hf_decay_ratio < 0.995f) {
                const f32 e =
                    pow10Dsp((((1.0f / hf_decay_ratio) - 1.0f) * 2) / 100 * (b / 10));
                const f32 f = 1.0f - e;
                const f32 g = 2.0f - (e * 2 * hf_cos);
                const f32 h = std::sqrt((g * g) - (f * f * 4));
                c = (g - h) / (f * 2);
                d = 1.0f - c;
            } else {
                c = 0.0f;
                d = 1.0f;
            }
            hf_decay_direct_gain[i] = finiteOr(pow10Dsp(b / 1000) * d * 0.7071f, 0.0f);
            hf_decay_previous_gain[i] = finiteOr(c, 0.0f);
            previous_feedback_output[i] = 0.0f;
            decay[i].decay_rate = 0.6f * (1.0f - coloration);
        }
    }
};

namespace reverb_tables {
struct Routing {
    std::span<const i32> output_early;
    std::span<const i32> target_early;
    std::span<const i32> target_feedback;
    std::span<const i32> output;
};
constexpr std::array<i32, 10> MONO_OUTPUT_EARLY{0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr std::array<i32, 10> STEREO_OUTPUT_EARLY{0, 0, 1, 1, 0, 1, 0, 0, 1, 1};
constexpr std::array<i32, 10> QUAD_OUTPUT_EARLY{0, 0, 1, 1, 0, 1, 2, 2, 3, 3};
constexpr std::array<i32, 10> EARLY_IDENTITY{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
constexpr std::array<i32, 20> SURROUND_OUTPUT_EARLY{0, 5, 0, 5, 1, 5, 1, 5, 4, 5,
                                                    4, 5, 2, 5, 2, 5, 3, 5, 3, 5};
constexpr std::array<i32, 20> SURROUND_TARGET_EARLY{0, 0, 1, 1, 2, 2, 3, 3, 4, 4,
                                                    5, 5, 6, 6, 7, 7, 8, 8, 9, 9};
constexpr std::array<i32, 4> MONO_OUTPUT{0, 0, 0, 0};
constexpr std::array<i32, 4> MONO_FEEDBACK{0, 1, 2, 3};
constexpr std::array<i32, 4> STEREO_OUTPUT{0, 0, 1, 1};
constexpr std::array<i32, 4> STEREO_FEEDBACK{2, 0, 3, 1};
constexpr std::array<i32, 4> QUAD_OUTPUT{0, 1, 2, 3};
constexpr std::array<i32, 4> QUAD_FEEDBACK{0, 1, 2, 3};
constexpr std::array<i32, 6> SURROUND_OUTPUT{0, 1, 2, 3, 4, 5};
constexpr std::array<i32, 6> SURROUND_FEEDBACK{0, 1, 2, 3, -1, 3};

inline Routing routing(u32 channel_count) {
    switch (channel_count) {
    case 1:
        return {MONO_OUTPUT_EARLY, EARLY_IDENTITY, MONO_FEEDBACK, MONO_OUTPUT};
    case 2:
        return {STEREO_OUTPUT_EARLY, EARLY_IDENTITY, STEREO_FEEDBACK, STEREO_OUTPUT};
    case 4:
        return {QUAD_OUTPUT_EARLY, EARLY_IDENTITY, QUAD_FEEDBACK, QUAD_OUTPUT};
    default:
        return {SURROUND_OUTPUT_EARLY, SURROUND_TARGET_EARLY, SURROUND_FEEDBACK,
                SURROUND_OUTPUT};
    }
}
} // namespace reverb_tables

// Reverb command (enabled): early reflections tapped off a pre-delay line,
// then a 4 line feedback delay network. Channels are in the legacy order
// (FL FR BL BR FC LFE).
inline void processReverb(ReverbState& state, const ReverbParameter& parameter,
                          const ChannelBuffers& inputs,
                          const ChannelBuffers& outputs, u32 sample_count) {
    const u32 channels = parameter.channel_count;
    const bool surround = channels == 6;
    const auto route = reverb_tables::routing(channels);
    const f32 reverb_gain = q14(parameter.reverb_gain);
    const f32 late_gain = q14(parameter.late_gain);
    const f32 out_gain = q14(parameter.out_gain);
    const f32 dry_gain = q14(parameter.dry_gain);
    std::array<f32, CHANNEL_COUNT_MAX> output{};
    std::array<f32, CHANNEL_COUNT_MAX> input{};
    std::array<f32, 4> feedback{};
    std::array<f32, 4> feedback_output{};
    for (u32 s = 0; s < sample_count; s++) {
        output.fill(0.0f);
        for (usize i = 0; i < route.target_early.size(); i++) {
            const auto early = static_cast<usize>(route.target_early[i]);
            const auto out = static_cast<usize>(route.output_early[i]);
            output[out] += state.pre_delay.tapUnsafe(state.early_delay_time[early], 0) *
                           state.early_gain[early];
        }
        if (surround)
            output[5] *= 0.2f;
        f32 pre_delay_value = 0.0f;
        for (u32 c = 0; c < channels; c++) {
            input[c] = inputs[c][s];
            pre_delay_value += input[c] * reverb_gain;
        }
        state.pre_delay.update(pre_delay_value);
        const f32 late = state.pre_delay.tap(state.pre_delay_time) * late_gain;
        for (u32 i = 0; i < 4; i++) {
            feedback_output[i] =
                state.fdn[i].read() * state.hf_decay_direct_gain[i] +
                state.previous_feedback_output[i] * state.hf_decay_previous_gain[i];
            state.previous_feedback_output[i] = feedback_output[i];
        }
        feedback[0] = feedback_output[2] + feedback_output[1];
        feedback[1] = -feedback_output[0] - feedback_output[3];
        feedback[2] = feedback_output[0] - feedback_output[3];
        feedback[3] = feedback_output[1] - feedback_output[2];
        for (u32 i = 0; i < 4; i++) {
            feedback_output[i] = state.decay[i].update(feedback[i] + late);
            state.fdn[i].update(feedback_output[i]);
        }
        for (usize i = 0; i < route.target_feedback.size(); i++) {
            const i32 target = route.target_feedback[i];
            if (target >= 0)
                output[static_cast<usize>(route.output[i])] +=
                    feedback_output[static_cast<usize>(target)];
        }
        if (surround)
            output[4] += state.front_center.update(
                (feedback_output[2] - feedback_output[3]) * 0.5f);
        for (u32 c = 0; c < channels; c++)
            outputs[c][s] = output[c] * out_gain + input[c] * dry_gain;
    }
}

// ------------------------------------------------------------ Reverb 3D

namespace reverb3d_tables {
constexpr std::array<f32, 4> FDN_DELAY_MIN_TIMES{5.0f, 6.0f, 13.0f, 14.0f};
constexpr std::array<f32, 4> FDN_DELAY_MAX_TIMES{45.704f, 82.782f, 149.94f, 271.58f};
constexpr std::array<f32, 4> DECAY_DELAY_MAX_TIMES1{17.0f, 13.0f, 9.0f, 7.0f};
constexpr std::array<f32, 4> DECAY_DELAY_MAX_TIMES2{19.0f, 11.0f, 10.0f, 6.0f};
constexpr std::array<f32, 20> EARLY_DELAY_TIMES{
    0.017136f, 0.059154f, 0.16173f, 0.39019f, 0.42526f, 0.45541f, 0.68974f,
    0.74591f,  0.83384f,  0.8595f,  0.0f,     0.075024f, 0.16879f, 0.2999f,
    0.33744f,  0.3719f,   0.59901f, 0.71674f, 0.81786f, 0.85166f};
constexpr std::array<f32, 20> EARLY_GAIN{
    0.67096f, 0.61027f, 1.0f,     0.35680f, 0.68361f, 0.65978f, 0.51939f,
    0.24712f, 0.45945f, 0.45021f, 0.64196f, 0.54879f, 0.92925f, 0.38270f,
    0.72867f, 0.69794f, 0.5464f,  0.24563f, 0.45214f, 0.44042f};

constexpr std::array<i32, 20> MONO_OUTPUT_EARLY{};
constexpr std::array<i32, 20> STEREO_OUTPUT_EARLY{0, 0, 0, 1, 1, 1, 1, 0, 0, 0,
                                                  1, 1, 1, 0, 0, 0, 0, 1, 1, 1};
constexpr std::array<i32, 20> QUAD_OUTPUT_EARLY{0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                1, 1, 1, 0, 0, 0, 0, 3, 3, 3};
constexpr std::array<i32, 20> EARLY_IDENTITY{0,  1,  2,  3,  4,  5,  6,
                                             7,  8,  9,  10, 11, 12, 13,
                                             14, 15, 16, 17, 18, 19};
// Indexed by the early delay index like the other layouts, so only the first
// 20 entries are used
constexpr std::array<i32, 40> SURROUND_OUTPUT_EARLY{
    4, 5, 0, 5, 0, 5, 1, 5, 1, 5, 1, 5, 1, 5, 2, 5, 2, 5, 2, 5,
    1, 5, 1, 5, 1, 5, 0, 5, 0, 5, 0, 5, 0, 5, 3, 5, 3, 5, 3, 5};
constexpr std::array<i32, 40> SURROUND_TARGET_EARLY{
    0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,  6,  7,  7,  8,  8,  9,  9,
    10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19};
constexpr std::array<i32, 1> MONO_FEEDBACK{0};
constexpr std::array<i32, 2> STEREO_FEEDBACK{0, 1};
constexpr std::array<i32, 4> QUAD_FEEDBACK{0, 1, 2, 3};
constexpr std::array<i32, 6> SURROUND_FEEDBACK{0, 1, 2, 3, -1, 3};
} // namespace reverb3d_tables

struct Reverb3dState {
    static constexpr u32 EARLY_COUNT = 20;
    bool initialized{false};
    std::array<DelayLine3d, 4> fdn{};
    std::array<DecayDelay<DelayLine3d>, 4> decay1{};
    std::array<DecayDelay<DelayLine3d>, 4> decay2{};
    DelayLine3d pre_delay{};
    DelayLine3d front_center{};
    f32 dry_gain{0.0f};
    std::array<u32, EARLY_COUNT> early_delay_time{};
    f32 previous_pre_delay_value{0.0f};
    f32 previous_pre_delay_gain{0.0f};
    f32 target_pre_delay_gain{0.0f};
    f32 early_reflections_gain{0.0f};
    f32 late_reverb_gain{0.0f};
    u32 reflection_delay_time{0};
    f32 echo_late_reverb_decay{0.0f};
    std::array<f32, 4> decay_direct_fdn_gain{};
    std::array<f32, 4> decay_current_fdn_gain{};
    std::array<f32, 4> decay_current_output_gain{};
    std::array<f32, 4> previous_feedback_output_decayed{};

    void init(const Reverb3dParameter& parameter) {
        *this = {};
        initialized = true;
        const u32 rate = parameter.sample_rate / 1000;
        for (u32 i = 0; i < 4; i++) {
            fdn[i].init(rate, reverb3d_tables::FDN_DELAY_MAX_TIMES[i]);
            decay1[i].line.init(rate, reverb3d_tables::DECAY_DELAY_MAX_TIMES1[i]);
            decay2[i].line.init(rate, reverb3d_tables::DECAY_DELAY_MAX_TIMES2[i]);
        }
        pre_delay.init(rate, 400.0f);
        front_center.init(rate, 5.0f);
        updateParameter(parameter);
    }

    void updateParameter(const Reverb3dParameter& parameter) {
        const u32 rate = parameter.sample_rate / 1000;
        const f32 sample_rate = static_cast<f32>(parameter.sample_rate);
        early_delay_time.fill(0);
        dry_gain = parameter.dry_gain;
        previous_feedback_output_decayed.fill(0.0f);
        previous_pre_delay_value = 0.0f;
        early_reflections_gain = pow10Dsp(
            std::min(parameter.room_gain + parameter.reflections_gain, 5000.0f) / 2000.0f);
        late_reverb_gain = pow10Dsp(
            std::min(parameter.room_gain + parameter.reverb_gain, 5000.0f) / 2000.0f);
        const f32 hf_room_gain = pow10Dsp(parameter.room_hf / 2000.0f);
        if (hf_room_gain < 1.0f) {
            const f32 a = 1.0f - hf_room_gain;
            const f32 b =
                2.0f - ((2.0f * hf_room_gain) *
                        cosDegrees(256.0f * parameter.hf_reference / sample_rate));
            const f32 c = std::sqrt(b * b - (4.0f * (1.0f - hf_room_gain) *
                                             (1.0f - hf_room_gain)));
            previous_pre_delay_gain = finiteOr((b - c) / (2.0f * a), 0.0f);
            target_pre_delay_gain = 1.0f - previous_pre_delay_gain;
        } else {
            previous_pre_delay_gain = 0.0f;
            target_pre_delay_gain = 1.0f;
        }
        reflection_delay_time = delaySampleCount(
            rate, 1000.0f * (parameter.reflection_delay + parameter.reverb_delay_time));
        echo_late_reverb_decay = 0.6f * parameter.diffusion * 0.01f;
        const f32 hf_angle = 128.0f * 0.5f * parameter.hf_reference / sample_rate;
        for (u32 i = 0; i < 4; i++) {
            fdn[i].setDelay(reverb3d_tables::FDN_DELAY_MIN_TIMES[i] +
                            (parameter.density / 100 *
                             (reverb3d_tables::FDN_DELAY_MAX_TIMES[i] -
                              reverb3d_tables::FDN_DELAY_MIN_TIMES[i])));
            const u32 samples = fdn[i].current_count + decay1[i].line.current_count +
                                decay2[i].line.current_count;
            const f32 a = (-60.0f * static_cast<f32>(samples)) /
                          (parameter.decay_time * sample_rate);
            const f32 b = a / parameter.hf_decay_ratio;
            const f32 c = cosDegrees(hf_angle) / sinDegrees(hf_angle);
            const f32 d = pow10Dsp((b - a) / 40.0f);
            const f32 e = pow10Dsp((b + a) / 40.0f) * 0.7071f;
            decay_direct_fdn_gain[i] = finiteOr(e * ((d * c) + 1.0f) / (c + d), 0.0f);
            decay_current_fdn_gain[i] = finiteOr(e * (1.0f - (d * c)) / (c + d), 0.0f);
            decay_current_output_gain[i] = finiteOr((c - d) / (c + d), 0.0f);
            decay1[i].decay_rate = echo_late_reverb_decay;
            decay2[i].decay_rate = echo_late_reverb_decay * -0.9f;
        }
        for (u32 i = 0; i < EARLY_COUNT; i++)
            early_delay_time[i] = std::min(
                delaySampleCount(rate, (parameter.reflection_delay * 1000.0f) +
                                           (reverb3d_tables::EARLY_DELAY_TIMES[i] *
                                            1000.0f *
                                            ((parameter.reverb_delay_time * 0.9998f) +
                                             0.02f))),
                pre_delay.count_max);
    }
};

// Reverb 3D command (enabled), channels in the legacy order
inline void processReverb3d(Reverb3dState& state, const Reverb3dParameter& parameter,
                            const ChannelBuffers& inputs,
                            const ChannelBuffers& outputs, u32 sample_count) {
    using namespace reverb3d_tables;
    const u32 channels = parameter.channel_count;
    const bool mono = channels == 1;
    const bool surround = channels == 6;
    std::span<const i32> output_early = MONO_OUTPUT_EARLY;
    std::span<const i32> target_early = EARLY_IDENTITY;
    std::span<const i32> target_feedback = MONO_FEEDBACK;
    if (channels == 2) {
        output_early = STEREO_OUTPUT_EARLY;
        target_feedback = STEREO_FEEDBACK;
    } else if (channels == 4) {
        output_early = QUAD_OUTPUT_EARLY;
        target_feedback = QUAD_FEEDBACK;
    } else if (surround) {
        output_early = SURROUND_OUTPUT_EARLY;
        target_early = SURROUND_TARGET_EARLY;
        target_feedback = SURROUND_FEEDBACK;
    }
    std::array<f32, CHANNEL_COUNT_MAX> output{};
    std::array<f32, CHANNEL_COUNT_MAX> input{};
    std::array<f32, 4> feedback{};
    std::array<f32, 4> feedback_output{};
    std::array<f32, 4> values{};
    for (u32 s = 0; s < sample_count; s++) {
        output.fill(0.0f);
        const f32 tap_out = state.pre_delay.tapUnsafe(state.reflection_delay_time, 1);
        for (usize i = 0; i < target_early.size(); i++) {
            const auto early = static_cast<usize>(target_early[i]);
            const auto out = static_cast<usize>(output_early[early]);
            output[out] += state.pre_delay.tapUnsafe(state.early_delay_time[early], 1) *
                           EARLY_GAIN[early];
        }
        f32 pre_delay_value = 0.0f;
        for (u32 c = 0; c < channels; c++) {
            input[c] = inputs[c][s];
            pre_delay_value += input[c];
        }
        for (u32 c = 0; c < channels; c++)
            output[c] *= state.early_reflections_gain;
        state.previous_pre_delay_value =
            pre_delay_value * state.target_pre_delay_gain +
            state.previous_pre_delay_value * state.previous_pre_delay_gain;
        state.pre_delay.update(state.previous_pre_delay_value);
        for (u32 i = 0; i < 4; i++) {
            const f32 fdn_value = state.fdn[i].read();
            const f32 value = fdn_value * state.decay_direct_fdn_gain[i] +
                              state.previous_feedback_output_decayed[i];
            state.previous_feedback_output_decayed[i] =
                fdn_value * state.decay_current_fdn_gain[i] +
                value * state.decay_current_output_gain[i];
            feedback_output[i] = value;
        }
        feedback[0] = feedback_output[2] + feedback_output[1];
        feedback[1] = -feedback_output[0] - feedback_output[3];
        feedback[2] = feedback_output[0] - feedback_output[3];
        feedback[3] = feedback_output[1] - feedback_output[2];
        for (u32 i = 0; i < 4; i++) {
            const f32 temp =
                state.decay1[i].update(tap_out * state.late_reverb_gain + feedback[i]);
            values[i] = state.decay2[i].update(temp);
            state.fdn[i].update(values[i]);
        }
        for (usize c = 0; c < target_feedback.size(); c++) {
            const i32 target = target_feedback[c];
            if (target >= 0)
                outputs[c][s] = output[c] + values[static_cast<usize>(target)] +
                                input[c] * state.dry_gain;
        }
        if (mono)
            outputs[0][s] += values[1];
        // The reference adds onto whatever the buffer held; the center gets
        // its own value here
        if (surround)
            outputs[4][s] = output[4] +
                            state.front_center.update((values[2] - values[3]) * 0.5f) +
                            input[4] * state.dry_gain;
    }
}

// --------------------------------------------------------------- Limiter

struct LimiterState {
    bool initialized{false};
    u32 channel_count{0};
    i32 delay_count_max{0};
    std::array<f32, CHANNEL_COUNT_MAX> detector_average{};
    std::array<f32, CHANNEL_COUNT_MAX> compression_gain_average{};
    std::vector<f32> delayed;
    std::array<i32, CHANNEL_COUNT_MAX> delayed_position{};

    void init(const LimiterParameter& parameter) {
        initialized = true;
        channel_count = std::min<u32>(parameter.channel_count, CHANNEL_COUNT_MAX);
        delay_count_max = std::max(parameter.delay_buffer_sample_count_max, 0);
        detector_average.fill(0.0f);
        compression_gain_average.fill(1.0f);
        delayed.assign(static_cast<usize>(channel_count) *
                           static_cast<usize>(delay_count_max),
                       0.0f);
        delayed_position.fill(0);
    }
};

// Limiter command (enabled), V1 without and V2 with `statistics`. The DSP
// works on full scale 32767 (short.MaxValue) values.
inline void processLimiter(LimiterState& state, const LimiterParameter& parameter,
                           const ChannelBuffers& inputs,
                           const ChannelBuffers& outputs, u32 sample_count,
                           LimiterStatistics* statistics) {
    constexpr f32 TO_DSP = 32768.0f / 32767.0f;
    constexpr f32 FROM_DSP = 32767.0f / 32768.0f;
    const u32 channels = std::min(state.channel_count,
                                  static_cast<u32>(parameter.channel_count));
    const i32 delay_min =
        std::min(parameter.delay_buffer_sample_count_min, state.delay_count_max);
    for (u32 c = 0; c < channels; c++) {
        f32& detector = state.detector_average[c];
        f32& gain_average = state.compression_gain_average[c];
        i32& position = state.delayed_position[c];
        for (u32 s = 0; s < sample_count; s++) {
            const f32 input = inputs[c][s] * TO_DSP * parameter.input_gain;
            const f32 input_max = std::abs(input);
            const f32 input_coefficient = input_max > detector
                                              ? parameter.attack_coefficient
                                              : parameter.release_coefficient;
            detector += input_coefficient * (input_max - detector);
            f32 attenuation = 1.0f;
            if (detector > parameter.threshold)
                attenuation = parameter.threshold / detector;
            const f32 output_coefficient = gain_average > attenuation
                                               ? parameter.attack_coefficient
                                               : parameter.release_coefficient;
            gain_average += output_coefficient * (attenuation - gain_average);
            const f32 compression_gain = gain_average;
            const usize index = static_cast<usize>(c) *
                                    static_cast<usize>(state.delay_count_max) +
                                static_cast<usize>(position);
            f32 delayed_sample = 0.0f;
            if (index < state.delayed.size()) {
                delayed_sample = state.delayed[index];
                state.delayed[index] = input;
            }
            outputs[c][s] =
                delayed_sample * compression_gain * parameter.output_gain * FROM_DSP;
            position++;
            if (delay_min <= 0)
                position = 0;
            else
                while (position >= delay_min)
                    position -= delay_min;
            if (statistics) {
                statistics->input_max[c] = std::max(statistics->input_max[c], input_max);
                statistics->compression_gain_min[c] =
                    std::min(statistics->compression_gain_min[c], compression_gain);
            }
        }
    }
}

// ------------------------------------------------------------ Compressor

struct CompressorState {
    bool initialized{false};
    f32 input_average{0.0f};
    f32 unknown4{1.0f};
    f32 compression_gain_average{1.0f};
    f32 gain_reduction{0.0f};
    f32 unknown10{0.0f};
    f32 unknown14{0.0f};
    f32 previous_compression_alpha{0.0f};
    f32 makeup_gain{0.0f};
    f32 output_gain{1.0f};

    void init(const CompressorParameter& parameter) {
        *this = {};
        initialized = true;
        updateParameter(parameter);
    }
    void updateParameter(const CompressorParameter& parameter) {
        const f32 threshold = parameter.threshold;
        const f32 ratio = 1.0f / parameter.ratio;
        const f32 makeup = parameter.makeup_gain_enabled
                               ? (threshold * 0.5f * (ratio - 1.0f)) - 3.0f
                               : 0.0f;
        previous_compression_alpha = parameter.attack_coefficient;
        makeup_gain = makeup;
        gain_reduction = (1.0f - ratio) / static_cast<f32>(CHANNEL_COUNT_MAX);
        unknown10 = threshold - 1.5f;
        unknown14 = threshold + 1.5f;
        output_gain = finiteOr(decibelToLinear(parameter.output_gain + makeup), 1.0f);
    }
};

// Compressor command (enabled): RMS detector over all channels, soft knee of
// 3 dB around the threshold, one gain for every channel
inline void processCompressor(CompressorState& state,
                              const CompressorParameter& parameter,
                              const ChannelBuffers& inputs,
                              const ChannelBuffers& outputs, u32 sample_count) {
    const u32 channels = parameter.channel_count;
    std::array<f32, CHANNEL_COUNT_MAX> input{};
    f32 unknown4 = state.unknown4;
    f32 previous_alpha = state.previous_compression_alpha;
    for (u32 s = 0; s < sample_count; s++) {
        f32 mean_square = 0.0f;
        for (u32 c = 0; c < channels; c++) {
            input[c] = inputs[c][s];
            mean_square += input[c] * input[c];
        }
        mean_square /= static_cast<f32>(channels);
        state.input_average += parameter.input_gain * (mean_square - state.input_average);
        const f32 mean = state.input_average;
        f32 y = std::log10(std::max(mean, 1.0e-10f)) * 10.0f;
        f32 z = 1.0f;
        bool above_knee = y >= state.unknown10;
        if (mean < 1.0e-10f) {
            y = -100.0f;
            above_knee = state.unknown10 <= -100.0f;
        }
        if (above_knee) {
            f32 gain;
            if (y >= state.unknown14)
                gain = ((1.0f / parameter.ratio) - 1.0f) * (y - parameter.threshold);
            else
                gain = (y - state.unknown10) *
                       ((y - state.unknown10) * -state.gain_reduction);
            z = finiteOr(decibelToLinear(gain), 1.0f);
        }
        f32 unknown4_new = z;
        f32 alpha;
        if ((unknown4 - z) <= 0.08f) {
            alpha = parameter.release_coefficient;
            if ((unknown4 - z) >= -0.08f) {
                if (std::abs(state.compression_gain_average - z) >= 0.001f)
                    unknown4_new = unknown4;
                alpha = previous_alpha;
            }
        } else {
            alpha = parameter.attack_coefficient;
        }
        state.compression_gain_average += alpha * (z - state.compression_gain_average);
        const f32 compression_gain = state.compression_gain_average;
        for (u32 c = 0; c < channels; c++)
            outputs[c][s] = input[c] * compression_gain * state.output_gain;
        unknown4 = unknown4_new;
        previous_alpha = alpha;
    }
    state.unknown4 = unknown4;
    state.previous_compression_alpha = previous_alpha;
}

// --------------------------------------------------------- Aux / capture

// AuxiliaryBufferInfo fields
namespace aux_info {
constexpr u64 READ_OFFSET = 0x0;
constexpr u64 WRITE_OFFSET = 0x4;
constexpr u64 LOST_SAMPLE_COUNT = 0x8;
constexpr u64 TOTAL_SAMPLE_COUNT = 0xC;
constexpr u64 INFO_SIZE = 0x40;
constexpr u64 HEADER_SIZE = 0x80;

inline u32 read(GuestMemory& memory, u64 address) {
    u32 value = 0;
    if (!memory.read(address, &value, sizeof(value)))
        return 0;
    return value;
}
inline void write(GuestMemory& memory, u64 address, u32 value) {
    memory.write(address, &value, sizeof(value));
}
// Read and write offsets and the total count; the lost count survives
inline void reset(GuestMemory& memory, u64 info) {
    const u64 zero = 0;
    memory.write(info + READ_OFFSET, &zero, sizeof(zero));
    write(memory, info + TOTAL_SAMPLE_COUNT, 0);
}
} // namespace aux_info

// Mix buffer samples to the int32 DSP scale the game sees (truncating, like
// the DSP's float to int conversion)
inline i32 toDspInt(f32 sample) {
    const f32 value = std::trunc(sample * 32768.0f);
    if (!(value > -2147483648.0f))
        return std::numeric_limits<i32>::min();
    if (value >= 2147483647.0f)
        return std::numeric_limits<i32>::max();
    return static_cast<i32>(value);
}
inline f32 fromDspInt(i32 sample) {
    return static_cast<f32>(sample) / 32768.0f;
}

// Writes `data` into the ring at `base` from the info's write offset plus
// `write_offset`. `update_count` (last channel only) advances the write offset.
inline u32 auxWrite(GuestMemory& memory, u64 info, u64 base, u32 count_max,
                    std::span<const i32> data, u32 write_offset, u32 update_count) {
    if (count_max == 0 || base == 0)
        return 0;
    u32 target = write_offset + aux_info::read(memory, info + aux_info::WRITE_OFFSET);
    if (target > count_max)
        return 0;
    const u32 count = static_cast<u32>(data.size());
    u32 remaining = count;
    u32 offset = 0;
    while (remaining != 0) {
        const u32 chunk = std::min(count_max - target, remaining);
        if (chunk != 0)
            memory.write(base + static_cast<u64>(target) * sizeof(i32),
                         data.data() + offset, static_cast<u64>(chunk) * sizeof(i32));
        target = (target + chunk) % count_max;
        remaining -= chunk;
        offset += chunk;
    }
    if (update_count != 0) {
        const u32 offset_now = aux_info::read(memory, info + aux_info::WRITE_OFFSET);
        aux_info::write(memory, info + aux_info::WRITE_OFFSET,
                        (offset_now + update_count) % count_max);
    }
    return count;
}

// Reads `data` from the ring at `base` from the info's read offset plus
// `read_offset`; returns the samples read (0 on a bad offset)
inline u32 auxRead(GuestMemory& memory, u64 info, u64 base, u32 count_max,
                   std::span<i32> data, u32 read_offset, u32 update_count) {
    if (count_max == 0 || base == 0)
        return 0;
    u32 target = read_offset + aux_info::read(memory, info + aux_info::READ_OFFSET);
    if (target > count_max)
        return 0;
    const u32 count = static_cast<u32>(data.size());
    u32 remaining = count;
    u32 offset = 0;
    while (remaining != 0) {
        const u32 chunk = std::min(count_max - target, remaining);
        if (chunk != 0 &&
            !memory.read(base + static_cast<u64>(target) * sizeof(i32),
                         data.data() + offset, static_cast<u64>(chunk) * sizeof(i32)))
            std::fill_n(data.data() + offset, chunk, 0);
        target = (target + chunk) % count_max;
        remaining -= chunk;
        offset += chunk;
    }
    if (update_count != 0) {
        const u32 offset_now = aux_info::read(memory, info + aux_info::READ_OFFSET);
        aux_info::write(memory, info + aux_info::READ_OFFSET,
                        (offset_now + update_count) % count_max);
    }
    return count;
}

// Capture ring write: as auxWrite on the DSP info, plus the total and lost
// sample counts the game compares against the CPU info
inline u32 captureWrite(GuestMemory& memory, u64 cpu_info, u64 dsp_info, u64 base,
                        u32 count_max, std::span<const i32> data, u32 write_offset,
                        u32 update_count) {
    if (count_max == 0 || base == 0)
        return 0;
    u32 target = write_offset + aux_info::read(memory, dsp_info + aux_info::WRITE_OFFSET);
    if (target > count_max)
        return 0;
    const u32 count = static_cast<u32>(data.size());
    u32 remaining = count;
    u32 offset = 0;
    while (remaining != 0) {
        const u32 chunk = std::min(count_max - target, remaining);
        if (chunk != 0)
            memory.write(base + static_cast<u64>(target) * sizeof(i32),
                         data.data() + offset, static_cast<u64>(chunk) * sizeof(i32));
        target = (target + chunk) % count_max;
        remaining -= chunk;
        offset += chunk;
    }
    if (update_count != 0) {
        using namespace aux_info;
        const u32 total_diff = aux_info::read(memory, dsp_info + TOTAL_SAMPLE_COUNT) -
                               aux_info::read(memory, cpu_info + TOTAL_SAMPLE_COUNT);
        if (total_diff >= count_max) {
            const u32 cpu_lost = aux_info::read(memory, cpu_info + LOST_SAMPLE_COUNT);
            const u32 lost_diff =
                aux_info::read(memory, dsp_info + LOST_SAMPLE_COUNT) - cpu_lost;
            u32 new_lost = lost_diff + update_count;
            if (lost_diff > new_lost)
                new_lost = cpu_lost - 1;
            aux_info::write(memory, dsp_info + LOST_SAMPLE_COUNT, new_lost);
        }
        const u32 new_write =
            (aux_info::read(memory, dsp_info + WRITE_OFFSET) + update_count) % count_max;
        aux_info::write(memory, dsp_info + WRITE_OFFSET, new_write);
        aux_info::write(memory, dsp_info + TOTAL_SAMPLE_COUNT, total_diff + new_write);
    }
    return count;
}

// ---------------------------------------------------------- Effect slot

// One effect of the renderer: the server's view of it (update, status) and
// the DSP state its commands keep across frames.
class Effect {
  public:
    EffectType type() const { return type_; }
    // UNUSED_MIX_ID until an update assigned one
    i32 mixId() const { return mix_id_; }
    u32 processingOrder() const { return processing_order_; }
    bool isEnabled() const { return enabled_; }
    UsageState usageState() const { return usage_state_; }
    // The work buffer could not be attached: the effect generates nothing
    bool shouldSkip() const { return buffer_unmapped_; }
    // State stored by the last update (StoreStatus)
    EffectState status() const { return status_; }

    const BufferMixParameter& bufferMixParameter() const { return buffer_mix_; }
    const AuxiliaryBufferParameter& auxParameter() const { return aux_; }
    const DelayParameter& delayParameter() const { return delay_; }
    const ReverbParameter& reverbParameter() const { return reverb_; }
    const Reverb3dParameter& reverb3dParameter() const { return reverb3d_; }
    const BiquadFilterEffectParameter& biquadParameter() const { return biquad_; }
    const LimiterParameter& limiterParameter() const { return limiter_; }
    const CompressorParameter& compressorParameter() const { return compressor_; }
    const DelayState& delayState() const { return delay_state_; }
    const ReverbState& reverbState() const { return reverb_state_; }
    const Reverb3dState& reverb3dState() const { return reverb3d_state_; }
    const LimiterState& limiterState() const { return limiter_state_; }
    const CompressorState& compressorState() const { return compressor_state_; }
    // V2 limiter statistics as the client sees them (one frame behind the DSP)
    const LimiterStatistics& limiterStatistics() const { return cpu_statistics_; }

    // A client update of the slot (StateUpdater.UpdateEffects + the effect's
    // Update + StoreStatus + the V2 result state reset)
    void update(const EffectInParameter& in, bool renderer_active,
                const Behaviour& behaviour) {
        if (in.type != type_)
            *this = Effect(in.type);
        const bool is_new = in.is_new != 0;
        switch (type_) {
        case EffectType::Invalid:
            break;
        case EffectType::BufferMix:
            updateBase(in);
            buffer_mix_ = load<BufferMixParameter>(in);
            enabled_ = in.is_enabled != 0;
            break;
        case EffectType::AuxiliaryBuffer:
        case EffectType::CaptureBuffer: {
            updateBase(in);
            aux_ = load<AuxiliaryBufferParameter>(in);
            enabled_ = in.is_enabled != 0;
            if (buffer_unmapped_ || is_new) {
                const u64 send = aux_.send_buffer_info_address;
                const bool send_unmapped = !attach(send, behaviour);
                if (type_ == EffectType::CaptureBuffer) {
                    buffer_unmapped_ = send_unmapped;
                    if (!buffer_unmapped_) {
                        send_info_ = send;
                        send_base_ = send ? send + aux_info::HEADER_SIZE : 0;
                        return_info_ = 0;
                        return_base_ = 0;
                    }
                } else {
                    const u64 ret = aux_.return_buffer_info_address;
                    const bool return_unmapped = !attach(ret, behaviour);
                    buffer_unmapped_ = send_unmapped && return_unmapped;
                    if (!buffer_unmapped_) {
                        send_info_ = send ? send + aux_info::INFO_SIZE : 0;
                        send_base_ = send ? send + aux_info::HEADER_SIZE : 0;
                        return_info_ = ret ? ret + aux_info::INFO_SIZE : 0;
                        return_base_ = ret ? ret + aux_info::HEADER_SIZE : 0;
                    }
                }
            }
            break;
        }
        case EffectType::Delay:
            updateWithStatus(in, delay_, delay_.status, behaviour);
            break;
        case EffectType::Reverb:
            updateWithStatus(in, reverb_, reverb_.status, behaviour);
            break;
        case EffectType::Reverb3d:
            updateWithStatus(in, reverb3d_, reverb3d_.parameter_status, behaviour);
            break;
        case EffectType::BiquadFilter:
            updateBase(in);
            biquad_ = load<BiquadFilterEffectParameter>(in);
            enabled_ = in.is_enabled != 0;
            break;
        case EffectType::Limiter:
            updateBase(in);
            limiter_ = load<LimiterParameter>(in);
            enabled_ = in.is_enabled != 0;
            if (buffer_unmapped_ || is_new) {
                usage_state_ = UsageState::New;
                limiter_.status = UsageState::Invalid;
                buffer_unmapped_ = !attach(in.buffer_base, behaviour);
            }
            break;
        case EffectType::Compressor:
            // Nothing is taken from a V1 parameter
            if (!behaviour.effect_info_v2)
                break;
            updateBase(in);
            compressor_ = load<CompressorParameter>(in);
            enabled_ = in.is_enabled != 0;
            break;
        }

        status_ = storeStatus(renderer_active);
        if (behaviour.effect_info_v2 && is_new && type_ == EffectType::Limiter) {
            dsp_statistics_.reset();
            cpu_statistics_.reset();
        }
    }

    // Runs the effect's commands for one frame on the buffers of its mix
    // (index = the parameters' buffer index within the mix). Afterwards the
    // parameter status counts as consumed (UpdateForCommandGeneration).
    void process(std::span<const std::span<f32>> mix_buffers, u32 sample_count,
                 [[maybe_unused]] u32 sample_rate, GuestMemory* memory,
                 const Behaviour& behaviour) {
        if (type_ == EffectType::Invalid || shouldSkip())
            return;
        // UpdateResultStateForCommandGeneration: the client sees what the DSP
        // had at the start of this frame
        if (behaviour.effect_info_v2)
            cpu_statistics_ = dsp_statistics_;

        switch (type_) {
        case EffectType::BufferMix:
            processBufferMix(mix_buffers, sample_count);
            break;
        case EffectType::AuxiliaryBuffer:
            processAux(mix_buffers, sample_count, memory);
            break;
        case EffectType::CaptureBuffer:
            processCapture(mix_buffers, sample_count, memory);
            break;
        case EffectType::Delay:
            processDelayEffect(mix_buffers, sample_count, behaviour);
            break;
        case EffectType::Reverb:
            processReverbEffect(mix_buffers, sample_count, behaviour);
            break;
        case EffectType::Reverb3d:
            processReverb3dEffect(mix_buffers, sample_count, behaviour);
            break;
        case EffectType::BiquadFilter:
            processBiquadEffect(mix_buffers, sample_count, behaviour);
            break;
        case EffectType::Limiter:
            processLimiterEffect(mix_buffers, sample_count, behaviour);
            break;
        case EffectType::Compressor:
            processCompressorEffect(mix_buffers, sample_count);
            break;
        case EffectType::Invalid:
            break;
        }
        updateForCommandGeneration();
    }

    // EffectOutStatusVersion1 (0x10 bytes) or Version2 (0x90 bytes)
    void writeStatus(void* out, bool version2) const {
        EffectOutStatusV2 status{};
        status.state = status_;
        if (version2 && type_ == EffectType::Limiter)
            std::memcpy(status.result_state, &cpu_statistics_, sizeof(cpu_statistics_));
        std::memcpy(out, &status,
                    version2 ? sizeof(EffectOutStatusV2) : sizeof(EffectOutStatusV1));
    }

    Effect() = default;

  private:
    explicit Effect(EffectType type) : type_(type) {}

    template <typename T>
    static T load(const EffectInParameter& in) {
        T value;
        std::memcpy(&value, in.specific, sizeof(T));
        return value;
    }

    // A work buffer attaches when it has an address (the pool lookup is the
    // integrator's), or always with force mapping
    static bool attach(u64 address, const Behaviour& behaviour) {
        return address != 0 || behaviour.memory_pool_force_mapping;
    }

    void updateBase(const EffectInParameter& in) {
        mix_id_ = in.mix_id;
        processing_order_ = in.processing_order;
    }

    // Delay, reverb and reverb 3D: the server keeps its own status until the
    // last one was consumed, a new effect starts over
    template <typename T>
    void updateWithStatus(const EffectInParameter& in, T& parameter, UsageState& status,
                          const Behaviour& behaviour) {
        const T incoming = load<T>(in);
        if (!isChannelCountValid(incoming.channel_count_max))
            return;
        updateBase(in);
        const UsageState old_status = status;
        parameter = incoming;
        if (!isChannelCountValid(incoming.channel_count))
            return;
        enabled_ = in.is_enabled != 0;
        if (old_status != UsageState::Enabled)
            status = old_status;
        if (buffer_unmapped_ || in.is_new != 0) {
            usage_state_ = UsageState::New;
            status = UsageState::Invalid;
            buffer_unmapped_ = !attach(in.buffer_base, behaviour);
        }
    }

    EffectState storeStatus(bool renderer_active) const {
        if (renderer_active)
            return usage_state_ == UsageState::Disabled ? EffectState::Disabled
                                                        : EffectState::Enabled;
        return usage_state_ == UsageState::New ? EffectState::Enabled
                                               : EffectState::Disabled;
    }

    void updateForCommandGeneration() {
        usage_state_ = enabled_ ? UsageState::Enabled : UsageState::Disabled;
        switch (type_) {
        case EffectType::Delay:
            delay_.status = UsageState::Enabled;
            break;
        case EffectType::Reverb:
            reverb_.status = UsageState::Enabled;
            break;
        case EffectType::Reverb3d:
            reverb3d_.parameter_status = UsageState::Enabled;
            break;
        case EffectType::BiquadFilter:
            biquad_.status = UsageState::Enabled;
            break;
        case EffectType::Limiter:
            limiter_.status = UsageState::Enabled;
            limiter_.statistics_reset = 0;
            break;
        case EffectType::Compressor:
            compressor_.status = UsageState::Enabled;
            break;
        default:
            break;
        }
    }

    static std::span<f32> mixBuffer(std::span<const std::span<f32>> mix, u32 index,
                                    u32 sample_count) {
        if (index >= mix.size() || mix[index].size() < sample_count)
            return {};
        return mix[index].first(sample_count);
    }

    static void copyBuffer(std::span<f32> output, std::span<const f32> input) {
        if (output.data() != input.data())
            std::copy(input.begin(), input.end(), output.begin());
    }

    // The channels' buffers. Delay converts the client's legacy 5.1 order to
    // the resource order before REV11; the reverbs work in the legacy order
    // and convert the REV11 one back (as the reference does).
    struct Channels {
        ChannelBuffers inputs{};
        ChannelBuffers outputs{};
        std::array<u16, CHANNEL_COUNT_MAX> input_index{};
        std::array<u16, CHANNEL_COUNT_MAX> output_index{};
        u32 count{0};
        bool valid{false};
    };
    enum class Remap { None, LegacyToResource, ResourceToLegacy };

    static void remapIndices(std::array<u16, CHANNEL_COUNT_MAX>& index) {
        // Swaps (2,3) with (4,5) either way
        std::swap(index[2], index[4]);
        std::swap(index[3], index[5]);
    }

    static Channels gather(std::span<const std::span<f32>> mix, const u8* input,
                           const u8* output, u32 channel_count, u32 sample_count,
                           Remap remap) {
        Channels channels;
        channels.count = std::min(channel_count, CHANNEL_COUNT_MAX);
        for (u32 c = 0; c < channels.count; c++) {
            channels.input_index[c] = input[c];
            channels.output_index[c] = output[c];
        }
        if (remap != Remap::None && channels.count == 6) {
            remapIndices(channels.input_index);
            remapIndices(channels.output_index);
        }
        channels.valid = true;
        for (u32 c = 0; c < channels.count; c++) {
            channels.inputs[c] = mixBuffer(mix, channels.input_index[c], sample_count);
            channels.outputs[c] = mixBuffer(mix, channels.output_index[c], sample_count);
            if (channels.inputs[c].empty() || channels.outputs[c].empty())
                channels.valid = sample_count == 0;
        }
        return channels;
    }

    // Disabled delay/reverb/limiter/compressor/biquad: inputs to outputs
    static void copyThrough(const Channels& channels) {
        for (u32 c = 0; c < channels.count; c++)
            if (channels.input_index[c] != channels.output_index[c])
                copyBuffer(channels.outputs[c], channels.inputs[c]);
    }

    void processBufferMix(std::span<const std::span<f32>> mix, u32 sample_count) {
        if (!enabled_)
            return;
        const u32 count = std::min(buffer_mix_.mix_count, MIX_BUFFER_COUNT_MAX);
        for (u32 i = 0; i < count; i++) {
            const f32 volume = buffer_mix_.volumes[i];
            if (volume == 0.0f)
                continue;
            const auto input = mixBuffer(mix, buffer_mix_.input[i], sample_count);
            const auto output = mixBuffer(mix, buffer_mix_.output[i], sample_count);
            if (input.empty() || output.empty())
                continue;
            for (u32 s = 0; s < sample_count; s++)
                output[s] += input[s] * volume;
        }
    }

    void processAux(std::span<const std::span<f32>> mix, u32 sample_count,
                    GuestMemory* memory) {
        if (send_base_ == 0 || return_base_ == 0 || !memory)
            return;
        const u32 channels = std::min(aux_.channel_count, MIX_BUFFER_COUNT_MAX);
        const u32 count_max = aux_.buffer_storage_size;
        std::vector<i32> samples(sample_count);
        u32 write_offset = 0;
        for (u32 c = 0; c < channels; c++) {
            const u32 next = write_offset + sample_count;
            const u32 update_count = c + 1 == channels ? next : 0;
            const auto input = mixBuffer(mix, aux_.input[c], sample_count);
            const auto output = mixBuffer(mix, aux_.output[c], sample_count);
            if (input.empty() || output.empty()) {
                write_offset = next;
                continue;
            }
            if (enabled_) {
                for (u32 s = 0; s < sample_count; s++)
                    samples[s] = toDspInt(input[s]);
                auxWrite(*memory, send_info_, send_base_, count_max, samples,
                         write_offset, update_count);
                // The DSP converts the buffer in place and back
                for (u32 s = 0; s < sample_count; s++)
                    input[s] = fromDspInt(samples[s]);
                const u32 read = auxRead(*memory, return_info_, return_base_, count_max,
                                         samples, write_offset, update_count);
                for (u32 s = 0; s < sample_count; s++)
                    output[s] = s < read ? fromDspInt(samples[s]) : 0.0f;
            } else {
                aux_info::reset(*memory, send_info_);
                aux_info::reset(*memory, return_info_);
                if (aux_.input[c] != aux_.output[c])
                    copyBuffer(output, input);
            }
            write_offset = next;
        }
    }

    void processCapture(std::span<const std::span<f32>> mix, u32 sample_count,
                        GuestMemory* memory) {
        if (send_base_ == 0 || send_info_ == 0 || !memory)
            return;
        const u32 channels = std::min(aux_.channel_count, MIX_BUFFER_COUNT_MAX);
        const u32 count_max = aux_.buffer_storage_size;
        const u64 cpu_info = send_info_;
        const u64 dsp_info = send_info_ + aux_info::INFO_SIZE;
        std::vector<i32> samples(sample_count);
        u32 write_offset = 0;
        for (u32 c = 0; c < channels; c++) {
            const u32 next = write_offset + sample_count;
            const u32 update_count = c + 1 == channels ? next : 0;
            const auto input = mixBuffer(mix, aux_.input[c], sample_count);
            if (input.empty()) {
                write_offset = next;
                continue;
            }
            if (enabled_) {
                for (u32 s = 0; s < sample_count; s++)
                    samples[s] = toDspInt(input[s]);
                captureWrite(*memory, cpu_info, dsp_info, send_base_, count_max, samples,
                             write_offset, update_count);
                for (u32 s = 0; s < sample_count; s++)
                    input[s] = fromDspInt(samples[s]);
            } else {
                aux_info::reset(*memory, dsp_info);
            }
            write_offset = next;
        }
    }

    void processDelayEffect(std::span<const std::span<f32>> mix, u32 sample_count,
                            const Behaviour& behaviour) {
        if (!isChannelCountValid(delay_.channel_count))
            return;
        const auto channels =
            gather(mix, delay_.input, delay_.output, delay_.channel_count, sample_count,
                   behaviour.new_effect_channel_mapping ? Remap::None
                                                        : Remap::LegacyToResource);
        if (!channels.valid)
            return;
        if (enabled_) {
            if (delay_.status == UsageState::Invalid || !delay_state_.initialized ||
                delay_state_.channel_count < delay_.channel_count)
                delay_state_.init(delay_);
            else if (delay_.status == UsageState::New)
                delay_state_.updateParameter(delay_);
            processDelay(delay_state_, delay_, channels.inputs, channels.outputs,
                         sample_count);
        } else {
            copyThrough(channels);
        }
    }

    void processReverbEffect(std::span<const std::span<f32>> mix, u32 sample_count,
                             const Behaviour& behaviour) {
        if (!isChannelCountValid(reverb_.channel_count))
            return;
        const auto channels =
            gather(mix, reverb_.input, reverb_.output, reverb_.channel_count, sample_count,
                   behaviour.new_effect_channel_mapping ? Remap::ResourceToLegacy
                                                        : Remap::None);
        if (!channels.valid)
            return;
        if (enabled_) {
            if (reverb_.status == UsageState::Invalid || !reverb_state_.initialized)
                reverb_state_.init(reverb_, behaviour.long_size_pre_delay);
            else if (reverb_.status == UsageState::New)
                reverb_state_.updateParameter(reverb_);
            processReverb(reverb_state_, reverb_, channels.inputs, channels.outputs,
                          sample_count);
        } else {
            copyThrough(channels);
        }
    }

    void processReverb3dEffect(std::span<const std::span<f32>> mix, u32 sample_count,
                               const Behaviour& behaviour) {
        if (!isChannelCountValid(reverb3d_.channel_count))
            return;
        const auto channels = gather(mix, reverb3d_.input, reverb3d_.output,
                                     reverb3d_.channel_count, sample_count,
                                     behaviour.new_effect_channel_mapping
                                         ? Remap::ResourceToLegacy
                                         : Remap::None);
        if (!channels.valid)
            return;
        if (enabled_) {
            if (reverb3d_.parameter_status == UsageState::Invalid ||
                !reverb3d_state_.initialized)
                reverb3d_state_.init(reverb3d_);
            else if (reverb3d_.parameter_status == UsageState::New)
                reverb3d_state_.updateParameter(reverb3d_);
            processReverb3d(reverb3d_state_, reverb3d_, channels.inputs,
                            channels.outputs, sample_count);
        } else {
            copyThrough(channels);
        }
    }

    void processBiquadEffect(std::span<const std::span<f32>> mix, u32 sample_count,
                             const Behaviour& behaviour) {
        const auto channels = gather(mix, biquad_.input, biquad_.output,
                                     biquad_.channel_count, sample_count, Remap::None);
        if (!channels.valid)
            return;
        if (!enabled_) {
            copyThrough(channels);
            return;
        }
        const bool reset_state =
            biquad_.status == UsageState::Invalid ||
            (biquad_.status == UsageState::New &&
             !behaviour.biquad_filter_effect_state_clear_fix);
        i16 numerator[3];
        i16 denominator[2];
        std::memcpy(numerator, biquad_.numerator, sizeof(numerator));
        std::memcpy(denominator, biquad_.denominator, sizeof(denominator));
        for (u32 c = 0; c < channels.count; c++) {
            if (reset_state)
                biquad_state_[c] = {};
            copyBuffer(channels.outputs[c], channels.inputs[c]);
            processBiquad(channels.outputs[c], numerator, denominator, biquad_state_[c]);
        }
    }

    void processLimiterEffect(std::span<const std::span<f32>> mix, u32 sample_count,
                              const Behaviour& behaviour) {
        if (!isChannelCountValid(limiter_.channel_count))
            return;
        const auto channels = gather(mix, limiter_.input, limiter_.output,
                                     limiter_.channel_count, sample_count, Remap::None);
        if (!channels.valid)
            return;
        if (!enabled_) {
            copyThrough(channels);
            return;
        }
        if (limiter_.status == UsageState::Invalid || !limiter_state_.initialized ||
            limiter_state_.channel_count < limiter_.channel_count ||
            limiter_state_.delay_count_max != limiter_.delay_buffer_sample_count_max)
            limiter_state_.init(limiter_);
        LimiterStatistics* statistics = nullptr;
        if (behaviour.effect_info_v2 && limiter_.statistics_enabled) {
            statistics = &dsp_statistics_;
            if (limiter_.statistics_reset)
                statistics->reset();
        }
        processLimiter(limiter_state_, limiter_, channels.inputs, channels.outputs,
                       sample_count, statistics);
    }

    void processCompressorEffect(std::span<const std::span<f32>> mix, u32 sample_count) {
        if (!isChannelCountValid(compressor_.channel_count))
            return;
        const auto channels =
            gather(mix, compressor_.input, compressor_.output, compressor_.channel_count,
                   sample_count, Remap::None);
        if (!channels.valid)
            return;
        if (!enabled_) {
            copyThrough(channels);
            return;
        }
        if (compressor_.status == UsageState::Invalid || !compressor_state_.initialized)
            compressor_state_.init(compressor_);
        else if (compressor_.status == UsageState::New)
            compressor_state_.updateParameter(compressor_);
        processCompressor(compressor_state_, compressor_, channels.inputs,
                          channels.outputs, sample_count);
    }

    EffectType type_{EffectType::Invalid};
    bool enabled_{false};
    bool buffer_unmapped_{false};
    UsageState usage_state_{UsageState::Invalid};
    EffectState status_{EffectState::Disabled};
    i32 mix_id_{UNUSED_MIX_ID};
    u32 processing_order_{std::numeric_limits<u32>::max()};

    BufferMixParameter buffer_mix_{};
    AuxiliaryBufferParameter aux_{};
    DelayParameter delay_{};
    ReverbParameter reverb_{};
    Reverb3dParameter reverb3d_{};
    BiquadFilterEffectParameter biquad_{};
    LimiterParameter limiter_{};
    CompressorParameter compressor_{};

    // Aux/capture ring addresses: aux uses the DSP infos (+0x40), capture the
    // CPU info at the info address (its DSP info follows at +0x40)
    u64 send_info_{0};
    u64 send_base_{0};
    u64 return_info_{0};
    u64 return_base_{0};

    DelayState delay_state_{};
    ReverbState reverb_state_{};
    Reverb3dState reverb3d_state_{};
    std::array<std::array<f32, 2>, CHANNEL_COUNT_MAX> biquad_state_{};
    LimiterState limiter_state_{};
    CompressorState compressor_state_{};
    LimiterStatistics dsp_statistics_{};
    LimiterStatistics cpu_statistics_{};
};

} // namespace hydra::audio::dsp::effects
