#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class Texture final : public ITexture {
  public:
    Texture(MTL::Device* device, const TextureDescriptor& descriptor);
    ~Texture() override;

    ITextureView*
    createView(const TextureViewDescriptor& view_descriptor) override;

    // Copying
    void copyBufferBytes(ICommandBuffer* command_buffer, const ITexture* src,
                         u64 src_offset, u64 dst_offset, u64 size) override;
    void copyFrom(ICommandBuffer* command_buffer, const BufferBase* src,
                  const ztd::Range<u32> dst_levels,
                  const ztd::Range<u32> dst_layers) override;
    bool copyFrom(ICommandBuffer* command_buffer, const ITexture* src,
                  const u32 src_level, const u32 src_layer, const u32 dst_level,
                  const u32 dst_layer, const u32 level_count,
                  const u32 layer_count) override;
    bool copyFrom(ICommandBuffer* command_buffer, const ITexture* src,
                  const MTL::Origin src_origin, const u32 src_level,
                  const u32 src_layer, const MTL::Origin dst_origin,
                  const u32 dst_level, const u32 dst_layer, const MTL::Size size,
                  const u32 layer_count) override;

  private:
    MTL::Texture* texture;
    MTL::Buffer* buffer_backing = nullptr;
    MTL::ComputePipelineState* byte_copy_pipeline = nullptr;

    void copyDepthStencilFromBuffer(MTL::BlitCommandEncoder* encoder,
                                    const u8* src, const MTL::Size dims,
                                    const u32 layer, const u32 level);

  public:
    GETTER(texture, getTexture);
    GETTER(buffer_backing, getBufferBacking);

  public:
    // ABI-stable implementations selected by the live module.
    ITextureView* createViewHot0(const TextureViewDescriptor& view_descriptor);
    void copyBufferBytesHot1(ICommandBuffer* command_buffer,
                             const ITexture* src, u64 src_offset,
                             u64 dst_offset, u64 size);
    void copyFromHot2(ICommandBuffer* command_buffer, const BufferBase* src,
                      const ztd::Range<u32> dst_levels,
                      const ztd::Range<u32> dst_layers);
    void copyDepthStencilFromBufferHot3(MTL::BlitCommandEncoder* encoder,
                                        const u8* src, const MTL::Size dims,
                                        const u32 layer, const u32 level);
    bool copyFromHot4(ICommandBuffer* command_buffer, const ITexture* src,
                      const u32 src_level, const u32 src_layer,
                      const u32 dst_level, const u32 dst_layer,
                      const u32 level_count, const u32 layer_count);
    bool copyFromHot5(ICommandBuffer* command_buffer, const ITexture* src,
                      const MTL::Origin src_origin, const u32 src_level,
                      const u32 src_layer, const MTL::Origin dst_origin,
                      const u32 dst_level, const u32 dst_layer,
                      const MTL::Size size, const u32 layer_count);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
