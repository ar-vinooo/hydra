#pragma once

#include <algorithm>
#include <span>
#include <vector>

namespace hydra::hw::tegra_x1::gpu::resource {
// Exact union: merge only overlapping or adjacent intervals. Never include
// an untouched gap, which could discard the contents of an unrelated alias.
inline std::vector<ztd::Range<uptr>>
coalesceRanges(std::span<const ztd::Range<uptr>> input) {
    std::vector<ztd::Range<uptr>> sorted;
    sorted.reserve(input.size());
    for (const auto& range : input)
        if (range.getBegin() < range.getEnd())
            sorted.push_back(range);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& left, const auto& right) {
                  return left.getBegin() < right.getBegin();
              });
    std::vector<ztd::Range<uptr>> result;
    result.reserve(sorted.size());
    for (const auto& range : sorted) {
        if (!result.empty() && range.getBegin() <= result.back().getEnd())
            result.back() = {result.back().getBegin(),
                             std::max(result.back().getEnd(), range.getEnd())};
        else
            result.push_back(range);
    }
    return result;
}
} // namespace hydra::hw::tegra_x1::gpu::resource
