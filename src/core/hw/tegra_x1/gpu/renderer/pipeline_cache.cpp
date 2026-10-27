#include "core/hw/tegra_x1/gpu/renderer/pipeline_cache.hpp"

#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/pipeline_base.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

PipelineBase* PipelineCache::createHot0(const PipelineDescriptor& descriptor) {
    return renderer.createPipeline(descriptor);
}

u32 PipelineCache::hashHot1(const PipelineDescriptor& descriptor) {
    ztd::hash::XxHash32 hash;

    // Shaders
    // TODO: use the shader hash instead of the pointer?
    hash.add(descriptor.shaders[0]);
    hash.add(descriptor.shaders[1]);
    hash.add(descriptor.shaders[static_cast<u32>(ShaderType::Geometry)]);
    hash.add(descriptor.geometry_passthrough);
    hash.add(descriptor.shaders[static_cast<u32>(ShaderType::TessControl)]);
    hash.add(descriptor.shaders[static_cast<u32>(ShaderType::TessEvaluation)]);
    hash.add(descriptor.tessellation_factors);
    hash.add(descriptor.tessellation_parameters);
    hash.add(descriptor.tessellation_patch_size);

    // Vertex state

    // Vertex attributes
    for (const auto& vertex_attrib_state :
         descriptor.vertex_state.vertex_attrib_states) {
        hash.add(vertex_attrib_state.buffer_id);
        // is_fixed is in vertex shader hash
        hash.add(vertex_attrib_state.offset);
        // size and type are in vertex shader hash
        hash.add(vertex_attrib_state.bgra);
    }

    // Vertex arrays
    for (const auto& vertex_array : descriptor.vertex_state.vertex_arrays) {
        hash.add(vertex_array.enable);
        hash.add(vertex_array.stride);
        hash.add(vertex_array.is_per_instance);
        hash.add(vertex_array.divisor);
    }

    // Color state

    // Color targets
    for (const auto& color_target_state : descriptor.color_target_states) {
        hash.add(static_cast<u32>(color_target_state.format));
        hash.add(static_cast<u32>(color_target_state.write_mask));
        hash.add(color_target_state.blend_enabled);
        if (color_target_state.blend_enabled) {
            hash.add(static_cast<u32>(color_target_state.rgb_op));
            hash.add(static_cast<u32>(color_target_state.src_rgb_factor));
            hash.add(static_cast<u32>(color_target_state.dst_rgb_factor));
            hash.add(static_cast<u32>(color_target_state.alpha_op));
            hash.add(static_cast<u32>(color_target_state.src_alpha_factor));
            hash.add(static_cast<u32>(color_target_state.dst_alpha_factor));
        }
    }

    hash.add(static_cast<u32>(descriptor.depth_stencil_format));
    hash.add(static_cast<u32>(descriptor.topology_class));
    hash.add(descriptor.alpha_to_coverage);
    hash.add(descriptor.alpha_to_one);
    hash.add(descriptor.sample_count);

    return hash.toHashCode();
}

// The fields of the hash
bool PipelineCache::equals(const PipelineDescriptor& a,
                           const PipelineDescriptor& b) {
    constexpr auto GEOMETRY = static_cast<u32>(ShaderType::Geometry);
    if (a.shaders[0] != b.shaders[0] || a.shaders[1] != b.shaders[1] ||
        a.shaders[GEOMETRY] != b.shaders[GEOMETRY] ||
        a.geometry_passthrough != b.geometry_passthrough)
        return false;
    constexpr auto TESS_CONTROL = static_cast<u32>(ShaderType::TessControl);
    constexpr auto TESS_EVALUATION =
        static_cast<u32>(ShaderType::TessEvaluation);
    if (a.shaders[TESS_CONTROL] != b.shaders[TESS_CONTROL] ||
        a.shaders[TESS_EVALUATION] != b.shaders[TESS_EVALUATION] ||
        a.tessellation_factors != b.tessellation_factors ||
        a.tessellation_parameters != b.tessellation_parameters ||
        a.tessellation_patch_size != b.tessellation_patch_size)
        return false;

    for (usize i = 0; i < std::size(a.vertex_state.vertex_attrib_states);
         i++) {
        const auto& attrib_a = a.vertex_state.vertex_attrib_states[i];
        const auto& attrib_b = b.vertex_state.vertex_attrib_states[i];
        if (attrib_a.buffer_id != attrib_b.buffer_id ||
            attrib_a.offset != attrib_b.offset ||
            attrib_a.bgra != attrib_b.bgra)
            return false;
    }

    for (usize i = 0; i < std::size(a.vertex_state.vertex_arrays); i++) {
        const auto& array_a = a.vertex_state.vertex_arrays[i];
        const auto& array_b = b.vertex_state.vertex_arrays[i];
        if (array_a.enable != array_b.enable ||
            array_a.stride != array_b.stride ||
            array_a.is_per_instance != array_b.is_per_instance ||
            array_a.divisor != array_b.divisor)
            return false;
    }

    for (usize i = 0; i < std::size(a.color_target_states); i++) {
        const auto& target_a = a.color_target_states[i];
        const auto& target_b = b.color_target_states[i];
        if (target_a.format != target_b.format ||
            target_a.write_mask != target_b.write_mask ||
            target_a.blend_enabled != target_b.blend_enabled)
            return false;
        if (target_a.blend_enabled &&
            (target_a.rgb_op != target_b.rgb_op ||
             target_a.src_rgb_factor != target_b.src_rgb_factor ||
             target_a.dst_rgb_factor != target_b.dst_rgb_factor ||
             target_a.alpha_op != target_b.alpha_op ||
             target_a.src_alpha_factor != target_b.src_alpha_factor ||
             target_a.dst_alpha_factor != target_b.dst_alpha_factor))
            return false;
    }

    return a.depth_stencil_format == b.depth_stencil_format &&
           a.topology_class == b.topology_class &&
           a.alpha_to_coverage == b.alpha_to_coverage &&
           a.alpha_to_one == b.alpha_to_one && a.sample_count == b.sample_count;
}

void PipelineCache::destroyElement(PipelineBase* pipeline) { delete pipeline; }

} // namespace hydra::hw::tegra_x1::gpu::renderer
