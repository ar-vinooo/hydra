#pragma once

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class IRenderer;

// Value snapshot of dynamic raster state, without guest register references.
// Uses the current renderer types; not a portable capture file format.
struct DrawState {
    bool depth_test_enabled{false};
    bool depth_write_enabled{false};
    engines::CompareOp depth_compare_op{engines::CompareOp::Always};
    simd::float4 blend_constant{};
    StencilState stencil{};
    bool cull_enabled{false};
    engines::CullFaceMode cull_mode{engines::CullFaceMode::Back};
    engines::Winding front_face{engines::Winding::CounterClockwise};
    f32 depth_bias_constant{0.0f};
    f32 depth_bias_slope{0.0f};
    f32 depth_bias_clamp{0.0f};
    bool depth_clamp{false};
    // SET_FRONT_POLYGON_MODE: triangles drawn as lines
    bool polygon_lines{false};
    std::array<MTL::Viewport, VIEWPORT_COUNT> viewports{};
    std::array<MTL::ScissorRect, VIEWPORT_COUNT> scissors{};
};

void applyDrawState(IRenderer& renderer, const DrawState& state);

} // namespace hydra::hw::tegra_x1::gpu::renderer
