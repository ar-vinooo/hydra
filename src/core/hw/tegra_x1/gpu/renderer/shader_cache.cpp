#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"

#include "core/hw/tegra_x1/gpu/engines/3d.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

ShaderBase* ShaderCache::create(const GuestShaderDescriptor& descriptor) {
    ShaderDescriptor host_descriptor{};
    host_descriptor.type = engines::toRendererShaderType(descriptor.stage);

    // Decompile
    ztd::io::MemoryStream code_stream(
        std::span(reinterpret_cast<u8*>(descriptor.code_ptr),
                  0x1000)); // TODO: size
    shader_decomp::decompile(code_stream, host_descriptor.type,
                             descriptor.state, host_descriptor.backend,
                             host_descriptor.code,
                             host_descriptor.resource_mapping);

    return renderer.createShader(host_descriptor);
}

u32 ShaderCache::hash(const GuestShaderDescriptor& descriptor) {
    // Hash enum values as words rather than feeding each byte separately.
    // Keep explicit fields: hashing whole structs would include padding.
    ztd::hash::XxHash32 hash;
    hash.add(static_cast<u32>(descriptor.stage));
    hash.add(descriptor.code_ptr);
    for (const auto& [index, format] :
         descriptor.state.buffer_texture_formats) {
        hash.add(index);
        hash.add(static_cast<u32>(format));
    }

    // Take a few samples from the code
    // TODO: this should be limited by the size of the code
    ztd::io::MemoryStream code_stream(
        std::span(reinterpret_cast<u8*>(descriptor.code_ptr),
                  0x1000)); // TODO: size
    code_stream.seekBy(80); // Header
    for (u32 i = 0; i < 8; i++) {
        hash.add(code_stream.read<u8>());
        code_stream.seekBy(17);
    }

    // Vertex state
    if (descriptor.stage == engines::ShaderStage::VertexB) {
        for (const auto& vertex_attrib_state :
             descriptor.state.vertex_attrib_states) {
            hash.add(vertex_attrib_state.is_fixed);
            hash.add(static_cast<u32>(vertex_attrib_state.size));
            hash.add(static_cast<u32>(vertex_attrib_state.type));
        }
        hash.add(descriptor.state.fragment_input_mask);
        hash.add(descriptor.state.depth_zero_to_one);
    }

    // Color target data types
    if (descriptor.stage == engines::ShaderStage::Fragment) {
        hash.add(descriptor.state.debug_highlight);
        for (const auto& color_target_data_type :
             descriptor.state.color_target_data_types) {
            hash.add(static_cast<u32>(color_target_data_type));
        }
    }

    return hash.toHashCode();
}

void ShaderCache::destroyElement(ShaderBase* shader) { delete shader; }

} // namespace hydra::hw::tegra_x1::gpu::renderer
