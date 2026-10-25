#pragma once

#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/const.hpp"

#include <array>
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

// The vertex function that draws the vertices an emulated geometry shader
// emitted (see geometry.hpp): it reads their output records and applies what
// a vertex shader's epilogue does
struct GeometryPassthroughState {
    // User outputs the fragment shader reads (bit per location)
    u32 output_mask{0};
    bool layer{false};
    bool viewport_index{false};
    // The geometry shader emits points
    bool point_list{false};
    bool program_point_size{false};
    f32 point_size{1.0f};
    // Positions are window coordinates (viewport transform disabled)
    u32 window_space_width{0};
    u32 window_space_height{0};
    bool y_negate{false};
    bool depth_zero_to_one{false};
};
std::string generateGeometryPassthrough(const GeometryPassthroughState& state);

// Tessellation emulation (see tessellation.hpp): the function writing each
// patch's Metal tessellation factors, from the control shader's patch levels
// or, without a control shader, from the fixed function levels (outer 0-3,
// inner 0-1)
std::string generateTessellationFactors(u32 domain, bool from_patches,
                                        const std::array<f32, 6>& levels);
// The function copying the input control points when there is no control
// shader
std::string generateTessellationCopy(u32 patch_size);

// Finds the input attribute a geometry shader copies into the render target
// layer (e.g. UE4's WriteToSliceMainGS, which forwards the vertex shader's
// layer index). Returns its address, or nothing if the layer is computed
// differently. The code includes the 0x50 byte program header.
std::optional<u32> findGeometryLayerSource(std::span<const u8> code);

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp
