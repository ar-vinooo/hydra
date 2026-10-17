#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/const.hpp"

#include <span>

namespace hydra::hw::tegra_x1::gpu::renderer {
struct GuestShaderState;
}

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp {

class ObserverBase;
class BuilderBase;

void decompile(
    ztd::io::MemoryStream& code_stream, const ShaderType type,
    const GuestShaderState& state, ShaderBackend& out_backend,
    std::vector<u8>& out_code, ResourceMapping& out_resource_mapping,
    const std::function<std::optional<u32>(u32, u32)>& read_const_word = {});

void decompileHot(
    ztd::io::MemoryStream& code_stream, const ShaderType type,
    const GuestShaderState& state, ShaderBackend& out_backend,
    std::vector<u8>& out_code, ResourceMapping& out_resource_mapping,
    const std::function<std::optional<u32>(u32, u32)>& read_const_word);

// Finds the input attribute a geometry shader copies into the render target
// layer (e.g. UE4's WriteToSliceMainGS, which forwards the vertex shader's
// layer index). Returns its address, or nothing if the layer is computed
// differently. The code includes the 0x50 byte program header.
std::optional<u32> findGeometryLayerSource(std::span<const u8> code);

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp
