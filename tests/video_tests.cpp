#include "core/video/decoder.hpp"
#include "core/video/submit.hpp"
#include "core/video/h264.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace hydra::video;

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void testSubmissions() {
    std::vector<std::uint8_t> bytes;
    auto word = [&](std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    };
    word(1); word(0); word(1); word(1); // Count header
    word(0x31); word(0x100); word(7);  // One command buffer
    word(2); word(1); word(0); word(0); word(0); // Syncpoint increment
    word(10); // Fence threshold
    auto parsed = parseSubmission(bytes);
    check(parsed.has_value() && parsed->commands.size() == 1 &&
              parsed->commands[0].handle == 0x31 &&
              parsed->commands[0].offset == 0x100 &&
              parsed->commands[0].word_count == 7 &&
              parsed->syncpoint_count == 1 && parsed->fence_count == 1,
          "Host1x count header and command descriptors decode");
    bytes.pop_back();
    check(!parseSubmission(bytes), "Truncated Host1x fence array is rejected");
    bytes.resize(16);
    bytes[0] = 0xff; bytes[1] = 0xff; bytes[2] = 0xff; bytes[3] = 0xff;
    check(!parseSubmission(bytes), "Host1x count overflow is rejected");
    check(!parseSubmission({}), "Empty Host1x submission is rejected");
}

std::vector<std::vector<std::uint8_t>> loadPackets(const char* name) {
    std::ifstream input(std::filesystem::path(HYDRA_VIDEO_FIXTURE_DIR) / name,
                        std::ios::binary);
    check(input.good(), "Video fixture exists");
    std::array<char, 4> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    check(std::string_view(magic.data(), magic.size()) == "HYVP", "Fixture magic");
    auto readWord = [&]() {
        std::array<unsigned char, 4> bytes{};
        input.read(reinterpret_cast<char*>(bytes.data()), 4);
        check(input.good(), "Fixture word is complete");
        return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
               (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
    };
    const auto count = readWord();
    check(count <= 64, "Fixture packet count is bounded");
    std::vector<std::vector<std::uint8_t>> packets;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto size = readWord();
        check(size < 1024 * 1024, "Fixture packet is bounded");
        auto& packet = packets.emplace_back(size);
        input.read(reinterpret_cast<char*>(packet.data()),
                   static_cast<std::streamsize>(packet.size()));
        check(input.good(), "Fixture packet is complete");
    }
    return packets;
}

void verifyFrame(const Frame& frame, PixelFormat format) {
    check(frame.width == 32 && frame.height == 16, "Decoded frame dimensions");
    check(frame.format == format, "Requested output format");
    if (format == PixelFormat::Rgba8) {
        check(frame.strides[0] == 128 && frame.pixels.size() == 2048, "RGBA layout");
        for (std::size_t i = 0; i < frame.pixels.size(); i += 4)
            check(frame.pixels[i] > 245 && frame.pixels[i + 1] < 8 &&
                      frame.pixels[i + 2] < 8 && frame.pixels[i + 3] == 255,
                  "Decoded red colour and opaque alpha");
    } else {
        check(frame.strides[0] == 32 && frame.strides[1] == 32 &&
                  frame.offsets[1] == 512 && frame.pixels.size() == 768,
              "NV12 plane offsets and row strides");
        check(frame.pixels[0] >= 79 && frame.pixels[0] <= 83 &&
                  frame.pixels[512] >= 88 && frame.pixels[512] <= 92 &&
                  frame.pixels[513] >= 238 && frame.pixels[513] <= 242,
              "NV12 red luma and chroma");
    }
}

void testCodec(Codec codec, const char* fixture, PixelFormat output) {
    auto created = Decoder::create(codec, output);
    if (!created)
        throw std::runtime_error(created.error());
    auto& decoder = **created;
    check(!decoder.decode({}), "Empty packet is rejected without draining");
    const auto packets = loadPackets(fixture);
    // Repeat after flush/reset to cover delayed H.264 output and stream reuse.
    for (int iteration = 0; iteration < 2; ++iteration) {
        std::vector<Frame> frames;
        std::int64_t timestamp = 0;
        for (const auto& packet : packets) {
            const auto submitted = timestamp++;
            auto decoded = decoder.decode(packet, submitted);
            if (!decoded)
                throw std::runtime_error(decoded.error());
#ifdef __APPLE__
            // NVDEC semantics: each H.264 picture is written when submitted
            if (codec == Codec::H264 && output == PixelFormat::Nv12)
                check(decoded->size() == 1 &&
                          decoded->front().timestamp == submitted,
                      "H264 NV12 pictures are output in decode order");
#endif
            for (auto& frame : *decoded)
                frames.push_back(std::move(frame));
        }
        auto delayed = decoder.flush();
        if (!delayed)
            throw std::runtime_error(delayed.error());
        for (auto& frame : *delayed)
            frames.push_back(std::move(frame));
        check(frames.size() == 3, "All frames survive decode and flush");
        std::vector<std::int64_t> timestamps;
        for (const auto& frame : frames) {
            verifyFrame(frame, output);
            timestamps.push_back(frame.timestamp);
        }
        std::sort(timestamps.begin(), timestamps.end());
        check(timestamps == std::vector<std::int64_t>{0, 1, 2},
              "Decoded and delayed frames retain their submitted surface identity");
        // Owned pixels survive reuse and destruction of FFmpeg's frame buffers.
        decoder.reset();
        verifyFrame(frames.front(), output);
    }
    std::vector<std::uint8_t> oversized(32 * 1024 * 1024 + 1);
    check(!decoder.decode(oversized), "Oversized video packet is rejected");
    const std::array<std::uint8_t, 16> invalid{};
    check(!decoder.decode(invalid), "Invalid bitstream reports an error");
}

void testNvdecH264Headers() {
    std::array<std::uint8_t, 0x2fc> picture{};
    auto word = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) picture[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    };
    word(0x60, 1); word(0x64, 2); word(0x68, 1);
    word(0x70, 1); word(0x78, 2); word(0x80, 1); word(0x88, 1);
    // Matches the independently encoded fixture's SPS/PPS, as inspected with
    // FFmpeg trace_headers. Signed fields are stored in hardware bit widths.
    const std::uint64_t flags = 2 | 4 | (1ull << 12) | (56ull << 16) |
        (30ull << 22) | (30ull << 27) | (2ull << 32);
    word(0xb0, static_cast<std::uint32_t>(flags)); word(0xb4, static_cast<std::uint32_t>(flags >> 32));
    std::fill(picture.begin() + 0x1c0, picture.begin() + 0x2a0, 16);
    auto decoder = Decoder::create(Codec::H264);
    check(decoder.has_value(), "NVDEC fixture decoder opens");
    std::vector<Frame> frames;
    bool first = true;
    for (const auto& packet : loadPackets("h264.packets")) {
        std::vector<std::pair<std::size_t, std::size_t>> starts;
        for (std::size_t i = 0; i + 3 < packet.size();) {
            std::size_t prefix = 0;
            if (packet[i] == 0 && packet[i + 1] == 0) {
                if (packet[i + 2] == 1) prefix = 3;
                else if (packet[i + 2] == 0 && packet[i + 3] == 1) prefix = 4;
            }
            if (prefix) { starts.emplace_back(i, prefix); i += prefix; }
            else ++i;
        }
        std::vector<std::uint8_t> slices;
        for (std::size_t i = 0; i < starts.size(); ++i) {
            const auto [offset, prefix] = starts[i];
            const auto type = packet[offset + prefix] & 31;
            const auto end = i + 1 < starts.size() ? starts[i + 1].first : packet.size();
            if (type != 7 && type != 8)
                slices.insert(slices.end(), packet.data() + offset, packet.data() + end);
        }
        word(0x48, static_cast<std::uint32_t>(slices.size()));
        auto composed = composeH264(picture, slices, first);
        check(composed.has_value(), "NVDEC picture parameters reconstruct H264 SPS/PPS");
        first = false;
        auto decoded = (*decoder)->decode(*composed);
        if (!decoded) throw std::runtime_error(decoded.error());
        for (auto& frame : *decoded) frames.push_back(std::move(frame));
    }
    auto delayed = (*decoder)->flush();
    check(delayed.has_value(), "NVDEC reconstructed stream drains");
    for (auto& frame : *delayed) frames.push_back(std::move(frame));
    check(frames.size() == 3, "All hardware-context reconstructed H264 frames decode");
    for (const auto& frame : frames) verifyFrame(frame, PixelFormat::Nv12);
    word(0x64, 0);
    const std::array<std::uint8_t, 1> stream{1};
    word(0x48, 1);
    check(!composeH264(picture, stream, true), "Invalid NVDEC dimensions are rejected");
    check(!composeH264({}, stream, true), "Truncated NVDEC context is rejected");
}
} // namespace

int main() {
    try {
        testSubmissions();
        testNvdecH264Headers();
        testCodec(Codec::H264, "h264.packets", PixelFormat::Nv12);
        testCodec(Codec::H264, "h264.packets", PixelFormat::Rgba8);
        testCodec(Codec::VP9, "vp9.packets", PixelFormat::Nv12);
        testCodec(Codec::VP9, "vp9.packets", PixelFormat::Rgba8);
        std::cout << "PASS: H264/VP9, NV12/RGBA pixels, delayed frames, reset, invalid packets\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
