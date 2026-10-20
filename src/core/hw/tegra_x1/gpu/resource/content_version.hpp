#pragma once

#include <map>
#include <mutex>
#include <vector>

namespace hydra::hw::tegra_x1::gpu::resource {

// Versions of memory contents, separate from resource identity: a buffer or
// texture keeps its ID and generation while the guest (CPU, DMA) or the GPU
// rewrites the bytes behind it. Every write advances one global counter and
// stamps the host address range it touched. A command which read a range at
// version V sees different bytes later exactly when latest(range) > V.
class ContentVersions {
  public:
    // Records a write and returns its version (versions start at 1)
    u64 write(ztd::Range<uptr> range);
    // The newest write overlapping the range; 0 if there is none
    u64 latest(ztd::Range<uptr> range) const;
    // Version of the newest write anywhere: a snapshot taken now
    u64 current() const;
    // Number of tracked ranges (bounded)
    usize size() const;

    // At most this many ranges are kept. Past it the oldest knowledge is
    // dropped by raising a floor under every range, which only ever reports
    // more change than happened.
    static constexpr usize MAX_RANGES = 16384;

  private:
    struct Entry {
        uptr end;
        u64 version;
    };
    mutable std::mutex mutex;
    std::map<uptr, Entry> ranges;
    u64 counter{0};
    u64 floor{0};
};

// What a command reads and writes, for dependency decisions
struct ContentManifest {
    struct Read {
        ztd::Range<uptr> range;
        // The version the command was prepared against
        u64 version;
    };
    std::vector<Read> reads;
    std::vector<ztd::Range<uptr>> writes;

    void addRead(const ContentVersions& versions, ztd::Range<uptr> range) {
        addRead(versions.current(), range);
    }
    // For many reads of one command: take the version once
    void addRead(u64 version, ztd::Range<uptr> range) {
        if (range.getBegin() != range.getEnd())
            reads.push_back({range, version});
    }
    void addWrite(ztd::Range<uptr> range) {
        if (range.getBegin() != range.getEnd())
            writes.push_back(range);
    }
    // Every read still sees the bytes it was prepared against
    bool readsCurrent(const ContentVersions& versions) const {
        for (const auto& read : reads)
            if (versions.latest(read.range) > read.version)
                return false;
        return true;
    }
    // Whether running this command after `earlier` (which comes first in
    // guest order) needs that command's results or order: it reads or
    // overwrites what `earlier` wrote, or writes what `earlier` read
    bool dependsOn(const ContentManifest& earlier) const;
};

} // namespace hydra::hw::tegra_x1::gpu::resource
