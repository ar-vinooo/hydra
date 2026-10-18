#include "core/hw/tegra_x1/gpu/renderer/buffer_cache.hpp"

#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"

#ifdef ZTD_PLATFORM_APPLE
#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/upload_buffer_pool.hpp"
namespace hydra::hw::tegra_x1::gpu::renderer::metal {
std::shared_ptr<UploadBufferPool> snapshotBufferPool(MTL::Device* device);
}
#endif

namespace hydra::hw::tegra_x1::gpu::renderer {
namespace {
std::shared_ptr<BufferBase> ownSnapshot(BufferBase* snapshot) {
    if (snapshot == nullptr)
        return {};
#ifdef ZTD_PLATFORM_APPLE
    auto* native = static_cast<metal::Buffer*>(snapshot)->getBuffer();
    auto pool = metal::snapshotBufferPool(native->device());
    return std::shared_ptr<BufferBase>(snapshot, [pool](BufferBase* buffer) {
        // CPU views may outlive the command buffer that created a snapshot.
        // Retire only at the last CPU release, on the current ordered GPU
        // command buffer. Without a GPU-thread context, release normally.
        if (tls_crnt_command_buffer != nullptr) {
            auto* native_buffer =
                static_cast<metal::Buffer*>(buffer)->getBuffer();
            native_buffer->retain();
            static_cast<metal::CommandBuffer*>(tls_crnt_command_buffer)
                ->getCommandBuffer()->addCompletedHandler(
                    [pool, native_buffer](MTL::CommandBuffer* completed) {
                        if (completed->status() ==
                            MTL::CommandBufferStatusCompleted)
                            pool->recycle(native_buffer);
                        else
                            native_buffer->release();
                    });
        }
        delete buffer;
    });
#else
    return std::shared_ptr<BufferBase>(snapshot);
#endif
}
} // namespace

BufferCache::~BufferCache() = default;

BufferView BufferCache::snapshot(ztd::Range<uptr> range) {
    if (range.getSize() == 0)
        return {};
    auto* buffer = renderer.createCpuBufferSnapshot(range);
    if (buffer == nullptr) {
        buffer = renderer.allocateTemporaryBuffer(range.getSize());
        if (buffer == nullptr)
            return {};
        buffer->copyFrom(range.getBegin());
    }
    return BufferView(ownSnapshot(buffer), 0, range.getSize());
}

void BufferCache::markGpuModified(ztd::Range<uptr> range) {
    uptr begin = range.getBegin();
    uptr end = range.getEnd();
    // Merge with overlapping or touching intervals
    auto it = gpu_modified.upper_bound(begin);
    if (it != gpu_modified.begin() && std::prev(it)->second >= begin)
        --it;
    while (it != gpu_modified.end() && it->first <= end) {
        begin = std::min(begin, it->first);
        end = std::max(end, it->second);
        it = gpu_modified.erase(it);
    }
    // Bounded: past this the oldest knowledge is dropped, and those ranges
    // fall back to snapshots again
    if (gpu_modified.size() >= 256)
        gpu_modified.erase(gpu_modified.begin());
    gpu_modified.emplace(begin, end);
}

bool BufferCache::intersectsGpuModified(ztd::Range<uptr> range) const {
    auto it = gpu_modified.upper_bound(range.getBegin());
    if (it != gpu_modified.begin() && std::prev(it)->second > range.getBegin())
        return true;
    return it != gpu_modified.end() && it->first < range.getEnd();
}

BufferView BufferCache::getHot0(ICommandBuffer* command_buffer,
                            ztd::Range<uptr> range) {
    if (!gpu_modified.empty() && intersectsGpuModified(range)) {
        auto view = renderer.getGuestMemoryBuffer(range);
        if (view.isValid())
            return view;
    }
    auto& entry = find(range);
    if (entry.buffer != nullptr) {
        // Check for memory invalidation
        if (entry.invalidation_range.has_value() &&
            entry.invalidation_range->intersects(range)) {
            const auto invalidation_range = entry.invalidation_range.value();
            updateRange(command_buffer, entry, invalidation_range);
            entry.invalidation_range = std::nullopt;
        } else if (CONFIG_INSTANCE.getCpuBackend() == CpuBackend::Dynarmic) {
            // HACK: force update all buffers on dynarmic
            updateRange(command_buffer, entry, entry.range);
        }
    } else {
        // Create new buffer
        entry.buffer = ownSnapshot(renderer.createCpuBufferSnapshot(entry.range));
        if (!entry.buffer) {
            entry.buffer.reset(renderer.createBuffer(entry.range.getSize()));
            updateRange(command_buffer, entry, entry.range);
        }
    }

    return BufferView{entry.buffer, range.getBegin() - entry.range.getBegin(),
                      range.getSize()};
}

void BufferCache::invalidateMemoryHot1(ztd::Range<uptr> range) {
    auto it = entries.upper_bound(range.getBegin());
    if (it != entries.begin())
        it--;

    while (it != entries.end() &&
           it->second.range.getBegin() < range.getEnd()) {
        auto& entry = it->second;
        if (entry.range.getEnd() > range.getBegin()) {
            const auto invalidation_range = range.clampedTo(entry.range);
            if (entry.invalidation_range.has_value()) {
                // Combine with an existing invalidation range if it exists
                entry.invalidation_range =
                    entry.invalidation_range.value().merged(invalidation_range);
            } else {
                // Set the range directly
                entry.invalidation_range = invalidation_range;
            }
        }
        it++;
    }
}

void BufferCache::updateRangeHot2(ICommandBuffer* command_buffer,
                              BufferEntry& entry, ztd::Range<uptr> range) {
    // CPU write tracking invalidates whole pages, often touching many small
    // constant buffers whose bytes did not change. Host-visible snapshots are
    // immutable: compare the invalidated bytes before allocating a replacement.
    // Bound the comparison to the small-buffer snapshot path.
    if (entry.range.getSize() <= 64 * 1024) {
        const auto previous = entry.buffer->getPtr();
        if (previous != 0 &&
            std::memcmp(reinterpret_cast<const void*>(range.getBegin()),
                        reinterpret_cast<const void*>(previous + range.getBegin() -
                                                       entry.range.getBegin()),
                        range.getSize()) == 0)
            return;
    }
    if (auto* snapshot = renderer.createCpuBufferSnapshot(entry.range)) {
        // Replace the resource instead of mutating bytes the GPU may still
        // read. BufferViews retain the old owner; Metal retains encoded GPU
        // resources until their command buffer completes.
        entry.buffer = ownSnapshot(snapshot);
    } else if (entry.inline_copy) {
        // Do an inline update if possible
        entry.buffer->copyFrom(range.getBegin(),
                               range.getBegin() - entry.range.getBegin(),
                               range.getSize());
        entry.inline_copy = false;
    } else {
        // Copy from a temporary buffer
        // Share the bounded upload pool used by texture uploads. Retirement
        // waits for GPU completion before staging memory can be reused.
        auto tmp_buffer = renderer.allocateTextureUploadBuffer(range.getSize());
        tmp_buffer->copyFrom(range.getBegin());
        entry.buffer->copyFrom(command_buffer, tmp_buffer,
                               range.getBegin() - entry.range.getBegin(), 0,
                               range.getSize());
        renderer.retireTextureUploadBuffer(command_buffer, tmp_buffer);
    }
}

BufferEntry& BufferCache::findHot3(ztd::Range<uptr> range) {
    // Check for containing interval
    auto it = entries.upper_bound(range.getBegin());
    if (it != entries.begin()) {
        auto prev = std::prev(it);
        if (prev->second.range.getEnd() >= range.getEnd()) {
            // Fully contained
            return prev->second;
        }
    }

    // Insert and merge
    auto new_range = range;

    it = entries.lower_bound(range.getBegin());

    // Merge with previous if overlapping/touching
    if (it != entries.begin()) {
        auto prev = std::prev(it);
        if (prev->second.range.getEnd() >= new_range.getBegin()) {
            new_range = ztd::Range<uptr>(
                prev->second.range.getBegin(),
                std::max(new_range.getEnd(), prev->second.range.getEnd()));
            it = entries.erase(prev);
        }
    }

    // Merge with following entries
    while (it != entries.end() && it->first <= new_range.getEnd()) {
        new_range = ztd::Range<uptr>(
            new_range.getBegin(),
            std::max(new_range.getEnd(), it->second.range.getEnd()));
        it = entries.erase(it);
    }

    // Insert merged interval
    auto inserted =
        entries.emplace(new_range.getBegin(),
                        BufferEntry{.buffer = nullptr, .range = new_range});

    return inserted.first->second;
}

} // namespace hydra::hw::tegra_x1::gpu::renderer
