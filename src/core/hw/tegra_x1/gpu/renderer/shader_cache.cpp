#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"

#include "core/hw/tegra_x1/gpu/engines/3d.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.hpp"

#include <atomic>
#include <cstdlib>
#include <fstream>

namespace hydra::hw::tegra_x1::gpu::renderer {

ShaderBase* ShaderCache::find(const GuestShaderDescriptor& descriptor) {
    auto actual = descriptor;
    const auto base_hash = hash(descriptor);
    if (const auto it = branch_dependencies.find(base_hash);
        it != branch_dependencies.end()) {
        for (const auto& [location, previous] : it->second) {
            const auto value = descriptor.read_const_word
                                   ? descriptor.read_const_word(location.first,
                                                                location.second)
                                   : std::nullopt;
            (void)previous;
            if (!value) {
                LOG_ERROR(ShaderDecompiler, "Missing branch table c{}[{}]",
                          location.first, location.second);
                return nullptr;
            }
            actual.state.branch_constants[location] = *value;
        }
    }
    return CacheBase<ShaderCache, ShaderBase*, GuestShaderDescriptor>::find(
        actual);
}

ShaderBase* ShaderCache::create(const GuestShaderDescriptor& descriptor) {
    ShaderDescriptor host_descriptor{};
    host_descriptor.type = engines::toRendererShaderType(descriptor.stage);

    // Decompile
    // Gameplay shaders can exceed 4 KiB, and guest-contiguous code may span
    // physically separate pages. Decode an owned, bounded snapshot.
    auto code = tls_crnt_gmmu->readMappedBytes(descriptor.gpu_addr, 0x10000);
    const usize minimum_size =
        descriptor.stage == engines::ShaderStage::Compute ? 8 : 80 + 8;
    if (code.size() < minimum_size) {
        LOG_ERROR(ShaderDecompiler, "Shader at {:#x} has only {} mapped bytes",
                  descriptor.gpu_addr, code.size());
        return nullptr;
    }
    if (const char* dump_dir = std::getenv("HYDRA_DUMP_SHADERS")) {
        static std::atomic<u32> input_index{0};
        std::ofstream out(
            fmt::format("{}/gpu_{:#x}_{}_{:04}.bin", dump_dir,
                        descriptor.gpu_addr, host_descriptor.type,
                        input_index++),
            std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()),
                  static_cast<std::streamsize>(code.size()));
    }
    ztd::io::MemoryStream code_stream(code);
    shader_decomp::decompile(
        code_stream, host_descriptor.type, descriptor.state,
        host_descriptor.backend, host_descriptor.code,
        host_descriptor.resource_mapping, descriptor.read_const_word);
    auto base_descriptor = descriptor;
    base_descriptor.state.branch_constants.clear();
    branch_dependencies[hash(base_descriptor)] =
        host_descriptor.resource_mapping.branch_constants;
    return renderer.createShader(host_descriptor);
}

u32 ShaderCache::hash(const GuestShaderDescriptor& descriptor) {
    // Hash enum values as words rather than feeding each byte separately.
    // Keep explicit fields: hashing whole structs would include padding.
    ztd::hash::XxHash32 hash;
    hash.add(static_cast<u32>(descriptor.stage));
    hash.add(descriptor.code_ptr);
    for (const auto& [location, value] : descriptor.state.branch_constants) {
        hash.add(location.first);
        hash.add(location.second);
        hash.add(value);
    }
    for (const auto& [index, format] :
         descriptor.state.buffer_texture_formats) {
        hash.add(index);
        hash.add(static_cast<u32>(format));
    }
    if (descriptor.stage == engines::ShaderStage::Compute) {
        hash.add(descriptor.state.shared_memory_size);
        for (const auto& [index, format] : descriptor.state.image_formats) {
            hash.add(index);
            hash.add(static_cast<u32>(format));
        }
    }

    // Take a few samples from the code
    for (usize offset = 80; offset < 224; offset += 18) {
        const auto addr = descriptor.gpu_addr + offset;
        uptr base;
        const auto* as = tls_crnt_gmmu->findAddrImplRef(addr, base);
        if (as == nullptr || as->ptr == 0)
            break;
        hash.add(*reinterpret_cast<const u8*>(as->ptr + addr - base));
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
