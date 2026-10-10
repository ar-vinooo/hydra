#include "core/horizon/services/audio/audio_renderer.hpp"

#include "core/debugger/debugger_manager.hpp"
#include "core/horizon/kernel/process.hpp"

namespace hydra::horizon::services::audio {

namespace {

#pragma pack(push, 1)
struct UpdateDataHeader {
    u32 revision;
    u32 behavior_size;
    u32 mempools_size;
    u32 voices_size;
    u32 channels_size;
    u32 effects_size;
    u32 mixes_size;
    u32 sinks_size;
    u32 performance_manager_size;
    u32 _unknown;
    u32 render_info_size;
    u32 _reserved[4];
    u32 total_size;
};

enum class MemPoolState : u32 {
    Invalid,
    New,
    RequestDetach,
    Detached,
    RequestAttach,
    Attached,
    Released,
};

struct MemPoolInfoIn {
    vaddr_t address;
    u64 size;
    MemPoolState state;
    u32 _padding[3];
};

struct MemPoolInfoOut {
    MemPoolState new_state;
    u32 _padding[3];
};

enum class VoicePlayState : u8 {
    Started = 0,
    Stopped = 1,
    Paused = 2,
};

struct BiquadFilter {
    bool enable;
    u8 _padding;
    i16 numerator[3];
    i16 denominator[2];
};

struct WaveBuffer {
    vaddr_t address;
    u64 size;
    u32 start_sample_offset;
    u32 end_sample_offset;
    bool is_looping;
    bool end_of_stream;
    bool sent_to_server;
    u8 _padding1[5];
    vaddr_t context_addr;
    u64 context_sz;
    u64 _padding2;
};

struct VoiceInfoIn {
    u32 id;
    u32 node_id;
    bool is_new;
    bool is_used;
    VoicePlayState play_state;
    PcmFormat sample_format : 8;
    u32 sample_rate;
    u32 priority;
    u32 sorting_order;
    u32 channel_count;
    f32 pitch;
    f32 volume;
    BiquadFilter biquads[2];
    u32 wave_buffer_count;
    i16 wave_buffer_head;
    u16 _padding1;
    u32 _padding2;
    const void* extra_params_ptr;
    u64 extra_params_sz;
    u32 dest_mix_id;
    u32 dest_splitter_id;
    WaveBuffer wave_buffers[4];
    u32 channel_ids[6];
    u8 _padding3[24];
};

enum class EffectState : u8 {
    Enabled = 3,
    Disabled = 4,
};

struct EffectInfoOutV1 {
    EffectState state;
    u8 _reserved[15];
};

struct SinkInfoOut {
    u32 last_written_offset;
    u32 _padding;
    u64 _reserved[3];
};

struct ErrorInfo {
    result_t result;
    u32 _padding;
    u64 extra_error_info;
};

struct BehaviorInfoOut {
    ErrorInfo error_infos[10];
    u32 error_info_count;
    u32 _reserved[3];
};

struct RenderInfoOut {
    u64 elapsed_frame_count;
    u64 _reserved;
};

struct PerformanceInfoOut {
    u32 history_size;
    u32 _reserved[3];
};
#pragma pack(pop)

} // namespace

} // namespace hydra::horizon::services::audio

ENABLE_ENUM_FORMATTING(hydra::horizon::services::audio::MemPoolState, Invalid,
                       "invalid", New, "new", RequestDetach, "request detach",
                       Detached, "detached", RequestAttach, "request attach",
                       Attached, "attached", Released, "released")

namespace hydra::horizon::services::audio {

DEFINE_SERVICE_COMMAND_TABLE(IAudioRenderer, 4, requestUpdate, 5, start, 6,
                             stop, 7, querySystemEvent, 8,
                             setRenderingTimeLimit, 9, getRenderingTimeLimit,
                             10, requestUpdateAuto)

IAudioRenderer::IAudioRenderer(const AudioRendererParameters& params_,
                               const u64 work_buffer_size_)
    : params{params_}, work_buffer_size{work_buffer_size_},
      event{new kernel::Event(false, "IAudioRenderer event")} {
    voices.resize(params.voice_count);
    voice_states.resize(params.voice_count);

    // HACK: create a thread that signals the handle every audio frame (5ms)
    new std::thread([&] {
        GET_CURRENT_PROCESS_DEBUGGER().registerThisThread("Audren signal");
        while (true) {
            event->signal();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        GET_CURRENT_PROCESS_DEBUGGER().unregisterThisThread();
    });
}

result_t
IAudioRenderer::requestUpdate(InBuffer<BufferAttr::MapAlias> in_buffer,
                              OutBuffer<BufferAttr::MapAlias> out_buffer,
                              OutBuffer<BufferAttr::MapAlias> out_perf_buffer) {
    return requestUpdateImpl(in_buffer.stream, out_buffer.stream,
                             out_perf_buffer.stream);
}

result_t
IAudioRenderer::querySystemEvent(kernel::Process* process,
                                 OutHandle<HandleAttr::Copy> out_handle) {
    out_handle = process->addHandle(event);
    return RESULT_SUCCESS;
}

result_t IAudioRenderer::setRenderingTimeLimit(u32 time_limit) {
    rendering_time_limit = time_limit;
    return RESULT_SUCCESS;
}

result_t IAudioRenderer::getRenderingTimeLimit(u32* out_time_limit) const {
    *out_time_limit = rendering_time_limit;
    return RESULT_SUCCESS;
}

result_t IAudioRenderer::requestUpdateAuto(
    InBuffer<BufferAttr::AutoSelect> in_buffer,
    OutBuffer<BufferAttr::AutoSelect> out_buffer,
    OutBuffer<BufferAttr::AutoSelect> out_perf_buffer) {
    return requestUpdateImpl(in_buffer.stream, out_buffer.stream,
                             out_perf_buffer.stream);
}

result_t IAudioRenderer::requestUpdateImpl(
    std::optional<ztd::io::MemoryStream> in_stream,
    std::optional<ztd::io::MemoryStream> out_stream,
    std::optional<ztd::io::MemoryStream> out_perf_stream) {
    ONCE(LOG_FUNC_STUBBED(Services));

    // Header
    const auto in_header = in_stream->read<UpdateDataHeader>();

    // TODO: correct?
    auto header = out_stream->writeReturningPtr<UpdateDataHeader>();
    header->revision = in_header.revision; // makeMagic4('R', 'E', 'V', '4');
    header->total_size = sizeof(UpdateDataHeader);

    in_stream->seekBy(in_header.behavior_size);

    // Mempools
    u32 mempool_count = (params.effect_count + params.voice_count * 4);
    header->mempools_size = mempool_count * sizeof(MemPoolInfoOut);
    header->total_size += header->mempools_size;
    for (u32 i = 0; i < mempool_count; i++) {
        const auto mempool_in = in_stream->read<MemPoolInfoIn>();

        MemPoolInfoOut mempool{};
        switch (mempool_in.state) {
        case MemPoolState::RequestAttach:
            mempool.new_state = MemPoolState::Attached;
            break;
        case MemPoolState::RequestDetach:
            mempool.new_state = MemPoolState::Detached;
            break;
        default:
            // Other states don't change
            mempool.new_state = mempool_in.state;
            break;
        }
        out_stream->write(mempool);
    }

    // Audio frames rendered since the last update (5ms each)
    constexpr auto FRAME_DURATION = std::chrono::milliseconds(5);
    const auto now = std::chrono::steady_clock::now();
    u32 frame_count = 0;
    if (last_update_time == std::chrono::steady_clock::time_point{}) {
        last_update_time = now;
    } else {
        frame_count = static_cast<u32>(
            std::min<i64>((now - last_update_time) / FRAME_DURATION, 20));
        last_update_time += frame_count * FRAME_DURATION;
        if (now - last_update_time > 20 * FRAME_DURATION)
            last_update_time = now;
    }

    // Voices
    header->voices_size = params.voice_count * sizeof(VoiceInfoOut);
    header->total_size += header->voices_size;
    for (u32 i = 0; i < params.voice_count; i++) {
        const auto voice_in = in_stream->read<VoiceInfoIn>();

        VoiceInfoOut& voice = voices[i];
        VoiceState& state = voice_states[i];
        if (voice_in.is_new) {
            voice = {};
            state = {};
        }

        // Receive new wave buffers
        for (u32 j = 0; j < SIZEOF_ARRAY(voice_in.wave_buffers); j++) {
            const auto& wave_buffer = voice_in.wave_buffers[j];
            if (wave_buffer.sent_to_server)
                continue;

            state.slots[j] = {
                .valid = wave_buffer.end_sample_offset >
                         wave_buffer.start_sample_offset,
                .start = wave_buffer.start_sample_offset,
                .end = wave_buffer.end_sample_offset,
                .is_looping = wave_buffer.is_looping,
            };
            if (j == state.crnt_slot)
                state.offset = wave_buffer.start_sample_offset;
        }

        if (!voice_in.is_used) {
            // Nothing to do
        } else if (voice_in.play_state == VoicePlayState::Started) {
            // Consume samples
            const u32 sample_rate = voice_in.sample_rate != 0
                                        ? voice_in.sample_rate
                                        : params.sample_rate;
            const f32 pitch = voice_in.pitch > 0.0f ? voice_in.pitch : 1.0f;
            u64 sample_count = static_cast<u64>(
                static_cast<f64>(frame_count) * params.sample_count *
                sample_rate / params.sample_rate * static_cast<f64>(pitch));
            for (u32 guard = 0; sample_count != 0 && guard < 64; guard++) {
                auto& slot = state.slots[state.crnt_slot];
                if (!slot.valid)
                    break;

                if (state.offset < slot.start || state.offset > slot.end)
                    state.offset = slot.start;
                const u64 taken =
                    std::min<u64>(sample_count, slot.end - state.offset);
                state.offset += static_cast<u32>(taken);
                voice.played_sample_count += taken;
                sample_count -= taken;

                if (state.offset >= slot.end) {
                    if (slot.is_looping) {
                        state.offset = slot.start;
                    } else {
                        slot.valid = false;
                        voice.num_wave_buffers_consumed++;
                        state.crnt_slot = (state.crnt_slot + 1) %
                                          SIZEOF_ARRAY(state.slots);
                        state.offset = state.slots[state.crnt_slot].start;
                    }
                }
            }
        } else if (voice_in.play_state == VoicePlayState::Stopped) {
            // Release all wave buffers
            for (auto& slot : state.slots) {
                if (slot.valid) {
                    slot.valid = false;
                    voice.num_wave_buffers_consumed++;
                }
            }
            voice.played_sample_count = 0;
            state.crnt_slot = 0;
            state.offset = 0;
        }
        // Paused: nothing to do

        out_stream->write(voice);
    }

    // Channels
    header->channels_size = 0x0;
    header->total_size += header->channels_size;
    // TODO

    // Effects
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (false) {
        // header->effects_size = TODO;
        // TODO
    } else {
        header->effects_size = params.effect_count * sizeof(EffectInfoOutV1);
        for (u32 i = 0; i < params.effect_count; i++) {
            out_stream->write<EffectInfoOutV1>({
                .state = EffectState::Enabled,
            });
        }
    }
    header->total_size += header->effects_size;

    // Sinks
    header->sinks_size = params.sink_count * sizeof(SinkInfoOut);
    header->total_size += header->sinks_size;
    for (u32 i = 0; i < params.sink_count; i++) {
        out_stream->write<SinkInfoOut>({
            .last_written_offset = 0,
        });
    }

    // Behavior
    header->behavior_size = sizeof(BehaviorInfoOut);
    header->total_size += header->behavior_size;
    out_stream->write<BehaviorInfoOut>({
        .error_info_count = 0,
    });

    // Render info
    // TODO: if elapsed frame count supported
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (false) {
        header->render_info_size = sizeof(RenderInfoOut);
        header->total_size += header->render_info_size;
        out_stream->write<RenderInfoOut>({
            .elapsed_frame_count = 0,
        });
    }

    // Performance
    header->performance_manager_size = sizeof(PerformanceInfoOut);
    header->total_size += header->performance_manager_size;
    // HACK
    if (out_perf_stream) {
        out_perf_stream->write<PerformanceInfoOut>({
            .history_size = 0,
        });
    }

    return RESULT_SUCCESS;
}

} // namespace hydra::horizon::services::audio
