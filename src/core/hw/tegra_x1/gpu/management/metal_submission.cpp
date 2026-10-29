#include "core/hw/tegra_x1/gpu/management/metal_submission.hpp"
#include "core/hw/tegra_x1/gpu/capture/guest_events.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

namespace {

auto admitSubmission(const std::shared_ptr<CompletionQueue>& completions) {
    // Tests and standalone command buffers share a fallback budget.
    static execution::WorkManager fallback{{64, 0}};
    auto work = (completions ? completions->submissions : fallback).admit();
    ASSERT(work && work->start(), MetalRenderer,
           "Cannot submit to a stopped GPU work manager");
    return work;
}

} // namespace

void commitManagedSubmission(
    MTL::CommandBuffer* commands, const std::shared_ptr<CompletionQueue>& completions,
    std::unordered_map<const void*, std::shared_ptr<void>> resources) {
    auto work = admitSubmission(completions);
    commands->addCompletedHandler(
        [work = std::move(work), resources = std::move(resources)](
            MTL::CommandBuffer* completed) mutable {
            tls_releasing_completed_resources =
                completed->status() == MTL::CommandBufferStatusCompleted;
            resources.clear();
            tls_releasing_completed_resources = false;
            work->finish(completed->status() == MTL::CommandBufferStatusCompleted);
        });
    commands->commit();
}

void commitManagedSubmission(
    CommandBuffer& commands, const std::shared_ptr<CompletionQueue>& completions,
    std::unordered_map<const void*, std::shared_ptr<void>> resources) {
    auto work = admitSubmission(completions);
    commands.addCompletedHandler(
        [work = std::move(work), resources = std::move(resources)](
            const GpuCompletion& completion) mutable {
            tls_releasing_completed_resources = completion.succeeded;
            resources.clear();
            tls_releasing_completed_resources = false;
            work->finish(completion.succeeded);
        });
    commands.commitNative();
}

void CommandBuffer::attachGpuCompletionCallbacks() {
    if (!gpu_completion_callbacks.empty()) {
        addCompletedHandler(
            [callbacks = std::move(gpu_completion_callbacks)](
                const GpuCompletion& completion) {
                for (const auto& callback : callbacks)
                    callback(completion.succeeded);
            });
        gpu_completion_callbacks.clear();
    }
}

void CommandBuffer::attachPassProfiling() {
    if (profiled_passes.empty())
        return;
    auto* profiler = GpuPassProfiler::get(getDevice());
    addCompletedHandler([profiler, passes = std::move(profiled_passes)](
                            const GpuCompletion& completion) {
        profiler->resolve(passes, completion.succeeded);
    });
    profiled_passes.clear();
}

void CommandBuffer::commitSubmission() {
    // The draw queue may still encode and retain resources into this buffer
    syncDrawQueue();
    attachGpuCompletionCallbacks();
    attachPassProfiling();
    render_pass_gap = 3;
    auto& events = capture::GuestEvents::instance();
    if (events.enabled()) {
        static std::atomic<u64> next_capture_submission{1};
        const auto id =
            next_capture_submission.fetch_add(1, std::memory_order_relaxed);
        const auto origin = capture::currentGuestEventOrigin();
        const auto native =
            mtl4_command_buffer != nullptr
                ? reinterpret_cast<uptr>(mtl4_command_buffer)
                : reinterpret_cast<uptr>(command_buffer);
        const auto wrapper = reinterpret_cast<uptr>(this);
        events.event("native_submit", origin, native, 0, id, wrapper);
        addCompletedHandler([id, origin, native,
                             wrapper](const GpuCompletion& completion) {
            capture::GuestEvents::instance().event(
                "native_complete", origin, native, 0, id,
                completion.succeeded ? 1 : 0, wrapper);
        });
    }
    commitManagedSubmission(*this, completions, takeRetainedResources());
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
