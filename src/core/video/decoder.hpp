#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace hydra::video {

enum class Codec { H264, VP9 };
enum class PixelFormat { Nv12, Rgba8 };

struct Frame {
    int width;
    int height;
    PixelFormat format;
    // RGBA: stride[0] bytes per row. NV12: Y followed by interleaved UV,
    // with the second plane beginning at offsets[1]. Owns its pixel storage.
    int strides[2]{};
    std::size_t offsets[2]{};
    std::vector<std::uint8_t> pixels;
    std::int64_t timestamp = -1;
};

// Accepts complete elementary-stream packets, not MP4/IVF containers.
// NVDEC must reconstruct codec headers and retain the packet's surface identity.
// One decoder belongs to one guest channel and is used by one worker at a time.
class Decoder {
  public:
    static std::expected<std::unique_ptr<Decoder>, std::string>
    create(Codec codec, PixelFormat output = PixelFormat::Nv12);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    std::expected<std::vector<Frame>, std::string>
    decode(std::span<const std::uint8_t> packet, std::int64_t timestamp = -1);
    // Drains delayed frames. Call reset before submitting a new stream.
    std::expected<std::vector<Frame>, std::string> flush();
    void reset();

  private:
    struct Impl;
    explicit Decoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl;
};

} // namespace hydra::video
