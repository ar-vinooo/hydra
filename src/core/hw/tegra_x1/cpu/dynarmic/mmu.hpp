#pragma once

#include "core/horizon/kernel/const.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"

namespace hydra::hw::tegra_x1::cpu::dynarmic {

// Must cover config.page_table_address_space_bits (39)
constexpr u64 PAGE_COUNT = (1ull << 39) / GUEST_PAGE_SIZE;
static_assert(horizon::kernel::ADDRESS_SPACE.getEnd() / GUEST_PAGE_SIZE <=
              PAGE_COUNT);

class Mmu : public IMmu {
  public:
    explicit Mmu(System& system);
    ~Mmu() override;

    void map(vaddr_t dst_va, ztd::Range<uptr> range,
             const horizon::kernel::MemoryState state) override;
    void map(vaddr_t dst_va, ztd::Range<vaddr_t> range) override;
    void unmap(ztd::Range<vaddr_t> range) override;
    void protect(ztd::Range<vaddr_t> range,
                 horizon::kernel::MemoryPermission perm) override;

    uptr unmapAddr(vaddr_t va) const override;
    MemoryRegion queryRegion(vaddr_t va) const override;
    void setMemoryAttribute(ztd::Range<vaddr_t> range,
                            horizon::kernel::MemoryAttribute mask,
                            horizon::kernel::MemoryAttribute value) override;

    uptr getPageTablePtr() const { return reinterpret_cast<uptr>(pages); }

  protected:
    // Write tracking
    void setWriteTrackingEnabled(ztd::Range<vaddr_t> range,
                                 bool enable) override {
        // TODO: implement
        (void)range;
        (void)enable;
        ONCE(LOG_FUNC_NOT_IMPLEMENTED(Dynarmic));
    }
    bool trySuspendWriteTracking(ztd::Range<vaddr_t> range) override {
        // TODO: implement
        (void)range;
        ONCE(LOG_FUNC_NOT_IMPLEMENTED(Dynarmic));
        return false;
    }
    void resumeWriteTracking(ztd::Range<vaddr_t> range) override {
        // TODO: implement
        (void)range;
        ONCE(LOG_FUNC_NOT_IMPLEMENTED(Dynarmic));
    }

  private:
    // Lazily committed anonymous mappings; zero means unmapped / Free
    uptr* pages;
    horizon::kernel::MemoryState* states;
};

} // namespace hydra::hw::tegra_x1::cpu::dynarmic
