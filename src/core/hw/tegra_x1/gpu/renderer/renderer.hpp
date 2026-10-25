#pragma once

#include "core/hw/tegra_x1/gpu/capture/guest_events.hpp"
#include "core/hw/tegra_x1/gpu/optimization_policy.hpp"
#include "core/hw/tegra_x1/gpu/resource/content_version.hpp"
#include "core/hw/tegra_x1/gpu/resource/range_set.hpp"
#include "core/hw/tegra_x1/gpu/resource/resource_registry.hpp"

#include "core/hw/tegra_x1/gpu/renderer/buffer_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_view.hpp"
#include "core/hw/tegra_x1/gpu/renderer/index_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/pipeline_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/render_pass_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/sampler_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/shader_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"

#include "core/hw/tegra_x1/gpu/renderer/texture_binding.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace hydra::hw::tegra_x1::gpu::renderer {

class ICommandBuffer;
class ISurfaceCompositor;
class ITexture;
class ITextureView;
class SamplerBase;
class RenderPassBase;
class PipelineBase;

struct Info {
    bool supports_quads_primitive;

    bool isPrimitiveSupported(engines::PrimitiveType primitive_type) const {
        // NOLINTNEXTLINE(readability-trivial-switch)
        switch (primitive_type) {
        case engines::PrimitiveType::Quads:
            return supports_quads_primitive;
        default:
            return true;
        }
    }
};

// A Kepler compute grid launch
struct ComputeDispatch {
    ShaderBase* shader{nullptr};
    // Indexed by the host binding
    std::array<BufferView, CONST_BUFFER_BINDING_COUNT> uniform_buffers{};
    struct StorageBufferBinding {
        u32 index;
        BufferView buffer;
        // The shader may write it, and it aliases guest memory
        bool written{false};
    };
    std::vector<StorageBufferBinding> storage_buffers;
    struct TextureBinding {
        u32 index;
        ITextureView* texture;
        SamplerBase* sampler;
    };
    std::vector<TextureBinding> textures;
    struct ImageBinding {
        u32 index;
        ITextureView* texture;
    };
    std::vector<ImageBinding> images;
    MTL::Size grid_size;
    MTL::Size block_size;
    // DEBUG: guest program address
    u64 debug_program_addr{0};
};

enum class MemoryInvalidationScope {
    None = 0,
    BufferCache = ZTD_BIT(0),
    TextureCache = ZTD_BIT(1),
    ShaderCache = ZTD_BIT(2),
};
ZTD_ENABLE_ENUM_BITWISE_OPERATORS(MemoryInvalidationScope)

class DrawQueue;

class IRenderer {
  public:
    IRenderer()
        : buffer_cache(*this), texture_cache(*this), sampler_cache(*this),
          render_pass_cache(*this), shader_cache(*this), pipeline_cache(*this),
          index_cache(*this) {}
    virtual ~IRenderer() = default;

    void recordGpuWrites(
        ICommandBuffer* command_buffer,
        std::span<const ztd::Range<uptr>> ranges,
        capture::GuestEventOrigin origin = capture::currentGuestEventOrigin()) {
        std::vector<ztd::Range<uptr>> merged;
        const bool coalesce = optimization::enabled();
        if (coalesce)
            merged = resource::coalesceRanges(ranges);
        auto write = content_versions.issueGpuWrites(
            coalesce ? std::span<const ztd::Range<uptr>>(merged) : ranges);
        if (write.version() != 0) {
            const auto version = write.version();
            const auto command_identity =
                reinterpret_cast<uptr>(command_buffer);
            auto& events = capture::GuestEvents::instance();
            if (events.enabled())
                for (const auto& range : ranges)
                    if (range.getBegin() < range.getEnd())
                        events.event("gpu_write_issued", origin,
                                     range.getBegin(), range.getSize(), version,
                                     0, command_identity);
            command_buffer->onGpuCompletedChecked(
                [write = std::move(write), origin, version,
                 command_identity](bool succeeded) {
                    write.complete(succeeded);
                    capture::GuestEvents::instance().event(
                        "gpu_complete", origin, 0, 0, version,
                        succeeded ? 1 : 0, command_identity);
                });
        }
    }

    void invalidateMemory(
        ztd::Range<uptr> range,
        MemoryInvalidationScope scope = MemoryInvalidationScope::BufferCache |
                                        MemoryInvalidationScope::TextureCache |
                                        MemoryInvalidationScope::ShaderCache) {
        recordMemoryWrite(range, scope);
        invalidateMemoryCaches(range, scope);
    }

  private:
    void recordMemoryWrite(ztd::Range<uptr> range,
                           MemoryInvalidationScope scope) {
        // The bytes behind this range changed (guest CPU or DMA write)
        const auto version = content_versions.write(range);
        auto& events = capture::GuestEvents::instance();
        if (events.enabled() && range.getBegin() < range.getEnd()) {
            events.memory("memory_write", range.getBegin(), range.getSize(),
                          version, capture::currentGuestEventOrigin());
            events.event("resource_invalidate",
                         capture::currentGuestEventOrigin(), range.getBegin(),
                         range.getSize(), version, static_cast<u64>(scope));
        }
    }

    void invalidateMemoryCaches(ztd::Range<uptr> range,
                                MemoryInvalidationScope scope) {
        if (any(scope & MemoryInvalidationScope::BufferCache)) {
            std::scoped_lock lock(buffer_cache.getMutex());
            buffer_cache.invalidateMemory(range);
        }

        if (any(scope & MemoryInvalidationScope::TextureCache)) {
            std::scoped_lock lock(texture_cache.getMutex());
            texture_cache.invalidateMemory(range);
        }

        if (any(scope & MemoryInvalidationScope::ShaderCache)) {
            // ShaderCache validates a full mapped code snapshot at every
            // lookup, including untracked CPU writes and GPU remaps. No
            // pointer-based eviction is needed for this host-memory range.
        }
    }

  public:
    // The caches are locked by the GPU thread for whole draws: other threads
    // queue their invalidations for it, it applies them between its entries
    void queueMemoryInvalidation(ztd::Range<uptr> range) {
        // Reports publish from a completion thread. Preserve their bytes at
        // publication, before a later decoder consumes the invalidation.
        auto& events = capture::GuestEvents::instance();
        if (events.enabled() && range.getBegin() < range.getEnd())
            events.memory("memory_published", range.getBegin(),
                          range.getSize());
        std::scoped_lock lock(queued_invalidation_mutex);
        queued_invalidations.push_back(range);
        has_queued_invalidations.store(true, std::memory_order_release);
    }
    void applyQueuedMemoryInvalidations() {
        if (!has_queued_invalidations.load(std::memory_order_acquire))
            return;
        std::vector<ztd::Range<uptr>> ranges;
        {
            std::scoped_lock lock(queued_invalidation_mutex);
            ranges.swap(queued_invalidations);
            has_queued_invalidations.store(false, std::memory_order_relaxed);
        }
        if (optimization::enabled()) {
            const auto scope = MemoryInvalidationScope::BufferCache |
                               MemoryInvalidationScope::TextureCache |
                               MemoryInvalidationScope::ShaderCache;
            // Keep each epoch/trace transition in its original order. Only
            // cache eviction work is coalesced at this existing boundary.
            for (const auto& range : ranges)
                recordMemoryWrite(range, scope);
            for (const auto& range : resource::coalesceRanges(ranges))
                invalidateMemoryCaches(range, scope);
        } else {
            for (const auto& range : ranges)
                invalidateMemory(range);
        }
    }

    // Writes GPU-rendered textures in the range back to guest memory, from a
    // CPU thread that is about to access it (memory tracking faults)
    void flushMemoryForCpu(ztd::Range<uptr> range) {
        std::scoped_lock lock(texture_cache.getMutex());
        std::unique_ptr<ICommandBuffer> command_buffer(createCommandBuffer());
        texture_cache.flushMemory(command_buffer.get(), range);
    }

    virtual u64 getAllocatedResourceBytes() const { return 0; }
    // Encodes draws on another thread, if the backend has one
    virtual DrawQueue* getDrawQueue() { return nullptr; }
    // DEBUG: logs the GPU time per pass since the last call (HYDRA_PERF_STATS)
    virtual void logPassProfile([[maybe_unused]] f64 frames) {}

    // Surface
    virtual void setSurface(void* surface) = 0;
    virtual ISurfaceCompositor* acquireNextSurface() = 0;

    // Buffer
    virtual BufferBase* createBuffer(u64 size) = 0;
    // Immutable CPU-uploaded snapshots avoid interrupting a render encoder.
    // Backends may decline large buffers or retain the original copy path.
    virtual BufferBase*
    createCpuBufferSnapshot([[maybe_unused]] ztd::Range<uptr> range) {
        return nullptr;
    }
    virtual BufferBase* allocateTemporaryBuffer(const u64 size) = 0;
    virtual void freeTemporaryBuffer(BufferBase* buffer) = 0;

    virtual BufferBase* allocateTextureUploadBuffer(u64 size) {
        return allocateTemporaryBuffer(size);
    }
    virtual void
    retireTextureUploadBuffer([[maybe_unused]] ICommandBuffer* command_buffer,
                              BufferBase* buffer) {
        freeTemporaryBuffer(buffer);
    }

    // Texture
    virtual ITexture* createTexture(const TextureDescriptor& descriptor) = 0;
    virtual void blitTexture(ICommandBuffer* command_buffer, ITextureView* src,
                             simd::float3 src_origin, MTL::Size src_size, u32 src_level,
                             u32 src_layer, ITextureView* dst,
                             simd::float3 dst_origin, MTL::Size dst_size, u32 dst_level,
                             u32 dst_layer, u32 level_count,
                             u32 layer_count, const BlitOptions& options) = 0;

    // Sampler
    virtual SamplerBase* createSampler(const SamplerDescriptor& descriptor) = 0;

    // Command buffer
    virtual ICommandBuffer* createCommandBuffer() = 0;
    virtual bool synchronize([[maybe_unused]] ICommandBuffer* command_buffer) {
        return true;
    }
    // Like synchronize, but only waits if commands which may write guest
    // memory were encoded since the last checkpoint. Shader code is read from
    // guest memory by the CPU.
    virtual bool synchronizeGuestWrites(ICommandBuffer* command_buffer) {
        return synchronize(command_buffer);
    }
    virtual void setOcclusionCounterEnabled([[maybe_unused]] bool enabled) {}
    virtual void resetOcclusionCounter() {}
    // Reports the ZPASS counter as it is at this point of the command stream.
    // `publish` runs once the GPU finished the draws before the report and
    // the reports queued earlier: on a thread of the renderer, not on the
    // caller's. `writes` are the host ranges it writes, see flushReports. The
    // callbacks of the command buffer follow the report.
    virtual void queueOcclusionReport([[maybe_unused]] ICommandBuffer* command_buffer,
                                      ReportPublisher publish,
                                      [[maybe_unused]] std::vector<ztd::Range<uptr>> writes) {
        publish(0);
    }
    // Runs `write` once the GPU finished the commands encoded so far, after
    // the reports and writes queued before it. It runs on a thread of the
    // renderer, not on the caller's, and `succeeded` is false if the commands
    // failed. `writes` are the host ranges it writes, see flushReports. This is
    // how a semaphore release or a syncpoint increment reaches the guest
    // without the GPU thread waiting for the GPU.
    virtual void queueGuestWrite([[maybe_unused]] ICommandBuffer* command_buffer,
                                 std::function<void(bool succeeded)> write,
                                 [[maybe_unused]] std::vector<ztd::Range<uptr>> writes) {
        write(true);
    }
    // Makes sure that no queued report is going to write to the range
    // anymore, submitting the command buffer and waiting for the GPU if one
    // still does. Anything which reads or writes guest memory in the stream
    // order has to call it for the memory that a report may have targeted.
    virtual void flushReports([[maybe_unused]] ICommandBuffer* command_buffer,
                              [[maybe_unused]] ztd::Range<uptr> range) {}
    // Waits until all queued reports reached guest memory or were dropped.
    // Whatever tells the guest that earlier work is done (a fence, a released
    // semaphore) calls it after waiting for the GPU, so that the guest finds
    // the reports of that work.
    virtual void drainReports() {}

    // Render pass
    virtual RenderPassBase*
    createRenderPass(const RenderPassDescriptor& descriptor) = 0;
    virtual void bindRenderPass(const RenderPassBase* render_pass) = 0;

    // Clear
    // Area and stencil write mask of the following clears
    virtual void setClearState(const ClearState& clear_state) = 0;
    virtual void clearColor(ICommandBuffer* command_buffer,
                            u32 render_target_id, u32 layer, u8 mask,
                            const simd::uint4 color) = 0;
    virtual void clearDepth(ICommandBuffer* command_buffer, u32 layer,
                            const float value) = 0;
    virtual void clearStencil(ICommandBuffer* command_buffer, u32 layer,
                              const u32 value) = 0;

    // Shader
    // The title whose shaders are stored and prewarmed, set before it starts
    virtual void setTitleId([[maybe_unused]] u64 title_id) {}
    virtual ShaderBase* createShader(const ShaderDescriptor& descriptor) = 0;

    // Pipeline
    virtual PipelineBase*
    createPipeline(const PipelineDescriptor& descriptor) = 0;
    virtual void bindPipeline(const PipelineBase* pipeline) = 0;

    // Depth stencil
    virtual void setDepthTestEnabled(bool enabled) = 0;
    virtual void setDepthWriteEnabled(bool enabled) = 0;
    virtual void setDepthCompareOp(engines::CompareOp op) = 0;
    virtual void setBlendConstant(const simd::float4 color) = 0;
    virtual void setDepthBias(f32 constant, f32 slope, f32 clamp) = 0;
    virtual void setStencilState(const StencilState& stencil) = 0;
    virtual void setCullState(bool enabled, engines::CullFaceMode mode,
                              engines::Winding front_face) = 0;
    // Depth outside the clip range is clamped instead of clipped
    virtual void setDepthClamp(bool enabled) = 0;
    // Triangles are drawn as their edges (polygon mode LINE)
    virtual void setPolygonLines(bool enabled) = 0;

    // DEBUG: save the current contents of a texture to a PNG file
    virtual void
    debugDumpTexture([[maybe_unused]] ICommandBuffer* command_buffer,
                     [[maybe_unused]] ITextureView* view,
                     [[maybe_unused]] const std::string& path) {}

    // Viewport and scissor
    virtual void setViewport(u32 index, const MTL::Viewport& viewport) = 0;
    virtual void setScissor(u32 index, const MTL::ScissorRect& scissor) = 0;

    // Resource binding
    virtual void bindVertexBuffer(const BufferView& buffer, u32 index) = 0;
    virtual void bindIndexBuffer(const BufferView& index_buffer,
                                 engines::IndexType index_type) = 0;
    virtual void bindUniformBuffer(const BufferView& buffer,
                                   ShaderType shader_type, u32 index) = 0;
    // Storage buffers alias guest memory: shader writes land in the guest's
    // pages without a copy. The range is rounded out to host pages
    virtual BufferView getGuestMemoryBuffer(ztd::Range<uptr> range) = 0;
    virtual void bindStorageBuffer(const BufferView& buffer,
                                   ShaderType shader_type, u32 index) = 0;
    virtual void bindTexture(ITextureView* texture, SamplerBase* sampler,
                             ShaderType shader_type, u32 index) = 0;
    virtual std::vector<TextureBinding> getTextureBindings(
        [[maybe_unused]] ShaderType shader_type) const { return {}; }
    // TODO: images

    // Resource unbinding
    virtual void unbindUniformBuffers(ShaderType shader_type) = 0;
    virtual void unbindStorageBuffers(ShaderType shader_type) = 0;
    virtual void unbindTextures(ShaderType shader_type) = 0;

    // DEBUG: submit the recorded work, wait for it and return its GPU time in
    // milliseconds
    virtual f64
    debugSyncCommandBuffer([[maybe_unused]] ICommandBuffer* command_buffer) {
        return 0.0;
    }

    // Blocking copies of a texture's host contents for capture and replay:
    // one blob per layer and level (layer major, tightly packed rows, depth
    // before stencil for combined formats). The caller submits earlier work
    // that writes the texture first. False if the format is not supported.
    virtual bool readTextureContents([[maybe_unused]] ITexture* texture,
                                     [[maybe_unused]] std::vector<std::vector<u8>>& out) {
        return false;
    }
    virtual bool writeTextureContents(
        [[maybe_unused]] ITexture* texture,
        [[maybe_unused]] const std::vector<std::vector<u8>>& contents) {
        return false;
    }
    // Blocking copy of the bytes a buffer view covers, also from buffers the
    // CPU cannot map. Earlier work that writes it has to be submitted.
    virtual bool readBufferContents([[maybe_unused]] const BufferView& view,
                                    [[maybe_unused]] std::vector<u8>& out) {
        return false;
    }

    // Compute
    // True for a successfully encoded dispatch or an empty no-op. False must
    // not be treated as an issued write by command/resource tracking.
    virtual bool
    dispatchCompute([[maybe_unused]] ICommandBuffer* command_buffer,
                    [[maybe_unused]] const ComputeDispatch& dispatch) {
        return false;
    }

    // Draw
    virtual void draw(ICommandBuffer* command_buffer,
                      const engines::PrimitiveType primitive_type,
                      const u32 start, const u32 count, const u32 base_instance,
                      const u32 instance_count) = 0;
    virtual void drawIndexed(ICommandBuffer* command_buffer,
                             const engines::PrimitiveType primitive_type,
                             const u32 start, const u32 count,
                             const u32 base_vertex, const u32 base_instance,
                             const u32 instance_count) = 0;

    // Debug
    void captureFrames(u32 count) { frames_to_capture = count; }
    bool isCapturing() const {
        return capturing.load(std::memory_order_relaxed);
    }
    // Debug group name for the next draw while a GPU capture is active
    void setCaptureDrawLabel(std::string label) {
        capture_draw_label = std::move(label);
    }
    // Called on presentation. Only requests capture transitions: stopping a
    // capture while the GPU thread is encoding crashes Metal's capture layer.
    void notifyDebugFrameBoundary() {
        pollCaptureTrigger();
        if (frames_to_capture == 0)
            return;
        const auto phase = capture_phase.load(std::memory_order_acquire);
        if (phase == CapturePhase::Idle)
            capture_phase.store(CapturePhase::BeginRequested,
                                std::memory_order_release);
        else if (phase == CapturePhase::Capturing && --frames_to_capture == 0)
            capture_phase.store(CapturePhase::EndRequested,
                                std::memory_order_release);
    }
    // Called by the GPU thread between command buffers, when nothing is
    // being encoded
    void applyCaptureRequests() {
        const auto phase = capture_phase.load(std::memory_order_acquire);
        if (phase == CapturePhase::BeginRequested) {
            beginCapture();
            capturing = true;
            capture_phase.store(CapturePhase::Capturing,
                                std::memory_order_release);
        } else if (phase == CapturePhase::EndRequested) {
            capturing = false;
            endCapture();
            capture_phase.store(CapturePhase::Idle, std::memory_order_release);
        }
    }

  protected:
    Info info{};

    // Capture
    virtual void beginCapture() = 0;
    virtual void endCapture() = 0;

    // Consumed by the backend's draw calls (GPU thread)
    std::string capture_draw_label;

  private:
    std::mutex queued_invalidation_mutex;
    std::vector<ztd::Range<uptr>> queued_invalidations;
    std::atomic<bool> has_queued_invalidations{false};

    // Caches
    resource::ResourceRegistry resource_registry;
    resource::ContentVersions content_versions;
    BufferCache buffer_cache;
    TextureCache texture_cache;
    SamplerCache sampler_cache;
    RenderPassCache render_pass_cache;
    ShaderCache shader_cache;
    PipelineCache pipeline_cache;
    IndexCache index_cache;

    // Capture
    enum class CapturePhase {
        Idle,
        BeginRequested,
        Capturing,
        EndRequested,
    };
    u32 frames_to_capture{0};
    std::atomic<CapturePhase> capture_phase{CapturePhase::Idle};
    std::atomic<bool> capturing{false};

    // HYDRA_GPU_CAPTURE_DIR=<dir>: creating "<dir>/trigger" (optionally
    // containing a frame count, 1..8) captures the following frames as a
    // .gputrace in <dir>. Requires MTL_CAPTURE_ENABLED=1 outside Xcode.
    void pollCaptureTrigger() {
        static const char* dir = std::getenv("HYDRA_GPU_CAPTURE_DIR");
        if (dir == nullptr || frames_to_capture != 0)
            return;
        const auto trigger = std::filesystem::path(dir) / "trigger";
        std::error_code ec;
        if (!std::filesystem::exists(trigger, ec))
            return;
        u32 count = 1;
        {
            std::ifstream in(trigger);
            in >> count;
        }
        std::filesystem::remove(trigger, ec);
        captureFrames(std::clamp(count, 1u, 8u));
    }

  public:
    CONST_REF_GETTER(info, getInfo);
    REF_GETTER(resource_registry, getResourceRegistry);
    REF_GETTER(content_versions, getContentVersions);
    REF_GETTER(buffer_cache, getBufferCache);
    REF_GETTER(texture_cache, getTextureCache);
    REF_GETTER(sampler_cache, getSamplerCache);
    REF_GETTER(render_pass_cache, getRenderPassCache);
    REF_GETTER(shader_cache, getShaderCache);
    REF_GETTER(pipeline_cache, getPipelineCache);
    REF_GETTER(index_cache, getIndexCache);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer
