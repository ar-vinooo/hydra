#pragma once

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::cpu {
class IMmu;
}

namespace hydra::hw::tegra_x1::gpu::renderer {

class SamplerBase;
class IRenderer;

class SamplerCache
    : public CacheBase<SamplerCache, SamplerBase*, SamplerDescriptor, true, true> {
  public:
    explicit SamplerCache(IRenderer& renderer_) : renderer{renderer_} {}

    void destroy() {}

    SamplerBase* create(const SamplerDescriptor& descriptor);
    void update([[maybe_unused]] SamplerBase* sampler) {}
    static u32 hash(const SamplerDescriptor& descriptor);
    static bool equals(const SamplerDescriptor& a, const SamplerDescriptor& b);

    static void destroyElement(SamplerBase* sampler);

    // The draw encoding thread looks samplers up too (see DrawQueue)
    std::mutex& getMutex() { return mutex; }

  private:
    IRenderer& renderer;
    std::mutex mutex;

  public:
    // ABI-stable implementations selected by the live module.
    SamplerBase* createHot0(const SamplerDescriptor& descriptor);
    static u32 hashHot1(const SamplerDescriptor& descriptor);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
