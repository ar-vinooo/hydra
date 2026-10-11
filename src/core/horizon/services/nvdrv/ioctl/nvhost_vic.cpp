#include "core/horizon/services/nvdrv/ioctl/nvhost_vic.hpp"

#include "core/horizon/kernel/process.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/system.hpp"

#include <atomic>

namespace hydra::horizon::services::nvdrv::ioctl {

NvResult NvHostVic::executeVideo(IoctlContext& context) {
    auto& gpu = context.system.getGpu();
    auto& mmu = *context.process->getMmu();
    const auto guest = gpu.resolveHost1x(u64(video_registers[0x1c2]) << 8, 0x610);
    if (!guest) return NvResult::InvalidAddress;
    std::array<u8, 0x610> config{};
    for (usize offset = 0; offset < config.size();) {
        const auto address = *guest + offset;
        const auto host = mmu.unmapAddr(address);
        if (!host) return NvResult::InvalidAddress;
        const auto count = std::min(config.size() - offset, usize(0x1000 - (address & 0xfff)));
        std::memcpy(config.data() + offset, reinterpret_cast<const void*>(host), count);
        offset += count;
    }
    auto word = [&](usize offset) {
        u32 value;
        std::memcpy(&value, config.data() + offset, 4);
        return value;
    };
    auto wide = [&](usize offset) { return u64(word(offset)) | (u64(word(offset + 4)) << 32); };
    auto unsupported = [&](const char* reason) {
        static std::atomic<u32> reports{0};
        if (reports.fetch_add(1) < 16)
            LOG_WARN(Services, "VIC unsupported {}: formats {}/{}, output {:08x}/{:08x}, slot {:016x}, rect {:08x}/{:08x}/{:08x}/{:08x}, destination {:08x}/{:08x}, target {:08x}/{:08x}, flags {:016x}/{:016x}/{:016x}/{:016x}",
                reason, word(0xd0) & 127, word(0x20) & 127,
                word(0x20), word(0x24), wide(0x90),
                word(0xb0), word(0xb4), word(0xb8), word(0xbc),
                word(0xc0), word(0xc4), word(0x18), word(0x1c),
                wide(0x10), wide(0x38), wide(0xf8), wide(0x118));
        return NvResult::NotSupported;
    };
    const auto output_format = word(0x20) & 127;
    const bool rgb = output_format == 31 || output_format == 32 || output_format == 35;
    // VIC config ABI: output at 0x20, eight 0xb0-byte slots at 0x90.
    // Initial implementation handles one progressive NVDEC picture, cropping
    // without scaling, and NV12/NV21 or packed RGB output in both layouts.
    if (!(wide(0x90) & 1)) return unsupported("disabled slot");
    for (usize slot = 1; slot < 8; ++slot)
        if (wide(0x90 + slot * 0xb0) & 1) return unsupported("multiple slots");
    if ((wide(0x90) & 0xfffffeu) != 0x100 ||
        (wide(0x10) & (7ull << 48)) ||
        (wide(0x38) >> 63) || (!rgb && (wide(0xf8) >> 63)) || (wide(0x118) >> 63))
        return unsupported("slot flags or transform");
    const auto input_format = word(0xd0) & 127;
    if ((input_format != 67 && input_format != 68) ||
        (!rgb && output_format != 67 && output_format != 68)) return unsupported("pixel format");
    const auto width = (word(0x24) & 0x3fff) + 1;
    const auto height = ((word(0x24) >> 14) & 0x3fff) + 1;
    if (width > 4096 || height > 4096 || width % 2 || height % 2)
        return NvResult::InvalidSize;
    const auto source_width = (word(0xb4) >> 16) + 1;
    const auto source_height = (word(0xbc) >> 16) + 1;
    // Rectangles can describe the padded picture (e.g. 1920x1088) while
    // the output surface clips it to the visible 1920x1080 area.
    if (word(0xb0) || word(0xb8) || (word(0xb4) & 0xffff) || (word(0xbc) & 0xffff) ||
        source_width < width || source_height < height ||
        word(0xc0) != word(0xb4) || word(0xc4) != word(0xbc) ||
        word(0x18) != word(0xc0) || word(0x1c) != word(0xc4))
        return unsupported("rectangle or scaling");
    const auto frame = gpu.getVideoFrame(u64(video_registers[0x100]) << 8);
    if (!frame || frame->format != video::PixelFormat::Nv12)
        return NvResult::NotInitialized;
    if (u32(frame->width) < source_width || u32(frame->height) < source_height)
        return NvResult::InvalidSize;
    const auto block_kind = (word(0x20) >> 11) & 15;
    const auto gob_height = (word(0x20) >> 15) & 15;
    if (block_kind > 1 || gob_height > 5) return unsupported("surface layout");
    // VIC works in 10-bit component space. Its signed 20-bit coefficients
    // are packed by matrix column; the translation is added after r_shift.
    std::array<std::array<i64, 4>, 3> matrix{};
    for (usize column = 0; column < 4; ++column)
        for (usize component = 0; component < 3; ++component) {
            const auto bits = (wide(0xf0 + column * 8) >> (component * 20)) & 0xfffff;
            matrix[component][column] = i64(bits ^ 0x80000) - 0x80000;
        }
    const auto matrix_shift = (wide(0xf0) >> 60) & 15;
    const auto matrix_enabled = (wide(0xf8) >> 63) != 0;
    const auto clamp_low = i64(wide(0xa0) & 1023);
    const auto clamp_high = i64((wide(0xa0) >> 10) & 1023);
    const auto alpha = (wide(0xa0) >> 32) & 1023;
    if (rgb && clamp_low > clamp_high) return NvResult::BadParameter;
    const u32 plane_count = rgb ? 1 : 2;
    std::array<std::vector<u8>, 2> planes;
    std::array<u64, 2> guests{};
    for (u32 plane = 0; plane < plane_count; ++plane) {
        const auto dimensions = word(0x28 + plane * 4);
        const auto plane_width = (dimensions & 0x3fff) + 1;
        const auto plane_height = ((dimensions >> 14) & 0x3fff) + 1;
        const auto stride = ((plane_width * (rgb ? 4 : (plane ? 2 : 1)) + 15) & ~15u);
        const auto rows = height / (plane ? 2 : 1);
        if (stride < width * (rgb ? 4 : 1) || stride > 16384 || plane_height < rows || plane_height > 4096)
            return NvResult::InvalidSize;
        const auto storage_stride = block_kind ? ((stride + 63) & ~63u) : stride;
        const auto storage_rows = block_kind ?
            ((plane_height + (8u << gob_height) - 1) & ~((8u << gob_height) - 1)) : plane_height;
        std::vector<u8> linear(usize(storage_stride) * storage_rows);
        const auto* source = frame->pixels.data() + frame->offsets[plane];
        for (u32 row = 0; row < rows; ++row) {
            auto* output = linear.data() + usize(row) * storage_stride;
            const auto* input = source + usize(row) * u32(frame->strides[plane]);
            if (rgb) {
                const auto* chroma = frame->pixels.data() + frame->offsets[1] +
                    usize(row / 2) * u32(frame->strides[1]);
                for (u32 column = 0; column < width; ++column) {
                    const std::array<i64, 3> yuv{input[column] * 4,
                        chroma[column & ~1u] * 4, chroma[(column & ~1u) + 1] * 4};
                    std::array<u8, 3> color{};
                    for (usize component = 0; component < 3; ++component) {
                        i64 value = yuv[component];
                        if (matrix_enabled) {
                            value = 0;
                            for (usize channel = 0; channel < 3; ++channel)
                                value += yuv[channel] * matrix[component][channel];
                            value = ((value >> matrix_shift) + matrix[component][3]) >> 8;
                        }
                        color[component] = u8(std::clamp(value, clamp_low, clamp_high) >> 2);
                    }
                    auto* pixel = output + column * 4;
                    pixel[0] = color[output_format == 32 ? 2 : 0];
                    pixel[1] = color[1];
                    pixel[2] = color[output_format == 32 ? 0 : 2];
                    pixel[3] = output_format == 35 ? 255 : u8(std::clamp(i64(alpha), clamp_low, clamp_high) >> 2);
                }
            } else if (plane && output_format == 68) {
                for (u32 column = 0; column < width; column += 2) {
                    output[column] = input[column + 1];
                    output[column + 1] = input[column];
                }
            } else std::memcpy(output, input, width);
        }
        planes[plane].resize(linear.size());
        if (block_kind) {
            hw::tegra_x1::gpu::convertLinearToBlockLinear(storage_stride,
                storage_stride * storage_rows, storage_stride, plane_height, 1,
                gob_height, 0, linear.data(), planes[plane].data());
        } else planes[plane] = std::move(linear);
        const auto address = gpu.resolveHost1x(u64(video_registers[0x1c8 + plane]) << 8, planes[plane].size());
        if (!address) return NvResult::InvalidAddress;
        guests[plane] = *address;
        for (usize offset = 0; offset < planes[plane].size();) {
            const auto current = *address + offset;
            if (!mmu.unmapAddr(current)) return NvResult::InvalidAddress;
            offset += std::min(planes[plane].size() - offset, usize(0x1000 - (current & 0xfff)));
        }
    }
    for (u32 plane = 0; plane < plane_count; ++plane) {
        mmu.trackWrite(ztd::Range<vaddr_t>::fromSize(guests[plane], planes[plane].size()));
        for (usize offset = 0; offset < planes[plane].size();) {
            const auto current = guests[plane] + offset;
            const auto count = std::min(planes[plane].size() - offset, usize(0x1000 - (current & 0xfff)));
            std::memcpy(reinterpret_cast<void*>(mmu.unmapAddr(current)), planes[plane].data() + offset, count);
            offset += count;
        }
    }
    LOG_INFO(Services, "VIC wrote {}x{} {} picture from 0x{:x} to 0x{:x}/0x{:x}",
        width, height, rgb ? (output_format == 32 ? "BGRA" : "RGBA") : (output_format == 68 ? "NV21" : "NV12"), u64(video_registers[0x100]) << 8,
        u64(video_registers[0x1c8]) << 8, u64(video_registers[0x1c9]) << 8);
    return NvResult::Success;
}
} // namespace hydra::horizon::services::nvdrv::ioctl
