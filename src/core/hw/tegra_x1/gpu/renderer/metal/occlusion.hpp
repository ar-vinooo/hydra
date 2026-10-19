#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "core/hw/tegra_x1/gpu/renderer/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/completion_queue.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

// Visibility results of one render encoder, 8 bytes per counted draw. A new
// render pass can reset a visibility buffer, so encoders never share one.
struct OcclusionPage {
    static constexpr u32 SLOTS = 4096;
    MTL::Buffer* buffer{nullptr};
    // Slots handed to draws, only the GPU thread changes it
    u32 used{0};
};

// Zeroed pages: creating a Metal buffer is a kernel call
class OcclusionPagePool
    : public std::enable_shared_from_this<OcclusionPagePool> {
  public:
    explicit OcclusionPagePool(MTL::Device* device_) : device{device_} {}
    ~OcclusionPagePool();

    // The page returns to the pool when the last reference is gone, which
    // only happens after the GPU finished writing to it
    std::shared_ptr<OcclusionPage> acquire();

  private:
    static constexpr usize MAX_FREE_PAGES = 256;
    void release(OcclusionPage* page);

    MTL::Device* device;
    std::mutex mutex;
    std::vector<MTL::Buffer*> free_buffers;
};

// ZPASS pixel counting (SET_ZPASS_PIXEL_COUNT, CLEAR_REPORT_VALUE and the
// reports of the counter).
//
// Draws count into pages of Metal visibility results. The counter at a point
// of the command stream is the sum of the results of all earlier draws since
// the last reset. It is only known once the GPU finished those draws, so
// everything is handled in stream order by the completion queue: segments of
// pages are added to a running total, a reset clears it and a report publishes
// it, each as soon as the command buffers they depend on completed. Nothing
// waits for the GPU on the encoding thread.
class OcclusionCounter {
  public:
    OcclusionCounter(MTL::Device* device,
                     std::shared_ptr<CompletionQueue> completions_);

    void setEnabled(bool value) { enabled = value; }

    // Called before a draw of the render pass. Returns the offset in the
    // visibility result buffer that counts the draw, none if counting is off.
    std::optional<u64> prepareDraw(CommandBuffer& command_buffer,
                                   MTL::RenderPassDescriptor* render_pass);
    // CLEAR_REPORT_VALUE: draws so far no longer count
    void reset();
    // Publishes the counter at this point of the stream. Returns the sequence
    // number of the report in the completion queue.
    u64 queueReport(CommandBuffer& command_buffer, ReportPublisher publish,
                    std::vector<ztd::Range<uptr>> writes);

  private:
    // Only the completion queue touches the totals after their creation
    struct Totals {
        u64 value{0};
        // A failed command buffer loses samples: the counter stays invalid
        // until the next reset
        bool valid{true};
    };
    // Results of the draws of the open page which no report or reset covers
    struct OpenSegment {
        std::shared_ptr<OcclusionPage> page;
        std::shared_ptr<CompletionToken> token;
        u32 begin{0};
    };

    void closeSegment();

    std::shared_ptr<CompletionQueue> completions;
    std::shared_ptr<OcclusionPagePool> pool;
    std::shared_ptr<Totals> totals{std::make_shared<Totals>()};
    OpenSegment open;
    bool enabled{false};
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
