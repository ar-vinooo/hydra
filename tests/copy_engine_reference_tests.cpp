// DMA copy engine driven through its method interface. Each case is a memory
// image plus a script of (method, value) writes; the memory after the script is
// written back for tests/copy_engine_reference.py to compare with its model.
//
// Binary input (little endian), repeated:
//   u32 memory size, u32 write count, count x (u32 method, u32 value), memory
// Binary output: the memory of every case, in order.
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using hydra::hw::tegra_x1::gpu::renderer::metal::stackAutoRelease;

namespace {
constexpr uptr BASE = 0x200000;

bool read(std::ifstream& in, void* data, usize size) {
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<usize>(in.gcount()) == size;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("usage: cases.bin results.bin");
        setenv("HYDRA_HOT_RELOAD", "0", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        Gpu gpu;
        GMmu gmmu(nullptr);
        std::vector<u8> memory(0x40000);
        gmmu.map(BASE, {reinterpret_cast<uptr>(memory.data()), memory.size()});
        tls_crnt_gmmu = &gmmu;
        std::unique_ptr<renderer::ICommandBuffer> commands(
            gpu.getRenderer().createCommandBuffer());
        tls_crnt_command_buffer = commands.get();
        gpu.subchannelMethod(0, 0, 0xb0b5);

        std::ifstream input(argv[1], std::ios::binary);
        std::ofstream output(argv[2], std::ios::binary);
        u32 header[2];
        while (read(input, header, sizeof(header))) {
            const u32 size = header[0], count = header[1];
            if (size > memory.size())
                throw std::runtime_error("memory image too large");
            std::vector<u32> writes(count * 2);
            if (!read(input, writes.data(), writes.size() * 4))
                throw std::runtime_error("truncated writes");
            std::fill(memory.begin(), memory.end(), 0);
            if (!read(input, memory.data(), size))
                throw std::runtime_error("truncated memory");
            for (u32 i = 0; i < count; ++i)
                gpu.subchannelMethod(0, writes[i * 2], writes[i * 2 + 1]);
            output.write(reinterpret_cast<const char*>(memory.data()), size);
        }
        tls_crnt_command_buffer = nullptr;
        commands.reset();
        tls_crnt_gmmu = nullptr;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
