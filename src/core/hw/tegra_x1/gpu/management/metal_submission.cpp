#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/management/metal_submission.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

void commitManagedSubmission(
    MTL::CommandBuffer* commands, const std::shared_ptr<CompletionQueue>& completions,
    std::unordered_map<const void*, std::shared_ptr<void>> resources) {
    // Tests and standalone command buffers share a fallback budget.
    static execution::WorkManager fallback{{64, 0}};
    auto work = (completions ? completions->submissions : fallback).admit();
    ASSERT(work && work->start(), MetalRenderer,
           "Cannot submit to a stopped GPU work manager");
    commands->addCompletedHandler(
        [work = std::move(work), resources = std::move(resources)](
            MTL::CommandBuffer* completed) mutable {
            resources.clear();
            work->finish(completed->status() == MTL::CommandBufferStatusCompleted);
        });
    commands->commit();
}

void CommandBuffer::attachGpuCompletionCallbacks() {
    if (!gpu_completion_callbacks.empty()) {
        command_buffer->addCompletedHandler(
            [callbacks = std::move(gpu_completion_callbacks)](
                MTL::CommandBuffer* completed) {
                const bool succeeded =
                    completed->status() == MTL::CommandBufferStatusCompleted;
                for (const auto& callback : callbacks)
                    callback(succeeded);
            });
        gpu_completion_callbacks.clear();
    }
}

void CommandBuffer::commitSubmission() {
    attachGpuCompletionCallbacks();
    commitManagedSubmission(command_buffer, completions, std::move(retained_resources));
    retained_resources.clear();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
