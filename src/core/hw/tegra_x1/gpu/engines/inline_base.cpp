#include "core/hw/tegra_x1/gpu/engines/inline_base.hpp"

#include "core/hw/tegra_x1/gpu/capture/guest_events.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"

namespace hydra::hw::tegra_x1::gpu::engines {

namespace {

// Larger transfers are garbage registers
constexpr usize MAX_TRANSFER_SIZE = 64 * 1024 * 1024;

// Calls f(host pointer, offset in the range, size) for each mapped part of a
// GPU range
template <typename F>
void forEachHostRange(uptr gpu_addr, u64 size, F&& f) {
    u64 offset = 0;
    while (offset < size) {
        uptr base;
        const auto as = tls_crnt_gmmu->findAddr(gpu_addr + offset, base);
        if (!as || as->ptr == 0) {
            ONCE(LOG_WARN(Engines, "Inline data written to unmapped memory {:#x}",
                          gpu_addr + offset));
            return;
        }
        const u64 inner = gpu_addr + offset - base;
        const u64 count = std::min(size - offset, as->size - inner);
        f(as->ptr + inner, offset, count);
        offset += count;
    }
}

} // namespace

void InlineBase::launchDmaImpl(Gpu& gpu, RegsInline& regs, const u32* regs_raw,
                               const u32 index, const u32 data) {
    (void)index;
    // cla140.h: SET_I2M_SEMAPHORE_A (address bits 39:32), B (31:0), C
    // (payload); LAUNCH_DMA DST_MEMORY_LAYOUT[0], REDUCTION_ENABLE[1],
    // REDUCTION_FORMAT[3:2], COMPLETION_TYPE[5:4], SEMAPHORE_STRUCT_SIZE[12],
    // REDUCTION_OP[15:13]
    constexpr u32 REG_SEMAPHORE_A = 0x77;
    constexpr u32 COMPLETION_TYPE_RELEASE_SEMAPHORE = 2;
    pending_semaphore.reset();
    if (extractBits(data, 4, 2) == COMPLETION_TYPE_RELEASE_SEMAPHORE)
        pending_semaphore = Semaphore{
            (static_cast<u64>(regs_raw[REG_SEMAPHORE_A] & 0xff) << 32) |
                regs_raw[REG_SEMAPHORE_A + 1],
            regs_raw[REG_SEMAPHORE_A + 2],
            extractBits(data, 12, 1) != 0,
            extractBits(data, 1, 1) != 0,
            static_cast<u32>(extractBits(data, 13, 3)),
            extractBits(data, 2, 2) == 0};
    startTransfer(regs, (data & 0x1) != 0);
    // Without inline data to wait for, the launch completes now
    if (!active)
        releaseSemaphore(gpu);
}

void InlineBase::loadInlineDataImpl(Gpu& gpu, RegsInline& regs, const u32 index,
                                    const u32 data) {
    (void)index;
    if (!active) {
        ONCE(LOG_WARN(Engines, "Inline data without LAUNCH_DMA"));
        startTransfer(regs, true);
        if (!active)
            return;
    }

    // The last word can be partial
    const usize count = std::min<usize>(4, copy_size - inline_data.size());
    const auto* bytes = reinterpret_cast<const u8*>(&data);
    inline_data.insert(inline_data.end(), bytes, bytes + count);
    if (inline_data.size() == copy_size) {
        writeTransfer(gpu, regs);
        active = false;
        releaseSemaphore(gpu);
    }
}

void InlineBase::releaseSemaphore(Gpu& gpu) {
    if (!pending_semaphore)
        return;
    const auto semaphore = *pending_semaphore;
    pending_semaphore.reset();

    const u64 size = semaphore.one_word ? 4 : 16;
    uptr base;
    const auto as = tls_crnt_gmmu->findAddr(semaphore.address, base);
    if (!as || as->ptr == 0 || semaphore.address - base + size > as->size ||
        (as->ptr + (semaphore.address - base)) %
                std::atomic_ref<u32>::required_alignment != 0) {
        ONCE(LOG_WARN(Engines, "Inline semaphore release to unmapped memory {:#x}",
                      semaphore.address));
        return;
    }
    auto* ptr = reinterpret_cast<u8*>(as->ptr + (semaphore.address - base));
    if (!semaphore.one_word) {
        const u64 timestamp = getGpuTimestamp();
        std::memcpy(ptr + 8, &timestamp, sizeof(timestamp));
    }
    // The payload is published last: a waiter that sees it also sees the data
    std::atomic_ref<u32> word(*reinterpret_cast<u32*>(ptr));
    if (semaphore.reduction) {
        u32 old = word.load(std::memory_order_relaxed);
        while (!word.compare_exchange_weak(
            old,
            reduceDmaSemaphoreWord(semaphore.reduction_op,
                                   semaphore.reduction_signed, old,
                                   semaphore.payload),
            std::memory_order_acq_rel)) {
        }
    } else {
        word.store(semaphore.payload, std::memory_order_release);
    }
    gpu.getRenderer().invalidateMemory(
        ztd::Range<uptr>::fromSize(reinterpret_cast<uptr>(ptr), size));
    capture::GuestEvents::instance().event(
        "semaphore_release", capture::currentGuestEventOrigin(),
        semaphore.address, size, 0, semaphore.payload);
}

void InlineBase::startTransfer(const RegsInline& regs, bool pitch) {
    is_pitch = pitch;
    copy_size = static_cast<usize>(regs.line_length_in) * regs.line_count;
    inline_data.clear();
    active = copy_size != 0 && copy_size <= MAX_TRANSFER_SIZE;
    if (copy_size > MAX_TRANSFER_SIZE)
        LOG_ERROR(Engines, "Inline transfer of {:#x} bytes", copy_size);
    if (active)
        inline_data.reserve(copy_size);
}

void InlineBase::writeTransfer(Gpu& gpu, const RegsInline& regs) {
    const uptr dst = regs.offset_out;
    const u32 line_length = regs.line_length_in;
    const u32 line_count = regs.line_count;

    // Destination offset of each byte run of a line, and the byte range all
    // lines touch
    std::vector<std::pair<u64, u32>> runs; // (destination offset, size)
    if (is_pitch) {
        for (u32 line = 0; line < line_count; line++)
            runs.emplace_back(static_cast<u64>(line) * regs.pitch_out,
                              line_length);
    } else {
        // Block linear surface: width in bytes, origin x in bytes. The layer
        // is not applied (like Eden), the address selects it.
        const u32 block_height_log2 = extractBits(regs.dst_block_size, 4, 4);
        const u32 block_depth_log2 = extractBits(regs.dst_block_size, 8, 4);
        for (u32 line = 0; line < line_count; line++) {
            const u32 y = regs.dst_origin_samples_y + line;
            // Runs end at 16 byte boundaries, the contiguous part of a GOB
            for (u32 x = 0; x < line_length;) {
                const u32 dst_x = regs.dst_origin_bytes_x + x;
                const u32 size = std::min(16 - dst_x % 16, line_length - x);
                runs.emplace_back(
                    getBlockLinearOffset(dst_x, y, 0, regs.dst_width,
                                         regs.dst_height, block_height_log2,
                                         block_depth_log2),
                    size);
                x += size;
            }
        }
    }
    u64 begin = ~0ull;
    u64 end = 0;
    for (const auto& [offset, size] : runs) {
        begin = std::min(begin, offset);
        end = std::max(end, offset + size);
    }
    if (begin >= end)
        return;

    // GPU writes to the destination must reach guest memory first, the inline
    // data then replaces them there
    {
        auto& texture_cache = gpu.getRenderer().getTextureCache();
        std::scoped_lock lock(texture_cache.getMutex());
        forEachHostRange(dst + begin, end - begin,
                         [&](uptr host, u64, u64 size) {
                             texture_cache.flushMemory(
                                 tls_crnt_command_buffer,
                                 ztd::Range<uptr>::fromSize(host, size));
                         });
    }

    usize src_offset = 0;
    for (const auto& [offset, size] : runs) {
        forEachHostRange(dst + offset, size,
                         [&](uptr host, u64 inner, u64 count) {
                             std::memcpy(reinterpret_cast<void*>(host),
                                         inline_data.data() + src_offset + inner,
                                         count);
                         });
        src_offset += size;
    }

    forEachHostRange(dst + begin, end - begin, [&](uptr host, u64, u64 size) {
        gpu.getRenderer().invalidateMemory(
            ztd::Range<uptr>::fromSize(host, size));
    });
}

} // namespace hydra::hw::tegra_x1::gpu::engines
