#pragma once

#include "core/hw/tegra_x1/gpu/perf_stats.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

#include <map>
#include <mutex>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

// Only completed uploads enter this pool. Command buffers keep the pool alive
// until their completion handlers run, independently of Renderer lifetime.
class UploadBufferPool {
  public:
    explicit UploadBufferPool(MTL::Device* device_,
                              u64 budget_ = 64 * 1024 * 1024,
                              usize max_buffers_ = 64)
        : device(device_->retain()), budget(budget_),
          max_buffers(max_buffers_) {}

    ~UploadBufferPool() {
        for (auto& [size, buffer] : ready)
            buffer->release();
        device->release();
    }

    MTL::Buffer* acquire(u64 size) {
        // Fixed size classes avoid retaining many nearly equal allocations.
        u64 capacity = 4096;
        while (capacity < size && capacity < max_buffer_size)
            capacity *= 2;
        capacity = std::max(capacity, size);
        {
            std::scoped_lock lock(mutex);
            auto it = ready.find(capacity);
            if (it != ready.end()) {
                auto* buffer = it->second;
                cached_bytes -= buffer->length();
                ready.erase(it);
                if (PerfStats::isEnabled())
                    PerfStats::instance().upload_buffers_reused++;
                return buffer;
            }
        }
        if (PerfStats::isEnabled())
            PerfStats::instance().upload_buffers_created++;
        return device->newBuffer(capacity, MTL::ResourceStorageModeShared);
    }

    // Takes ownership of one retained buffer reference.
    void recycle(MTL::Buffer* buffer) {
        const auto size = buffer->length();
        {
            std::scoped_lock lock(mutex);
            if (size <= max_buffer_size && size <= budget - cached_bytes &&
                ready.size() < max_buffers) {
                ready.emplace(size, buffer);
                cached_bytes += size;
                return;
            }
        }
        buffer->release();
    }

    u64 getCachedBytes() {
        std::scoped_lock lock(mutex);
        return cached_bytes;
    }

  private:
    static constexpr u64 max_buffer_size = 16 * 1024 * 1024;
    MTL::Device* device;
    const u64 budget;
    const usize max_buffers;
    std::mutex mutex;
    std::multimap<u64, MTL::Buffer*> ready;
    u64 cached_bytes{0};
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
