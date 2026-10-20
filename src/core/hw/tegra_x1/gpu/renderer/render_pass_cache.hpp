#pragma once

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class RenderPassBase;
class IRenderer;

class RenderPassCache
    : public CacheBase<RenderPassCache, RenderPassBase*, RenderPassDescriptor,
                       true, true> {
  public:
    explicit RenderPassCache(IRenderer& renderer_) : renderer{renderer_} {}

    void destroy() {}

    RenderPassBase* create(const RenderPassDescriptor& descriptor);
    void update([[maybe_unused]] RenderPassBase* render_pass) {}
    static u32 hash(const RenderPassDescriptor& descriptor);
    static bool equals(const RenderPassDescriptor& a,
                       const RenderPassDescriptor& b);

    static void destroyElement(RenderPassBase* render_pass);

  private:
    IRenderer& renderer;

  public:
    // ABI-stable implementations selected by the live module.
    RenderPassBase* createHot0(const RenderPassDescriptor& descriptor);
    static u32 hashHot1(const RenderPassDescriptor& descriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
