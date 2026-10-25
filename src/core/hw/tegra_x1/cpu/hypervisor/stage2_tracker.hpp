#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/hw/tegra_x1/cpu/hypervisor/const.hpp"

namespace hydra::hw::tegra_x1::cpu::hypervisor {

// Tracks CPU accesses to guest memory cached by the GPU with stage 2
// (hypervisor) permissions, in host pages. The VM invalidates its TLBs when
// those change, unlike for the guest's own page tables.
// - Write tracked pages are read only: a CPU write invalidates the GPU caches.
// - Pages the GPU rendered to are inaccessible: guest memory doesn't hold the
//   GPU's contents, so they are written back before the CPU reads or writes.
class Stage2Tracker {
  public:
    // Called without the tracker lock held, before the page becomes
    // accessible. Takes the host range of the page.
    struct Callbacks {
        std::function<void(ztd::Range<uptr>)> flush_gpu_writes;
        std::function<void(ztd::Range<uptr>)> invalidate_gpu_caches;
    };

    static Stage2Tracker& instance() {
        static Stage2Tracker tracker;
        return tracker;
    }

    void setCallbacks(const Callbacks& callbacks_) {
        std::scoped_lock lock(mutex);
        callbacks = callbacks_;
    }

    // Host ranges
    void setWriteTracked(ztd::Range<uptr> range, bool tracked);
    void protectGpuWritten(ztd::Range<uptr> range);

    // Write protects the pages again that were written since the last call
    void rearmWriteTracking();

    // Returns false if the fault isn't caused by tracking
    bool handleFault(paddr_t address, bool is_write);

  private:
    struct PageState {
        bool write_tracked{false};
        bool write_armed{false};
        bool gpu_written{false};
        // Incremented by every GPU write, so a write during a fault keeps the
        // page protected
        u32 gpu_epoch{0};
    };

    std::mutex mutex;
    std::unordered_map<paddr_t, PageState> pages;
    std::vector<paddr_t> written_pages;
    std::atomic<bool> has_written_pages{false};
    Callbacks callbacks;
    u64 fault_count{0};
    u64 flush_count{0};

    template <typename F>
    void forEachPage(ztd::Range<uptr> range, F&& func);
    static void applyProtection(paddr_t page, const PageState& state);
};

} // namespace hydra::hw::tegra_x1::cpu::hypervisor
