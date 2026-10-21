#include "core/hw/tegra_x1/gpu/resource/content_version.hpp"

#include <algorithm>

namespace hydra::hw::tegra_x1::gpu::resource {

u64 ContentVersions::write(ztd::Range<uptr> range) {
    std::scoped_lock lock(mutex);
    const u64 version = ++counter;
    stamp(range, version, {});
    return version;
}

void ContentVersions::GpuWrite::complete(bool succeeded) const {
    if (!status)
        return;
    auto expected = GpuStatus::Pending;
    status->compare_exchange_strong(
        expected, succeeded ? GpuStatus::Completed : GpuStatus::Failed,
        std::memory_order_acq_rel);
}

ContentVersions::GpuWrite ContentVersions::issueGpuWrites(
    std::span<const ztd::Range<uptr>> writes) {
    GpuWrite write;
    if (std::none_of(writes.begin(), writes.end(), [](const auto& range) {
            return range.getBegin() < range.getEnd();
        }))
        return write;
    write.status = std::make_shared<std::atomic<GpuStatus>>(GpuStatus::Pending);
    std::scoped_lock lock(mutex);
    write.issued = ++counter;
    for (const auto& range : writes)
        stamp(range, write.issued, write.status);
    return write;
}

void ContentVersions::stamp(
    ztd::Range<uptr> range, u64 version,
    const std::shared_ptr<std::atomic<GpuStatus>>& status) {
    const uptr begin = range.getBegin();
    const uptr end = range.getEnd();
    if (begin >= end)
        return;

    // Rewriting exactly the range written last time (a render target drawn
    // to again) only restamps it
    if (const auto same = ranges.find(begin);
        same != ranges.end() && same->second.end == end) {
        same->second.version = version;
        same->second.gpu_status = status;
        return;
    }

    // Cut the part of existing ranges the new one covers
    auto it = ranges.lower_bound(begin);
    if (it != ranges.begin()) {
        auto previous = std::prev(it);
        if (previous->second.end > begin) {
            const Entry tail = previous->second;
            previous->second.end = begin;
            if (tail.end > end)
                ranges.emplace(end, tail);
        }
    }
    while (it != ranges.end() && it->first < end) {
        if (it->second.end > end) {
            const Entry rest = it->second;
            it = ranges.erase(it);
            ranges.emplace(end, rest);
            break;
        }
        it = ranges.erase(it);
    }
    ranges[begin] = {end, version, status};

    if (ranges.size() > MAX_RANGES) {
        ranges.clear();
        floor = version;
    }
}

ContentVersions::State ContentVersions::inspect(ztd::Range<uptr> range) const {
    std::scoped_lock lock(mutex);
    State state;
    if (range.getBegin() >= range.getEnd())
        return state;
    state.issued = floor;
    // Eviction preserves change detection, but cannot prove completion.
    state.unknown = floor != 0;
    auto it = ranges.upper_bound(range.getBegin());
    if (it != ranges.begin())
        --it;
    for (; it != ranges.end() && it->first < range.getEnd(); ++it) {
        const auto& entry = it->second;
        if (entry.end <= range.getBegin())
            continue;
        state.issued = std::max(state.issued, entry.version);
        const auto status = entry.gpu_status
            ? entry.gpu_status->load(std::memory_order_acquire)
            : GpuStatus::Completed;
        if (status == GpuStatus::Completed)
            state.completed = std::max(state.completed, entry.version);
        state.pending |= status == GpuStatus::Pending;
        state.failed |= status == GpuStatus::Failed;
    }
    return state;
}

u64 ContentVersions::latest(ztd::Range<uptr> range) const {
    std::scoped_lock lock(mutex);
    u64 version = floor;
    const uptr begin = range.getBegin();
    const uptr end = range.getEnd();
    if (begin >= end)
        return 0;
    auto it = ranges.upper_bound(begin);
    if (it != ranges.begin()) {
        const auto previous = std::prev(it);
        if (previous->second.end > begin)
            version = std::max(version, previous->second.version);
    }
    for (; it != ranges.end() && it->first < end; ++it)
        version = std::max(version, it->second.version);
    return version;
}

u64 ContentVersions::current() const {
    std::scoped_lock lock(mutex);
    return counter;
}

usize ContentVersions::size() const {
    std::scoped_lock lock(mutex);
    return ranges.size();
}

namespace {
bool overlaps(const ztd::Range<uptr>& a, const ztd::Range<uptr>& b) {
    return a.getBegin() < b.getEnd() && b.getBegin() < a.getEnd();
}
} // namespace

bool ContentManifest::dependsOn(const ContentManifest& earlier) const {
    for (const auto& written : earlier.writes) {
        for (const auto& read : reads)
            if (overlaps(written, read.range))
                return true;
        for (const auto& own : writes)
            if (overlaps(written, own))
                return true;
    }
    for (const auto& earlier_read : earlier.reads)
        for (const auto& own : writes)
            if (overlaps(earlier_read.range, own))
                return true;
    return false;
}

} // namespace hydra::hw::tegra_x1::gpu::resource
