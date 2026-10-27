#pragma once

#include "core/horizon/services/const.hpp"

namespace hydra::horizon::services::codec {

class IHardwareOpusDecoder : public IService {
  public:
    IHardwareOpusDecoder(i32 sample_rate_, i32 channel_count_);
    ~IHardwareOpusDecoder() override;

  protected:
    result_t requestImpl([[maybe_unused]] RequestContext& context,
                         u32 id) override;

  private:
    // Commands
    result_t
    decodeInterleavedOld(InBuffer<BufferAttr::MapAlias> in_opus_buffer,
                         i32* out_decoded_data_size,
                         i32* out_decoded_sample_count,
                         OutBuffer<BufferAttr::MapAlias> out_pcm_buffer);
    result_t decodeInterleavedWithPerfOld(
        InBuffer<BufferAttr::MapAlias> in_opus_buffer,
        i32* out_decoded_data_size, i32* out_decoded_sample_count,
        u64* out_time_taken, OutBuffer<BufferAttr::MapAlias> out_pcm_buffer);
    result_t decodeInterleaved(bool reset_context,
                               InBuffer<BufferAttr::MapAlias> in_opus_buffer,
                               i32* out_decoded_data_size,
                               i32* out_decoded_sample_count,
                               u64* out_time_taken,
                               OutBuffer<BufferAttr::MapAlias> out_pcm_buffer);

    // Impl
    result_t
    decodeInterleavedImpl(std::optional<ztd::io::MemoryStream> in_opus_stream,
                          i32* out_decoded_data_size,
                          i32* out_decoded_sample_count,
                          std::optional<ztd::io::MemoryStream> out_pcm_stream);

    i32 sample_rate;
    i32 channel_count;
    // OpusDecoder, null if the decoder could not be created
    void* decoder{nullptr};
};

} // namespace hydra::horizon::services::codec
