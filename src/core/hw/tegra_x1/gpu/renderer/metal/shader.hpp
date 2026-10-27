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
    MTL::ArgumentEncoder* sampler_encoder{nullptr};
    std::string cache_key;
    // Key of the MSL source in ShaderLibraryCache (empty for other backends)
    std::string library_key;

  public:
    GETTER(function, getFunction);
    GETTER(sampler_encoder, getSamplerEncoder);
    const std::string& getCacheKey() const { return cache_key; }
    const std::string& getLibraryKey() const { return library_key; }
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
