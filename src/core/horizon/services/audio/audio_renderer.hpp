#pragma once

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
    std::chrono::steady_clock::time_point last_stats_time{};

    u32 rendering_time_limit{0x1000}; // TODO: what should this be?
    std::vector<VoiceInfoOut> voices;

    // Server-side source positions and owned guest wave buffers.
    struct WaveBufferSlot {
        bool valid{false};
        u32 start{0};
        u32 end{0};
        bool is_looping{false};
        std::vector<u8> data;
        vaddr_t address{0};
        std::vector<f32> decoded;
        bool decoded_ready{false};
        i16 history[2]{};
        bool has_context{false};
    };
    struct VoiceState {
        WaveBufferSlot slots[4];
        u32 crnt_slot{0};
        u32 offset{0};
        f64 fraction{0.0};
        i16 history[2]{};
        std::array<i16, 16> coefficients{};
        u8 previous_play_state{
            1}; // Stopped; preserve buffers queued before Start.
    };
    std::vector<VoiceState> voice_states;
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
