#pragma once

#include "core/hw/tegra_x1/gpu/command_ir/resource_set.hpp"
#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {
class ICommandBuffer;
class IRenderer;
class ITextureView;
} // namespace hydra::hw::tegra_x1::gpu::renderer

namespace hydra::hw::tegra_x1::gpu::command_ir {

// A scaled texture-to-texture copy (the 2D engine blit). It reads the source
// texture's memory and writes the destination's.
struct CopyCommand {
    renderer::ITextureView* src{nullptr};
    renderer::ITextureView* dst{nullptr};
    simd::float3 src_origin{};
    MTL::Size src_size{};
    u32 src_level{0};
    u32 src_layer{0};
    simd::float3 dst_origin{};
    MTL::Size dst_size{};
    u32 dst_level{0};
    u32 dst_layer{0};
    u32 level_count{1};
    u32 layer_count{1};
    renderer::BlitOptions options{};
    ResourceSet resources;
};

bool sealCopy(renderer::IRenderer& renderer, CopyCommand& command);
bool executeCopy(renderer::IRenderer& renderer,
                 renderer::ICommandBuffer* command_buffer,
                 const CopyCommand& command);

} // namespace hydra::hw::tegra_x1::gpu::command_ir
