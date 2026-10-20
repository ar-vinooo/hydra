#pragma once

#include "core/hw/tegra_x1/gpu/resource/resource_lifetime.hpp"

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class ICommandBuffer;
class BufferBase;
class ITexture;

class ITextureView : public resource::TrackedResource {
  public:
    ITextureView(ITexture* base_, const TextureViewDescriptor& descriptor_)
        : base{base_}, descriptor{descriptor_},
          base_owner{base_ ? base_->getResourceOwner() : nullptr} {}
    virtual ~ITextureView() = default;

    // Copying
    void copyFrom(ICommandBuffer* command_buffer, const BufferBase* src,
                  const ztd::Range<u32> dst_levels,
                  const ztd::Range<u32> dst_layers);
    void copyFrom(ICommandBuffer* command_buffer, const BufferBase* src);
    void copyFrom(ICommandBuffer* command_buffer, const ITextureView* src,
                  const u32 src_level, const u32 src_layer, const u32 dst_level,
                  const u32 dst_layer, const u32 level_count,
                  const u32 layer_count);

  protected:
    ITexture* base;
    const TextureViewDescriptor descriptor;
    std::shared_ptr<void> base_owner;

  public:
    GETTER(base, getBase);
    CONST_REF_GETTER(descriptor, getDescriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
