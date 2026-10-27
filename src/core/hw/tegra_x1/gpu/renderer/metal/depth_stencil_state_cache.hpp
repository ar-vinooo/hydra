#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

struct DepthStencilStateDescriptor {
    bool depth_test_enabled;
    bool depth_write_enabled;
    engines::CompareOp depth_compare_op;
    bool stencil_enabled{false};
    StencilFaceState stencil_front{};
    StencilFaceState stencil_back{};
};

class DepthStencilStateCache
    : public CacheBase<DepthStencilStateCache, MTL::DepthStencilState*,
                       DepthStencilStateDescriptor, true> {
  public:
    explicit DepthStencilStateCache(MTL::Device* device_) : device{device_} {}

    void destroy() {}

    MTL::DepthStencilState*
    create(const DepthStencilStateDescriptor& descriptor);
    void update([[maybe_unused]] MTL::DepthStencilState* depth_stencil_state) {}
    static u32 hash(const DepthStencilStateDescriptor& descriptor);
    static bool equals(const DepthStencilStateDescriptor& a,
                       const DepthStencilStateDescriptor& b);

    static void destroyElement(MTL::DepthStencilState* depth_stencil_state);

  private:
    MTL::Device* device;

  public:
    // ABI-stable implementations selected by the live module.
    MTL::DepthStencilState*
    createHot0(const DepthStencilStateDescriptor& descriptor);
    static u32 hashHot1(const DepthStencilStateDescriptor& descriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
