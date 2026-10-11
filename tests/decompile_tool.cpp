// DEBUG: decompiles a dumped guest shader and prints the generated source
#include <fstream>
#include <iostream>
#include <iterator>

#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.hpp"

using namespace hydra::hw::tegra_x1::gpu::renderer;

int main(int argc, char** argv) {
    if (argc < 3)
        return 1;
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<hydra::u8> code((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    const std::string type_name = argv[2];
    const auto type = type_name == "vertex"     ? ShaderType::Vertex
                      : type_name == "compute" ? ShaderType::Compute
                                               : ShaderType::Fragment;
    GuestShaderState state{};
    hydra::ShaderBackend backend{};
    std::vector<hydra::u8> out;
    ResourceMapping mapping;
    ztd::io::MemoryStream stream(code);
    shader_decomp::decompile(stream, type, state, backend, out, mapping);
    std::cout.write(reinterpret_cast<const char*>(out.data()),
                    static_cast<std::streamsize>(out.size()));
    return 0;
}
