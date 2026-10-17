#include "core/hw/tegra_x1/gpu/memory_util.hpp"

#include <cstddef>

namespace hydra::hw::tegra_x1::gpu {

namespace {

struct MemoryLayout {
    u32 block_height_gobs;
    u32 block_height;

    u32 x_gobs;
    u32 x_blocks;
    u32 y_gobs;
    u32 y_blocks;
    u32 z_gobs;
    u32 z_blocks;
};

MemoryLayout getMemoryLayout(u32 stride, u32 rows, u32 depth,
                             u32 block_height_gobs_log2,
                             u32 block_depth_gobs_log2) {
    MemoryLayout layout;
    layout.block_height_gobs = 1 << block_height_gobs_log2;
    layout.block_height = layout.block_height_gobs * GOB_HEIGHT;

    layout.x_gobs = align(stride, GOB_WIDTH) >> GOB_WIDTH_LOG2;
    layout.x_blocks = layout.x_gobs; // Blocks = GOBs (in X)
    layout.y_gobs = align(rows, layout.block_height) >> GOB_HEIGHT_LOG2;
    layout.y_blocks = layout.y_gobs >> block_height_gobs_log2;
    layout.z_gobs = depth;
    // Blocks span 2^block_depth slices, the last one can be partial
    layout.z_blocks = (depth + (1u << block_depth_gobs_log2) - 1) >>
                      block_depth_gobs_log2;

    return layout;
}

uint2 unswizzleGobCoords(u32 index) {
    const u32 local_y = ((index >> 1) & 0x06) | (index & 0x01);
    const u32 local_x = ((index << 3) & 0x10) | ((index << 1) & 0x20);
    return uint2({local_x, local_y});
}

} // namespace

void convertBlockLinearToLinear(u32 stride, u32 rows, u32 depth,
                                u32 block_height_gobs_log2,
                                u32 block_depth_gobs_log2, const u8* in_data,
                                const WriteGobFn& write_fn) {
    const auto layout = getMemoryLayout(
        stride, rows, depth, block_height_gobs_log2, block_depth_gobs_log2);
    const u32 block_depth = 1u << block_depth_gobs_log2;
    u32 gob_index = 0;
    for (u32 block_z = 0; block_z < layout.z_blocks; block_z++) {
        for (u32 block_y = 0; block_y < layout.y_blocks; block_y++) {
            for (u32 block_x = 0; block_x < layout.x_blocks; block_x++) {
                // Within a block, the GOBs of each slice are consecutive
                for (u32 gob_zy = 0;
                     gob_zy < block_depth * layout.block_height_gobs;
                     gob_zy++) {
                    const u32 gob_y = gob_zy % layout.block_height_gobs;
                    const u32 y =
                        block_y * layout.block_height + gob_y * GOB_HEIGHT;
                    const u32 z = block_z * block_depth +
                                  gob_zy / layout.block_height_gobs;
                    if (z >= depth || y >= rows) {
                        // Skip this GOB if we're past the valid height
                        gob_index++;
                        continue;
                    }

                    u8 out_gob[GOB_SIZE];
                    for (u32 i = 0; i < GOB_SIZE / sizeof(u128); i++) {
                        const auto local = unswizzleGobCoords(i);
                        *reinterpret_cast<u128*>(out_gob +
                                                 static_cast<usize>(local.y()) *
                                                     GOB_WIDTH +
                                                 local.x()) =
                            reinterpret_cast<const u128*>(
                                in_data +
                                static_cast<usize>(gob_index) * GOB_SIZE)[i];
                    }

                    write_fn(out_gob, block_x,
                             (block_y << block_height_gobs_log2) + gob_y, z);

                    gob_index++;
                }
            }
        }
    }
}

void convertBlockLinearToLinear(u32 src_stride, u32 dst_stride,
                                u32 dst_slice_stride, u32 rows, u32 depth,
                                u32 block_height_gobs_log2,
                                u32 block_depth_gobs_log2, const u8* in_data,
                                u8* out_data) {
    convertBlockLinearToLinear(
        src_stride, rows, depth, block_height_gobs_log2, block_depth_gobs_log2,
        in_data, [=](const u8* in_gob, u32 gob_x, u32 gob_y, u32 gob_z) {
            const u32 x = gob_x * GOB_WIDTH;
            for (u32 local_y = 0; local_y < GOB_HEIGHT; local_y++) {
                const u32 y = gob_y * GOB_HEIGHT + local_y;
                if (y >= rows)
                    break;

                const usize crnt_offset =
                    static_cast<usize>(gob_z) * dst_slice_stride +
                    static_cast<usize>(y) * dst_stride + x;
                std::memcpy(out_data + crnt_offset,
                            in_gob + static_cast<usize>(local_y) * GOB_WIDTH,
                            std::min(GOB_WIDTH, dst_stride - x));
            }
        });
}

void convertLinearToBlockLinear(u32 stride, u32 rows, u32 depth,
                                u32 block_height_gobs_log2,
                                u32 block_depth_gobs_log2,
                                const ReadGobFn& read_fn, u8* out_data) {
    const auto layout = getMemoryLayout(
        stride, rows, depth, block_height_gobs_log2, block_depth_gobs_log2);
    const u32 block_depth = 1u << block_depth_gobs_log2;
    u32 gob_index = 0;
    for (u32 block_z = 0; block_z < layout.z_blocks; block_z++) {
        for (u32 block_y = 0; block_y < layout.y_blocks; block_y++) {
            for (u32 block_x = 0; block_x < layout.x_blocks; block_x++) {
                // Within a block, the GOBs of each slice are consecutive
                for (u32 gob_zy = 0;
                     gob_zy < block_depth * layout.block_height_gobs;
                     gob_zy++) {
                    const u32 gob_y = gob_zy % layout.block_height_gobs;
                    const u32 y =
                        block_y * layout.block_height + gob_y * GOB_HEIGHT;
                    const u32 z = block_z * block_depth +
                                  gob_zy / layout.block_height_gobs;
                    if (z >= depth || y >= rows) {
                        // Skip this GOB if we're past the valid height
                        gob_index++;
                        continue;
                    }

                    u8 gob[GOB_SIZE];
                    read_fn(block_x,
                            (block_y << block_height_gobs_log2) + gob_y, z,
                            gob);

                    for (u32 i = 0; i < GOB_SIZE / sizeof(u128); i++) {
                        const auto local = unswizzleGobCoords(i);
                        reinterpret_cast<u128*>(out_data +
                                                static_cast<usize>(gob_index) *
                                                    GOB_SIZE)[i] =
                            *reinterpret_cast<const u128*>(
                                gob +
                                static_cast<usize>(local.y()) * GOB_WIDTH +
                                local.x());
                    }

                    gob_index++;
                }
            }
        }
    }
}

void convertLinearToBlockLinear(u32 src_stride, u32 src_slice_stride,
                                u32 dst_stride, u32 rows, u32 depth,
                                u32 block_height_gobs_log2,
                                u32 block_depth_gobs_log2, const u8* in_data,
                                u8* out_data) {
    convertLinearToBlockLinear(
        dst_stride, rows, depth, block_height_gobs_log2, block_depth_gobs_log2,
        [=](u32 gob_x, u32 gob_y, u32 gob_z, u8* out_gob) {
            const u32 x = gob_x * GOB_WIDTH;
            for (u32 local_y = 0; local_y < GOB_HEIGHT; local_y++) {
                const u32 y = gob_y * GOB_HEIGHT + local_y;
                if (y >= rows)
                    break;

                const usize crnt_offset =
                    static_cast<usize>(gob_z) * src_slice_stride +
                    static_cast<usize>(y) * src_stride + x;
                std::memcpy(out_gob + static_cast<usize>(local_y) * GOB_WIDTH,
                            in_data + crnt_offset,
                            std::min(GOB_WIDTH, src_stride - x));
            }
        },
        out_data);
}

u64 getBlockLinearOffset(u32 x, u32 y, u32 z, u32 width, u32 height,
                         u32 block_height_log2, u32 block_depth_log2) {
    const u32 gobs_x = align(width, GOB_WIDTH) / GOB_WIDTH;
    const u32 block_rows = GOB_HEIGHT << block_height_log2;
    const u32 block_slices = 1u << block_depth_log2;
    const u32 blocks_y = align(height, block_rows) / block_rows;
    const u64 block_size = static_cast<u64>(GOB_SIZE)
                           << (block_height_log2 + block_depth_log2);
    const u32 gob_x = x % GOB_WIDTH;
    const u32 gob_y = y % GOB_HEIGHT;
    return static_cast<u64>(z / block_slices) * blocks_y * gobs_x * block_size +
           static_cast<u64>(y / block_rows) * gobs_x * block_size +
           static_cast<u64>(x / GOB_WIDTH) * block_size +
           static_cast<u64>(z % block_slices) * (GOB_SIZE << block_height_log2) +
           static_cast<u64>((y % block_rows) / GOB_HEIGHT) * GOB_SIZE +
           (gob_x / 32) * 256 + (gob_y / 2) * 64 + ((gob_x % 32) / 16) * 32 +
           (gob_y % 2) * 16 + gob_x % 16;
}

} // namespace hydra::hw::tegra_x1::gpu
