#include "core/hw/tegra_x1/gpu/resource/content_version.hpp"

#include <algorithm>

namespace hydra::hw::tegra_x1::gpu::resource {

u64 ContentVersions::write(ztd::Range<uptr> range) {
    std::scoped_lock lock(mutex);
    const u64 version = ++counter;
    const uptr begin = range.getBegin();
    const uptr end = range.getEnd();
    if (begin >= end)
        return version;

    // Rewriting exactly the range written last time (a render target drawn
    // to again) only restamps it
    if (const auto same = ranges.find(begin);
        same != ranges.end() && same->second.end == end) {
        same->second.version = version;
        return version;
    }

    // Cut the part of existing ranges the new one covers
    auto it = ranges.lower_bound(begin);
    if (it != ranges.begin()) {
        auto previous = std::prev(it);
        if (previous->second.end > begin) {
            const Entry tail{previous->second.end, previous->second.version};
            previous->second.end = begin;
            if (tail.end > end)
                ranges.emplace(end, tail);
        }
    }
    while (it != ranges.end() && it->first < end) {
        if (it->second.end > end) {
            const Entry rest{it->second.end, it->second.version};
            it = ranges.erase(it);
            ranges.emplace(end, rest);
            break;
        }
        it = ranges.erase(it);
    }
    ranges[begin] = {end, version};

    if (ranges.size() > MAX_RANGES) {
        ranges.clear();
        floor = version;
    }
    return version;
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
