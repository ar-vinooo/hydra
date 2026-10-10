#pragma once

#include "core/audio/stream.hpp"
#include <cmath>

namespace hydra::audio {

inline usize pcmSampleBytes(PcmFormat format) {
    switch (format) {
    case PcmFormat::Int8:
        return 1;
    case PcmFormat::Int16:
        return 2;
    case PcmFormat::Int24:
        return 3;
    case PcmFormat::Int32:
    case PcmFormat::Float:
        return 4;
    default:
        return 0;
    }
}

// Guest PCM samples are little endian and may be unaligned.
inline f32 decodePcm(std::span<const u8> bytes, usize index, PcmFormat format) {
    const auto size = pcmSampleBytes(format);
    if (!size || index >= bytes.size() / size)
        return 0.0f;
    const auto* p = bytes.data() + index * size;
    u32 bits = 0;
    for (usize i = 0; i < size; i++)
        bits |= static_cast<u32>(p[i]) << (i * 8);
    switch (format) {
    case PcmFormat::Int8:
        return static_cast<f32>(static_cast<i8>(bits)) / 128.0f;
    case PcmFormat::Int16:
        return static_cast<f32>(static_cast<i16>(bits)) / 32768.0f;
    case PcmFormat::Int24: {
        const i32 value = (bits & 0x800000u)
                              ? static_cast<i32>(bits) - 0x1000000
                              : static_cast<i32>(bits);
        return static_cast<f32>(value) / 8388608.0f;
    }
    case PcmFormat::Int32:
        return static_cast<f32>(static_cast<i32>(bits)) / 2147483648.0f;
    case PcmFormat::Float: {
        const f32 value = std::bit_cast<f32>(bits);
        return std::isfinite(value) ? value : 0.0f;
    }
    default:
        return 0.0f;
    }
}

// Nintendo DSP ADPCM: one predictor/scale byte and fourteen signed nibbles
// per eight-byte mono frame. History persists between consecutive buffers.
inline std::vector<f32> decodeDspAdpcm(std::span<const u8> bytes,
                                       std::span<const i16, 16> coefficients,
                                       u32 sample_count, i16& history0,
                                       i16& history1) {
    sample_count =
        static_cast<u32>(std::min<usize>(sample_count, bytes.size() / 8 * 14));
    std::vector<f32> output(sample_count);
    for (u32 i = 0; i < sample_count; i++) {
        const auto* frame = bytes.data() + (i / 14) * 8;
        const u32 predictor = std::min<u32>(frame[0] >> 4, 7);
        const i64 scale = 1ll << (frame[0] & 15);
        const u8 packed = frame[1 + (i % 14) / 2];
        i32 nibble = (i % 2) ? packed & 15 : packed >> 4;
        if (nibble >= 8)
            nibble -= 16;
        const i64 predicted =
            (static_cast<i64>(nibble) * scale * 2048 + 1024 +
             static_cast<i64>(coefficients[predictor * 2]) * history0 +
             static_cast<i64>(coefficients[predictor * 2 + 1]) * history1) >>
            11;
        const i16 sample =
            static_cast<i16>(std::clamp<i64>(predicted, -32768, 32767));
        history1 = history0;
        history0 = sample;
        output[i] = static_cast<f32>(sample) / 32768.0f;
    }
    return output;
}

} // namespace hydra::audio
