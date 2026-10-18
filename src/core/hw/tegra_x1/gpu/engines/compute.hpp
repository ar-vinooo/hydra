#pragma once

#include "core/hw/tegra_x1/gpu/engines/inline_base.hpp"

namespace hydra::hw::tegra_x1::gpu::engines {

// Kepler compute (class B1C0) registers. Layout reference:
// video_core/engines/kepler_compute.h in https://github.com/eden-emulator/mirror
struct RegsCompute {
    RegsInline regs_inline;

    u32 padding_0x6e[0x3f];

    // 0xad
    u32 launch_desc_loc; // QMD address >> 8
    u32 padding_0xae;
    u32 launch;

    u32 padding_0xb0[0x4a7];

    // 0x557
    u32 tsc_addr_hi;
    u32 tsc_addr_lo;
    u32 tsc_limit;

    u32 padding_0x55a[0x3];

    // 0x55d
    u32 tic_addr_hi;
    u32 tic_addr_lo;
    u32 tic_limit;

    u32 padding_0x560[0x22];

    // 0x582
    u32 code_addr_hi;
    u32 code_addr_lo;

    u32 padding_0x584[0x3fe];

    // 0x982
    u32 tex_cb_index;
};
static_assert(sizeof(RegsCompute) == 0x983 * sizeof(u32));

// Compute queue meta data (QMD), the launch descriptor in GPU memory. Field
// positions follow NVIDIA's clb1c0qmd.h (QMDV01_07), checked by
// testComputeQmdLayout
struct ComputeQmd {
    u32 words[0x40];

    static std::optional<ComputeQmd> read(GMmu& gmmu, uptr address);

    u32 programStart() const { return words[0x8]; }
    u32 gridDimX() const { return words[0xc]; }
    u32 gridDimY() const { return words[0xd] & 0xffff; }
    u32 gridDimZ() const { return words[0xd] >> 16; }
    u32 sharedAlloc() const { return words[0x11] & 0x3ffff; }
    u32 blockDimX() const { return words[0x12] >> 16; }
    u32 blockDimY() const { return words[0x13] & 0xffff; }
    u32 blockDimZ() const { return words[0x13] >> 16; }
    u32 constBufferEnableMask() const { return words[0x14] & 0xff; }
    u64 constBufferAddress(u32 index) const {
        return u64(words[0x1d + index * 2]) |
               (u64(words[0x1e + index * 2] & 0xff) << 32);
    }
    u32 constBufferSize(u32 index) const {
        return (words[0x1e + index * 2] >> 15) & 0x1ffff;
    }
    u32 localPosAlloc() const { return words[0x2d] & 0xffffff; }
};

constexpr u32 COMPUTE_CONST_BUFFER_COUNT = 8;

class Compute : public EngineWithRegsBase<RegsCompute>, public InlineBase {
  public:
    explicit Compute(Gpu& gpu_) : gpu{gpu_} {}

    void method(u32 method, u32 arg) override;
    void methodHot(u32 method, u32 arg);

  private:
    Gpu& gpu;

    // Methods
    DEFINE_INLINE_ENGINE_METHODS;
    void launch(const u32 index, const u32 arg);

    // Helpers
    ztd::Range<uptr> getConstBuffer(const ComputeQmd& qmd, u32 index) const;
    // Reads the handle a shader texture or image binding refers to
    std::optional<u32> readTextureHandle(const ComputeQmd& qmd,
                                         u32 binding_index) const;
};

} // namespace hydra::hw::tegra_x1::gpu::engines
