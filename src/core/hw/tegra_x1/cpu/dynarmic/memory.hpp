#pragma once

#include <cerrno>

#include <sys/mman.h>

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
    uptr ptr{0x0};
    u64 allocated_size{0};

    // Helpers
    // Anonymous mappings are page aligned and zero filled, like guest memory
    void allocate() {
        allocated_size = getSize();
        if (allocated_size == 0) {
            ptr = 0x0;
            return;
        }

        void* p = mmap(nullptr, allocated_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ASSERT(p != MAP_FAILED, Dynarmic, "Failed to allocate memory: {:#x}",
               errno);
        ptr = reinterpret_cast<uptr>(p);
    }

    void free() {
        if (ptr != 0x0) {
            const auto res =
                munmap(reinterpret_cast<void*>(ptr), allocated_size);
            ASSERT(res == 0, Dynarmic, "Failed to deallocate memory: {:#x}",
                   errno);
        }
        ptr = 0x0;
        allocated_size = 0;
    }
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
