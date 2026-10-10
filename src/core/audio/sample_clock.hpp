#pragma once

#include "core/audio/stream.hpp"

namespace hydra::audio {

// Hardware requests include silence emitted during underruns. Those frames
// never consumed queued PCM and must not create a permanent producer debt.
inline u64 consumedSampleClock(u64 sample_clock, u64 clock_origin,
                               u64 missing_frames, u64 missing_origin) {
    const auto elapsed =
        sample_clock > clock_origin ? sample_clock - clock_origin : 0;
    const auto missing =
        missing_frames > missing_origin ? missing_frames - missing_origin : 0;
    return elapsed > missing ? elapsed - missing : 0;
}

// The guest DSP timeline keeps advancing during host PCM underruns. Reporting
// only enqueued PCM would leave the client's audio clock permanently behind.
inline u64 elapsedDspFrames(u64 sample_clock, u64 clock_origin,
                            u32 samples_per_frame) {
    if (!samples_per_frame)
        return 0;
    const u64 elapsed =
        sample_clock > clock_origin ? sample_clock - clock_origin : 0;
    return elapsed / samples_per_frame + 8;
}

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
