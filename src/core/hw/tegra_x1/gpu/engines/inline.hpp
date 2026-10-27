#pragma once

#include "core/hw/tegra_x1/gpu/engines/inline_base.hpp"

namespace hydra::hw::tegra_x1::gpu::engines {

struct RegsInline_ {
    RegsInline regs_inline;

    // 0x6e, SET_I2M_SEMAPHORE_A-C at 0x77-0x79
    u32 padding_0x6e[0xc];
};

class Inline : public EngineWithRegsBase<RegsInline_>, public InlineBase {
  public:
    explicit Inline(Gpu& gpu_) : gpu{gpu_} {}

    void method(u32 method, u32 arg) override;
    void methodHot(u32 method, u32 arg);

  private:
    Gpu& gpu;

    // Methods
    DEFINE_INLINE_ENGINE_METHODS;
};

} // namespace hydra::hw::tegra_x1::gpu::engines
