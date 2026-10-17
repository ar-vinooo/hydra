#include "core/hw/tegra_x1/gpu/engines/2d.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;

static_assert(offsetof(engines::Regs2D, rop) == 0x2a0);
static_assert(offsetof(engines::Regs2D, operation) == 0x2ac);
static_assert(offsetof(engines::Regs2D, sample_mode) == 0x88c);

namespace {
void expect(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

engines::Regs2D makeRegs(u32 width, u32 height) {
    engines::Regs2D regs{};
    regs.operation = 3;
    regs.src.format = regs.dst.format =
        ColorSurfaceFormat::RGBA8Unorm; // A8B8G8R8
    regs.src.layout = regs.dst.layout = engines::MemoryLayout::Pitch;
    regs.src.width = regs.dst.width = width;
    regs.src.height = regs.dst.height = height;
    regs.src.stride = regs.dst.stride = width * 4;
    regs.src.depth = regs.dst.depth = 1;
    return regs;
}
} // namespace

void runBlit2DTests() {
    // All 16 pattern-independent ternary ROPs, checked with a per-bit oracle.
    auto regs = makeRegs(4, 1);
    const engines::BlitRegion2D full{0, 0, 4, 1, 0, 0, 4, 1};
    const std::array<u8, 16> source{0, 255, 85, 170, 0x12, 0x34, 0x56, 0x78,
                                    0x87, 0x65, 0x43, 0x21, 1, 2, 4, 8};
    const std::array<u8, 16> original{255, 0, 170, 85, 0x56, 0x78, 0x12, 0x34,
                                      0x43, 0x21, 0x87, 0x65, 8, 4, 2, 1};
    regs.operation = 4;
    for (u32 truth = 0; truth < 16; ++truth) {
        regs.rop = truth * 17u;
        auto dst = original;
        expect(engines::rawBlit2D(regs, full, source.data(), source.size(),
                                  dst.data(), dst.size()), "binary ROP accepted");
        for (usize i = 0; i < dst.size(); ++i) {
            u8 expected = 0;
            for (u32 bit = 0; bit < 8; ++bit) {
                const u32 selector = (((source[i] >> bit) & 1u) << 1) |
                                     ((original[i] >> bit) & 1u);
                expected |= static_cast<u8>(((truth >> selector) & 1) << bit);
            }
            expect(dst[i] == expected, "ROP matches independent bit oracle");
        }
    }
    regs.rop = 0xf0; // Pattern copy must not silently become source copy.
    auto dst = original;
    expect(!engines::rawBlit2D(regs, full, source.data(), source.size(),
                               dst.data(), dst.size()) && dst == original,
           "pattern-dependent ROP rejected without writes");
    regs.operation = 2;
    expect(!engines::supportsRawBlit2D(regs), "blend requires a dedicated path");
    std::cout << "PASS: 2D binary ROP truth tables and unsupported operations\n";

    // In-place shift snapshots the complete source before destination writes.
    regs = makeRegs(4, 1);
    auto overlap = source;
    const engines::BlitRegion2D shift{1, 0, 3, 1, 0, 0, 3, 1};
    expect(engines::rawBlit2D(regs, shift, overlap.data(), overlap.size(),
                              overlap.data(), overlap.size()), "overlap accepted");
    for (usize i = 4; i < overlap.size(); ++i)
        expect(overlap[i] == source[i - 4], "overlap uses original source");
    dst = original;
    expect(!engines::rawBlit2D(regs, full, source.data(), source.size() - 1,
                               dst.data(), dst.size()) && dst == original,
           "unmapped source tail leaves entire destination untouched");
    std::cout << "PASS: 2D raw overlapping copies and atomic bounds rejection\n";

    // Point sampling, mirrored coordinates, clamped edges, and a destination
    // slice distinct from source slice zero. Padding and other slices survive.
    regs = makeRegs(4, 2);
    regs.src.width = 2;
    regs.src.height = 1;
    regs.src.stride = 8;
    regs.dst.stride = 20;
    regs.dst.depth = 3;
    regs.dst.layer = 2;
    const std::array<u8, 8> pixels{11, 12, 13, 14, 21, 22, 23, 24};
    std::vector<u8> volume(120, 0x9d);
    const engines::BlitRegion2D mirrored{0, 0, 4, 2, 2, 0, -2, 1};
    expect(engines::rawBlit2D(regs, mirrored, pixels.data(), pixels.size(),
                              volume.data(), volume.size()), "slice copy accepted");
    for (usize i = 0; i < volume.size(); ++i) {
        u8 expected = 0x9d;
        if (i >= 80 && (i - 80) % 20 < 16) {
            const usize x = ((i - 80) % 20) / 4;
            expected = pixels[(x < 2 ? 4 : 0) + (i % 4)];
        }
        expect(volume[i] == expected, "point mirror preserves other slices/padding");
    }
    regs.dst.layer = 3;
    expect(!engines::supportsRawBlit2D(regs), "out-of-range slice rejected");
    std::cout << "PASS: 2D point sampling and pitch volume slice copies\n";

    // Build the expected volume with the separate whole-volume swizzler.
    // Non-power-of-two height and block depth exercise interleaved Z GOBs.
    regs = makeRegs(16, 9);
    regs.dst.layout = engines::MemoryLayout::BlockLinear;
    regs.dst.depth = 4;
    regs.dst.layer = 2;
    regs.dst.block_height_gobs_log2 = 1;
    regs.dst.block_depth_gobs_log2 = 1;
    std::vector<u8> linear(16 * 9 * 4 * 4, 0x77);
    std::vector<u8> one_slice(16 * 9 * 4);
    for (usize i = 0; i < one_slice.size(); ++i)
        one_slice[i] = static_cast<u8>(i * 37 + 19);
    std::copy(one_slice.begin(), one_slice.end(), linear.begin() + static_cast<std::ptrdiff_t>(2 * one_slice.size()));
    std::vector<u8> encoded(4096, 0), expected_encoded(4096, 0);
    std::vector<u8> initial(linear.size(), 0x77);
    const auto encode = [](const std::vector<u8>& input, std::vector<u8>& output) {
        convertLinearToBlockLinear(64, 9, 4, 1, 1,
            [&](u32 gx, u32 gy, u32 gz, u8* gob) {
                std::memset(gob, 0, GOB_SIZE);
                for (u32 row = 0; row < 8 && gy * 8 + row < 9; ++row)
                    std::memcpy(gob + row * 64,
                                input.data() + gz * 64 * 9 +
                                    (gy * 8 + row) * 64 + gx * 64,
                                64);
            }, output.data());
    };
    encode(initial, encoded);
    encode(linear, expected_encoded);
    const engines::BlitRegion2D slice{0, 0, 16, 9, 0, 0, 16, 9};
    expect(engines::rawBlit2D(regs, slice, one_slice.data(), one_slice.size(),
                              encoded.data(), encoded.size()), "block volume copy accepted");
    expect(encoded == expected_encoded, "volume slice matches whole-volume swizzle");
    std::cout << "PASS: 2D block-linear volume slices with interleaved block depth\n";
}
