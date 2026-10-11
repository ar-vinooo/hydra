#pragma once

#include "core/hw/tegra_x1/gpu/engines/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class ShaderBase;
class IRenderer;

struct GuestShaderState {
    std::map<std::pair<u32, u32>, u32> branch_constants;
    std::map<u32, TextureFormat> buffer_texture_formats;
    // User inputs read by the fragment shader, which the vertex shader must
    // write (bit per location)
    u32 fragment_input_mask{0};
    // DEBUG: output a solid color from the fragment shader
    bool debug_highlight{false};
    // Clip space Z is already in the [0, 1] range
    bool depth_zero_to_one{false};
    engines::VertexAttribState vertex_attrib_states[VERTEX_ATTRIB_COUNT];
    ColorDataType color_target_data_types[COLOR_TARGET_COUNT] = {
        ColorDataType::Invalid};
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
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
