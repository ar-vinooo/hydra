#pragma once

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

class PipelineBase;
class IRenderer;

class PipelineCache
    : public CacheBase<PipelineCache, PipelineBase*, PipelineDescriptor,
                       true> {
  public:
    explicit PipelineCache(IRenderer& renderer_) : renderer{renderer_} {}

    void destroy() {}

    PipelineBase* create(const PipelineDescriptor& descriptor);
    void update([[maybe_unused]] PipelineBase* pipeline) {}
    static u32 hash(const PipelineDescriptor& descriptor);
    static bool equals(const PipelineDescriptor& a,
                       const PipelineDescriptor& b);

    static void destroyElement(PipelineBase* pipeline);

  private:
    IRenderer& renderer;

  public:
    // ABI-stable implementations selected by the live module.
    PipelineBase* createHot0(const PipelineDescriptor& descriptor);
    static u32 hashHot1(const PipelineDescriptor& descriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
