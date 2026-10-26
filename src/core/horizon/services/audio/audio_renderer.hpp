#pragma once

#include "core/audio/renderer_dsp.hpp"
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
