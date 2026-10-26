#include "core/audio/pcm_queue.hpp"
#include "core/audio/renderer_dsp.hpp"
#include "core/audio/sample_clock.hpp"
#include "core/audio/sample_decoder.hpp"

#include <iostream>
#include <stdexcept>

using namespace hydra;
using namespace hydra::audio;
namespace dsp = hydra::audio::dsp;

void check(bool condition, const char* description) {
    if (!condition)
        throw std::runtime_error(description);
}


// Mono PCM16 wave buffer whose sample i is `value(i)`
template <typename F>
dsp::WaveBuffer pcm16Wave(u32 count, F value) {
    dsp::WaveBuffer buffer;
    buffer.valid = true;
    buffer.start = 0;
    buffer.end = count;
    buffer.data.resize(count * 2);
    for (u32 i = 0; i < count; i++) {
        const i16 sample = value(i);
        std::memcpy(buffer.data.data() + i * 2, &sample, 2);
    }
    return buffer;
}

std::vector<i32> readIndices(dsp::VoiceSource& voice,
                             const dsp::SourceFormat& source, u32 count) {
    std::vector<f32> frames(count);
    const u32 read = dsp::readSource(voice, source, count, frames);
    std::vector<i32> indices;
    for (u32 i = 0; i < read; i++)
        indices.push_back(static_cast<i32>(std::lround(frames[i] * 32768.0f)));
    return indices;
}

void testRendererDsp() {
    const std::array<i16, 16> no_coefficients{};
    const dsp::SourceFormat mono{.format = PcmFormat::Int16,
                                 .channel_count = 1,
                                 .adpcm_coefficients = no_coefficients};
    const auto identity = [](u32 i) { return static_cast<i16>(i); };

    // A buffer plays start..end once, then the next one
    {
        dsp::VoiceSource voice;
        voice.buffers[0] = pcm16Wave(4, identity);
        voice.buffers[1] = pcm16Wave(3, [](u32 i) { return static_cast<i16>(100 + i); });
        const auto read = readIndices(voice, mono, 10);
        check(read == std::vector<i32>({0, 1, 2, 3, 100, 101, 102}),
              "Wave buffers play in order and the voice starves after them");
        check(voice.consumed == 2 && voice.index == 2 && voice.played == 7,
              "Consumed buffers and played samples are counted");
    }
    // Looping with a count: the first pass plays start..end, the loops
    // loop_start..loop_end, then the buffer ends
    {
        dsp::VoiceSource voice;
        voice.buffers[0] = pcm16Wave(10, identity);
        voice.buffers[0].looping = true;
        voice.buffers[0].loop_count = 2;
        voice.buffers[0].loop_start = 5;
        voice.buffers[0].loop_end = 8;
        const auto read = readIndices(voice, mono, 32);
        check(read == std::vector<i32>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 5, 6, 7,
                                        5, 6, 7}),
              "Loop count and loop range follow the Switch data source");
        check(voice.consumed == 1, "A finite loop ends its buffer");
    }
    // Looping forever without a loop range repeats the buffer
    {
        dsp::VoiceSource voice;
        voice.buffers[0] = pcm16Wave(3, identity);
        voice.buffers[0].looping = true;
        voice.buffers[0].loop_count = -1;
        const auto read = readIndices(voice, mono, 8);
        check(read == std::vector<i32>({0, 1, 2, 0, 1, 2, 0, 1}) &&
                  voice.consumed == 0,
              "An endless loop repeats the whole buffer");
    }
    // End of stream resets the played sample count
    {
        dsp::VoiceSource voice;
        voice.buffers[0] = pcm16Wave(4, identity);
        voice.buffers[0].end_of_stream = true;
        readIndices(voice, mono, 4);
        check(voice.played == 0 && voice.consumed == 1,
              "End of stream resets the played sample count");
    }
    // Flush skips buffers and reports them consumed
    {
        dsp::VoiceSource voice;
        voice.buffers[0] = pcm16Wave(4, identity);
        voice.buffers[1] = pcm16Wave(4, [](u32 i) { return static_cast<i16>(50 + i); });
        voice.flush(1);
        const auto read = readIndices(voice, mono, 2);
        check(voice.consumed == 1 && read == std::vector<i32>({50, 51}),
              "Flushed buffers are skipped and consumed");
    }

    // Sample rate conversion: every filter phase keeps a constant signal
    for (const auto quality : {dsp::SrcQuality::Default, dsp::SrcQuality::High}) {
        for (const f32 ratio : {0.5f, 1.0f, 1.2f, 1.5f}) {
            dsp::VoiceSource voice;
            voice.buffers[0] = pcm16Wave(4000, [](u32) { return static_cast<i16>(16384); });
            std::array<dsp::VoiceChannelState, 1> channels{};
            std::array<std::vector<f32>, 1> output{std::vector<f32>(240)};
            for (u32 frame = 0; frame < 3; frame++)
                dsp::processDataSource(voice, mono, channels, ratio, quality,
                                       240, output);
            for (const auto sample : output[0])
                check(std::abs(sample - 0.5f) < 0.01f,
                      "Resampled DC keeps its level");
        }
    }
    // The ratio sets the source samples consumed, the fraction carries over
    {
        dsp::VoiceSource voice;
        voice.buffers[0] = pcm16Wave(4000, identity);
        std::array<dsp::VoiceChannelState, 1> channels{};
        std::array<std::vector<f32>, 1> output{std::vector<f32>(240)};
        dsp::processDataSource(voice, mono, channels, 1.5f, dsp::SrcQuality::Default,
                               240, output);
        dsp::processDataSource(voice, mono, channels, 1.25f, dsp::SrcQuality::Default,
                               240, output);
        check(voice.played == 360 + 300 && voice.fraction == 0.0f,
              "Resampling consumes ratio x samples per frame");
    }

    // Biquad: identity passes, state carries between calls
    {
        std::array<f32, 4> samples{0.25f, -0.5f, 1.0f, 0.0f};
        std::array<f32, 2> state{};
        const i16 identity_num[3]{16384, 0, 0};
        const i16 zero_den[2]{0, 0};
        dsp::processBiquad(samples, identity_num, zero_den, state);
        check(samples == std::array<f32, 4>{0.25f, -0.5f, 1.0f, 0.0f},
              "Identity biquad");
        // One sample delay: y[n] = x[n - 1]
        const i16 delay_num[3]{0, 16384, 0};
        std::array<f32, 2> first{1.0f, 2.0f}, second{3.0f, 4.0f};
        state = {};
        dsp::processBiquad(first, delay_num, zero_den, state);
        dsp::processBiquad(second, delay_num, zero_den, state);
        check(first == std::array<f32, 2>{0.0f, 1.0f} &&
                  second == std::array<f32, 2>{2.0f, 3.0f},
              "Biquad state continues across frames");
        // Feedback with a negated denominator: y[n] = x[n] + 0.5 y[n - 1]
        const i16 one_num[3]{16384, 0, 0};
        const i16 feedback_den[2]{8192, 0};
        std::array<f32, 3> impulse{1.0f, 0.0f, 0.0f};
        state = {};
        dsp::processBiquad(impulse, one_num, feedback_den, state);
        check(impulse == std::array<f32, 3>{1.0f, 0.5f, 0.25f},
              "Biquad denominators are stored negated");
    }

    // Ramps: from the previous volume to the new one over the frame
    {
        std::array<f32, 4> samples{1.0f, 1.0f, 1.0f, 1.0f};
        dsp::processVolumeRamp(samples, 0.0f, 1.0f);
        check(samples == std::array<f32, 4>{0.0f, 0.25f, 0.5f, 0.75f},
              "Volume ramp");
        std::array<f32, 4> mix{};
        const std::array<f32, 4> input{1.0f, 1.0f, 1.0f, 1.0f};
        const f32 last = dsp::processMixRamp(mix, input, 1.0f, 0.0f);
        check(mix == std::array<f32, 4>{1.0f, 0.75f, 0.5f, 0.25f} &&
                  last == 0.25f,
              "Mix ramp returns the last value it added");
    }

    // Depop: what a stopped voice left decays over the following samples
    {
        std::array<f32, 3> buffer{};
        const f32 decay = dsp::depopDecay(48000);
        const f32 left = dsp::processDepop(buffer, 1.0f, decay);
        check(std::abs(buffer[0] - decay) < 1e-6f &&
                  std::abs(buffer[2] - decay * decay * decay) < 1e-6f &&
                  std::abs(left - buffer[2]) < 1e-6f,
              "Depop decays the last sample");
        check(dsp::depopDecay(32000) == 0.943695f, "Depop decay at 32 kHz");
    }

    // 5.1 to stereo with the default device coefficients
    check(std::abs(dsp::downmixSurroundToStereo(dsp::DEFAULT_SURROUND_TO_STEREO,
                                                1.0f, 1.0f, 1.0f, 1.0f) -
                   (1.0f + 0.707f + 0.251f + 0.707f)) < 1e-6f,
          "Surround downmix coefficients");
    std::cout << "Audio renderer DSP tests passed\n";
}

int main() {
    try {
        testRendererDsp();
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
        check(consumedSampleClock(2500, 1000, 900, 400) == 1000,
              "Only underruns since stream start are excluded");
        check(consumedSampleClock(1100, 1000, 600, 400) == 0,
              "Underrun correction saturates without unsigned wraparound");
        check(consumedSampleClock(999, 1000, 300, 400) == 0,
              "Earlier clock or missing counters cannot underflow");
        check(elapsedDspFrames(160000, 0, 160) == 1008,
              "Guest DSP time follows elapsed hardware time");
        check(elapsedDspFrames(160000, 0, 160) >
                  consumedSampleClock(160000, 0, 32000, 0) / 160 + 8,
              "Host underrun debt does not delay guest DSP time");
        check(elapsedDspFrames(4000, 4000, 240) == 8 &&
                  elapsedDspFrames(0, 0, 0) == 0,
              "DSP timeline startup and invalid frame-size bounds");
        {
            // Repeated callback starvation used to accumulate silence debt:
            // hardware advanced, so production replaced missing frames even
            // though those frames never drained the PCM queue. Simulate real
            // queue consumption through many stalls and recoveries.
            constexpr u64 lead = 8 * 160;
            u64 hardware = 0, missing = 0, generated = lead, queued = lead;
            u64 largest_queue = queued;
            for (u32 tick = 0; tick < 20000; ++tick) {
                const u64 requested = tick % 17 == 0 ? 4096 : 160;
                const u64 consumed = std::min(requested, queued);
                hardware += requested;
                missing += requested - consumed;
                queued -= consumed;
                const auto pcm_clock =
                    consumedSampleClock(hardware, 0, missing, 0);
                const auto frames =
                    framesForSampleClock(pcm_clock, generated, 160);
                const u64 produced_now = static_cast<u64>(frames) * 160;
                generated += produced_now;
                queued += produced_now;
                largest_queue = std::max(largest_queue, queued);
                check(generated - pcm_clock == queued,
                      "Producer timeline matches actual PCM still queued");
                check(queued == lead,
                      "Repeated underruns do not accumulate queue latency");
            }
            check(
                missing > 32000 && largest_queue == lead,
                "Queue stays bounded after more than a second of missing PCM");
        }
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
