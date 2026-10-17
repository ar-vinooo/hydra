#include "core/hw/tegra_x1/gpu/renderer/pipeline_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/gmmu.hpp"
#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace hydra;
using namespace hydra::hw::tegra_x1::gpu;
using namespace hydra::hw::tegra_x1::gpu::renderer;
// Pre-optimization hash implementations retained only for comparison.
u32 referenceShaderCacheHash(const GuestShaderDescriptor& descriptor) {
    ztd::hash::XxHash32 hash;
    hash.add(descriptor.stage);
    hash.add(descriptor.code_ptr);
    for (const auto& [index, format] :
         descriptor.state.buffer_texture_formats) {
        hash.add(index);
        hash.add(format);
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
            hash.add(vertex_attrib_state.size);
            hash.add(vertex_attrib_state.type);
        }
        hash.add(descriptor.state.fragment_input_mask);
        hash.add(descriptor.state.depth_zero_to_one);
    }

    // Color target data types
    if (descriptor.stage == engines::ShaderStage::Fragment) {
        hash.add(descriptor.state.debug_highlight);
        for (const auto& color_target_data_type :
             descriptor.state.color_target_data_types) {
            hash.add(color_target_data_type);
        }
    }

    return hash.toHashCode();
}

u32 referencePipelineCacheHash(const PipelineDescriptor& descriptor) {
    ztd::hash::XxHash32 hash;

    // Shaders
    // TODO: use the shader hash instead of the pointer?
    hash.add(descriptor.shaders[0]);
    hash.add(descriptor.shaders[1]);

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
        hash.add(color_target_state.format);
        hash.add(color_target_state.write_mask);
        hash.add(color_target_state.blend_enabled);
        if (color_target_state.blend_enabled) {
            hash.add(color_target_state.rgb_op);
            hash.add(color_target_state.src_rgb_factor);
            hash.add(color_target_state.dst_rgb_factor);
            hash.add(color_target_state.alpha_op);
            hash.add(color_target_state.src_alpha_factor);
            hash.add(color_target_state.dst_alpha_factor);
        }
    }

    hash.add(descriptor.depth_stencil_format);

    return hash.toHashCode();
}

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        // Vertex attribute register layout: UE4 vertex colors (B8G8R8A8)
        // set the BGRA flag in bit 31
        {
            const auto attrib = std::bit_cast<engines::VertexAttribState>(
                u32{0x80000000} | (u32{2} << 27) | (u32{0x0a} << 21) |
                (u32{0x24} << 7) | u32{3});
            check(attrib.bgra, "Vertex attribute BGRA flag is bit 31");
            check(attrib.type == engines::VertexAttribType::Unorm &&
                      attrib.size == engines::VertexAttribSize::_4x8 &&
                      attrib.offset == 0x24 && attrib.buffer_id == 3 &&
                      !attrib.is_fixed,
                  "Vertex attribute fields decode");
            check(!std::bit_cast<engines::VertexAttribState>(u32{0x40000000})
                       .bgra,
                  "Vertex attribute bit 30 is not BGRA");
        }

        std::array<u8, 4096> code{};
        GMmu gpu_mmu(nullptr);
        gpu_mmu.map(0x10000, {reinterpret_cast<uptr>(code.data()), code.size()});
        tls_crnt_gmmu = &gpu_mmu;
        GuestShaderDescriptor vertex{};
        vertex.stage = engines::ShaderStage::VertexB;
        vertex.code_ptr = reinterpret_cast<uptr>(code.data());
        vertex.gpu_addr = 0x10000;
        const auto shader_base = ShaderCache::hash(vertex);
        for (u32 i = 0; i < VERTEX_ATTRIB_COUNT; ++i) {
            auto changed = vertex;
            changed.state.vertex_attrib_states[i].size =
                engines::VertexAttribSize::_4x32;
            check(ShaderCache::hash(changed) != shader_base,
                  "Vertex size remains in shader key");
            changed = vertex;
            changed.state.vertex_attrib_states[i].type =
                engines::VertexAttribType::Float;
            check(ShaderCache::hash(changed) != shader_base,
                  "Vertex type remains in shader key");
            changed = vertex;
            changed.state.vertex_attrib_states[i].is_fixed = true;
            check(ShaderCache::hash(changed) != shader_base,
                  "Fixed vertex input remains in shader key");
        }
        auto changed = vertex;
        changed.state.depth_zero_to_one = true;
        check(ShaderCache::hash(changed) != shader_base,
              "Depth convention remains in shader key");
        changed = vertex;
        changed.state.fragment_input_mask = 0x80000000;
        check(ShaderCache::hash(changed) != shader_base,
              "Fragment inputs remain in shader key");
        code[80] = 7;
        check(ShaderCache::hash(vertex) != shader_base,
              "Code sample changes shader key");
        code[80] = 0;
        auto fragment = vertex;
        fragment.stage = engines::ShaderStage::Fragment;
        const auto fragment_base = ShaderCache::hash(fragment);
        for (u32 i = 0; i < COLOR_TARGET_COUNT; ++i) {
            changed = fragment;
            changed.state.color_target_data_types[i] = ColorDataType::Float;
            check(ShaderCache::hash(changed) != fragment_base,
                  "Color type remains in fragment key");
        }
        changed = fragment;
        changed.state.buffer_texture_formats[4] = TextureFormat::R8Unorm;
        check(ShaderCache::hash(changed) != fragment_base,
              "Buffer texture format remains in key");
        PipelineDescriptor pipeline{};
        const auto pipeline_base = PipelineCache::hash(pipeline);
        for (u32 i = 0; i < COLOR_TARGET_COUNT; ++i) {
            auto other = pipeline;
            other.color_target_states[i].format = TextureFormat::RGBA8Unorm;
            check(PipelineCache::hash(other) != pipeline_base,
                  "Target format remains in pipeline key");
            other = pipeline;
            other.color_target_states[i].write_mask =
                engines::ColorWriteMask::Red;
            check(PipelineCache::hash(other) != pipeline_base,
                  "Write mask remains in pipeline key");
        }
        auto other = pipeline;
        other.depth_stencil_format = TextureFormat::Z32Float;
        check(PipelineCache::hash(other) != pipeline_base,
              "Depth format remains in pipeline key");
        // Separate calls prevent optimizing away repeated real hash work.
        const auto benchmark = [&](const char* label, auto hash,
                                   const auto& descriptor) {
            constexpr u32 count = 100000;
            u32 checksum = 0;
            const auto start = std::chrono::steady_clock::now();
            for (u32 i = 0; i < count; ++i)
                checksum ^= hash(descriptor) + i;
            const auto ns = std::chrono::duration<double, std::nano>(
                                std::chrono::steady_clock::now() - start)
                                .count() /
                            count;
            std::cout << label << ": " << ns << " ns/hash, checksum "
                      << checksum << '\n';
        };
        benchmark("Vertex hash before", referenceShaderCacheHash, vertex);
        benchmark("Vertex hash after", ShaderCache::hash, vertex);
        benchmark("Pipeline hash before", referencePipelineCacheHash, pipeline);
        benchmark("Pipeline hash after", PipelineCache::hash, pipeline);
        std::cout << "PASS: shader and pipeline cache key discrimination\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
