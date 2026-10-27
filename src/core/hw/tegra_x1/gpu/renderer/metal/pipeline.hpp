#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/pipeline_base.hpp"

#include <atomic>
#include <memory>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class BinaryArchiveCache;

class Pipeline final : public PipelineBase {
  public:
    Pipeline(MTL::Device* device, const PipelineDescriptor& descriptor,
             BinaryArchiveCache& binary_archive_cache);
    ~Pipeline() override;

    // Getters
    // Null while compiling in the background (see BinaryArchiveCache)
    MTL::RenderPipelineState* getPipeline() const {
        return state->pipeline.load(std::memory_order_acquire);
    }
    bool isCompiling() const {
        return state->compiling.load(std::memory_order_acquire);
    }

  private:
    // Shared with a background compilation, which may outlive the pipeline
    struct State {
        std::atomic<MTL::RenderPipelineState*> pipeline{nullptr};
        std::atomic<bool> compiling{false};
        ~State() {
            if (auto* p = pipeline.load())
                p->release();
        }
    };
    std::shared_ptr<State> state{std::make_shared<State>()};
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
