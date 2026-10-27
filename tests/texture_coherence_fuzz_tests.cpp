// Model-based test of the texture cache's coherence between guest memory and
// GPU-owned render targets. Random sequences of GPU writes, guest (CPU) writes,
// flushes and reads of aliasing textures run against the real cache and a plain
// byte-array model of what the memory logically holds. Invariants:
//   reads    a texture over a range reads the logical bytes
//   flushes  after a flush the guest bytes of the range are the logical bytes
//   guest    bytes whose last writer was the guest never change in guest memory
// (a stale GPU copy must not be written over, or copied over, newer guest data)
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;

namespace {

constexpr usize REGION = 0x4000;
constexpr usize PAGE = 0x1000;

struct Model {
    std::vector<u8> guest;   // what guest memory holds
    std::vector<u8> logical; // what the memory is meant to hold
    std::vector<bool> guest_last; // last writer was the guest
};

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Rig {
    explicit Rig(u32 seed_)
        : seed{seed_}, rng{seed_},
          memory{static_cast<u8*>(std::aligned_alloc(PAGE, REGION)), std::free},
          commands{renderer.createCommandBuffer()},
          cache{renderer.getTextureCache()} {
        std::memset(memory.get(), 0, REGION);
        model.guest.assign(REGION, 0);
        model.logical.assign(REGION, 0);
        model.guest_last.assign(REGION, true);
    }

    u32 pick(u32 lo, u32 hi) {
        return std::uniform_int_distribution<u32>(lo, hi)(rng);
    }

    uptr base() const { return reinterpret_cast<uptr>(memory.get()); }

    TextureDescriptor describe(u32 offset, u32 width, u32 height,
                               TextureFormat format) const {
        return TextureDescriptor(base() + offset, MTL::TextureType2D, format, true,
                                 width * 4, width, height, 1, 1, 1, 0, 0, 0);
    }

    // Random texture over the region
    TextureDescriptor randomTexture(bool any_format) {
        const u32 width = std::array<u32, 4>{4, 8, 16, 32}[pick(0, 3)];
        const u32 height = std::array<u32, 3>{2, 4, 8}[pick(0, 2)];
        const u32 bytes = width * height * 4;
        const u32 offset = pick(0, static_cast<u32>((REGION - bytes) / 64)) * 64;
        const auto format = any_format && pick(0, 1) == 0
                                ? TextureFormat::R32Uint
                                : TextureFormat::RGBA8Unorm;
        return describe(offset, width, height, format);
    }

    struct Entry {
        usize begin, end;
        std::string text;
    };

    void log(usize begin, usize end, const std::string& text) {
        history.push_back({begin, end, text});
    }

    // The operations that touched the failing bytes, oldest first
    [[noreturn]] void fail(const std::string& what, usize begin = 0,
                           usize end = REGION) {
        std::ostringstream out;
        out << "seed " << seed << " (memory at " << std::hex << base()
            << std::dec << "): " << what << "\noperations on "
            << std::hex << begin << ".." << end << ":" << std::dec;
        for (const auto& entry : history)
            if (entry.begin < end && begin < entry.end)
                out << "\n  " << entry.text;
        if (end - begin == 1) {
            out << "\nvalues of byte " << std::hex << begin << std::dec << ":";
            for (const auto& value : byte_values[begin])
                out << "\n  " << value;
        }
        throw Failure(out.str());
    }

    void sync() { renderer.synchronize(commands.get()); }

    void syncGuest() {
        // The cache wrote guest memory through the host pointers
        std::memcpy(model.guest.data(), memory.get(), REGION);
    }

    void gpuWrite() {
        const auto d = randomTexture(false);
        log(d.ptr - base(), d.ptr - base() + d.size, fmt::format("gpu write {:#x}+{:#x}", d.ptr - base(), d.size));
        auto* view = cache.find(commands.get(), d, TextureUsage::Write);
        sync();
        std::vector<u8> bytes(d.size);
        for (auto& byte : bytes)
            byte = static_cast<u8>(pick(1, 255));
        if (!renderer.writeTextureContents(view->getBase(), {bytes}))
            fail("cannot write a render target");
        const auto offset = d.ptr - base();
        std::memcpy(model.logical.data() + offset, bytes.data(), d.size);
        for (u32 i = 0; i < d.size; i++) {
            model.guest_last[offset + i] = false;
            byte_values[offset + i].push_back(fmt::format(
                "gpu {:#x} (write at {:#x}+{:#x})", bytes[i], offset, d.size));
        }
    }

    void guestWrite() {
        const u32 length = pick(1, 128) * 4;
        const u32 offset = pick(0, static_cast<u32>((REGION - length) / 4)) * 4;
        // The memory tracker writes GPU data back before the guest touches a
        // page, then reports the write
        const usize first = offset / PAGE * PAGE;
        const usize last = (offset + length + PAGE - 1) / PAGE * PAGE;
        log(offset, offset + length, fmt::format("guest write {:#x}+{:#x} (flushes pages {:#x}..{:#x} first)", offset, length, first, last));
        cache.flushMemory(commands.get(), ztd::Range<uptr>(base() + first, base() + last));
        sync();
        syncGuest();
        for (usize i = first; i < last; i++)
            if (model.guest[i] != model.logical[i])
                fail(fmt::format("the flush before a guest write left byte {:#x} "
                                 "stale ({:#x} != {:#x})", i, model.guest[i],
                                 model.logical[i]), i, i + 1);
        for (u32 i = 0; i < length; i++) {
            const u8 byte = static_cast<u8>(pick(1, 255));
            memory.get()[offset + i] = byte;
            model.logical[offset + i] = byte;
            model.guest_last[offset + i] = true;
            byte_values[offset + i].push_back(fmt::format(
                "guest {:#x} (write at {:#x}+{:#x})", byte, offset, length));
        }
        cache.invalidateMemory(ztd::Range<uptr>(base() + offset, base() + offset + length));
        syncGuest();
    }

    void flush() {
        const u32 begin = pick(0, static_cast<u32>(REGION / 64 - 1)) * 64;
        const u32 end = std::min<u32>(REGION, begin + pick(1, 64) * 64);
        log(begin, end, fmt::format("flush {:#x}..{:#x}", begin, end));
        cache.flushMemory(commands.get(), ztd::Range<uptr>(base() + begin, base() + end));
        sync();
        syncGuest();
        for (u32 i = begin; i < end; i++)
            if (model.guest[i] != model.logical[i])
                fail(fmt::format("after a flush guest byte {:#x} is {:#x}, not "
                                 "the logical {:#x}", i, model.guest[i],
                                 model.logical[i]), i, i + 1);
    }

    void read() {
        const auto d = randomTexture(true);
        log(d.ptr - base(), d.ptr - base() + d.size, fmt::format("read {} {:#x}+{:#x}", d.format, d.ptr - base(), d.size));
        auto* view = cache.find(commands.get(), d, TextureUsage::Read);
        sync();
        syncGuest();
        std::vector<std::vector<u8>> contents;
        if (!renderer.readTextureContents(view->getBase(), contents) ||
            contents.size() != 1 || contents[0].size() != d.size)
            fail("cannot read a texture back");
        const auto offset = d.ptr - base();
        for (u32 i = 0; i < d.size; i++)
            if (contents[0][i] != model.logical[offset + i])
                fail(fmt::format("a texture over {:#x}+{:#x} reads {:#x} at byte "
                                 "{} but the memory logically holds {:#x}",
                                 offset, d.size, contents[0][i], i,
                                 model.logical[offset + i]), offset + i, offset + i + 1);
    }

    void checkGuest() {
        syncGuest();
        for (usize i = 0; i < REGION; i++)
            if (model.guest_last[i] && model.guest[i] != model.logical[i])
                fail(fmt::format("guest byte {:#x} changed from {:#x} to {:#x} "
                                 "although the guest wrote it last", i,
                                 model.logical[i], model.guest[i]), i, i + 1);
    }

    void run(u32 operations) {
        for (u32 i = 0; i < operations; i++) {
            switch (pick(0, 9)) {
            case 0:
            case 1:
            case 2:
                gpuWrite();
                break;
            case 3:
            case 4:
            case 5:
                guestWrite();
                break;
            case 6:
                flush();
                break;
            default:
                read();
                break;
            }
            checkGuest();
        }
    }

    u32 seed;
    std::mt19937 rng;
    metal::Renderer renderer;
    std::unique_ptr<u8, void (*)(void*)> memory;
    std::unique_ptr<ICommandBuffer> commands;
    TextureCache& cache;
    Model model;
    std::vector<Entry> history;
    // Every value each byte logically held, with the operation that wrote it
    std::vector<std::vector<std::string>> byte_values =
        std::vector<std::vector<std::string>>(REGION);
};

} // namespace

void runTextureCoherenceFuzz() {
    NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
    const u32 seeds = std::getenv("HYDRA_FUZZ_SEEDS")
                          ? static_cast<u32>(std::atoi(std::getenv("HYDRA_FUZZ_SEEDS")))
                          : 12;
    const u32 first = std::getenv("HYDRA_FUZZ_FIRST")
                          ? static_cast<u32>(std::atoi(std::getenv("HYDRA_FUZZ_FIRST")))
                          : 1;
    for (u32 seed = first; seed < first + seeds; seed++) {
        Rig rig(seed);
        rig.run(std::getenv("HYDRA_FUZZ_OPS") ? static_cast<u32>(std::atoi(std::getenv("HYDRA_FUZZ_OPS"))) : 120);
    }
    std::cout << "PASS: texture cache coherence under random GPU writes, guest "
                 "writes, flushes and aliasing reads\n";
}
