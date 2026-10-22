#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

#include <array>
#include <chrono>
#include <vector>

namespace MTLFX {
class FrameInterpolator;
class FrameInterpolatorBase;
} // namespace MTLFX

namespace MTL4FX {
class FrameInterpolator;
}

namespace hydra {
enum class FrameGeneration : u32;
}

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class CommandBuffer;
class Mtl4Context;

// Post-processes the presented frame before it reaches the drawable:
// - Frame generation: presentation ticks without a new guest frame show a frame
//   interpolated between the last two guest frames, from optical flow computed
//   on the GPU (games give no motion vectors). Each guest frame is shown one
//   guest frame late, in exchange for the smoother motion.
// - Contrast adaptive sharpening.
// When neither is enabled, the compositor draws to the drawable directly.
class FramePresenter {
  public:
    // With a Metal 4 context, MetalFX encodes into Metal 4 command buffers
    explicit FramePresenter(MTL::Device* device,
                            Mtl4Context* mtl4_context = nullptr);
    ~FramePresenter();
    FramePresenter(const FramePresenter&) = delete;
    FramePresenter& operator=(const FramePresenter&) = delete;

    // Whether presented frames go through the presenter
    static bool isEnabled();

    // Texture the layers of this display refresh are composited into, null if
    // the presenter cannot handle this size (the compositor then draws to the
    // drawable)
    MTL::Texture* getCanvas(u32 width, u32 height, MTL::PixelFormat format);
    // Encodes the frame for this display refresh into the drawable, which has
    // the size of the canvas. `new_frame` is whether the canvas holds a new
    // guest frame.
    void present(CommandBuffer* command_buffer, MTL::Texture* drawable,
                 bool new_frame);
    // Forgets the guest frames seen so far (the presenter was not used)
    void reset() { history = 0; }
    // What the last present showed: a game frame shown for the first time,
    // a generated frame, or an image shown before
    enum class Shown { GameFrame, Generated, Repeated };
    Shown lastShown() const { return last_shown; }

  private:
    // Smallest canvas dimension to generate frames for
    static constexpr u32 MIN_SIZE = 64;
    // Luma level the final motion field is at (luma is half size, so 1/8)
    static constexpr u32 FINEST_FLOW_LEVEL = 2;
    static constexpr u32 FLOW_LEVEL_COUNT = 5;

    struct Slot {
        MTL::Texture* color{nullptr};
        // Half size, with mipmaps for the coarse to fine motion search
        MTL::Texture* luma{nullptr};
    };

    // Optical flow, per direction: 0 is previous to current frame (forward),
    // 1 is current to previous (backward). Texels hold the motion in
    // normalized coordinates (xy) and the match cost (z).
    struct Flow {
        // Raw search result per level, finest first
        std::vector<MTL::Texture*> levels;
        // Finest level with outliers removed
        MTL::Texture* filtered{nullptr};
    };

    MTL::Device* device;
    Mtl4Context* mtl4_context;
    bool pipelines_valid{false};
    MTL::ComputePipelineState* luma_pipeline{nullptr};
    MTL::ComputePipelineState* search_pipeline{nullptr};
    MTL::ComputePipelineState* filter_pipeline{nullptr};
    MTL::ComputePipelineState* interpolate_pipeline{nullptr};
    MTL::ComputePipelineState* motion_pipeline{nullptr};
    MTL::Function* present_vertex{nullptr};
    MTL::Function* present_fragment{nullptr};
    MTL::RenderPipelineState* present_pipeline{nullptr};
    MTL::PixelFormat present_format{MTL::PixelFormatInvalid};
    MTL::RenderPassDescriptor* present_pass;

    // Resources for the current canvas size
    u32 width{0};
    u32 height{0};
    MTL::PixelFormat format{MTL::PixelFormatInvalid};
    std::array<Slot, 3> slots{};
    std::array<Flow, 2> flows{};
    u32 flow_level_count{0};
    bool resources_valid{false};
    // 1x1 stand-in for the coarser level of the coarsest search
    MTL::Texture* zero_flow{nullptr};
    MTL::Texture* generated{nullptr};

    // MetalFX frame interpolation, null if unsupported. One of the API
    // specific interpolators below.
    MTLFX::FrameInterpolatorBase* metalfx{nullptr};
    MTLFX::FrameInterpolator* metalfx_mtl3{nullptr};
    MTL4FX::FrameInterpolator* metalfx_mtl4{nullptr};
    MTL::Texture* metalfx_motion{nullptr};
    MTL::Texture* metalfx_depth{nullptr};
    MTL::Texture* metalfx_output{nullptr};
    bool metalfx_depth_cleared{false};
    u64 metalfx_last_frame{0};

    // Roles of the slots
    u32 canvas_slot{0};
    u32 current_slot{1};
    u32 previous_slot{2};
    // Guest frames in the slots (0 to 2)
    u32 history{0};
    u64 frame_index{0};
    Shown last_shown{Shown::Repeated};
    // The game frame shown last, to tell a repeat from a first showing
    u64 shown_frame_index{~0ull};
    // The guest frame the motion was searched for, and whether it includes
    // MetalFX's motion texture. The search only runs when a refresh shows a
    // generated frame: a game that keeps up with the display needs none.
    u64 flow_frame_index{~0ull};
    bool flow_has_metalfx{false};
    std::chrono::steady_clock::time_point last_frame_time{};
    f32 frame_delta_time{1.0f / 30.0f};
    u64 generated_frame_index{~0ull};
    f32 generated_fraction{-1.0f};
    bool generated_with_metalfx{false};
    MTL::Texture* generated_source{nullptr};
    bool repeated_generated{false};

    bool createPipelines();
    bool resize(u32 width, u32 height, MTL::PixelFormat format);
    void releaseResources();
    MTL::Texture* newTexture(u32 texture_width, u32 texture_height,
                             MTL::PixelFormat texture_format,
                             MTL::TextureUsage usage, u32 mip_count = 1);

    void pushFrame(CommandBuffer* command_buffer);
    MTL::Texture* getFrameForRefresh(CommandBuffer* command_buffer,
                                     FrameGeneration mode);
    void encodeLuma(CommandBuffer* command_buffer, const Slot& slot);
    void encodeFlow(CommandBuffer* command_buffer, bool metalfx_enabled);
    void encodeInterpolation(CommandBuffer* command_buffer, f32 t);
    bool encodeMetalFx(CommandBuffer* command_buffer);
    void encodePresent(CommandBuffer* command_buffer, MTL::Texture* source,
                       MTL::Texture* drawable);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
