#pragma once

#include "core/audio/stream.hpp"

namespace hydra::audio {

// After starvation, the client head identifies the oldest queued wave.
// Preserve an in-flight wave; a delayed client snapshot must not rewind it.
inline u32 resumeWaveHead(std::span<const bool, 4> valid, u32 current,
                          i16 head) {
    if (current >= 4)
        current = 0;
    if (!valid[current] && head >= 0 && head < 4 &&
        valid[static_cast<u32>(head)])
        return static_cast<u32>(head);
    return current;
}

// Own submitted samples until the audio callback consumes them. Producers may
// immediately reuse their mix buffer after enqueue() returns.
class PcmQueue {
  public:
    void enqueue(buffer_id_t id, std::span<const u8> samples) {
        std::lock_guard lock(mutex);
        buffers.push({id, std::vector<u8>(samples.begin(), samples.end())});
    }

    std::vector<buffer_id_t> read(std::span<u8> output,
                                  usize* bytes_read = nullptr) {
        std::vector<buffer_id_t> finished;
        std::fill(output.begin(), output.end(), 0);
        std::lock_guard lock(mutex);
        usize written = 0;
        while (!buffers.empty() && written < output.size()) {
            auto& buffer = buffers.front();
            const auto count = std::min(output.size() - written,
                                        buffer.samples.size() - offset);
            if (count)
                std::memcpy(output.data() + written,
                            buffer.samples.data() + offset, count);
            offset += count;
            written += count;
            if (offset == buffer.samples.size()) {
                finished.push_back(buffer.id);
                buffers.pop();
                offset = 0;
            }
        }
        if (bytes_read)
            *bytes_read = written;
        return finished;
    }

  private:
    struct Buffer {
        buffer_id_t id;
        std::vector<u8> samples;
    };
    std::mutex mutex;
    std::queue<Buffer> buffers;
    usize offset{0};
};

} // namespace hydra::audio
