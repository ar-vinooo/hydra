#pragma once

#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"
#include <memory>

namespace hydra::hw::tegra_x1::gpu::renderer {

struct BufferView {
  public:
    BufferView() = default;
    explicit BufferView(BufferBase* base_, u64 offset_ = 0,
                        u64 size_ = invalid<u64>())
        : base{base_}, offset{offset_}, size{size_} {
        if (size == invalid<u64>())
            size = base->getSize() - offset;
    }

    bool isValid() const { return base != nullptr; }

    explicit BufferView(std::shared_ptr<BufferBase> owner_, u64 offset_ = 0,
                        u64 size_ = invalid<u64>())
        : BufferView(owner_.get(), offset_, size_) {
        owner = std::move(owner_);
        if (base)
            base->setResourceOwner(owner);
    }

    uptr getPtr() const { return base->getPtr() + offset; }

    // Copying
    void copyFrom(const uptr data, u64 size_ = invalid<u64>()) {
        if (size_ == invalid<u64>())
            size_ = size - offset;
        base->copyFrom(data, offset, size_);
    }
    void copyFrom(ICommandBuffer* command_buffer, const BufferView& src,
                  u64 size_ = invalid<u64>()) {
        if (size_ == invalid<u64>())
            size_ = std::min(src.size - src.offset, size - offset);
        base->copyFrom(command_buffer, src.base, offset, src.offset, size_);
    }
    void copyFrom(ICommandBuffer* command_buffer, ITextureView* src,
                  const MTL::Origin src_origin, const MTL::Size src_size,
                  const ztd::Range<u32> src_levels,
                  const ztd::Range<u32> src_layers) {
        base->copyFrom(command_buffer, src, src_origin, src_size, src_levels,
                       src_layers, offset);
    }

  protected:
    std::shared_ptr<BufferBase> owner;
    BufferBase* base{nullptr};
    u64 offset{0};
    u64 size{0};

  public:
    GETTER(base, getBase);
    GETTER(offset, getOffset);
    GETTER(size, getSize);
    CONST_REF_GETTER(owner, getOwner);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
