#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

#include <map>
#include <tuple>

namespace MTLFX {
class SpatialScaler;
}

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

// Upscales presented frames with MetalFX spatial scaling. Scalers are costly
// to create, so one is kept per input/output size and format.
class SpatialUpscaler {
  public:
    explicit SpatialUpscaler(MTL::Device* device);
    ~SpatialUpscaler();
    SpatialUpscaler(const SpatialUpscaler&) = delete;
    SpatialUpscaler& operator=(const SpatialUpscaler&) = delete;

    // Encodes the whole input scaled to the output size into the command
    // buffer (outside any encoder). Returns the output texture, or null if
    // MetalFX cannot scale this input, in which case the caller draws the
    // input as is.
    MTL::Texture* upscale(MTL::CommandBuffer* command_buffer,
                          MTL::Texture* input, u32 output_width,
                          u32 output_height);

    // Largest output dimension
    static constexpr u32 MAX_OUTPUT_SIZE = 16384;

  private:
    using Key = std::tuple<u32, u32, u32, u32, MTL::PixelFormat>;
    struct Entry {
        // Null if MetalFX does not support this configuration
        MTLFX::SpatialScaler* scaler;
        MTL::Texture* output;
        u64 used;
    };
    static constexpr usize MAX_ENTRIES = 4;

    Entry& getEntry(const Key& key);

    MTL::Device* device;
    bool supported;
    std::map<Key, Entry> entries;
    u64 clock{0};
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
