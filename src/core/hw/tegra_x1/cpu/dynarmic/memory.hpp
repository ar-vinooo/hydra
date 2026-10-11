#pragma once

#include "core/hw/tegra_x1/cpu/memory.hpp"

namespace hydra::hw::tegra_x1::cpu::dynarmic {

class Memory : public IMemory {
  public:
    explicit Memory(u64 size) : IMemory(size) { allocate(); }
    ~Memory() override { free(); }

    uptr getPtr() const override { return ptr; }

  protected:
    void resizeImpl() override {
        free();
        allocate();
    }

  private:
    uptr ptr;

    // Helpers
    // NOLINTBEGIN(cppcoreguidelines-no-malloc)
    void allocate() { ptr = reinterpret_cast<uptr>(malloc(getSize())); }
    void free() const { ::free(reinterpret_cast<void*>(ptr)); }
    // NOLINTEND(cppcoreguidelines-no-malloc)
};

// Host memory owned by someone else
class ExternalMemory : public IMemory {
  public:
    ExternalMemory(uptr ptr_, u64 size) : IMemory(size), ptr{ptr_} {}

    uptr getPtr() const override { return ptr; }

  protected:
    void resizeImpl() override {}

  private:
    uptr ptr;
};

} // namespace hydra::hw::tegra_x1::cpu::dynarmic
