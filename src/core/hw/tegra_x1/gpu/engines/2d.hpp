#pragma once

#include "core/hw/tegra_x1/gpu/engines/engine_base.hpp"
#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu {
class Gpu;
}

namespace hydra::hw::tegra_x1::gpu::renderer {
class ITextureView;
}

namespace hydra::hw::tegra_x1::gpu::engines {

struct Texture2DInfo {
    ColorSurfaceFormat format;
    MemoryLayout layout;
    struct {
        u32 : 4;
        u32 block_height_gobs_log2 : 3;
        u32 : 1;
        u32 block_depth_gobs_log2 : 3;
        u32 : 21;
    };
    u32 depth;
    u32 layer;
    u32 stride;
    u32 width;
    u32 height;
    Iova addr;
};

struct FixedPoint {
    u32 fractional;
    u32 integer;

    // Signed 32.32 fixed point
    explicit operator f64() const {
        const auto raw = static_cast<i64>((static_cast<u64>(integer) << 32) |
                                          static_cast<u64>(fractional));
        return static_cast<f64>(raw) / static_cast<f64>(1llu << 32llu);
    }
};

// TODO: operate directly on FixedPoint instead of casting to f64?
/*
inline FixedPoint operator*(const FixedPoint& a, const FixedPoint& b) {
    return FixedPoint{a.fractional * b.fractional, a.integer * b.integer};
}

inline FixedPoint operator/(const FixedPoint& a, const FixedPoint& b) {
    return FixedPoint{a.fractional / b.fractional, a.integer / b.integer};
}
*/

struct PixelsFromMemory {
    u32 dst_x0;
    u32 dst_y0;
    u32 dst_width;
    u32 dst_height;
    FixedPoint dudx;
    FixedPoint dvdy;
    FixedPoint src_x0;
    FixedPoint src_y0;
};

struct Regs2D {
    u32 padding_0x0[0x80];

    // 0x80
    Texture2DInfo dst;

    u32 padding_0x8a[0x2];

    // 0x8c
    Texture2DInfo src;

    u32 padding_0x96[0x18d];

    // 0x223
    struct {
        u32 origin : 1;
        u32 : 3;
        u32 filter : 1;
        u32 : 27;
    } sample_mode;

    u32 padding_0x224[0x8];

    // 0x22c
    PixelsFromMemory pixels_from_memory;
};

class TwoD : public EngineWithRegsBase<Regs2D> {
  public:
    explicit TwoD(Gpu& gpu_) : gpu{gpu_} {}

    void method(u32 method, u32 arg) override;
    void methodHot(u32 method, u32 arg);

  private:
    void copyGuestMemory(u32 bpp, u32 src_x, u32 src_y, u32 width,
                         u32 height);
    Gpu& gpu;

    // Commands
    void copy(const u32 index, const u32 pixels_from_memory_src_y0_int);

    // Helpers
    renderer::TextureDescriptor
    getTextureDescriptor(const Texture2DInfo& info) const;
    renderer::ITextureView*
    getTexture(const Texture2DInfo& info, renderer::TextureUsage usage,
               std::optional<renderer::TextureFormat> format_override =
                   std::nullopt);
};

} // namespace hydra::hw::tegra_x1::gpu::engines
