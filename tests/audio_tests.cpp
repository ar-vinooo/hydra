#include "core/audio/pcm_queue.hpp"
#include "core/audio/sample_clock.hpp"
#include "core/audio/sample_decoder.hpp"

#include <iostream>
#include <stdexcept>

using namespace hydra;
using namespace hydra::audio;

void check(bool condition, const char* description) {
    if (!condition)
        throw std::runtime_error(description);
}

int main() {
    try {
        const std::array<u8, 4> pcm16{0x00, 0x80, 0xff, 0x7f};
        check(decodePcm(pcm16, 0, PcmFormat::Int16) == -1.0f, "PCM16 minimum");
        check(decodePcm(pcm16, 1, PcmFormat::Int16) == 32767.0f / 32768.0f,
              "PCM16 maximum");
        check(decodePcm(pcm16, 2, PcmFormat::Int16) == 0.0f, "PCM bounds");
        const std::array<u8, 2> pcm8{0x80, 0x7f};
        check(decodePcm(pcm8, 0, PcmFormat::Int8) == -1.0f, "PCM8 sign");
        const std::array<u8, 6> pcm24{0, 0, 0x80, 0xff, 0xff, 0x7f};
        check(decodePcm(pcm24, 0, PcmFormat::Int24) == -1.0f, "PCM24 minimum");
        check(decodePcm(pcm24, 1, PcmFormat::Int24) > 0.999f, "PCM24 maximum");
        const std::array<u8, 4> pcm32{0, 0, 0, 0x80};
        check(decodePcm(pcm32, 0, PcmFormat::Int32) == -1.0f, "PCM32 minimum");
        const std::array<u8, 4> float_sample{0, 0, 0, 0x3f};
        check(decodePcm(float_sample, 0, PcmFormat::Float) == 0.5f,
              "Float PCM");
        const std::array<u8, 4> float_nan{0, 0, 0xc0, 0x7f};
        check(decodePcm(float_nan, 0, PcmFormat::Float) == 0.0f,
              "NaN rejected");

        std::array<i16, 16> coefficients{};
        std::array<u8, 8> frame{0, 0x17, 0x8f, 0, 0, 0, 0, 0};
        i16 history0 = 0, history1 = 0;
        auto samples =
            decodeDspAdpcm(frame, coefficients, 14, history0, history1);
        check(samples.size() == 14, "DSP frame length");
        check(samples[0] == 1.0f / 32768.0f && samples[1] == 7.0f / 32768.0f &&
                  samples[2] == -8.0f / 32768.0f &&
                  samples[3] == -1.0f / 32768.0f,
              "DSP nibble order and sign");
        coefficients[0] = 2048;
        frame.fill(0);
        history0 = 100;
        samples = decodeDspAdpcm(frame, coefficients, 14, history0, history1);
        check(samples[13] == 100.0f / 32768.0f && history0 == 100 &&
                  history1 == 100,
              "DSP predictor and history");
        samples = decodeDspAdpcm(frame, coefficients, 14, history0, history1);
        check(samples[0] == 100.0f / 32768.0f,
              "DSP consecutive buffer history");
        frame.fill(0x77);
        frame[0] = 15;
        samples = decodeDspAdpcm(frame, coefficients, 14, history0, history1);
        check(samples[0] == 32767.0f / 32768.0f, "DSP saturation");
        check(decodeDspAdpcm(std::span(frame).first(7), coefficients, 14,
                             history0, history1)
                  .empty(),
              "DSP incomplete frame bounds");

        check(framesForSampleClock(0, 0, 240) == 8,
              "Hardware clock startup lead");
        check(framesForSampleClock(0, 1920, 240) == 0,
              "Hardware clock no duplicate samples");
        check(framesForSampleClock(512, 1920, 240) == 3,
              "Hardware clock callback rounding");
        check(framesForSampleClock(100000, 0, 240) == 20,
              "Hardware clock bounded catchup");
        check(framesForSampleClock(1024, 0, 160, 1) == 1,
              "Large hardware callback renders one guest DSP frame at a time");
        check(interpolateSampleClock(1024, 1024, 5000000, 32000) == 1184,
              "Clock advances between block callbacks at the source rate");
        check(interpolateSampleClock(1024, 1024, 100000000, 32000) == 2048,
              "Clock stops extrapolating when backend callbacks stall");
        check(interpolateSampleClock(1024, 1024, 32000000, 32000) ==
                  interpolateSampleClock(2048, 1024, 0, 32000),
              "Clock remains continuous across 32 ms block callbacks");
        u64 produced = 0, requested = 0;
        for (u32 tick = 0; tick < 100000; tick++) {
            // Irregular callback sizes and polling intervals, with a device
            // oscillator deliberately running at a different rate from wall
            // time.
            if (tick % 3 == 0)
                requested += 511;
            if (tick % 7 == 0)
                requested += 127;
            produced += static_cast<u64>(
                            framesForSampleClock(requested, produced, 240)) *
                        240;
            check(produced >= requested + 1920 && produced < requested + 2160,
                  "Hardware clock lead stays bounded without cumulative drift");
        }

        PcmQueue queue;
        const std::array<bool, 4> pending{false, true, false, false};
        check(resumeWaveHead(pending, 0, 1) == 1,
              "Starved voice resumes the client's queued head");
        check(resumeWaveHead(pending, 1, 0) == 1,
              "Active wave is not rewound by stale head");
        check(resumeWaveHead(pending, 0, -1) == 0,
              "Invalid client head is ignored");
        std::array<u8, 4> input{1, 2, 3, 4};
        queue.enqueue(10, input);
        input.fill(99);
        std::array<u8, 2> first{};
        check(queue.read(first).empty() && first[0] == 1 && first[1] == 2,
              "Queue owns samples and preserves partial buffer");
        const std::array<u8, 2> next{5, 6};
        queue.enqueue(11, next);
        std::array<u8, 6> output{};
        const auto done = queue.read(output);
        check(done == std::vector<buffer_id_t>({10, 11}),
              "Queue completion order");
        check(output == std::array<u8, 6>{3, 4, 5, 6, 0, 0},
              "Queue cross-buffer copy and silence");
        output.fill(99);
        check(queue.read(output).empty() && output == std::array<u8, 6>{},
              "Queue underrun silence");
        queue.enqueue(12, {});
        queue.enqueue(13, next);
        check(queue.read(output) == std::vector<buffer_id_t>({12, 13}),
              "Queue empty buffer completion");
        std::cout << "Audio decoder and PCM queue regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
