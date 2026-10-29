#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_base.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class ShaderLibraryCache;

class Shader final : public ShaderBase {
  public:
    Shader(MTL::Device* device, const ShaderDescriptor& descriptor,
           ShaderLibraryCache& library_cache);
    ~Shader() override;

  private:
    MTL::Function* function{nullptr};
    std::string cache_key;
    // Key of the MSL source in ShaderLibraryCache (empty for other backends)
    std::string library_key;

  public:
    GETTER(function, getFunction);
    // The shader reads its samplers from a table at the sampler buffer index
    // instead of sampler arguments (more than 16 textures, see SamplerTable)
    bool usesSamplerTable() const {
        return getDescriptor().resource_mapping.sampler_buffer_index !=
               invalid<u32>();
    }
    const std::string& getCacheKey() const { return cache_key; }
    const std::string& getLibraryKey() const { return library_key; }
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
