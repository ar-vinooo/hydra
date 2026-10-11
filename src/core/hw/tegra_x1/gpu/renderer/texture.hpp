#pragma once

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class ICommandBuffer;
class BufferBase;
class ITextureView;

class ITexture {
  public:
    explicit ITexture(const TextureDescriptor& descriptor_)
        : descriptor{descriptor_} {}
    virtual ~ITexture() = default;

    virtual ITextureView*
    createView(const TextureViewDescriptor& view_descriptor) = 0;

    // Copying
    // Raw bytes shared by buffer textures, including different texel formats.
    virtual void copyBufferBytes(ICommandBuffer* command_buffer,
                                 const ITexture* src, u64 src_offset,
                                 u64 dst_offset, u64 size) = 0;
    virtual void copyFrom(ICommandBuffer* command_buffer, const BufferBase* src,
                          const ztd::Range<u32> dst_levels,
                          const ztd::Range<u32> dst_layers) = 0;
    void copyFrom(ICommandBuffer* command_buffer, const BufferBase* src) {
        copyFrom(command_buffer, src,
                 ztd::Range<u32>(0, descriptor.level_count),
                 ztd::Range<u32>(0, descriptor.layer_count));
    }
    virtual void copyFrom(ICommandBuffer* command_buffer, const ITexture* src,
                          const u32 src_level, const u32 src_layer,
                          const u32 dst_level, const u32 dst_layer,
                          const u32 level_count, const u32 layer_count) = 0;
    virtual void copyFrom(ICommandBuffer* command_buffer, const ITexture* src,
                          const uint3 src_origin, const u32 src_level,
                          const u32 src_layer, const uint3 dst_origin,
                          const u32 dst_level, const u32 dst_layer,
                          const uint3 size, const u32 layer_count) = 0;

  protected:
    const TextureDescriptor descriptor;

  public:
    CONST_REF_GETTER(descriptor, getDescriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
