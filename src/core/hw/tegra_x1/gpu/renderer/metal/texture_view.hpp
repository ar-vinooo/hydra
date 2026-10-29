#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class TextureView final : public ITextureView {
  public:
    TextureView(Texture* base, const TextureViewDescriptor& descriptor);
    ~TextureView() override;

    // Recreated after the base replaced its native texture (renamed buffer
    // textures), which only happens on the thread encoding commands
    MTL::Texture* getTexture() const {
        if (base_generation != getBaseImpl()->getGeneration())
            refresh();
        return texture;
    }

  private:
    mutable MTL::Texture* texture;
    mutable u32 base_generation;

    const Texture* getBaseImpl() const;
    MTL::Texture* createNative() const;
    void refresh() const;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
