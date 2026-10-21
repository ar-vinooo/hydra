#pragma once

#include "core/hw/tegra_x1/gpu/renderer/buffer_view.hpp"
#include "core/hw/tegra_x1/gpu/renderer/draw_state.hpp"

#include "core/hw/tegra_x1/gpu/command_ir/resource_set.hpp"

#include "core/hw/tegra_x1/gpu/renderer/texture_binding.hpp"

#include <vector>

namespace hydra::hw::tegra_x1::gpu {
class GMmu;
}

namespace hydra::hw::tegra_x1::gpu::renderer {
class PipelineBase;
class RenderPassBase;
class SamplerBase;
} // namespace hydra::hw::tegra_x1::gpu::renderer

namespace hydra::hw::tegra_x1::gpu::command_ir {

struct BufferBinding {
    u32 index;
    renderer::BufferView buffer;
};

using TextureBinding = renderer::TextureBinding;

struct StageBindings {
    bool active{false};
    // Producer snapshots inherited slots when descriptor pools are absent.
    bool reset_textures{false};
    std::vector<BufferBinding> uniforms;
    std::vector<BufferBinding> storage;
    std::vector<TextureBinding> textures;
};

// Host-resolved command, consumed synchronously before caches or mappings
// change. Cache resources and owned buffers carry leases; external raw
// resources still require caller ownership through encoding.
struct DrawCommand {
    CommandOrigin origin;
    renderer::PipelineBase* pipeline{nullptr};
    renderer::RenderPassBase* render_pass{nullptr};
    renderer::DrawState state{};
    bool occlusion_enabled{false};
    std::array<renderer::BufferView, VERTEX_ARRAY_COUNT> vertices{};
    std::array<StageBindings, renderer::GRAPHICS_SHADER_TYPE_COUNT> stages{};
    std::vector<ztd::Range<uptr>> storage_writes;
    std::vector<ResourceReference> resource_refs;
    std::vector<std::shared_ptr<void>> resource_owners;
    usize direct_resource_count{0};
    GMmu* address_space{nullptr};
    resource::ResourceHandle address_space_handle;
    u64 mapping_generation{0};
    std::vector<resource::MappingDependency> mapping_dependencies;
    bool resources_sealed{false};
    // Guest memory read and written, versioned when the command was sealed
    resource::ContentManifest contents;

    engines::PrimitiveType primitive{engines::PrimitiveType::Triangles};
    engines::IndexType index_type{engines::IndexType::None};
    renderer::BufferView indices;
    u32 first{0};
    u32 count{0};
    u32 base_vertex{0};
    u32 base_instance{0};
    u32 instance_count{1};
};

void captureDrawAddressSpace(renderer::IRenderer& renderer,
                             DrawCommand& command, GMmu* address_space);
bool sealDrawResources(renderer::IRenderer& renderer, DrawCommand& command);
bool validateDrawResources(renderer::IRenderer& renderer,
                           const DrawCommand& command);
// Whether every byte the draw reads is still what it was when sealed. A draw
// executed later than it was prepared (deferred or reordered) needs this, or
// a snapshot of what it read.
bool validateDrawContents(renderer::IRenderer& renderer,
                          const DrawCommand& command);

bool executeDraw(renderer::IRenderer& renderer,
                 renderer::ICommandBuffer* command_buffer,
                 const DrawCommand& command);

} // namespace hydra::hw::tegra_x1::gpu::command_ir
