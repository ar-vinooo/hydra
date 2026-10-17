// DMA engine volume uploads/readbacks, including partial slices and bounds.
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/memory_util.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include <iostream>
#include <stdexcept>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;

namespace {
void expect(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Independent byte-address formula: GOB coordinates plus bit-deposited local
// X/Y, as described in NVIDIA's block-linear layout and Eden decoders.cpp.
u64 offset(u32 x, u32 y, u32 z, u32 width, u32 height, u32 bh, u32 bd) {
    const u64 gobs_x = (width + 63) / 64;
    const u64 block_bytes = 512ull << (bh + bd);
    const u64 blocks_y = (height + (8u << bh) - 1) / (8u << bh);
    const u64 blocks = (z >> bd) * blocks_y * gobs_x +
                       (y >> (3 + bh)) * gobs_x + (x >> 6);
    const u64 local_x = (x & 15) | ((x & 16) << 1) | ((x & 32) << 3);
    const u64 local_y = ((y & 1) << 4) | ((y & 6) << 5);
    return blocks * block_bytes + ((z & ((1u << bd) - 1)) << (9 + bh)) +
           (((y >> 3) & ((1u << bh) - 1)) << 9) + local_x + local_y;
}

void testSlices() {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    Gpu gpu;
    GMmu gmmu(nullptr);
    constexpr uptr BASE = 0x200000, PITCH = 0x0, BL = 0x10000,
                   BL2 = 0x20000, OUT = 0x30000;
    std::vector<u8> memory(0x40000, 0xee);
    gmmu.map(BASE, {reinterpret_cast<uptr>(memory.data()), memory.size()});
    tls_crnt_gmmu = &gmmu;
    std::unique_ptr<renderer::ICommandBuffer> commands(
        gpu.getRenderer().createCommandBuffer());
    tls_crnt_command_buffer = commands.get();
    gpu.subchannelMethod(0, 0, 0xb0b5);
    const auto method = [&](u32 reg, u32 value) {
        gpu.subchannelMethod(0, reg, value);
    };
    const auto address = [&](u32 reg, uptr value) {
        method(reg, 0);
        method(reg + 1, static_cast<u32>(BASE + value));
    };
    const auto surface = [&](u32 reg, u32 width, u32 height, u32 depth,
                             u32 layer, u32 x, u32 y, u32 bh, u32 bd) {
        method(reg, (bh << 4) | (bd << 8));
        method(reg + 1, width);
        method(reg + 2, height);
        method(reg + 3, depth);
        method(reg + 4, layer);
        method(reg + 5, x | (y << 16));
    };
    const auto launch = [&](bool src_pitch, bool dst_pitch, u32 lines) {
        method(0x107, lines);
        method(0xc0, 1u | (u32(src_pitch) << 7) |
                         (u32(dst_pitch) << 8) | (1u << 9));
    };

    // Odd height and nontrivial block depth: the sixth padded slice must stay
    // untouched while LINE_COUNT walks 25 rows in each of five real slices.
    constexpr u32 WIDTH = 64, HEIGHT = 25, DEPTH = 5;
    for (u32 line = 0; line < HEIGHT * DEPTH; ++line)
        for (u32 x = 0; x < WIDTH; ++x)
            memory[PITCH + line * WIDTH + x] = static_cast<u8>(line * 17 + x);
    address(0x100, PITCH);
    address(0x102, BL);
    method(0x104, WIDTH);
    method(0x105, WIDTH);
    method(0x106, WIDTH);
    surface(0x1c3, WIDTH, HEIGHT, DEPTH, 0, 0, 0, 1, 1);
    launch(true, false, HEIGHT * DEPTH);
    std::vector<u8> expected(0x10000, 0xee);
    convertLinearToBlockLinear(WIDTH, WIDTH * HEIGHT, WIDTH, HEIGHT, DEPTH,
                               1, 1, memory.data() + PITCH, expected.data());
    for (u32 z = 0; z < DEPTH; ++z)
        for (u32 y = 0; y < HEIGHT; ++y)
            for (u32 x = 0; x < WIDTH; ++x) {
                const auto byte = offset(x, y, z, WIDTH, HEIGHT, 1, 1);
                expect(memory[BL + byte] == expected[byte],
                       "Volume upload agrees with full-volume converter");
                expect(memory[BL + byte] == memory[PITCH + (z * HEIGHT + y) * WIDTH + x],
                       "Volume upload uses independent 3D address formula");
            }
    expect(memory[BL + offset(0, 0, 5, WIDTH, HEIGHT, 1, 1)] == 0xee,
           "Upload preserves padded Z slice");

    address(0x100, BL);
    address(0x102, OUT);
    surface(0x1ca, WIDTH, HEIGHT, DEPTH, 0, 0, 0, 1, 1);
    launch(false, true, HEIGHT * DEPTH + 50);
    expect(std::equal(memory.begin() + PITCH, memory.begin() + PITCH + WIDTH * HEIGHT * DEPTH,
                      memory.begin() + OUT), "Volume readback round trip");
    expect(memory[OUT + WIDTH * HEIGHT * DEPTH] == 0xee,
           "Readback clips excess lines at the final real slice");

    // Different slice heights on each side: source wraps every 22 lines;
    // destination every 16. Starting layer and origin apply independently.
    address(0x102, BL2);
    method(0x106, 19);
    surface(0x1ca, WIDTH, HEIGHT, DEPTH, 1, 7, 3, 1, 1);
    surface(0x1c3, 128, 20, 4, 1, 11, 4, 2, 1);
    launch(false, false, 1000); // destination limits this to 3 * 16 = 48
    for (u32 z = 0; z < 4; ++z)
        for (u32 y = 0; y < 20; ++y)
            for (u32 x = 0; x < 128; ++x) {
                u8 value = 0xee;
                if (z >= 1 && y >= 4 && x >= 11 && x < 30) {
                    const u32 line = (z - 1) * 16 + y - 4;
                    const u32 sy = 3 + line % 22, sz = 1 + line / 22;
                    value = memory[BL + offset(7 + x - 11, sy, sz, WIDTH, HEIGHT, 1, 1)];
                }
                expect(memory[BL2 + offset(x, y, z, 128, 20, 2, 1)] == value,
                       "BL to BL wraps independent slice origins and preserves neighbours");
            }

    // A pitch volume retains its full programmed slice height even when
    // the tiled origin skips rows. Padding between pitch slices is untouched.
    std::fill(memory.begin() + BL2, memory.begin() + OUT, 0xee);
    address(0x100, PITCH);
    address(0x102, BL2);
    surface(0x1c3, 128, 20, 4, 1, 11, 4, 2, 1);
    launch(true, false, 48);
    for (u32 line = 0; line < 48; ++line)
        for (u32 x = 0; x < 19; ++x)
            expect(memory[BL2 + offset(11 + x, 4 + line % 16, 1 + line / 16,
                                      128, 20, 2, 1)] ==
                       memory[PITCH + ((line / 16) * 20 + line % 16) * WIDTH + x],
                   "Partial slice upload keeps full-height pitch slice stride");
    std::fill(memory.begin() + OUT, memory.end(), 0xee);
    address(0x100, BL2);
    address(0x102, OUT);
    surface(0x1ca, 128, 20, 4, 1, 11, 4, 2, 1);
    launch(false, true, 48);
    for (u32 row = 0; row < 56; ++row)
        for (u32 x = 0; x < WIDTH; ++x) {
            const u8 value = row % 20 < 16 && x < 19
                                 ? memory[PITCH + row * WIDTH + x]
                                 : 0xee;
            expect(memory[OUT + row * WIDTH + x] == value,
                   "Partial slice readback preserves pitch gaps and row tails");
        }
    address(0x100, BL);
    address(0x102, BL2);

    const auto unchanged = memory;
    surface(0x1ca, WIDTH, HEIGHT, DEPTH, 1, 60, 3, 1, 1);
    launch(false, false, 20);
    expect(memory == unchanged, "X extent beyond source surface is rejected");
    surface(0x1ca, WIDTH, HEIGHT, DEPTH, DEPTH, 0, 0, 1, 1);
    launch(false, false, 20);
    expect(memory == unchanged, "LAYER equal to DEPTH has no valid slices");
    surface(0x1ca, WIDTH, HEIGHT, DEPTH, 0, 0, HEIGHT, 1, 1);
    launch(false, false, 20);
    expect(memory == unchanged, "Y origin equal to HEIGHT has no valid rows");

    // A split guest span is not a contiguous host allocation. Tiled copy must
    // reject it before copying, including when the first mapping is valid.
    constexpr uptr SPLIT = BASE + 0x80000;
    std::array<u8, 512> head{}, tail{};
    head.fill(0x44);
    tail.fill(0x55);
    gmmu.map(SPLIT, {reinterpret_cast<uptr>(head.data()), head.size()});
    gmmu.map(SPLIT + 512, {reinterpret_cast<uptr>(tail.data()), tail.size()});
    method(0x100, 0);
    method(0x101, static_cast<u32>(SPLIT));
    surface(0x1ca, WIDTH, HEIGHT, DEPTH, 0, 0, 0, 1, 1);
    launch(false, false, 20);
    expect(memory == unchanged, "Split tiled host mapping is rejected without partial writes");

    tls_crnt_command_buffer = nullptr;
    commands.reset();
    tls_crnt_gmmu = nullptr;
    std::cout << "PASS: DMA volume upload/readback, origins, layers, independent slice wrapping and bounds\n";
}
} // namespace

int main() {
    try {
        testSlices();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
