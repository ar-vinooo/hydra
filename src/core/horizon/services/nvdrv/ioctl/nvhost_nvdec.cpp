#include "core/horizon/services/nvdrv/ioctl/nvhost_nvdec.hpp"

#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/system.hpp"
#ifdef HYDRA_FFMPEG_VIDEO
#include "core/video/decoder.hpp"
#include "core/video/h264.hpp"
#endif

namespace hydra::horizon::services::nvdrv::ioctl {
struct NvHostNvDec::Impl {
#ifdef HYDRA_FFMPEG_VIDEO
    struct Surface {
        u64 luma, chroma;
        u32 pitches[2];
        u32 width, height, gob_height;
        bool linear;
    };
    std::unique_ptr<video::Decoder> decoder;
    std::map<i64, Surface> pending;
    i64 next_timestamp = 0;
#endif
};
NvHostNvDec::NvHostNvDec() : impl(std::make_unique<Impl>()) {}
NvHostNvDec::~NvHostNvDec() = default;

NvResult NvHostNvDec::executeVideo([[maybe_unused]] IoctlContext& context) {
#ifndef HYDRA_FFMPEG_VIDEO
    ONCE(LOG_WARN(Services, "NVDEC cannot decode video: FFmpeg backend is disabled"));
    return NvResult::NotSupported;
#else
    if (video_registers[0x80] != 3) {
        LOG_WARN(Services, "NVDEC codec {} is not connected to its guest context yet", video_registers[0x80]);
        return NvResult::NotSupported;
    }
    auto& gpu = context.system.getGpu();
    auto& mmu = *context.process->getMmu();
    auto read = [&](u64 address, usize size) -> std::optional<std::vector<u8>> {
        const auto guest = gpu.resolveHost1x(address, size);
        if (!guest) return std::nullopt;
        std::vector<u8> result(size);
        usize copied = 0;
        while (copied < size) {
            const auto current = *guest + copied;
            const auto host = mmu.unmapAddr(current);
            if (!host) return std::nullopt;
            const auto amount = std::min(size - copied, usize(0x1000 - (current & 0xfff)));
            std::memcpy(result.data() + copied, reinterpret_cast<const void*>(host), amount);
            copied += amount;
        }
        return result;
    };
    const auto picture = read(u64(video_registers[0x101]) << 8, 0x2fc);
    if (!picture) return NvResult::InvalidAddress;
    auto word = [&](usize offset) {
        u32 value;
        std::memcpy(&value, picture->data() + offset, sizeof(value));
        return value;
    };
    const auto stream_size = word(0x48);
    if (!stream_size || stream_size > 32 * 1024 * 1024) return NvResult::InvalidSize;
    const auto stream = read(u64(video_registers[0x102]) << 8, stream_size);
    if (!stream) return NvResult::InvalidAddress;
    const u64 flags = u64(word(0xb0)) | (u64(word(0xb4)) << 32);
    const auto surface_index = u32((flags >> 34) & 0x7f);
    if (surface_index > 16 || impl->pending.size() >= 64) return NvResult::BadValue;
    // Interlaced surfaces need separate top/bottom field writeback.
    if (!word(0x60) || word(0x94) || word(0x98) || word(0xa0) || word(0xa4))
        return NvResult::NotSupported;
    const auto packet = video::composeH264(*picture, *stream,
        !impl->decoder || ((flags >> 46) & 0xffff) == 0);
    if (!packet) {
        LOG_WARN(Services, "NVDEC H264 context: {}", packet.error());
        return NvResult::BadValue;
    }
    if (!impl->decoder) {
        auto created = video::Decoder::create(video::Codec::H264);
        if (!created) {
            LOG_WARN(Services, "NVDEC decoder: {}", created.error());
            return NvResult::NotInitialized;
        }
        impl->decoder = std::move(*created);
    }
    Impl::Surface surface{
        .luma = (u64(video_registers[0x10c + surface_index]) << 8) + (u64(word(0x9c)) << 8),
        .chroma = (u64(video_registers[0x11d + surface_index]) << 8) + (u64(word(0xa8)) << 8),
        .pitches = {word(0x8c), word(0x90)},
        .width = word(0x64) * 16, .height = word(0x68) * 16,
        .gob_height = (word(0x6c) >> 2) & 7,
        .linear = (flags >> 63) != 0};
    if (surface.gob_height > 5 || surface.pitches[0] < surface.width ||
        surface.pitches[1] < surface.width || surface.pitches[0] > 16384 ||
        surface.pitches[1] > 16384) return NvResult::BadValue;
    const auto timestamp = impl->next_timestamp++;
    impl->pending.emplace(timestamp, surface);
    auto frames = impl->decoder->decode(*packet, timestamp);
    if (!frames) {
        impl->pending.erase(timestamp);
        LOG_WARN(Services, "NVDEC H264 decode: {}", frames.error());
        return NvResult::BadValue;
    }
    for (const auto& frame : *frames) {
        const auto found = impl->pending.find(frame.timestamp);
        if (found == impl->pending.end()) return NvResult::InvalidState;
        const auto& output = found->second;
        if (u32(frame.width) != output.width || u32(frame.height) != output.height)
            return NvResult::BadValue;
        std::array<std::vector<u8>, 2> planes;
        std::array<u64, 2> addresses{output.luma, output.chroma};
        std::array<u64, 2> guests;
        for (u32 plane = 0; plane < 2; ++plane) {
            const auto rows = output.height / (plane ? 2 : 1);
            const auto pitch = output.pitches[plane];
            const auto stride = output.linear ? pitch : ((pitch + 63) & ~63u);
            const auto padded_rows = output.linear ? rows :
                ((rows + (8u << output.gob_height) - 1) & ~((8u << output.gob_height) - 1));
            planes[plane].resize(usize(stride) * padded_rows);
            const auto guest = gpu.resolveHost1x(addresses[plane], planes[plane].size());
            if (!guest) return NvResult::InvalidAddress;
            guests[plane] = *guest;
            const auto* pixels = frame.pixels.data() + frame.offsets[plane];
            if (output.linear) {
                for (u32 row = 0; row < rows; ++row)
                    std::memcpy(planes[plane].data() + usize(row) * pitch,
                        pixels + usize(row) * u32(frame.strides[plane]), output.width);
            } else {
                hw::tegra_x1::gpu::convertLinearToBlockLinear(stride, rows, 1, output.gob_height, 0,
                    [&](u32 x_gob, u32 y_gob, u32, u8* gob) {
                        std::memset(gob, 0, 512);
                        const auto x = x_gob * 64;
                        if (x >= output.width) return;
                        for (u32 local_y = 0; local_y < 8; ++local_y) {
                            const auto y = y_gob * 8 + local_y;
                            if (y >= rows) break;
                            std::memcpy(gob + local_y * 64,
                                pixels + usize(y) * u32(frame.strides[plane]) + x,
                                std::min(64u, output.width - x));
                        }
                    }, planes[plane].data());
            }
            // Validate all guest pages before writing either plane.
            for (usize offset = 0; offset < planes[plane].size();) {
                const auto current = *guest + offset;
                if (!mmu.unmapAddr(current)) return NvResult::InvalidAddress;
                offset += std::min(planes[plane].size() - offset, usize(0x1000 - (current & 0xfff)));
            }
        }
        for (u32 plane = 0; plane < 2; ++plane) {
            mmu.trackWrite(ztd::Range<vaddr_t>::fromSize(guests[plane], planes[plane].size()));
            usize copied = 0;
            while (copied < planes[plane].size()) {
                const auto current = guests[plane] + copied;
                const auto amount = std::min(planes[plane].size() - copied, usize(0x1000 - (current & 0xfff)));
                std::memcpy(reinterpret_cast<void*>(mmu.unmapAddr(current)), planes[plane].data() + copied, amount);
                copied += amount;
            }
        }
        gpu.publishVideoFrame(output.luma, frame);
        LOG_INFO(Services, "NVDEC decoded {}x{} H264 frame {} to guest surfaces 0x{:x}/0x{:x}",
                 frame.width, frame.height, frame.timestamp, output.luma, output.chroma);
        impl->pending.erase(found);
    }
    return NvResult::Success;
#endif
}
} // namespace hydra::horizon::services::nvdrv::ioctl
