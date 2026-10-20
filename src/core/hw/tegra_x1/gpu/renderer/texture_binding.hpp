#pragma once

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {
class ITextureView;
class SamplerBase;
struct TextureBinding {
    u32 index;
    ITextureView* texture;
    SamplerBase* sampler;
};
} // namespace hydra::hw::tegra_x1::gpu::renderer
