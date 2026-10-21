#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace hydra::hw::tegra_x1::gpu {
class GMmu;
namespace resource {
struct ContentManifest;
}
namespace capture {

struct GuestEventOrigin {
    std::uint64_t channel{0}, sequence{0}, source{0};
};
GuestEventOrigin currentGuestEventOrigin();
void recordGuestCommand(std::string_view kind, GuestEventOrigin origin,
                        const resource::ContentManifest& contents);

// Independent of host-resolved .hcap files. Enable before starting the guest:
// HYDRA_GUEST_EVENTS=/absolute/path/events.jsonl. This records the observed
// guest event timeline and memory inputs. Snapshot reads may trigger the
// existing CPU access/writeback mechanism for GPU-protected guest pages.
// GPU-produced bytes remain unknown until an explicit writeback observation.
class GuestEvents {
  public:
    static GuestEvents& instance();
    bool enabled() const { return active.load(std::memory_order_acquire); }

    void event(std::string_view kind, GuestEventOrigin origin = {},
               std::uint64_t address = 0, std::uint64_t size = 0,
               std::uint64_t version = 0, std::uint64_t value = 0,
               std::uint64_t auxiliary = 0);
    void memory(std::string_view kind, std::uintptr_t address,
                std::uint64_t size, std::uint64_t version = 0,
                GuestEventOrigin origin = {});
    void channel(GMmu& space, GuestEventOrigin origin,
                 bool switch_channel = true);
    void addressSpaceDestroyed(std::uint64_t id);
    std::uint64_t beginReport(GuestEventOrigin origin);
    void completeReport(std::uint64_t id, bool succeeded);
    void initialSyncpoint(std::uint32_t id, std::uint32_t value);
    void finish();

  private:
    GuestEvents();
    void writeLocked(std::string_view kind, GuestEventOrigin origin,
                     std::uint64_t address, std::uint64_t size,
                     std::uint64_t version, std::uint64_t value,
                     std::uint64_t auxiliary,
                     std::span<const std::uint8_t> data = {});
    void truncateLocked(std::string_view reason);

    std::mutex mutex;
    std::ofstream output;
    std::atomic<bool> active{false};
    std::uint64_t order{0}, bytes{0}, limit{256ull << 20};
    struct Mapping {
        std::uintptr_t host;
        std::uint64_t size, guest;
        bool operator==(const Mapping&) const = default;
    };
    struct Space {
        std::uint64_t generation{0};
        std::map<std::uintptr_t, Mapping> mappings;
    };
    std::map<std::uint64_t, Space> spaces;
    std::map<std::uint64_t, std::uint64_t> pending_writes;
    std::uint64_t next_report{1};
    std::set<std::uint64_t> pending_reports;
    std::set<std::uint32_t> initial_syncpoints;
    std::set<std::uint64_t> pending_native;
};

} // namespace capture
} // namespace hydra::hw::tegra_x1::gpu
