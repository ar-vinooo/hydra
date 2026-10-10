#pragma once

#include "core/audio/stream.hpp"

namespace hydra::audio {

inline u64 interpolateSampleClock(u64 requested, u64 callback_frames,
                                  u64 elapsed_ns, u32 sample_rate) {
    const u64 elapsed_frames =
        std::min(elapsed_ns, u64{1000000000}) * sample_rate / 1000000000;
    return requested + std::min(elapsed_frames, callback_frames);
}

inline u32 framesForSampleClock(u64 requested_samples, u64 produced_samples,
                                u32 samples_per_frame, u32 max_frames = 20) {
    if (!samples_per_frame)
        return 0;
    const u64 target =
        requested_samples + static_cast<u64>(samples_per_frame) * 8;
    const u64 wanted =
        target > produced_samples ? target - produced_samples : 0;
    return static_cast<u32>(std::min<u64>(
        (wanted + samples_per_frame - 1) / samples_per_frame, max_frames));
}

} // namespace hydra::audio
