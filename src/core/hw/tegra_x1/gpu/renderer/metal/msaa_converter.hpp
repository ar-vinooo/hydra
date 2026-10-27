#pragma once

#include <map>
#include <mutex>
#include <tuple>

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class CommandBuffer;

// Converts between the two forms of a multisampled guest surface: a host
// multisampled texture (one texel per pixel) and the single sample image the
// guest stores it as, whose pixels are blocks of samples (see MsaaLayout).
// Every sample is copied unchanged, through render passes so that depth and
// stencil can be written too.
class MsaaConverter {
  public:
    static MsaaConverter& get(MTL::Device* device);

    // Copies each sample of `multisampled` to its texel in `scaled`.
    // guest_samples is the guest sample count, which selects the layout; the
    // host texture can have fewer samples (guest sample i is host sample
    // i % host samples).
    void expand(CommandBuffer* command_buffer, MTL::Texture* multisampled,
                MTL::Texture* scaled, u32 guest_samples);
    // Copies the texel of each sample in `scaled` to the sample
    void pack(CommandBuffer* command_buffer, MTL::Texture* scaled,
              MTL::Texture* multisampled, u32 guest_samples);

  private:
    explicit MsaaConverter(MTL::Device* device_) : device{device_} {}

    enum class Kind { Float, Uint, Sint, Depth, Stencil, DepthStencil };
    static Kind getKind(MTL::PixelFormat format);

    struct Pipeline {
        MTL::RenderPipelineState* state{nullptr};
        MTL::DepthStencilState* depth_stencil{nullptr};
    };
    // Target pixel format, its sample count, guest samples, packing
    Pipeline getPipeline(MTL::PixelFormat target_format, u32 target_samples,
                         u32 guest_samples, u32 host_samples, bool pack);

    void convert(CommandBuffer* command_buffer, MTL::Texture* source,
                 MTL::Texture* target, u32 guest_samples, bool pack);

    MTL::Device* device;
    std::mutex mutex;
    std::map<std::tuple<MTL::PixelFormat, u32, u32, u32, bool>, Pipeline>
        pipelines;
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
