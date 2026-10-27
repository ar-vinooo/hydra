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
    // Null while compiling in the background (see BinaryArchiveCache). With a
    // geometry shader, the passthrough draw (see geometry.hpp).
    MTL::RenderPipelineState* getPipeline() const {
        return state->pipeline.load(std::memory_order_acquire);
    }
    bool isCompiling() const {
        return state->compiling.load(std::memory_order_acquire);
    }
    // The vertex shader writing its output records, rasterization off
    MTL::RenderPipelineState* getCapturePipeline() const {
        return capture_pipeline;
    }
    // The geometry shader as a vertex function, rasterization off
    MTL::RenderPipelineState* getGeometryPipeline() const {
        return geometry_pipeline;
    }
    bool hasGeometry() const { return geometry_pipeline != nullptr; }
    // Tessellation (tessellation.hpp): the control shader and the factor
    // function, rasterization off; getPipeline() tessellates
    MTL::RenderPipelineState* getTessControlPipeline() const {
        return tess_control_pipeline;
    }
    MTL::RenderPipelineState* getTessFactorPipeline() const {
        return tess_factor_pipeline;
    }
    bool hasTessellation() const { return tess_factor_pipeline != nullptr; }

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
    MTL::RenderPipelineState* capture_pipeline{nullptr};
    MTL::RenderPipelineState* geometry_pipeline{nullptr};
    MTL::RenderPipelineState* tess_control_pipeline{nullptr};
    MTL::RenderPipelineState* tess_factor_pipeline{nullptr};
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
