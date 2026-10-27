#include "core/hw/tegra_x1/gpu/renderer/metal/gpu_pass_profiler.hpp"

#include "core/hw/tegra_x1/gpu/perf_stats.hpp"

#include <algorithm>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

GpuPassProfiler* GpuPassProfiler::get(MTL::Device* device) {
    static GpuPassProfiler* profiler = [device]() -> GpuPassProfiler* {
        // Sampling stops passes from overlapping on the GPU, which slows the
        // GPU down: it is enabled separately from the other statistics
        if (!PerfStats::isEnabled() ||
            std::getenv("HYDRA_GPU_PASS_TIMES") == nullptr ||
            !device->supportsCounterSampling(
                MTL::CounterSamplingPointAtStageBoundary))
            return nullptr;
        auto* created = new GpuPassProfiler(device);
        if (!created->sample_buffer) {
            delete created;
            return nullptr;
        }
        return created;
    }();
    return profiler;
}

GpuPassProfiler::GpuPassProfiler(MTL::Device* device) {
    MTL::CounterSet* timestamps = nullptr;
    const auto* sets = device->counterSets();
    for (NS::UInteger i = 0; sets && i < sets->count(); i++) {
        auto* set = static_cast<MTL::CounterSet*>(sets->object(i));
        if (set->name()->isEqualToString(MTL::CommonCounterSetTimestamp)) {
            timestamps = set;
            break;
        }
    }
    if (!timestamps) {
        LOG_WARN(MetalRenderer, "GPU timestamps cannot be sampled");
        return;
    }
    NS_STACK_SCOPED auto descriptor =
        MTL::CounterSampleBufferDescriptor::alloc()->init();
    descriptor->setCounterSet(timestamps);
    descriptor->setSampleCount(SAMPLE_COUNT);
    descriptor->setStorageMode(MTL::StorageModeShared);
    NS::Error* error = nullptr;
    sample_buffer = device->newCounterSampleBuffer(descriptor, &error);
    if (!sample_buffer) {
        LOG_WARN(MetalRenderer, "Cannot create a GPU timestamp buffer: {}",
                 error ? error->localizedDescription()->utf8String() : "");
        return;
    }
    // Metal 4 encoders write timestamps into a counter heap instead
    if (device->supportsFamily(MTL::GPUFamilyMetal4)) {
        NS_STACK_SCOPED auto heap_descriptor =
            MTL4::CounterHeapDescriptor::alloc()->init();
        heap_descriptor->setType(MTL4::CounterHeapTypeTimestamp);
        heap_descriptor->setCount(SAMPLE_COUNT);
        NS::Error* heap_error = nullptr;
        counter_heap = device->newCounterHeap(heap_descriptor, &heap_error);
        if (counter_heap) {
            counter_heap->invalidateCounterRange(NS::Range(0, SAMPLE_COUNT));
            const auto frequency = device->queryTimestampFrequency();
            if (frequency != 0)
                ns_per_tick = 1e9 / static_cast<f64>(frequency);
        } else {
            LOG_WARN(MetalRenderer, "Cannot create a GPU timestamp heap: {}",
                     heap_error
                         ? heap_error->localizedDescription()->utf8String()
                         : "");
        }
    }
    for (u32 sample = SAMPLE_COUNT; sample >= 2; sample -= 2)
        free_pairs.push_back(sample - 2);
    LOG_INFO(MetalRenderer, "GPU pass timestamps enabled");
}

u32 GpuPassProfiler::reserve() {
    std::scoped_lock lock(mutex);
    if (free_pairs.empty())
        return invalid<u32>();
    const auto sample = free_pairs.back();
    free_pairs.pop_back();
    return sample;
}

void GpuPassProfiler::resolve(const std::vector<Pass>& passes,
                              bool succeeded) {
    std::vector<std::pair<const Pass*, u64>> times;
    if (succeeded) {
        // Runs on a Metal completion thread: the resolved data is autoreleased
        NS_STACK_SCOPED auto pool = NS::AutoreleasePool::alloc()->init();
        times.reserve(passes.size());
        for (const auto& pass : passes) {
            u64 start, end;
            if (pass.mtl4) {
                const auto* data = counter_heap->resolveCounterRange(
                    NS::Range(pass.sample, 2));
                if (!data ||
                    data->length() < 2 * sizeof(MTL4::TimestampHeapEntry))
                    continue;
                const auto* entries =
                    static_cast<const MTL4::TimestampHeapEntry*>(
                        data->mutableBytes());
                start = entries[0].timestamp;
                end = entries[1].timestamp;
            } else {
                const auto* data = sample_buffer->resolveCounterRange(
                    NS::Range(pass.sample, 2));
                if (!data ||
                    data->length() < 2 * sizeof(MTL::CounterResultTimestamp))
                    continue;
                const auto* samples =
                    static_cast<const MTL::CounterResultTimestamp*>(
                        data->mutableBytes());
                start = samples[0].timestamp;
                end = samples[1].timestamp;
            }
            // Unsampled stages (e.g. a pass without draws) read as an error
            if (start == MTL::CounterErrorValue ||
                end == MTL::CounterErrorValue || end < start || start == 0)
                continue;
            // Apple GPU sample buffer timestamps are nanoseconds, counter
            // heap ones are ticks
            times.emplace_back(
                &pass, pass.mtl4 ? static_cast<u64>(
                                       static_cast<f64>(end - start) * ns_per_tick)
                                 : end - start);
        }
    }
    std::scoped_lock lock(mutex);
    for (const auto& [pass, ns] : times) {
        auto& kind = kind_totals[static_cast<usize>(pass->kind)];
        kind.ns += ns;
        kind.count++;
        auto& label = label_totals[pass->label];
        label.ns += ns;
        label.count++;
    }
    for (const auto& pass : passes) {
        // Counter heap entries keep their value until they are invalidated
        if (pass.mtl4)
            counter_heap->invalidateCounterRange(NS::Range(pass.sample, 2));
        free_pairs.push_back(pass.sample);
    }
}

std::vector<std::pair<std::string, GpuPassProfiler::Total>>
GpuPassProfiler::takeTop(usize count, Total (&kinds)[3]) {
    std::scoped_lock lock(mutex);
    std::vector<std::pair<std::string, Total>> top(label_totals.begin(),
                                                   label_totals.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
        return a.second.ns > b.second.ns;
    });
    if (top.size() > count)
        top.resize(count);
    for (usize i = 0; i < 3; i++) {
        kinds[i] = kind_totals[i];
        kind_totals[i] = {};
    }
    label_totals.clear();
    return top;
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
