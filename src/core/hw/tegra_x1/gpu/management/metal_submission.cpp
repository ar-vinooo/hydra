#include "core/hw/tegra_x1/gpu/management/metal_submission.hpp"
#include "core/hw/tegra_x1/gpu/capture/guest_events.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"

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
    auto& events = capture::GuestEvents::instance();
    if (events.enabled()) {
        static std::atomic<u64> next_capture_submission{1};
        const auto id =
            next_capture_submission.fetch_add(1, std::memory_order_relaxed);
        const auto origin = capture::currentGuestEventOrigin();
        const auto native = reinterpret_cast<uptr>(command_buffer);
        const auto wrapper = reinterpret_cast<uptr>(this);
        events.event("native_submit", origin, native, 0, id, wrapper);
        command_buffer->addCompletedHandler([id, origin, native, wrapper](
                                                MTL::CommandBuffer* completed) {
            capture::GuestEvents::instance().event(
                "native_complete", origin, native, 0, id,
                completed->status() == MTL::CommandBufferStatusCompleted ? 1
                                                                         : 0,
                wrapper);
        });
    }
    commitManagedSubmission(command_buffer, completions, std::move(retained_resources));
    retained_resources.clear();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
