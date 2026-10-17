#include "core/hot/table.hpp"
#include <cstring>

namespace {
[[maybe_unused]] void decompileFixture(
    ztd::io::MemoryStream&, hydra::hw::tegra_x1::gpu::renderer::ShaderType,
    const hydra::hw::tegra_x1::gpu::renderer::GuestShaderState&,
    hydra::ShaderBackend& backend, std::vector<hydra::u8>& code,
    hydra::hw::tegra_x1::gpu::renderer::ResourceMapping&,
    const std::function<std::optional<hydra::u32>(hydra::u32, hydra::u32)>&) {
    backend = hydra::ShaderBackend::Msl;
    const std::string source =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "fragment float4 main_() { return float4(" HYDRA_FIXTURE_COLOR
        ",0,0,1); }\n";
    code.assign(source.begin(), source.end());
}
} // namespace
extern "C" __attribute__((visibility("default"))) bool
hydra_hot_register(hydra::hot::Table* table, std::uint64_t size,
                   const char* abi) {
#ifdef HYDRA_FIXTURE_REJECT
    (void)table;
    (void)size;
    (void)abi;
    return false;
#else
    if (size != sizeof(*table) || std::strcmp(abi, hydra::hot::getAbi()) != 0)
        return false;
    hydra::hot::fillTable(*table);
    table->shader_decompile = decompileFixture;
    return true;
#endif
}
