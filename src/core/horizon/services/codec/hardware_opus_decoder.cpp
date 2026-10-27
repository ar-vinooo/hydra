#include "core/horizon/services/codec/hardware_opus_decoder.hpp"

#ifdef HYDRA_OPUS_ENABLED
#include <opus.h>
#endif

namespace hydra::horizon::services::codec {

namespace {

// Packets start with a big endian header: the size of the Opus data and the
// final range of the encoder
constexpr usize PACKET_HEADER_SIZE = 8;

// Longest Opus frame: 120 ms at 48 kHz
constexpr i32 MAX_FRAME_SAMPLES = 5760;

u32 readBigEndian(const u8* ptr) {
    return (u32(ptr[0]) << 24) | (u32(ptr[1]) << 16) | (u32(ptr[2]) << 8) |
           u32(ptr[3]);
}

} // namespace

DEFINE_SERVICE_COMMAND_TABLE(IHardwareOpusDecoder, 0, decodeInterleavedOld, 4,
                             decodeInterleavedWithPerfOld, 6, decodeInterleaved)

IHardwareOpusDecoder::IHardwareOpusDecoder(i32 sample_rate_, i32 channel_count_)
    : sample_rate{sample_rate_}, channel_count{channel_count_} {
#ifdef HYDRA_OPUS_ENABLED
    int error = OPUS_OK;
    decoder = opus_decoder_create(sample_rate, channel_count, &error);
    if (error != OPUS_OK)
        LOG_ERROR(Services,
                  "Failed to create Opus decoder (sample rate: {}, channel "
                  "count: {}): {}",
                  sample_rate, channel_count, opus_strerror(error));
#endif
}

IHardwareOpusDecoder::~IHardwareOpusDecoder() {
#ifdef HYDRA_OPUS_ENABLED
    if (decoder)
        opus_decoder_destroy(static_cast<OpusDecoder*>(decoder));
#endif
}

result_t IHardwareOpusDecoder::decodeInterleavedOld(
    InBuffer<BufferAttr::MapAlias> in_opus_buffer, i32* out_decoded_data_size,
    i32* out_decoded_sample_count,
    OutBuffer<BufferAttr::MapAlias> out_pcm_buffer) {
    return decodeInterleavedImpl(in_opus_buffer.stream, out_decoded_data_size,
                                 out_decoded_sample_count,
                                 out_pcm_buffer.stream);
}

result_t IHardwareOpusDecoder::decodeInterleavedWithPerfOld(
    InBuffer<BufferAttr::MapAlias> in_opus_buffer, i32* out_decoded_data_size,
    i32* out_decoded_sample_count, u64* out_time_taken,
    OutBuffer<BufferAttr::MapAlias> out_pcm_buffer) {
    // The decoding time is not measured
    *out_time_taken = 0;
    return decodeInterleavedImpl(in_opus_buffer.stream, out_decoded_data_size,
                                 out_decoded_sample_count,
                                 out_pcm_buffer.stream);
}

result_t IHardwareOpusDecoder::decodeInterleaved(
    bool reset_context, InBuffer<BufferAttr::MapAlias> in_opus_buffer,
    i32* out_decoded_data_size, i32* out_decoded_sample_count,
    u64* out_time_taken, OutBuffer<BufferAttr::MapAlias> out_pcm_buffer) {
#ifdef HYDRA_OPUS_ENABLED
    if (reset_context && decoder)
        opus_decoder_ctl(static_cast<OpusDecoder*>(decoder), OPUS_RESET_STATE);
#else
    (void)reset_context;
#endif
    *out_time_taken = 0;
    return decodeInterleavedImpl(in_opus_buffer.stream, out_decoded_data_size,
                                 out_decoded_sample_count,
                                 out_pcm_buffer.stream);
}

result_t IHardwareOpusDecoder::decodeInterleavedImpl(
    std::optional<ztd::io::MemoryStream> in_opus_stream,
    i32* out_decoded_data_size, i32* out_decoded_sample_count,
    std::optional<ztd::io::MemoryStream> out_pcm_stream) {
    *out_decoded_data_size = 0;
    *out_decoded_sample_count = 0;

#ifdef HYDRA_OPUS_ENABLED
    // TODO: use the real Codec result codes
    if (!decoder || !in_opus_stream || !out_pcm_stream)
        return MAKE_RESULT(Codec, 2);

    const usize in_size = in_opus_stream->getSize();
    if (in_size < PACKET_HEADER_SIZE)
        return MAKE_RESULT(Codec, 2);

    const u8* in_ptr = in_opus_stream->getPtr();
    const u32 packet_size = readBigEndian(in_ptr);
    if (packet_size > in_size - PACKET_HEADER_SIZE)
        return MAKE_RESULT(Codec, 2);

    // The output holds interleaved signed 16-bit samples
    const usize frame_bytes = sizeof(i16) * static_cast<usize>(channel_count);
    const usize out_frames = out_pcm_stream->getSize() / frame_bytes;
    const i32 max_frames =
        static_cast<i32>(std::min<usize>(out_frames, MAX_FRAME_SAMPLES));

    const i32 sample_count = opus_decode(
        static_cast<OpusDecoder*>(decoder), in_ptr + PACKET_HEADER_SIZE,
        static_cast<opus_int32>(packet_size),
        reinterpret_cast<opus_int16*>(out_pcm_stream->getPtr()), max_frames, 0);
    if (sample_count < 0) {
        LOG_ERROR(Services, "Opus decode failed: {}",
                  opus_strerror(sample_count));
        return MAKE_RESULT(Codec, 2);
    }

    ONCE(LOG_INFO(Services,
                  "Decoded first Opus packet: {} bytes, {} samples per channel",
                  packet_size, sample_count));
    *out_decoded_data_size = static_cast<i32>(PACKET_HEADER_SIZE + packet_size);
    *out_decoded_sample_count = sample_count;
    return RESULT_SUCCESS;
#else
    (void)in_opus_stream;
    (void)out_pcm_stream;
    ONCE(LOG_FUNC_STUBBED(Services));

    // HACK: libopus is not available
    return RESULT_SUCCESS;
#endif
}

} // namespace hydra::horizon::services::codec
