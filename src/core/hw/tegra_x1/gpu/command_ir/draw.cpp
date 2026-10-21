#include "core/hw/tegra_x1/gpu/command_ir/draw.hpp"
#include "core/hw/tegra_x1/gpu/perf_stats.hpp"

#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/pipeline_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/render_pass_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/sampler_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::command_ir {

namespace {
using resource::ResourceKind;

template <typename Visitor>
void visitDirectResources(const DrawCommand& command, Visitor&& visit) {
    visit(command.pipeline, ResourceKind::Pipeline);
    visit(command.render_pass, ResourceKind::RenderPass);
    for (const auto& view : command.vertices)
        visit(view.getBase(), ResourceKind::Buffer);
    visit(command.indices.getBase(), ResourceKind::Buffer);
    for (const auto& stage : command.stages) {
        if (!stage.active)
            continue;
        for (const auto& binding : stage.uniforms)
            visit(binding.buffer.getBase(), ResourceKind::Buffer);
        for (const auto& binding : stage.storage)
            visit(binding.buffer.getBase(), ResourceKind::Buffer);
        for (const auto& binding : stage.textures) {
            visit(binding.texture, ResourceKind::TextureView);
            visit(binding.sampler, ResourceKind::Sampler);
        }
    }
}

bool mappingMatches(resource::ResourceRegistry& registry,
                    const DrawCommand& command) {
    if (!command.address_space)
        return command.address_space_handle.getId() == 0;
    return registry.matches(command.address_space_handle,
                            ResourceKind::AddressSpace,
                            command.address_space) &&
           !command.address_space->gpuWorkCancelled() &&
           (command.address_space->getMappingGeneration() ==
                command.mapping_generation ||
            resource::validateMappings(*command.address_space,
                                       command.mapping_dependencies));
}

ztd::Range<uptr> textureRange(const renderer::ITextureView* view) {
    if (!view || !view->getBase())
        return {0, 0};
    return view->getBase()->getDescriptor().getRange();
}

// The guest ranges behind the command's buffers and textures, versioned now
void buildDrawContents(renderer::IRenderer& renderer, DrawCommand& command) {
    command.contents = {};
    // One version for every read of this command
    const u64 version = renderer.getContentVersions().current();
    const auto read = [&](const renderer::BufferView& view) {
        if (view.isValid())
            command.contents.addRead(version, view.getGuestRange());
    };
    for (const auto& view : command.vertices)
        read(view);
    read(command.indices);
    for (const auto& stage : command.stages) {
        if (!stage.active)
            continue;
        for (const auto& binding : stage.uniforms)
            read(binding.buffer);
        for (const auto& binding : stage.storage)
            read(binding.buffer);
        for (const auto& binding : stage.textures)
            command.contents.addRead(version, textureRange(binding.texture));
    }
    if (command.render_pass) {
        const auto& pass = command.render_pass->getDescriptor();
        for (const auto& target : pass.color_targets)
            command.contents.addWrite(textureRange(target.texture));
        command.contents.addWrite(textureRange(pass.depth_stencil_target.texture));
    }
    for (const auto& range : command.storage_writes)
        command.contents.addWrite(range);
}
} // namespace

void captureDrawAddressSpace(renderer::IRenderer& renderer,
                             DrawCommand& command, GMmu* address_space) {
    command.origin = currentCommandOrigin();
    command.resources_sealed = false;
    command.address_space = address_space;
    command.address_space_handle = {};
    command.mapping_generation = 0;
    if (address_space) {
        command.address_space_handle = renderer.getResourceRegistry().track(
            address_space->getResourceLifetime(), ResourceKind::AddressSpace,
            address_space);
        command.mapping_generation = address_space->getMappingGeneration();
    }
}

bool sealDrawResources(renderer::IRenderer& renderer, DrawCommand& command) {
    ScopedPerfTimer timer(PerfStats::instance().draw_prepare_ns,
                          PerfStats::isEnabled());
    ScopedGpuWork work(GpuWork::State);
    command.resources_sealed = false;
    command.resource_refs.clear();
    auto previous_owners = std::move(command.resource_owners);
    command.resource_owners.clear();
    auto& registry = renderer.getResourceRegistry();
    if (!mappingMatches(registry, command))
        return false;
    const auto track = [&](auto* object, ResourceKind kind) {
        if (object) {
            command.resource_refs.push_back(
                {object, kind,
                 registry.track(object->getResourceLifetime(), kind, object)});
            if (auto owner = object->getResourceOwner())
                if (std::find(command.resource_owners.begin(),
                              command.resource_owners.end(), owner) ==
                    command.resource_owners.end())
                    command.resource_owners.push_back(std::move(owner));
        }
    };
    visitDirectResources(command, track);
    command.direct_resource_count = command.resource_refs.size();
    if (command.pipeline)
        for (auto* shader : command.pipeline->getDescriptor().shaders)
            track(shader, ResourceKind::Shader);
    const auto track_view = [&](renderer::ITextureView* view) {
        track(view, ResourceKind::TextureView);
        if (view)
            track(view->getBase(), ResourceKind::Texture);
    };
    if (command.render_pass) {
        const auto& pass = command.render_pass->getDescriptor();
        for (const auto& target : pass.color_targets)
            track_view(target.texture);
        track_view(pass.depth_stencil_target.texture);
    }
    for (const auto& stage : command.stages)
        if (stage.active)
            for (const auto& binding : stage.textures)
                if (binding.texture)
                    track(binding.texture->getBase(), ResourceKind::Texture);
    command.resources_sealed = true;
    buildDrawContents(renderer, command);
    if (!validateDrawResources(renderer, command)) {
        command.resources_sealed = false;
        return false;
    }
    return true;
}

bool validateDrawResources(renderer::IRenderer& renderer,
                           const DrawCommand& command) {
    if (!command.resources_sealed)
        return false;
    auto& registry = renderer.getResourceRegistry();
    for (const auto& ref : command.resource_refs)
        if (!registry.matches(ref.handle, ref.kind, ref.object))
            return false;
    if (!mappingMatches(registry, command))
        return false;
    usize index = 0;
    bool matches = true;
    // Compare addresses only: resources may already have been destroyed.
    visitDirectResources(command, [&](const auto* object, ResourceKind kind) {
        if (!object)
            return;
        if (index >= command.resource_refs.size()) {
            matches = false;
        } else {
            const auto& ref = command.resource_refs[index];
            matches &= ref.object == object && ref.kind == kind;
        }
        ++index;
    });
    return matches && index == command.direct_resource_count;
}

bool validateDrawContents(renderer::IRenderer& renderer,
                          const DrawCommand& command) {
    return command.resources_sealed &&
           command.contents.readsCurrent(renderer.getContentVersions());
}

bool executeDraw(renderer::IRenderer& renderer,
                 renderer::ICommandBuffer* command_buffer,
                 const DrawCommand& command) {
    if (command.count == 0 || command.instance_count == 0)
        return true;
    ScopedPerfTimer timer(PerfStats::instance().draw_command_ns,
                          PerfStats::isEnabled());
    ScopedGpuWork work(GpuWork::Encode);
    const bool indexed = command.index_type != engines::IndexType::None;
    if (!command.pipeline || !command.render_pass || !command_buffer ||
        (indexed && !command.indices.isValid()) ||
        !validateDrawResources(renderer, command))
        return false;
    capture::recordGuestCommand("draw",
                                {command.origin.channel,
                                 command.origin.sequence,
                                 command.origin.source_address},
                                command.contents);

    for (const auto& owner : command.resource_owners)
        command_buffer->retainResource(owner);
    renderer.setOcclusionCounterEnabled(command.occlusion_enabled);
    renderer.bindRenderPass(command.render_pass);
    renderer.bindPipeline(command.pipeline);
    renderer::applyDrawState(renderer, command.state);
    for (u32 i = 0; i < VERTEX_ARRAY_COUNT; i++)
        renderer.bindVertexBuffer(command.vertices[i], i);
    for (u32 i = 0; i < renderer::GRAPHICS_SHADER_TYPE_COUNT; i++) {
        const auto& stage = command.stages[i];
        if (!stage.active)
            continue;
        const auto type = static_cast<renderer::ShaderType>(i);
        renderer.unbindUniformBuffers(type);
        for (const auto& binding : stage.uniforms)
            renderer.bindUniformBuffer(binding.buffer, type, binding.index);
        renderer.unbindStorageBuffers(type);
        for (const auto& binding : stage.storage)
            renderer.bindStorageBuffer(binding.buffer, type, binding.index);
        if (stage.reset_textures)
            renderer.unbindTextures(type);
        for (const auto& binding : stage.textures)
            renderer.bindTexture(binding.texture, binding.sampler, type,
                                 binding.index);
    }
    if (indexed) {
        renderer.bindIndexBuffer(command.indices, command.index_type);
        renderer.drawIndexed(command_buffer, command.primitive, command.first,
                             command.count, command.base_vertex,
                             command.base_instance, command.instance_count);
    } else {
        renderer.draw(command_buffer, command.primitive, command.first,
                      command.count, command.base_instance,
                      command.instance_count);
    }
    renderer.recordGpuWrites(command_buffer, command.contents.writes,
                             {command.origin.channel, command.origin.sequence,
                              command.origin.source_address});
    if (!command.storage_writes.empty()) {
        auto& cache = renderer.getTextureCache();
        std::scoped_lock lock(cache.getMutex());
        cache.synchronizeStorageWrites(command_buffer, command.storage_writes);
    }
    return true;
}

} // namespace hydra::hw::tegra_x1::gpu::command_ir
