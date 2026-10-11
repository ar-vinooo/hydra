#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace hydra::video {
namespace h264_detail {
class Bits {
  public:
    void put(std::uint32_t value, unsigned count = 1) {
        for (unsigned i = count; i > 0; --i) {
            if (position % 8 == 0)
                bytes.push_back(0);
            bytes.back() |= ((value >> (i - 1)) & 1) << (7 - position % 8);
            ++position;
        }
    }
    void ue(std::uint32_t value) {
        const auto code = value + 1;
        const auto width = static_cast<unsigned>(std::bit_width(code));
        put(0, width - 1);
        put(code, width);
    }
    void se(std::int32_t value) {
        ue(value <= 0 ? std::uint32_t(-value) * 2 : std::uint32_t(value) * 2 - 1);
    }
    void nal(std::vector<std::uint8_t>& output, std::uint8_t type) {
        put(1); // rbsp_stop_one_bit; the remaining bits are already zero.
        output.insert(output.end(), {0, 0, 0, 1, type});
        unsigned zeros = 0;
        for (const auto byte : bytes) {
            if (zeros >= 2 && byte <= 3) {
                output.push_back(3); // Annex B emulation prevention.
                zeros = 0;
            }
            output.push_back(byte);
            zeros = byte == 0 ? zeros + 1 : 0;
        }
    }
  private:
    std::vector<std::uint8_t> bytes;
    unsigned position = 0;
};
}

// NVDEC H.264 picture ABI, including the parameter block at 0x58 and scaling
// matrices at 0x1c0/0x220. Layout reference: video_core/host1x/codec_types.h in
// https://github.com/eden-emulator/mirror . No guest structs are dereferenced.
inline std::expected<std::vector<std::uint8_t>, std::string>
composeH264(std::span<const std::uint8_t> picture,
            std::span<const std::uint8_t> stream, bool headers) {
    if (picture.size() < 0x2fc || stream.empty() || stream.size() > 32 * 1024 * 1024)
        return std::unexpected("Invalid NVDEC H264 picture or stream size");
    auto word = [&](std::size_t offset) {
        return std::uint32_t(picture[offset]) | (std::uint32_t(picture[offset + 1]) << 8) |
               (std::uint32_t(picture[offset + 2]) << 16) | (std::uint32_t(picture[offset + 3]) << 24);
    };
    if (word(0x48) != stream.size())
        return std::unexpected("NVDEC H264 stream length mismatch");
    const auto width = word(0x64), height = word(0x68);
    const auto frame_only = word(0x60);
    const auto flags = std::uint64_t(word(0xb0)) | (std::uint64_t(word(0xb4)) << 32);
    auto field = [&](unsigned shift, unsigned bits) -> std::uint32_t {
        return static_cast<std::uint32_t>((flags >> shift) & ((std::uint64_t(1) << bits) - 1));
    };
    auto signed_field = [&](unsigned shift, unsigned bits) -> std::int32_t {
        const auto value = field(shift, bits);
        return (value & (1u << (bits - 1))) ? std::int32_t(value) - (1 << bits) : std::int32_t(value);
    };
    const auto order = field(14, 2);
    if (!width || width > 256 || !height || height > 256 || frame_only > 1 ||
        (!frame_only && height % 2) || order > 2 || word(0x58) > 12 ||
        word(0x78) > 15 || word(0x7c) > 15 || field(34, 7) > 16)
        return std::unexpected("Invalid NVDEC H264 parameters");
    std::vector<std::uint8_t> result;
    if (headers) {
        // A zero scale signals the default list in the bitstream, rather than
        // an ordinary coefficient. The hardware context holds expanded lists.
        const auto end = word(0x88) ? 0x2a0u : 0x220u;
        if (std::find(picture.begin() + 0x1c0, picture.begin() + end, 0) != picture.begin() + end)
            return std::unexpected("Invalid zero H264 scaling coefficient");
        h264_detail::Bits sps;
        sps.put(100, 8); sps.put(0, 8); sps.put(51, 8); sps.ue(0);
        const auto chroma = field(12, 2);
        sps.ue(chroma);
        if (chroma == 3) sps.put(0);
        sps.ue(0); sps.ue(0); // 8-bit luma/chroma.
        sps.put((word(0x2d0) >> 1) & 1); sps.put(0);
        sps.ue(field(8, 4)); sps.ue(order);
        if (order == 0) sps.ue(word(0x58));
        else if (order == 1) { sps.put(word(0x5c) != 0); sps.se(0); sps.se(0); sps.ue(0); }
        sps.ue(std::max(4u, std::max(word(0x78), word(0x7c)) + 1));
        sps.put(0); sps.ue(width - 1); sps.ue(height / (frame_only ? 1 : 2) - 1);
        sps.put(frame_only);
        if (!frame_only) sps.put(field(0, 1));
        sps.put(field(1, 1)); sps.put(0); sps.put(0); // No cropping or VUI in the hardware context.
        sps.nal(result, 0x67);

        h264_detail::Bits pps;
        pps.ue(0); pps.ue(0); pps.put(word(0x70) != 0); pps.put(word(0x74) != 0);
        pps.ue(0); pps.ue(word(0x78)); pps.ue(word(0x7c));
        pps.put(field(2, 1)); pps.put(field(32, 2), 2);
        pps.se(signed_field(16, 6)); pps.se(0); pps.se(signed_field(22, 5));
        pps.put(word(0x80) != 0); pps.put(field(3, 1)); pps.put(word(0x84) != 0);
        const auto transform = word(0x88) != 0;
        pps.put(transform); pps.put(1);
        auto scaling = [&](std::size_t offset, unsigned dimension) {
            pps.put(1);
            // Build the H.264 diagonal scan for either matrix dimension.
            int last = 8;
            for (unsigned diagonal = 0; diagonal < dimension * 2 - 1; ++diagonal) {
                for (unsigned step = 0; step <= diagonal; ++step) {
                    const auto row = diagonal % 2 == 0 ? diagonal - step : step;
                    const auto column = diagonal - row;
                    if (row >= dimension || column >= dimension) continue;
                    const auto next = picture[offset + row * dimension + column];
                    pps.se(int(next) - last);
                    last = next;
                }
            }
        };
        for (unsigned i = 0; i < 6; ++i) scaling(0x1c0 + i * 16, 4);
        if (transform)
            for (unsigned i = 0; i < (chroma == 3 ? 6u : 2u); ++i) {
                if (i >= 2) return std::unexpected("NVDEC 4:4:4 scaling matrices are unavailable");
                scaling(0x220 + i * 64, 8);
            }
        pps.se(signed_field(27, 5)); pps.nal(result, 0x68);
    }
    result.insert(result.end(), stream.begin(), stream.end());
    return result;
}
} // namespace hydra::video
