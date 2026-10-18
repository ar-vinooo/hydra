#pragma once

#include "core/hw/tegra_x1/gpu/engines/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class ShaderBase;
class IRenderer;

struct GuestShaderState {
    u32 program_offset{0};
    std::map<std::pair<u32, u32>, u32> branch_constants;
    std::map<u32, TextureFormat> buffer_texture_formats;
    // Sampled (non-buffer) textures with integer formats
    std::map<u32, TextureFormat> integer_texture_formats;
    std::map<u32, f32> texture_lod_bias;
    // Textures whose sampler border handling Metal cannot express (custom
    // border colors, integer borders, mirror to border). The shader samples
    // them with its own samplers and blends the border in
    std::map<u32, SamplerDescriptor> border_samplers;
    std::vector<u32> unnormalized_textures;
    // User inputs read by the fragment shader, which the vertex shader must
    // write (bit per location)
    u32 fragment_input_mask{0};
    // DEBUG: output a solid color from the fragment shader
    bool debug_highlight{false};
    uint32_t visual_output{0};
    uint32_t visual_input{0};
    // Clip space Z is already in the [0, 1] range
    bool depth_zero_to_one{false};
    // Output attribute address the vertex shader exports as the render target
    // layer, in place of an unsupported geometry shader that copies it (0 if
    // none)
    u32 layer_output_addr{0};
    // Clip distances enabled by the guest (bit per distance)
    u32 clip_distance_mask{0};
    // Points are drawn: the vertex shader always exports a point size, its
    // own when program point size is enabled, otherwise point_size
    bool point_list{false};
    bool program_point_size{false};
    f32 point_size{1.0f};
    // The window origin is lower left (SR_Y_DIRECTION reads -1.0)
    bool y_negate{false};
    // Size of the window the vertex shader outputs positions in (0 when it
    // outputs clip space)
    u32 window_space_width{0};
    u32 window_space_height{0};
    // Point sprite coordinates have their origin at the bottom
    bool point_coord_origin_bottom{false};
    // Fixed function alpha test of render target 0 (Invalid when disabled)
    engines::CompareOp alpha_test_func{engines::CompareOp::Invalid};
    f32 alpha_test_ref{0.0f};
    // Render target 0 blends with the second color output (SRC1 factors): the
    // shader's output 1 is that color
    bool dual_source_blending{false};
    engines::VertexAttribState vertex_attrib_states[VERTEX_ATTRIB_COUNT];
    MTL::DataType color_target_data_types[COLOR_TARGET_COUNT] = {
        MTL::DataTypeNone};
    // Compute
    u32 shared_memory_size{0};
    std::map<u32, TextureFormat> image_formats;
};

struct GuestShaderDescriptor {
    engines::ShaderStage stage;
    uptr code_ptr;
    uptr gpu_addr;
    GuestShaderState state;
    std::function<std::optional<u32>(u32, u32)> read_const_word;
};

class ShaderCache
    : public CacheBase<ShaderCache, ShaderBase*, GuestShaderDescriptor> {
  public:
    explicit ShaderCache(IRenderer& renderer_) : renderer{renderer_} {}

    void destroy() {}

    ShaderBase* find(const GuestShaderDescriptor& descriptor);
    ShaderBase* create(const GuestShaderDescriptor& descriptor);
    void update([[maybe_unused]] ShaderBase* shader) {}
    static u32 hash(const GuestShaderDescriptor& descriptor);

    static void destroyElement(ShaderBase* shader);

  private:
    IRenderer& renderer;
    std::map<u32, std::map<std::pair<u32, u32>, u32>> branch_dependencies;

  public:
    // ABI-stable implementations selected by the live module.
    ShaderBase* findHot0(const GuestShaderDescriptor& descriptor);
    ShaderBase* createHot1(const GuestShaderDescriptor& descriptor);
    static u32 hashHot2(const GuestShaderDescriptor& descriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
