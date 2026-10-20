#pragma once

#include "core/hw/tegra_x1/gpu/command_ir/resource_set.hpp"
#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

#include <optional>

namespace hydra::hw::tegra_x1::gpu::renderer {
class ICommandBuffer;
class IRenderer;
class RenderPassBase;
} // namespace hydra::hw::tegra_x1::gpu::renderer

namespace hydra::hw::tegra_x1::gpu::command_ir {

// A clear of the attachments of one render pass, resolved by the engine.
// Clears read nothing; they write the attachments they clear.
struct ClearCommand {
    renderer::RenderPassBase* render_pass{nullptr};
    renderer::ClearState state{};
    struct Color {
        u32 target;
        u32 layer;
        u8 mask;
        simd::uint4 value;
    };
    struct Depth {
        u32 layer;
        f32 value;
    };
    struct Stencil {
        u32 layer;
        u32 value;
    };
    std::optional<Color> color;
    std::optional<Depth> depth;
    std::optional<Stencil> stencil;
    ResourceSet resources;
};

// Pins the pass and its attachments and records the memory the clear writes
bool sealClear(renderer::IRenderer& renderer, ClearCommand& command);
// A clear without a pass or without anything to clear succeeds trivially
bool executeClear(renderer::IRenderer& renderer,
                  renderer::ICommandBuffer* command_buffer,
                  const ClearCommand& command);

} // namespace hydra::hw::tegra_x1::gpu::command_ir
