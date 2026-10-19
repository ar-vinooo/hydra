#pragma once

#include "core/horizon/kernel/auto_object.hpp"
#include "core/horizon/kernel/const.hpp"
#include "core/hw/tegra_x1/cpu/mmu.hpp"

namespace hydra::hw::tegra_x1::cpu {
class ICpu;
class IMmu;
} // namespace hydra::hw::tegra_x1::cpu

namespace hydra::horizon::kernel {

class Process;

class SharedMemory : public AutoObject {
    friend Process;
  public:
    static constexpr AutoObjectTypeId TYPE_ID = AutoObjectTypeId::SharedMemory;

    SharedMemory(hw::tegra_x1::cpu::ICpu& cpu, u64 size,
                 std::string_view debug_name = "SharedMemory");
    ~SharedMemory() override;

    // Getters
    uptr getPtr() const;
    u64 getSize() const { return memory->getSize(); }
    const std::shared_ptr<hw::tegra_x1::cpu::IMemory>& getBacking() const { return memory; }

  private:
    std::shared_ptr<hw::tegra_x1::cpu::IMemory> memory;
    void mapToRange(hw::tegra_x1::cpu::IMmu* mmu, const ztd::Range<vaddr_t> range,
                    MemoryPermission perm);
};

} // namespace hydra::horizon::kernel
