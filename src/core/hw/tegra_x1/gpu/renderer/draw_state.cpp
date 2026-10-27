#include "core/hw/tegra_x1/gpu/renderer/draw_state.hpp"

#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/renderer.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

void applyDrawState(IRenderer& renderer, const DrawState& state) {
    ScopedPerfTimer timer(PerfStats::instance().draw_apply_ns,
                          PerfStats::isEnabled());
    renderer.setDepthTestEnabled(state.depth_test_enabled);
    renderer.setDepthWriteEnabled(state.depth_write_enabled);
    renderer.setDepthCompareOp(state.depth_compare_op);
    renderer.setBlendConstant(state.blend_constant);
    renderer.setStencilState(state.stencil);
    renderer.setCullState(state.cull_enabled, state.cull_mode,
                          state.front_face);
    renderer.setDepthBias(state.depth_bias_constant, state.depth_bias_slope,
                          state.depth_bias_clamp);
    renderer.setDepthClamp(state.depth_clamp);
    renderer.setPolygonLines(state.polygon_lines);
    for (u32 i = 0; i < VIEWPORT_COUNT; i++) {
        renderer.setViewport(i, state.viewports[i]);
        renderer.setScissor(i, state.scissors[i]);
    }
}

} // namespace hydra::hw::tegra_x1::gpu::renderer
