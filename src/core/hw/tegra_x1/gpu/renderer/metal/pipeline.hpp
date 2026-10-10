#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/pipeline_base.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class BinaryArchiveCache;

class Pipeline final : public PipelineBase {
  public:
    Pipeline(MTL::Device* device, const PipelineDescriptor& descriptor,
             BinaryArchiveCache& binary_archive_cache);
    ~Pipeline() override;

    // Getters
    MTL::RenderPipelineState* getPipeline() const { return pipeline; }

  private:
    MTL::RenderPipelineState* pipeline;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
