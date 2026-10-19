// IndexCache::decode driven with guest primitive types and index buffers; the
// result is printed for tests/index_decode_reference.py.
//
// Input lines: "<primitive> <index type 0=u8 1=u16 2=u32 3=none> <count>
//               <restart enabled> <restart index> [<indices decimal...>]"
// Output lines: "<passthrough 0|1> <out type> <out primitive> <out count>
//                [<decoded indices...>]"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/index_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::renderer;
using metal::stackAutoRelease;

int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("usage: cases.txt results.txt");
        setenv("HYDRA_HOT_RELOAD", "0", 1);
        setenv("HYDRA_GPU_SYNC", "1", 1);
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        metal::Renderer renderer;
        std::unique_ptr<ICommandBuffer> commands(renderer.createCommandBuffer());
        std::ifstream input(argv[1]);
        std::ofstream output(argv[2]);
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream row(line);
            u32 primitive, type, count, restart_enabled, restart_index;
            row >> primitive >> type >> count >> restart_enabled >> restart_index;
            std::vector<u32> indices;
            for (u32 value; row >> value;)
                indices.push_back(value);

            // Guest index memory, in the index size of the descriptor
            std::vector<u8> memory;
            const u32 size = type == 0 ? 1 : type == 1 ? 2 : 4;
            if (type != 3) {
                memory.resize(std::max<usize>(indices.size(), 1) * size + 16);
                for (usize i = 0; i < indices.size(); ++i)
                    std::memcpy(memory.data() + i * size, &indices[i], size);
            }
            IndexDescriptor descriptor{
                .type = static_cast<engines::IndexType>(type),
                .primitive_type = static_cast<engines::PrimitiveType>(primitive),
                .count = count,
                .mem_range = type == 3
                                 ? std::nullopt
                                 : std::optional(ztd::Range<uptr>::fromSize(
                                       reinterpret_cast<uptr>(memory.data()),
                                       indices.size() * size)),
                .restart_enabled = restart_enabled != 0,
                .restart_index = restart_index,
            };
            engines::IndexType out_type;
            engines::PrimitiveType out_primitive;
            u32 out_count;
            const auto view = renderer.getIndexCache().decode(
                commands.get(), descriptor, out_type, out_primitive, out_count);
            // A draw whose primitive type, index type and count are unchanged is
            // drawn from the guest indices as they are
            const bool passthrough =
                out_primitive == descriptor.primitive_type &&
                out_type == descriptor.type && out_count == descriptor.count;
            output << (passthrough ? 1 : 0) << ' ' << static_cast<u32>(out_type)
                   << ' ' << static_cast<u32>(out_primitive) << ' ' << out_count;
            if (!passthrough && view.isValid()) {
                const u32 out_size = out_type == engines::IndexType::UInt32 ? 4 : 2;
                for (u32 i = 0; i < out_count; ++i) {
                    u32 value = 0;
                    std::memcpy(&value,
                                reinterpret_cast<const u8*>(view.getPtr()) +
                                    static_cast<usize>(i) * out_size,
                                out_size);
                    output << ' ' << value;
                }
            }
            output << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
