#pragma once

#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class Buffer final : public BufferBase {
  public:
    Buffer(MTL::Device* device, u64 size);
    explicit Buffer(MTL::Buffer* buffer_);
    ~Buffer() override;

    uptr getPtr() const override {
        return reinterpret_cast<uptr>(buffer->contents());
    }

    // Copying
    void copyFrom(ICommandBuffer* command_buffer, ITextureView* src,
                  const MTL::Origin src_origin, const MTL::Size src_size,
                  const ztd::Range<u32> src_levels,
                  const ztd::Range<u32> src_layers, u64 dst_offset) override;

  private:
    MTL::Buffer* buffer;

    // Copying
    void copyFromImpl(const uptr data, u64 dst_offset, u64 size_) override;
    void copyFromImpl(ICommandBuffer* command_buffer, BufferBase* src,
                      u64 dst_offset, u64 src_offset, u64 size_) override;

  public:
    GETTER(buffer, getBuffer);

  public:
    // ABI-stable implementations selected by the live module.
    void copyFromHot0(ICommandBuffer* command_buffer, ITextureView* src,
                      const MTL::Origin src_origin, const MTL::Size src_size,
                      const ztd::Range<u32> src_levels,
                      const ztd::Range<u32> src_layers, u64 dst_offset);
    void copyFromImplHot1(const uptr data, u64 dst_offset, u64 size_);
    void copyFromImplHot2(ICommandBuffer* command_buffer, BufferBase* src,
                          u64 dst_offset, u64 src_offset, u64 size_);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
