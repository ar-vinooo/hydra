#include "core/audio/renderer_effects.hpp"

#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace hydra;
namespace dsp = hydra::audio::dsp;
namespace fx = hydra::audio::dsp::effects;

namespace {

void check(bool condition, const char* description) {
    if (!condition)
        throw std::runtime_error(description);
}

bool near(f32 a, f32 b, f32 tolerance = 1e-5f) {
    return std::abs(a - b) <= tolerance;
}

constexpr u32 FRAME = 240;
constexpr u64 WORK_BUFFER = 0x80000;

// Guest memory backed by a byte map of a flat range
struct FakeMemory : fx::GuestMemory {
    u64 base;
    std::vector<u8> bytes;
    FakeMemory(u64 base_, usize size) : base(base_), bytes(size, 0) {}
    bool read(u64 address, void* data, u64 size) override {
        if (address < base || address + size > base + bytes.size())
            return false;
        std::memcpy(data, bytes.data() + (address - base), size);
        return true;
    }
    bool write(u64 address, const void* data, u64 size) override {
        if (address < base || address + size > base + bytes.size())
            return false;
        std::memcpy(bytes.data() + (address - base), data, size);
        return true;
    }
    u32 u32At(u64 address) {
        u32 value = 0;
        read(address, &value, 4);
        return value;
    }
    i32 i32At(u64 address) {
        i32 value = 0;
        read(address, &value, 4);
        return value;
    }
    void setU32(u64 address, u32 value) { write(address, &value, 4); }
    void setI32(u64 address, i32 value) { write(address, &value, 4); }
};

// The mix's buffers
struct Mix {
    std::vector<std::vector<f32>> storage;
    std::vector<std::span<f32>> views;
    explicit Mix(u32 count) : storage(count, std::vector<f32>(FRAME, 0.0f)) {
        for (auto& buffer : storage)
            views.emplace_back(buffer);
    }
    std::span<const std::span<f32>> span() const { return views; }
    std::vector<f32>& operator[](usize i) { return storage[i]; }
    void clear() {
        for (auto& buffer : storage)
            std::fill(buffer.begin(), buffer.end(), 0.0f);
    }
};

template <typename T>
fx::EffectInParameter makeIn(fx::EffectType type, const T& specific, bool enabled,
                             bool is_new, u64 buffer_base = WORK_BUFFER) {
    fx::EffectInParameter in{};
    in.type = type;
    in.is_new = is_new ? 1 : 0;
    in.is_enabled = enabled ? 1 : 0;
    in.mix_id = 0;
    in.buffer_base = buffer_base;
    in.buffer_size = 0x10000;
    in.processing_order = 0;
    std::memcpy(in.specific, &specific, sizeof(T));
    return in;
}

const fx::Behaviour LATEST =
    fx::Behaviour::fromRevision(static_cast<u32>(fx::Behaviour::revisionMagic(12)));

i32 q14(f32 value) {
    return static_cast<i32>(value * 16384.0f);
}

void testLayouts() {
    check(sizeof(fx::EffectInParameter) == 0xC0, "EffectInParameter size");
    check(offsetof(fx::EffectInParameter, specific) == 0x20, "Specific data offset");
    check(sizeof(fx::EffectOutStatusV1) == 0x10 && sizeof(fx::EffectOutStatusV2) == 0x90,
          "Out status sizes");
    check(sizeof(fx::BufferMixParameter) == 0x94, "BufferMixParameter size");
    check(sizeof(fx::AuxiliaryBufferParameter) == 0x6C, "AuxiliaryBufferParameter size");
    check(offsetof(fx::AuxiliaryBufferParameter, buffer_storage_size) == 0x38,
          "Aux storage size offset");
    check(offsetof(fx::AuxiliaryBufferParameter, return_buffer_info_address) == 0x50,
          "Aux return info offset");
    check(sizeof(fx::DelayParameter) == 0x35, "DelayParameter size");
    check(offsetof(fx::DelayParameter, feedback_gain) == 0x20, "Delay feedback offset");
    check(sizeof(fx::ReverbParameter) == 0x41, "ReverbParameter size");
    check(offsetof(fx::ReverbParameter, late_mode) == 0x20, "Reverb late mode offset");
    check(sizeof(fx::Reverb3dParameter) == 0x49, "Reverb3dParameter size");
    check(offsetof(fx::Reverb3dParameter, dry_gain) == 0x44, "Reverb3d dry offset");
    check(sizeof(fx::BiquadFilterEffectParameter) == 0x18, "Biquad parameter size");
    check(offsetof(fx::BiquadFilterEffectParameter, channel_count) == 0x16,
          "Biquad channel count offset");
    check(sizeof(fx::LimiterParameter) == 0x44, "LimiterParameter size");
    check(offsetof(fx::LimiterParameter, delay_buffer_sample_count_min) == 0x38,
          "Limiter delay min offset");
    check(offsetof(fx::LimiterParameter, statistics_reset) == 0x42,
          "Limiter statistics reset offset");
    check(sizeof(fx::LimiterStatistics) == 0x30, "LimiterStatistics size");
    check(sizeof(fx::CompressorParameter) == 0x38, "CompressorParameter size");
    check(offsetof(fx::CompressorParameter, output_gain) == 0x30,
          "Compressor output gain offset");
    check(static_cast<u8>(fx::EffectState::Enabled) == 3 &&
              static_cast<u8>(fx::EffectState::Disabled) == 4,
          "Effect state values");
    check(static_cast<u8>(fx::EffectType::Compressor) == 9, "Effect type values");
}

void testBehaviour() {
    // 'REV1' as stored: little-endian 'R','E','V','1'
    const u32 rev1 = 0x31564552;
    check(fx::Behaviour::revisionMagic(1) == static_cast<i32>(rev1), "REV1 magic");
    const auto b1 = fx::Behaviour::fromRevision(rev1);
    check(!b1.long_size_pre_delay && !b1.biquad_filter_effect_state_clear_fix &&
              !b1.effect_info_v2 && !b1.new_effect_channel_mapping,
          "REV1 has no effect features");
    const auto b3 = fx::Behaviour::fromRevision(0x33564552);
    check(b3.long_size_pre_delay && !b3.biquad_filter_effect_state_clear_fix,
          "REV3 long pre-delay");
    const auto b9 = fx::Behaviour::fromRevision(0x39564552);
    check(b9.effect_info_v2 && b9.biquad_filter_effect_state_clear_fix &&
              !b9.new_effect_channel_mapping,
          "REV9 effect info V2");
    const auto b11 = fx::Behaviour::fromRevision(
        static_cast<u32>(fx::Behaviour::revisionMagic(11)));
    check(b11.new_effect_channel_mapping && b11.effect_info_v2, "REV11 channel mapping");
    const auto b10 = fx::Behaviour::fromRevision(
        static_cast<u32>(fx::Behaviour::revisionMagic(10)));
    check(!b10.new_effect_channel_mapping, "REV10 legacy channel mapping");
}

void testUpdateAndStatus() {
    fx::Effect effect;
    check(effect.type() == fx::EffectType::Invalid &&
              effect.mixId() == fx::UNUSED_MIX_ID,
          "A slot starts unused");

    fx::DelayParameter delay{};
    delay.channel_count_max = 1;
    delay.channel_count = 1;
    delay.sample_rate = 48000u << 14;
    delay.delay_time_max = 20;
    delay.delay_time = 10;
    auto in = makeIn(fx::EffectType::Delay, delay, true, true);
    in.mix_id = 3;
    in.processing_order = 7;
    effect.update(in, false, LATEST);
    check(effect.type() == fx::EffectType::Delay && effect.mixId() == 3 &&
              effect.processingOrder() == 7 && effect.isEnabled(),
          "Update takes the base parameters");
    check(effect.usageState() == fx::UsageState::New &&
              effect.delayParameter().status == fx::UsageState::Invalid,
          "A new delay is New with an Invalid parameter status");
    check(effect.status() == fx::EffectState::Enabled,
          "A new effect reports Enabled while the renderer is stopped");

    // Invalid channel count max: the update is ignored
    auto bad = delay;
    bad.channel_count_max = 3;
    auto bad_in = makeIn(fx::EffectType::Delay, bad, false, false);
    bad_in.mix_id = 9;
    effect.update(bad_in, true, LATEST);
    check(effect.mixId() == 3 && effect.isEnabled(),
          "Invalid channel count max is rejected");

    // Disabled after a frame
    Mix mix(2);
    effect.update(makeIn(fx::EffectType::Delay, delay, false, false), true, LATEST);
    effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(effect.usageState() == fx::UsageState::Disabled &&
              effect.delayParameter().status == fx::UsageState::Enabled,
          "Command generation consumes the status");
    effect.update(makeIn(fx::EffectType::Delay, delay, false, false), true, LATEST);
    check(effect.status() == fx::EffectState::Disabled, "Disabled effect status");
    std::array<u8, 0x90> out{};
    out.fill(0xcc);
    effect.writeStatus(out.data(), true);
    check(out[0] == 4 && out[1] == 0 && out[0x8f] == 0, "V2 status bytes");
    out.fill(0xcc);
    effect.writeStatus(out.data(), false);
    check(out[0] == 4 && out[15] == 0 && out[16] == 0xcc, "V1 status is 16 bytes");

    // Type change resets the slot
    fx::BufferMixParameter mix_parameter{};
    effect.update(makeIn(fx::EffectType::BufferMix, mix_parameter, false, false), true,
                  LATEST);
    check(effect.type() == fx::EffectType::BufferMix &&
              effect.usageState() == fx::UsageState::Invalid &&
              effect.status() == fx::EffectState::Enabled,
          "A type change makes a fresh effect");

    // Missing work buffer: skipped, nothing generated
    fx::Effect unmapped;
    unmapped.update(makeIn(fx::EffectType::Delay, delay, true, true, 0), true, LATEST);
    check(unmapped.shouldSkip(), "Delay without work buffer is skipped");
    mix[0][0] = 1.0f;
    unmapped.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(unmapped.usageState() == fx::UsageState::New, "A skipped effect is not consumed");
    auto forced = LATEST;
    forced.memory_pool_force_mapping = true;
    unmapped.update(makeIn(fx::EffectType::Delay, delay, true, false, 0), true, forced);
    check(!unmapped.shouldSkip(), "Force mapping attaches the buffer");

    // Compressor needs V2 parameters
    fx::Effect compressor;
    fx::CompressorParameter compressor_parameter{};
    compressor_parameter.channel_count = 1;
    auto v1 = LATEST;
    v1.effect_info_v2 = false;
    compressor.update(makeIn(fx::EffectType::Compressor, compressor_parameter, true, true),
                      true, v1);
    check(compressor.mixId() == fx::UNUSED_MIX_ID && !compressor.isEnabled(),
          "Compressor ignores V1 parameters");
}

void testBufferMix() {
    fx::BufferMixParameter parameter{};
    parameter.mix_count = 3;
    parameter.input[0] = 0;
    parameter.output[0] = 1;
    parameter.volumes[0] = 0.5f;
    parameter.input[1] = 2;
    parameter.output[1] = 1;
    parameter.volumes[1] = 0.0f;
    parameter.input[2] = 1;
    parameter.output[2] = 2;
    parameter.volumes[2] = 2.0f;
    fx::Effect effect;
    effect.update(makeIn(fx::EffectType::BufferMix, parameter, true, true), true, LATEST);
    Mix mix(3);
    for (u32 i = 0; i < FRAME; i++) {
        mix[0][i] = 0.25f;
        mix[1][i] = 0.1f;
        mix[2][i] = 0.3f;
    }
    effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(near(mix[1][10], 0.1f + 0.125f), "BufferMix adds input * volume");
    check(near(mix[2][10], 0.3f + 2.0f * 0.225f), "BufferMix runs in order");
    check(mix[0][10] == 0.25f, "BufferMix leaves its inputs");
    effect.update(makeIn(fx::EffectType::BufferMix, parameter, false, false), true, LATEST);
    effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(near(mix[1][10], 0.225f), "Disabled BufferMix does nothing");
}

void testAux() {
    constexpr u64 SEND = 0x10000;
    constexpr u64 RETURN = 0x20000;
    constexpr u32 STORAGE = 1100;
    FakeMemory memory(0x10000, 0x20000);
    fx::AuxiliaryBufferParameter parameter{};
    parameter.channel_count = 2;
    parameter.input[0] = 0;
    parameter.input[1] = 1;
    parameter.output[0] = 2;
    parameter.output[1] = 3;
    parameter.buffer_storage_size = STORAGE;
    parameter.send_buffer_info_address = SEND;
    parameter.return_buffer_info_address = RETURN;
    fx::Effect effect;
    effect.update(makeIn(fx::EffectType::AuxiliaryBuffer, parameter, true, true), true,
                  LATEST);
    check(!effect.shouldSkip(), "Aux attaches its buffers");

    // The game's return data
    for (u32 i = 0; i < STORAGE; i++)
        memory.setI32(RETURN + 0x80 + i * 4, static_cast<i32>(i) * 16 - 4000);

    Mix mix(4);
    u32 frame_start = 0;
    for (u32 frame = 0; frame < 3; frame++) {
        for (u32 i = 0; i < FRAME; i++) {
            mix[0][i] = static_cast<f32>(i + frame * 1000) / 32768.0f + 0.4f / 32768.0f;
            mix[1][i] = -static_cast<f32>(i + 1) / 32768.0f;
        }
        effect.process(mix.span(), FRAME, 48000, &memory, LATEST);
        for (u32 c = 0; c < 2; c++) {
            // A channel starting past the ring's end is dropped (as the DSP
            // does): nothing sent, silence returned
            const bool dropped = frame_start + c * FRAME > STORAGE;
            for (u32 i = 0; i < FRAME; i++) {
                const u32 ring = (frame_start + c * FRAME + i) % STORAGE;
                const i32 expected_send = c == 0 ? static_cast<i32>(i + frame * 1000)
                                                 : -static_cast<i32>(i + 1);
                const f32 expected_return =
                    dropped ? 0.0f
                            : static_cast<f32>(static_cast<i32>(ring) * 16 - 4000) /
                                  32768.0f;
                check(dropped || memory.i32At(SEND + 0x80 + ring * 4) == expected_send,
                      "Aux writes the send ring in DSP scale (truncated)");
                check(mix[2 + c][i] == expected_return,
                      "Aux reads the return ring into the outputs");
            }
            check(dropped == (frame == 2 && c == 1), "Aux drop case");
        }
        check(mix[0][0] == static_cast<f32>(frame * 1000) / 32768.0f,
              "Aux quantizes its input buffer like the DSP");
        // The last channel advances the offsets, unless it was dropped
        if (frame != 2)
            frame_start = (frame_start + 2 * FRAME) % STORAGE;
        check(memory.u32At(SEND + 0x40 + 4) == frame_start, "Send DSP write offset");
        check(memory.u32At(RETURN + 0x40 + 0) == frame_start, "Return DSP read offset");
        check(memory.u32At(SEND + 0x4) == 0, "The CPU info is the game's");
    }
    check(frame_start == 960, "A dropped last channel stalls the offsets");

    // Disabled: the infos reset (lost count kept), inputs copied through
    memory.setU32(SEND + 0x40 + 0x8, 77);
    memory.setU32(SEND + 0x40 + 0xC, 99);
    effect.update(makeIn(fx::EffectType::AuxiliaryBuffer, parameter, false, false), true,
                  LATEST);
    for (u32 i = 0; i < FRAME; i++)
        mix[0][i] = 0.5f;
    effect.process(mix.span(), FRAME, 48000, &memory, LATEST);
    check(memory.u32At(SEND + 0x40) == 0 && memory.u32At(SEND + 0x44) == 0 &&
              memory.u32At(SEND + 0x4C) == 0 && memory.u32At(SEND + 0x48) == 77 &&
              memory.u32At(RETURN + 0x40) == 0,
          "Disabled aux resets the DSP infos");
    check(mix[2][5] == 0.5f, "Disabled aux copies input to output");
}

void testCapture() {
    constexpr u64 SEND = 0x10000;
    constexpr u32 STORAGE = 600;
    FakeMemory memory(0x10000, 0x10000);
    fx::AuxiliaryBufferParameter parameter{};
    parameter.channel_count = 1;
    parameter.input[0] = 1;
    parameter.buffer_storage_size = STORAGE;
    parameter.send_buffer_info_address = SEND;
    fx::Effect effect;
    effect.update(makeIn(fx::EffectType::CaptureBuffer, parameter, true, true), true,
                  LATEST);
    Mix mix(2);
    for (u32 i = 0; i < FRAME; i++)
        mix[1][i] = static_cast<f32>(i) / 32768.0f;
    effect.process(mix.span(), FRAME, 48000, &memory, LATEST);
    for (u32 i = 0; i < FRAME; i++)
        check(memory.i32At(SEND + 0x80 + i * 4) == static_cast<i32>(i),
              "Capture writes its ring");
    check(memory.u32At(SEND + 0x44) == FRAME && memory.u32At(SEND + 0x4C) == FRAME,
          "Capture advances write offset and total count");
    effect.process(mix.span(), FRAME, 48000, &memory, LATEST);
    // The reference's total: unread (DSP - CPU total) + the new write offset
    check(memory.u32At(SEND + 0x44) == 2 * FRAME && memory.u32At(SEND + 0x4C) == 3 * FRAME,
          "Capture total count update");
    // The game fell behind a full ring: lost samples are counted
    memory.setU32(SEND + 0x4C, STORAGE);
    effect.process(mix.span(), FRAME, 48000, &memory, LATEST);
    check(memory.u32At(SEND + 0x44) == (3 * FRAME) % STORAGE,
          "Capture write offset wraps");
    check(memory.u32At(SEND + 0x48) == FRAME, "Capture counts lost samples");
    check(memory.u32At(SEND + 0x4C) == STORAGE + (3 * FRAME) % STORAGE,
          "Capture total after an overrun");
    check(mix[1][7] == 7.0f / 32768.0f, "Capture leaves its input");
    effect.update(makeIn(fx::EffectType::CaptureBuffer, parameter, false, false), true,
                  LATEST);
    effect.process(mix.span(), FRAME, 48000, &memory, LATEST);
    check(memory.u32At(SEND + 0x44) == 0 && memory.u32At(SEND + 0x4C) == 0,
          "Disabled capture resets its info");
}

// Runs `frames` frames, the impulse in the first sample of buffer `input`
std::vector<std::vector<f32>> runFrames(fx::Effect& effect, Mix& mix, u32 frames,
                                        u32 channels, u32 impulse_channel,
                                        const fx::Behaviour& behaviour = LATEST) {
    std::vector<std::vector<f32>> out(channels);
    for (u32 frame = 0; frame < frames; frame++) {
        mix.clear();
        if (frame == 0)
            mix[impulse_channel][0] = 1.0f;
        effect.process(mix.span(), FRAME, 48000, nullptr, behaviour);
        for (u32 c = 0; c < channels; c++)
            out[c].insert(out[c].end(), mix[c].begin(), mix[c].end());
    }
    return out;
}

void testDelay() {
    fx::DelayParameter parameter{};
    parameter.channel_count_max = 2;
    parameter.channel_count = 1;
    parameter.input[0] = 0;
    parameter.output[0] = 0;
    parameter.sample_rate = 48000u << 14;
    parameter.delay_time_max = 50;
    parameter.delay_time = 10;
    parameter.in_gain = static_cast<u32>(q14(1.0f));
    parameter.feedback_gain = static_cast<u32>(q14(0.5f));
    parameter.out_gain = static_cast<u32>(q14(0.5f));
    parameter.dry_gain = static_cast<u32>(q14(0.25f));
    fx::Effect effect;
    effect.update(makeIn(fx::EffectType::Delay, parameter, true, true), true, LATEST);
    Mix mix(2);
    const auto out = runFrames(effect, mix, 6, 1, 0);
    // 10 ms is 480 samples, the line wraps one early (479)
    check(near(out[0][0], 0.25f), "Delay dry path");
    check(near(out[0][479], 0.5f), "Delay echo at the delay time with out gain");
    check(near(out[0][958], 0.25f), "Delay feedback echo");
    check(near(out[0][1437], 0.125f), "Delay feedback decays");
    f32 rest = 0.0f;
    for (usize i = 1; i < out[0].size(); i++)
        if (i % 479 != 0)
            rest += std::abs(out[0][i]);
    check(rest < 1e-6f, "Delay is silent between echoes");

    // Stereo full spread: the echo crosses to the other side
    parameter.channel_count = 2;
    parameter.input[1] = 1;
    parameter.output[1] = 1;
    parameter.channel_spread = static_cast<u32>(q14(1.0f));
    fx::Effect stereo;
    stereo.update(makeIn(fx::EffectType::Delay, parameter, true, true), true, LATEST);
    const auto st = runFrames(stereo, mix, 6, 2, 0);
    check(near(st[0][479], 0.5f) && near(st[1][479], 0.0f), "Stereo first echo");
    check(near(st[1][958], 0.5f * 0.5f * 0.98f) && near(st[0][958], 0.0f),
          "Channel spread crosses the feedback");

    // New parameters (status New): gains change, the line keeps its content
    mix.clear();
    mix[0][0] = 1.0f;
    stereo.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    auto louder = parameter;
    louder.out_gain = static_cast<u32>(q14(1.0f));
    louder.status = fx::UsageState::New;
    stereo.update(makeIn(fx::EffectType::Delay, louder, true, false), true, LATEST);
    check(stereo.delayParameter().status == fx::UsageState::New, "Client New status kept");
    const auto after = runFrames(stereo, mix, 2, 2, 1);
    check(near(after[0][479 - FRAME], 1.0f), "Updated gains apply to the kept line");

    // Disabled: input copied to a different output
    parameter.channel_count = 1;
    parameter.output[0] = 1;
    fx::Effect disabled;
    disabled.update(makeIn(fx::EffectType::Delay, parameter, false, true), true, LATEST);
    mix.clear();
    mix[0][3] = 0.7f;
    disabled.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(mix[1][3] == 0.7f, "Disabled delay copies through");

    // Legacy 5.1 order before REV11: client channels 2,3 are the back pair
    fx::DelayParameter surround{};
    surround.channel_count_max = 6;
    surround.channel_count = 6;
    for (u8 c = 0; c < 6; c++) {
        surround.input[c] = c;
        surround.output[c] = static_cast<u8>(c + 6);
    }
    fx::Effect copy;
    copy.update(makeIn(fx::EffectType::Delay, surround, false, true), true, LATEST);
    Mix wide(12);
    wide[2][0] = 0.2f;
    auto legacy = LATEST;
    legacy.new_effect_channel_mapping = false;
    copy.process(wide.span(), FRAME, 48000, nullptr, legacy);
    check(wide[6 + 2][0] == 0.2f, "Remapped channels still copy pairwise");
}

struct DecayReport {
    f32 peak{0.0f};
    f32 early_energy{0.0f};
    f32 late_energy{0.0f};
    bool finite{true};
};

DecayReport analyze(const std::vector<std::vector<f32>>& out, usize early_end,
                    usize late_start) {
    DecayReport report;
    for (const auto& channel : out)
        for (usize i = 0; i < channel.size(); i++) {
            const f32 v = channel[i];
            if (!std::isfinite(v))
                report.finite = false;
            report.peak = std::max(report.peak, std::abs(v));
            if (i < early_end)
                report.early_energy += v * v;
            else if (i >= late_start)
                report.late_energy += v * v;
        }
    return report;
}

fx::ReverbParameter reverbParameter() {
    fx::ReverbParameter parameter{};
    parameter.channel_count_max = 2;
    parameter.channel_count = 2;
    parameter.input[1] = 1;
    parameter.output[1] = 1;
    parameter.sample_rate = 48 << 14;
    parameter.early_mode = 2;
    parameter.early_gain = q14(1.0f);
    parameter.pre_delay_time = q14(20.0f);
    parameter.late_mode = 1;
    parameter.late_gain = q14(1.0f);
    parameter.decay_time = q14(1.5f);
    parameter.high_frequency_decay_ratio = q14(0.5f);
    parameter.coloration = q14(0.5f);
    parameter.reverb_gain = q14(1.0f);
    parameter.out_gain = q14(1.0f);
    parameter.dry_gain = 0;
    return parameter;
}

void testReverb() {
    auto parameter = reverbParameter();
    fx::Effect effect;
    effect.update(makeIn(fx::EffectType::Reverb, parameter, true, true), true, LATEST);
    Mix mix(2);
    // 4 seconds
    const auto out = runFrames(effect, mix, 800, 2, 0);
    const auto report = analyze(out, 48000 / 2, 48000 * 7 / 2);
    check(report.finite, "Reverb output is finite");
    check(report.peak > 0.01f && report.peak < 4.0f, "Reverb response is bounded");
    check(report.early_energy > 0.0f && report.late_energy < report.early_energy * 1e-5f,
          "Reverb tail decays");
    check(effect.reverbParameter().status == fx::UsageState::Enabled,
          "Reverb status consumed");
    // Pre-delay: nothing before 20 ms
    f32 before = 0.0f;
    for (usize i = 0; i < 900; i++)
        before += std::abs(out[0][i]) + std::abs(out[1][i]);
    check(before == 0.0f, "Reverb pre-delay");

    // A new effect (Invalid status) starts from a clear state
    mix.clear();
    mix[0][0] = 1.0f;
    effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    effect.update(makeIn(fx::EffectType::Reverb, parameter, true, true), true, LATEST);
    check(effect.reverbParameter().status == fx::UsageState::Invalid,
          "is_new marks the reverb Invalid");
    const auto cleared = runFrames(effect, mix, 40, 2, 0);
    f32 restarted = 0.0f;
    for (usize i = 0; i < 900; i++)
        restarted += std::abs(cleared[0][i]) + std::abs(cleared[1][i]);
    check(restarted == 0.0f, "Reverb state reset on Invalid status");

    // Status New keeps the tail
    mix.clear();
    mix[0][0] = 1.0f;
    for (u32 i = 0; i < 10; i++) {
        effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
        mix.clear();
    }
    auto changed = parameter;
    changed.status = fx::UsageState::New;
    effect.update(makeIn(fx::EffectType::Reverb, changed, true, false), true, LATEST);
    effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    f32 tail = 0.0f;
    for (u32 i = 0; i < FRAME; i++)
        tail += std::abs(mix[0][i]);
    check(tail > 0.0f, "Reverb parameter update keeps the tail");

    // Disabled
    parameter.output[0] = 1;
    parameter.channel_count = 1;
    fx::Effect disabled;
    disabled.update(makeIn(fx::EffectType::Reverb, parameter, false, true), true, LATEST);
    mix.clear();
    mix[0][9] = -0.3f;
    disabled.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(mix[1][9] == -0.3f, "Disabled reverb copies through");

    // Every channel layout stays bounded
    for (const u16 channels : std::array<u16, 3>{1, 4, 6}) {
        auto p = reverbParameter();
        p.channel_count_max = 6;
        p.channel_count = channels;
        for (u8 c = 0; c < 6; c++) {
            p.input[c] = c;
            p.output[c] = c;
        }
        fx::Effect layout;
        layout.update(makeIn(fx::EffectType::Reverb, p, true, true), true, LATEST);
        Mix m(6);
        const auto o = runFrames(layout, m, 400, channels, 0);
        const auto r = analyze(o, 24000, 72000);
        check(r.finite && r.peak > 0.0f && r.peak < 4.0f, "Reverb layouts bounded");
    }
}

fx::Reverb3dParameter reverb3dParameter() {
    fx::Reverb3dParameter parameter{};
    parameter.channel_count_max = 2;
    parameter.channel_count = 2;
    parameter.input[1] = 1;
    parameter.output[1] = 1;
    parameter.sample_rate = 48000;
    parameter.room_hf = -100.0f;
    parameter.hf_reference = 5000.0f;
    parameter.decay_time = 1.49f;
    parameter.hf_decay_ratio = 0.83f;
    parameter.room_gain = -1000.0f;
    parameter.reflections_gain = -2602.0f;
    parameter.reverb_gain = 200.0f;
    parameter.diffusion = 100.0f;
    parameter.reflection_delay = 0.007f;
    parameter.reverb_delay_time = 0.011f;
    parameter.density = 100.0f;
    parameter.dry_gain = 0.0f;
    return parameter;
}

void testReverb3d() {
    auto parameter = reverb3dParameter();
    fx::Effect effect;
    effect.update(makeIn(fx::EffectType::Reverb3d, parameter, true, true), true, LATEST);
    Mix mix(6);
    const auto out = runFrames(effect, mix, 800, 2, 0);
    const auto report = analyze(out, 24000, 48000 * 7 / 2);
    check(report.finite, "Reverb 3D output is finite");
    check(report.peak > 1e-3f && report.peak < 4.0f, "Reverb 3D response is bounded");
    check(report.early_energy > 0.0f && report.late_energy < report.early_energy * 1e-5f,
          "Reverb 3D tail decays");

    // is_new resets the state
    effect.update(makeIn(fx::EffectType::Reverb3d, parameter, true, true), true, LATEST);
    check(effect.reverb3dParameter().parameter_status == fx::UsageState::Invalid,
          "is_new marks the 3D reverb Invalid");
    mix.clear();
    mix[0][0] = 1.0f;
    effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(effect.reverb3dParameter().parameter_status == fx::UsageState::Enabled,
          "3D reverb status consumed");

    // Disabled copies through
    parameter.output[0] = 2;
    parameter.output[1] = 3;
    fx::Effect disabled;
    disabled.update(makeIn(fx::EffectType::Reverb3d, parameter, false, true), true, LATEST);
    mix.clear();
    mix[1][4] = 0.6f;
    disabled.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(mix[3][4] == 0.6f, "Disabled 3D reverb copies through");

    for (const u16 channels : std::array<u16, 3>{1, 4, 6}) {
        auto p = reverb3dParameter();
        p.channel_count_max = 6;
        p.channel_count = channels;
        for (u8 c = 0; c < 6; c++) {
            p.input[c] = c;
            p.output[c] = c;
        }
        fx::Effect layout;
        layout.update(makeIn(fx::EffectType::Reverb3d, p, true, true), true, LATEST);
        Mix m(6);
        const auto o = runFrames(layout, m, 400, channels, 0);
        const auto r = analyze(o, 24000, 72000);
        check(r.finite && r.peak > 0.0f && r.peak < 4.0f, "Reverb 3D layouts bounded");
    }
}

void testBiquad() {
    fx::BiquadFilterEffectParameter parameter{};
    parameter.channel_count = 2;
    parameter.input[0] = 0;
    parameter.output[0] = 0;
    parameter.input[1] = 1;
    parameter.output[1] = 2;
    // Low pass
    parameter.numerator[0] = 1810;
    parameter.numerator[1] = 3620;
    parameter.numerator[2] = 1810;
    parameter.denominator[0] = 21590;
    parameter.denominator[1] = -12442;
    parameter.status = fx::UsageState::Invalid;
    fx::Effect effect;
    effect.update(makeIn(fx::EffectType::BiquadFilter, parameter, true, true), true, LATEST);
    Mix mix(3);
    std::array<std::array<f32, 2>, 2> state{};
    const i16 num[3] = {1810, 3620, 1810};
    const i16 den[2] = {21590, -12442};
    for (u32 frame = 0; frame < 3; frame++) {
        mix.clear();
        std::array<std::vector<f32>, 2> expected;
        for (u32 c = 0; c < 2; c++) {
            for (u32 i = 0; i < FRAME; i++)
                mix[c][i] = std::sin(static_cast<f32>(i * (c + 3) + frame * 17)) * 0.5f;
            expected[c] = mix[c];
            dsp::processBiquad(expected[c], num, den, state[c]);
        }
        effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
        check(mix[0] == expected[0], "Biquad effect filters in place");
        check(mix[2] == expected[1], "Biquad effect filters into its output");
        // The client keeps sending its status
        effect.update(makeIn(fx::EffectType::BiquadFilter, parameter, true, false), true,
                      LATEST);
        parameter.status = fx::UsageState::Enabled;
        effect.update(makeIn(fx::EffectType::BiquadFilter, parameter, true, false), true,
                      LATEST);
    }

    // New: the state survives from REV6, is cleared before it
    for (const bool fixed : {true, false}) {
        auto behaviour = LATEST;
        behaviour.biquad_filter_effect_state_clear_fix = fixed;
        fx::Effect filter;
        parameter.status = fx::UsageState::Invalid;
        filter.update(makeIn(fx::EffectType::BiquadFilter, parameter, true, true), true,
                      behaviour);
        mix.clear();
        mix[0][FRAME - 1] = 1.0f;
        filter.process(mix.span(), FRAME, 48000, nullptr, behaviour);
        parameter.status = fx::UsageState::New;
        filter.update(makeIn(fx::EffectType::BiquadFilter, parameter, true, false), true,
                      behaviour);
        mix.clear();
        filter.process(mix.span(), FRAME, 48000, nullptr, behaviour);
        check((mix[0][0] != 0.0f) == fixed, "Biquad New status state clear (REV6)");
    }

    // Disabled copies
    fx::Effect disabled;
    disabled.update(makeIn(fx::EffectType::BiquadFilter, parameter, false, true), true,
                    LATEST);
    mix.clear();
    mix[1][2] = 0.9f;
    disabled.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(mix[2][2] == 0.9f, "Disabled biquad copies through");
}

void testLimiter() {
    fx::LimiterParameter parameter{};
    parameter.channel_count_max = 2;
    parameter.channel_count = 2;
    parameter.input[1] = 1;
    parameter.output[1] = 1;
    parameter.sample_rate = 48000;
    parameter.attack_coefficient = 0.1f;
    parameter.release_coefficient = 0.0005f;
    parameter.threshold = 0.5f;
    parameter.input_gain = 1.0f;
    parameter.output_gain = 1.0f;
    parameter.delay_buffer_sample_count_min = 48;
    parameter.delay_buffer_sample_count_max = 48;
    parameter.statistics_enabled = 1;
    fx::Effect effect;
    effect.update(makeIn(fx::EffectType::Limiter, parameter, true, true), true, LATEST);
    check(effect.limiterStatistics().compression_gain_min[0] == 1.0f,
          "Limiter statistics reset when new");
    Mix mix(2);
    f32 late_peak = 0.0f;
    f32 out_peak_quiet = 0.0f;
    for (u32 frame = 0; frame < 20; frame++) {
        for (u32 i = 0; i < FRAME; i++) {
            const f32 phase = static_cast<f32>(frame * FRAME + i) * 0.05f;
            mix[0][i] = 0.9f * std::sin(phase);
            mix[1][i] = 0.1f * std::sin(phase);
        }
        effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
        if (frame >= 2)
            for (u32 i = 0; i < FRAME; i++) {
                late_peak = std::max(late_peak, std::abs(mix[0][i]));
                out_peak_quiet = std::max(out_peak_quiet, std::abs(mix[1][i]));
            }
    }
    check(late_peak <= 0.5f * 1.05f, "Limiter holds the threshold after the attack");
    check(late_peak > 0.4f, "Limiter does not over-limit");
    check(near(out_peak_quiet, 0.1f, 0.002f), "Limiter passes quiet channels");

    // V2 statistics, as the client sees them after the next frame
    effect.update(makeIn(fx::EffectType::Limiter, parameter, true, false), true, LATEST);
    effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    std::array<u8, 0x90> status{};
    effect.writeStatus(status.data(), true);
    fx::LimiterStatistics statistics;
    std::memcpy(&statistics, status.data() + 0x10, sizeof(statistics));
    check(near(statistics.input_max[0], 0.9f * 32768.0f / 32767.0f, 0.001f),
          "Limiter statistics input max (full scale 32767)");
    check(statistics.compression_gain_min[0] < 0.6f &&
              statistics.compression_gain_min[1] == 1.0f,
          "Limiter statistics compression gain");
    check(near(statistics.input_max[1], 0.1f * 32768.0f / 32767.0f, 0.001f),
          "Limiter statistics per channel");

    // Statistics reset request
    parameter.statistics_reset = 1;
    effect.update(makeIn(fx::EffectType::Limiter, parameter, true, false), true, LATEST);
    mix.clear();
    effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(effect.limiterParameter().statistics_reset == 0, "Statistics reset consumed");
    effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(effect.limiterStatistics().input_max[0] == 0.0f, "Limiter statistics reset");

    // V1 has no statistics; disabled copies
    auto v1 = LATEST;
    v1.effect_info_v2 = false;
    parameter.output[0] = 1;
    fx::Effect bypass;
    bypass.update(makeIn(fx::EffectType::Limiter, parameter, false, true), true, v1);
    mix.clear();
    mix[0][1] = 0.95f;
    bypass.process(mix.span(), FRAME, 48000, nullptr, v1);
    check(mix[1][1] == 0.95f, "Disabled limiter copies through");
}

void testCompressor() {
    fx::CompressorParameter parameter{};
    parameter.channel_count_max = 2;
    parameter.channel_count = 2;
    parameter.input[1] = 1;
    parameter.output[1] = 1;
    parameter.sample_rate = 48000;
    parameter.threshold = -20.0f;
    parameter.ratio = 4.0f;
    parameter.input_gain = 0.01f;
    parameter.attack_coefficient = 0.01f;
    parameter.release_coefficient = 0.001f;
    parameter.output_gain = 0.0f;
    const auto run = [&](f32 amplitude) {
        fx::Effect effect;
        effect.update(makeIn(fx::EffectType::Compressor, parameter, true, true), true,
                      LATEST);
        Mix mix(2);
        f32 peak = 0.0f;
        for (u32 frame = 0; frame < 40; frame++) {
            for (u32 i = 0; i < FRAME; i++) {
                const f32 v = amplitude * std::sin(static_cast<f32>(frame * FRAME + i) * 0.05f);
                mix[0][i] = v;
                mix[1][i] = v;
            }
            effect.process(mix.span(), FRAME, 48000, nullptr, LATEST);
            if (frame == 39)
                for (u32 i = 0; i < FRAME; i++)
                    peak = std::max(peak, std::abs(mix[0][i]));
            check(std::isfinite(mix[0][0]), "Compressor output is finite");
        }
        return peak;
    };
    // -9 dB RMS, 11 dB over the threshold at 4:1: about -8.2 dB of gain
    const f32 loud = run(0.5f);
    const f32 expected = 0.5f * std::pow(10.0f, -0.75f * (10.0f * std::log10(0.125f) + 20.0f) / 20.0f);
    check(near(loud, expected, 0.03f), "Compressor reduces gain above the threshold");
    const f32 quiet = run(0.01f);
    check(near(quiet, 0.01f, 0.0005f), "Compressor passes signals below the threshold");

    // Makeup gain and disabled path
    parameter.makeup_gain_enabled = 1;
    fx::Effect makeup;
    makeup.update(makeIn(fx::EffectType::Compressor, parameter, true, true), true, LATEST);
    Mix mix(3);
    makeup.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(near(makeup.compressorState().output_gain,
               std::pow(10.0f, ((-20.0f * 0.5f * (0.25f - 1.0f)) - 3.0f) / 20.0f), 1e-4f),
          "Compressor makeup gain");
    parameter.output[0] = 2;
    fx::Effect disabled;
    disabled.update(makeIn(fx::EffectType::Compressor, parameter, false, true), true,
                    LATEST);
    mix[0][0] = 0.8f;
    disabled.process(mix.span(), FRAME, 48000, nullptr, LATEST);
    check(mix[2][0] == 0.8f, "Disabled compressor copies through");
}

} // namespace

int main() {
    try {
        testLayouts();
        testBehaviour();
        testUpdateAndStatus();
        testBufferMix();
        testAux();
        testCapture();
        testDelay();
        testReverb();
        testReverb3d();
        testBiquad();
        testLimiter();
        testCompressor();
    } catch (const std::exception& error) {
        std::cerr << "Audio effects test failed: " << error.what() << std::endl;
        return 1;
    }
    std::cout << "Audio effects tests passed" << std::endl;
    return 0;
}
