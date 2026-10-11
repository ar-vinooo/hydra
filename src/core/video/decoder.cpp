#include "core/video/decoder.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#ifdef __APPLE__
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#endif

namespace hydra::video {
namespace {
constexpr int MAX_DIMENSION = 4096;
constexpr std::size_t MAX_PACKET_BYTES = 32 * 1024 * 1024;

std::string errorMessage(const char* operation, int result) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(result, text.data(), text.size());
    return std::string(operation) + ": " + text.data();
}

#ifdef __APPLE__
// H.264 through VideoToolbox. NVDEC writes every picture to its surface when
// it is submitted, in decode order, and the guest reorders for display.
// FFmpeg's H.264 decoder always reorders to display order and holds pictures
// back, so the last submitted surface would stay empty. VideoToolbox emits
// pictures in decode order unless temporal processing is requested.
class VideoToolboxH264 {
  public:
    ~VideoToolboxH264() { destroySession(); }

    std::expected<std::vector<Frame>, std::string>
    decode(std::span<const std::uint8_t> packet, std::int64_t timestamp) {
        // Split the Annex B stream into NAL units
        std::vector<std::span<const std::uint8_t>> units;
        std::size_t begin = std::string::npos;
        for (std::size_t i = 0; i + 2 < packet.size(); ++i) {
            if (packet[i] != 0 || packet[i + 1] != 0 || packet[i + 2] != 1)
                continue;
            if (begin != std::string::npos)
                units.push_back(trimmed(packet.subspan(begin, i - begin)));
            begin = i + 3;
            i += 2;
        }
        if (begin != std::string::npos)
            units.push_back(trimmed(packet.subspan(begin)));
        if (units.empty())
            return std::unexpected("H264 packet has no NAL units");

        // Parameter sets configure the session; the rest becomes one AVCC
        // sample with 4-byte big endian lengths
        std::vector<std::uint8_t> sample;
        std::vector<std::uint8_t> new_sps, new_pps;
        for (const auto unit : units) {
            if (unit.empty())
                continue;
            switch (unit[0] & 0x1f) {
            case 7:
                new_sps.assign(unit.begin(), unit.end());
                break;
            case 8:
                new_pps.assign(unit.begin(), unit.end());
                break;
            case 9: // Access unit delimiter
                break;
            default: {
                const auto size = static_cast<std::uint32_t>(unit.size());
                sample.insert(sample.end(),
                              {static_cast<std::uint8_t>(size >> 24),
                               static_cast<std::uint8_t>(size >> 16),
                               static_cast<std::uint8_t>(size >> 8),
                               static_cast<std::uint8_t>(size)});
                sample.insert(sample.end(), unit.begin(), unit.end());
                break;
            }
            }
        }
        if ((!new_sps.empty() && new_sps != sps) ||
            (!new_pps.empty() && new_pps != pps)) {
            if (!new_sps.empty())
                sps = std::move(new_sps);
            if (!new_pps.empty())
                pps = std::move(new_pps);
            destroySession();
        }
        if (sample.empty())
            return std::vector<Frame>{};
        if (session == nullptr) {
            if (auto created = createSession(); !created)
                return std::unexpected(created.error());
        }

        CMBlockBufferRef block = nullptr;
        auto status = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, nullptr, sample.size(), kCFAllocatorDefault,
            nullptr, 0, sample.size(), kCMBlockBufferAssureMemoryNowFlag,
            &block);
        if (status != noErr)
            return std::unexpected(osError("Allocate video sample", status));
        status = CMBlockBufferReplaceDataBytes(sample.data(), block, 0,
                                               sample.size());
        CMSampleBufferRef sample_buffer = nullptr;
        const CMSampleTimingInfo timing{
            .duration = kCMTimeInvalid,
            .presentationTimeStamp = CMTimeMake(timestamp, 1),
            .decodeTimeStamp = kCMTimeInvalid};
        const std::size_t sample_size = sample.size();
        if (status == noErr)
            status = CMSampleBufferCreateReady(kCFAllocatorDefault, block,
                                               format, 1, 1, &timing, 1,
                                               &sample_size, &sample_buffer);
        CFRelease(block);
        if (status != noErr)
            return std::unexpected(osError("Create video sample", status));

        std::vector<Frame> frames;
        output = &frames;
        output_error.clear();
        VTDecodeInfoFlags info = 0;
        // No flags: synchronous decode, pictures in decode order
        status = VTDecompressionSessionDecodeFrame(session, sample_buffer, 0,
                                                   nullptr, &info);
        VTDecompressionSessionWaitForAsynchronousFrames(session);
        output = nullptr;
        CFRelease(sample_buffer);
        if (status != noErr)
            return std::unexpected(osError("Decode video frame", status));
        if (!output_error.empty())
            return std::unexpected(output_error);
        return frames;
    }

    std::expected<std::vector<Frame>, std::string> flush() {
        // Pictures are never held back
        return std::vector<Frame>{};
    }

    void reset() {
        destroySession();
        sps.clear();
        pps.clear();
    }

  private:
    VTDecompressionSessionRef session = nullptr;
    CMVideoFormatDescriptionRef format = nullptr;
    std::vector<std::uint8_t> sps, pps;
    std::vector<Frame>* output = nullptr;
    std::string output_error;

    static std::span<const std::uint8_t>
    trimmed(std::span<const std::uint8_t> unit) {
        // Drop trailing zeros, including the first byte of a 4-byte start code
        while (!unit.empty() && unit.back() == 0)
            unit = unit.first(unit.size() - 1);
        return unit;
    }

    static std::string osError(const char* operation, OSStatus status) {
        return std::string(operation) + ": OSStatus " + std::to_string(status);
    }

    std::expected<void, std::string> createSession() {
        if (sps.empty() || pps.empty())
            return std::unexpected("H264 picture before parameter sets");
        const std::array<const std::uint8_t*, 2> sets{sps.data(), pps.data()};
        const std::array<std::size_t, 2> sizes{sps.size(), pps.size()};
        auto status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
            kCFAllocatorDefault, sets.size(), sets.data(), sizes.data(), 4,
            &format);
        if (status != noErr)
            return std::unexpected(osError("H264 parameter sets", status));

        const SInt32 pixel_format =
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        CFNumberRef pixel_format_number =
            CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                           &pixel_format);
        const void* keys[] = {kCVPixelBufferPixelFormatTypeKey};
        const void* values[] = {pixel_format_number};
        CFDictionaryRef attributes = CFDictionaryCreate(
            kCFAllocatorDefault, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFRelease(pixel_format_number);

        const VTDecompressionOutputCallbackRecord callback{
            .decompressionOutputCallback = &VideoToolboxH264::onOutput,
            .decompressionOutputRefCon = this};
        status = VTDecompressionSessionCreate(kCFAllocatorDefault, format,
                                              nullptr, attributes, &callback,
                                              &session);
        CFRelease(attributes);
        if (status != noErr) {
            destroySession();
            return std::unexpected(osError("Create video decoder", status));
        }
        return {};
    }

    void destroySession() {
        if (session != nullptr) {
            VTDecompressionSessionWaitForAsynchronousFrames(session);
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }
        if (format != nullptr) {
            CFRelease(format);
            format = nullptr;
        }
    }

    static void onOutput(void* context, void*, OSStatus status,
                         VTDecodeInfoFlags, CVImageBufferRef image,
                         CMTime timestamp, CMTime) {
        auto* self = static_cast<VideoToolboxH264*>(context);
        if (status != noErr) {
            self->output_error = osError("Decode video frame", status);
            return;
        }
        if (image == nullptr || self->output == nullptr)
            return;

        CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        const auto width = CVPixelBufferGetWidthOfPlane(image, 0);
        const auto height = CVPixelBufferGetHeightOfPlane(image, 0);
        if (width == 0 || height == 0 || width > MAX_DIMENSION ||
            height > MAX_DIMENSION ||
            CVPixelBufferGetPlaneCount(image) != 2) {
            CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
            self->output_error = "Decoded frame layout is not NV12";
            return;
        }
        Frame frame{.width = static_cast<int>(width),
                    .height = static_cast<int>(height),
                    .format = PixelFormat::Nv12};
        frame.timestamp = timestamp.value;
        frame.strides[0] = static_cast<int>(width);
        frame.strides[1] = static_cast<int>(width);
        frame.offsets[1] = width * height;
        frame.pixels.resize(width * height + width * ((height + 1) / 2));
        for (std::size_t plane = 0; plane < 2; ++plane) {
            const auto* source = static_cast<const std::uint8_t*>(
                CVPixelBufferGetBaseAddressOfPlane(image, plane));
            const auto source_stride =
                CVPixelBufferGetBytesPerRowOfPlane(image, plane);
            const auto rows = CVPixelBufferGetHeightOfPlane(image, plane);
            for (std::size_t row = 0; row < rows; ++row)
                std::memcpy(frame.pixels.data() + frame.offsets[plane] +
                                row * width,
                            source + row * source_stride, width);
        }
        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        self->output->push_back(std::move(frame));
    }
};
#endif
} // namespace

struct Decoder::Impl {
#ifdef __APPLE__
    std::unique_ptr<VideoToolboxH264> video_toolbox;
#endif
    AVCodecContext* context{};
    AVFrame* frame{};
    AVPacket* packet{};
    SwsContext* scaler{};
    PixelFormat output;

    explicit Impl(PixelFormat output_) : output(output_) {}
    ~Impl() {
        sws_freeContext(scaler);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }

    std::expected<Frame, std::string> copyFrame() {
        const auto width = frame->width;
        const auto height = frame->height;
        if (width <= 0 || height <= 0 || width > MAX_DIMENSION ||
            height > MAX_DIMENSION)
            return std::unexpected("Decoded frame dimensions exceed video limits");
        const auto format = output == PixelFormat::Nv12 ? AV_PIX_FMT_NV12
                                                        : AV_PIX_FMT_RGBA;
        const auto size = av_image_get_buffer_size(format, width, height, 1);
        if (size < 0)
            return std::unexpected(errorMessage("Frame size", size));
        Frame result{.width = width, .height = height, .format = output};
        result.timestamp = frame->pts;
        result.pixels.resize(static_cast<std::size_t>(size));
        std::array<std::uint8_t*, 4> planes{};
        std::array<int, 4> strides{};
        const auto fill = av_image_fill_arrays(planes.data(), strides.data(),
                                               result.pixels.data(), format,
                                               width, height, 1);
        if (fill < 0)
            return std::unexpected(errorMessage("Frame layout", fill));
        scaler = sws_getCachedContext(scaler, width, height,
                                     static_cast<AVPixelFormat>(frame->format),
                                     width, height, format, SWS_POINT, nullptr,
                                     nullptr, nullptr);
        if (!scaler)
            return std::unexpected("Cannot create video colour converter");
        // Honour full/limited range and the stream's matrix when converting
        // decoded YUV to RGB. Default to BT.601 when metadata is absent.
        int matrix = SWS_CS_DEFAULT;
        if (frame->colorspace == AVCOL_SPC_BT709)
            matrix = SWS_CS_ITU709;
        else if (frame->colorspace == AVCOL_SPC_BT2020_NCL ||
                 frame->colorspace == AVCOL_SPC_BT2020_CL)
            matrix = SWS_CS_BT2020;
        const int* coefficients = sws_getCoefficients(matrix);
        const int source_full_range = frame->color_range == AVCOL_RANGE_JPEG;
        const int destination_full_range = output == PixelFormat::Rgba8
                                               ? 1 : source_full_range;
        const auto colour = sws_setColorspaceDetails(
            scaler, coefficients, source_full_range, coefficients,
            destination_full_range, 0, 1 << 16, 1 << 16);
        if (colour < 0)
            return std::unexpected(errorMessage("Video colour metadata", colour));
        const auto rows = sws_scale(scaler, frame->data, frame->linesize, 0,
                                   height, planes.data(), strides.data());
        if (rows != height)
            return std::unexpected("Incomplete video colour conversion");
        result.strides[0] = strides[0];
        if (output == PixelFormat::Nv12) {
            result.strides[1] = strides[1];
            result.offsets[1] = static_cast<std::size_t>(planes[1] - planes[0]);
        }
        return result;
    }

    std::expected<void, std::string> receive(std::vector<Frame>& frames) {
        std::size_t output_bytes = 0;
        for (const auto& output_frame : frames)
            output_bytes += output_frame.pixels.size();
        for (;;) {
            const auto status = avcodec_receive_frame(context, frame);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
                return {};
            if (status < 0)
                return std::unexpected(errorMessage("Decode frame", status));
            auto copied = copyFrame();
            av_frame_unref(frame);
            if (!copied)
                return std::unexpected(copied.error());
            if (frames.size() >= 64 ||
                copied->pixels.size() > 128 * 1024 * 1024 - output_bytes)
                return std::unexpected("Decoded packet exceeds output limits");
            output_bytes += copied->pixels.size();
            frames.push_back(std::move(*copied));
        }
    }

    std::expected<std::vector<Frame>, std::string> send(AVPacket* input) {
        std::vector<Frame> frames;
        auto status = avcodec_send_packet(context, input);
        if (status == AVERROR(EAGAIN)) {
            if (auto received = receive(frames); !received)
                return std::unexpected(received.error());
            status = avcodec_send_packet(context, input);
        }
        if (status < 0)
            return std::unexpected(errorMessage("Submit video packet", status));
        if (auto received = receive(frames); !received)
            return std::unexpected(received.error());
        return frames;
    }
};

Decoder::Decoder(std::unique_ptr<Impl> impl_) : impl(std::move(impl_)) {}
Decoder::~Decoder() = default;

std::expected<std::unique_ptr<Decoder>, std::string>
Decoder::create(Codec codec, PixelFormat output) {
    AVCodecID id;
    switch (codec) {
    case Codec::H264: id = AV_CODEC_ID_H264; break;
    case Codec::VP9: id = AV_CODEC_ID_VP9; break;
    default: return std::unexpected("Unsupported video codec");
    }
    if (output != PixelFormat::Nv12 && output != PixelFormat::Rgba8)
        return std::unexpected("Unsupported video output format");
#ifdef __APPLE__
    if (codec == Codec::H264 && output == PixelFormat::Nv12) {
        auto state = std::make_unique<Impl>(output);
        state->video_toolbox = std::make_unique<VideoToolboxH264>();
        return std::unique_ptr<Decoder>(new Decoder(std::move(state)));
    }
#endif
    const auto* implementation = avcodec_find_decoder(id);
    if (!implementation)
        return std::unexpected("FFmpeg does not provide the requested decoder");
    auto state = std::make_unique<Impl>(output);
    state->context = avcodec_alloc_context3(implementation);
    state->frame = av_frame_alloc();
    state->packet = av_packet_alloc();
    if (!state->context || !state->frame || !state->packet)
        return std::unexpected("Cannot allocate video decoder");
    state->context->thread_count = 1;
    state->context->max_pixels = MAX_DIMENSION * MAX_DIMENSION;
    const auto status = avcodec_open2(state->context, implementation, nullptr);
    if (status < 0)
        return std::unexpected(errorMessage("Open video decoder", status));
    return std::unique_ptr<Decoder>(new Decoder(std::move(state)));
}

std::expected<std::vector<Frame>, std::string>
Decoder::decode(std::span<const std::uint8_t> packet, std::int64_t timestamp) {
    if (packet.empty() || packet.size() > MAX_PACKET_BYTES ||
        packet.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::unexpected("Invalid video packet size");
#ifdef __APPLE__
    if (impl->video_toolbox)
        return impl->video_toolbox->decode(packet, timestamp);
#endif
    av_packet_unref(impl->packet);
    // av_new_packet supplies FFmpeg's required zero-filled input padding.
    const auto status = av_new_packet(impl->packet, static_cast<int>(packet.size()));
    if (status < 0)
        return std::unexpected(errorMessage("Allocate video packet", status));
    std::memcpy(impl->packet->data, packet.data(), packet.size());
    impl->packet->pts = timestamp;
    auto decoded = impl->send(impl->packet);
    av_packet_unref(impl->packet);
    return decoded;
}

std::expected<std::vector<Frame>, std::string> Decoder::flush() {
#ifdef __APPLE__
    if (impl->video_toolbox)
        return impl->video_toolbox->flush();
#endif
    return impl->send(nullptr);
}

void Decoder::reset() {
#ifdef __APPLE__
    if (impl->video_toolbox) {
        impl->video_toolbox->reset();
        return;
    }
#endif
    avcodec_flush_buffers(impl->context);
    av_frame_unref(impl->frame);
    av_packet_unref(impl->packet);
}

} // namespace hydra::video
