#pragma once

#include "core/audio/renderer_dsp.hpp"
#include "core/audio/renderer_effects.hpp"
#include "core/horizon/kernel/kernel.hpp"
#include "core/horizon/services/audio/const.hpp"
#include "core/horizon/services/const.hpp"

namespace hydra::horizon::services::audio {

struct VoiceInfoOut {
    u64 played_sample_count;
    u32 num_wave_buffers_consumed;
    u32 voice_drops_count;
};

class IAudioRenderer : public IService {
  public:
    IAudioRenderer(System& system, const AudioRendererParameters& params_,
                   const u64 work_buffer_size_);
    ~IAudioRenderer() override;

  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    friend struct AudioRendererTestAccess;
    AudioRendererParameters params;
    // TODO: use
    [[maybe_unused]] u64 work_buffer_size;

    struct RenderState;
    std::unique_ptr<RenderState> render_state;
    std::mutex render_mutex;
    bool renderAudio();

    kernel::Event* event;
    std::jthread signal_thread;
    std::unique_ptr<IStream> output_stream;
    std::atomic<u64> queued_samples{0};
    std::atomic<bool> running{false};
    std::atomic<u64> elapsed_frames{0};
    u64 update_count{0};
    u64 sample_clock_origin{0};
    u64 missing_frames_origin{0};
    u64 produced_samples{0};
    u64 head_resyncs{0};
    // DEBUG: HYDRA_AUDIO_STATS counters, per stats period
    u64 stats_frames{0};
    u64 stats_starved{0};
    u64 stats_biquad{0};
    u64 stats_clipped{0};
    bool logged_biquad{false};
    std::chrono::steady_clock::time_point last_stats_time{};

    u32 rendering_time_limit{0x1000}; // TODO: what should this be?
    std::vector<VoiceInfoOut> voices;

    // Server-side voice state: the wave buffers it owns, its position and the
    // per channel DSP state (see core/audio/renderer_dsp.hpp)
    struct VoiceState {
        audio::dsp::VoiceSource source;
        std::array<audio::dsp::VoiceChannelState,
                   audio::dsp::VOICE_CHANNEL_COUNT_MAX>
            channels{};
        std::array<i16, 16> coefficients{};
        f32 previous_volume{0.0f};
        // Stopped or paused after playing: the next frame only depops
        bool was_playing{false};
        // Biquad filters enabled in the last frame (their state is kept)
        std::array<bool, audio::dsp::BIQUAD_FILTER_COUNT> biquad_enabled{};
        u16 decoding_flags{0};
        audio::dsp::SrcQuality src_quality{audio::dsp::SrcQuality::Default};
        u32 flush_count{0};
        // Stopped; preserve buffers queued before Start.
        u8 previous_play_state{1};
    };
    std::vector<VoiceState> voice_states;

    // Splitters: a voice or mix sent to several destination mixes, each with
    // its own buffer volumes (ramped like a channel's mix volumes) and, from
    // REV12, two biquad filters
    struct SplitterBiquad {
        bool enable;
        u8 _reserved;
        i16 numerator[3];
        i16 denominator[2];
    };
    struct SplitterDestinationState {
        u32 mix_id{0x7fffffff}; // UnusedMixId
        bool used{false};
        // Mixed through, so the previous volumes catch up after the frame
        bool need_update{false};
        std::array<f32, audio::dsp::MIX_BUFFER_COUNT_MAX> volume{};
        std::array<f32, audio::dsp::MIX_BUFFER_COUNT_MAX> previous{};
        std::array<SplitterBiquad, audio::dsp::BIQUAD_FILTER_COUNT> biquads{};
        std::array<bool, audio::dsp::BIQUAD_FILTER_COUNT> biquad_enabled{};
        std::array<std::array<f32, 2>, audio::dsp::BIQUAD_FILTER_COUNT>
            biquad_state{};
        bool isConfigured() const { return used && mix_id != 0x7fffffff; }
    };
    struct SplitterState {
        u32 sample_rate{0};
        // Destination ids, in order
        std::vector<u32> destinations;
    };
    // Effects of the mixes, and the client process whose memory the aux
    // and capture effects exchange samples through
    std::vector<audio::dsp::effects::Effect> effects;
    audio::dsp::effects::Behaviour effect_behaviour;
    kernel::Process* guest_process{nullptr};

    std::vector<SplitterState> splitters;
    std::vector<SplitterDestinationState> splitter_destinations;
    // Parses (and, with `apply`, takes) the splitter update at `offset`.
    // Returns where the mixes start, or nothing if the data is invalid.
    std::optional<u64> updateSplitters(const u8* data, u64 size, u64 offset,
                                       bool apply);
    // Destination `index` of a splitter, null past its end
    SplitterDestinationState* getSplitterDestination(u32 splitter, u32 index);
    // Channel resource mix volumes of the last frame (mix ramps), by id
    std::vector<std::array<f32, audio::dsp::MIX_BUFFER_COUNT_MAX>>
        previous_channel_mix;
    // Values stopped voices left in the mix buffers, by mix and buffer
    std::vector<std::array<f32, audio::dsp::MIX_BUFFER_COUNT_MAX>> depop;
    // Renders one DSP frame of params.sample_count stereo samples
    void renderFrame(std::vector<f32>& stereo);
    std::chrono::steady_clock::time_point last_update_time{};
    std::chrono::steady_clock::time_point last_render_time{};
    u32 observed_formats{0};
    bool logged_header{false};
    bool logged_output{false};

    // Commands
    result_t requestUpdate(kernel::Process* process,
                           InBuffer<BufferAttr::MapAlias> in_buffer,
                           OutBuffer<BufferAttr::MapAlias> out_buffer,
                           OutBuffer<BufferAttr::MapAlias> out_perf_buffer);
    result_t start();
    result_t stop();
    result_t querySystemEvent(kernel::Process* process,
                              OutHandle<HandleAttr::Copy> out_handle);
    result_t setRenderingTimeLimit(u32 time_limit);
    result_t getRenderingTimeLimit(u32* out_time_limit) const;
    result_t
    requestUpdateAuto(kernel::Process* process,
                      InBuffer<BufferAttr::AutoSelect> in_buffer,
                      OutBuffer<BufferAttr::AutoSelect> out_buffer,
                      OutBuffer<BufferAttr::AutoSelect> out_perf_buffer);

    // Impl
    result_t
    requestUpdateImpl(kernel::Process* process,
                      std::optional<ztd::io::MemoryStream> in_stream,
                      std::optional<ztd::io::MemoryStream> out_stream,
                      std::optional<ztd::io::MemoryStream> out_perf_stream);
};

} // namespace hydra::horizon::services::audio
