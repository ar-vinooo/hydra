#pragma once

#include "core/hw/tegra_x1/gpu/renderer/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/gpu_pass_profiler.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/completion_queue.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/encoder.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/mtl4_context.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class UploadBufferPool;
struct Mtl4Submission;

// How the GPU finished a command buffer
struct GpuCompletion {
    bool succeeded;
    // Seconds, in the host's GPU timeline
    f64 gpu_start_time;
    f64 gpu_end_time;
    // Empty if it succeeded
    std::string error;
};
using GpuCompletionHandler = std::function<void(const GpuCompletion&)>;

enum class EncoderType {
    None,
    Render,
    Compute,
    Blit,
};

struct MtlBufferState {
    MTL::Buffer* buffer{nullptr};
    u64 offset{0};
};

struct EncoderRenderState {
    MTL::RenderPipelineState* pipeline{nullptr};
    MTL::DepthStencilState* depth_stencil_state{nullptr};
    MTL::CullMode cull_mode{MTL::CullModeNone};
    MTL::Winding front_face_winding{MTL::WindingClockwise};
    std::optional<std::array<float, 3>> depth_bias;
    std::optional<std::array<float, 4>> blend_color;
    std::optional<std::array<u32, 2>> stencil_references;
    std::array<MTL::Viewport, VIEWPORT_COUNT> viewports{};
    std::array<MTL::ScissorRect, VIEWPORT_COUNT> scissors{};
    usize viewport_count{0};
    usize scissor_count{0};
    std::array<std::array<MtlBufferState, BUFFER_COUNT>,
               static_cast<usize>(ShaderType::Count)>
        buffers{};
    std::array<std::array<MTL::Texture*, TEXTURE_COUNT>,
               static_cast<usize>(ShaderType::Count)>
        textures{};
    std::array<std::array<MTL::SamplerState*, TEXTURE_COUNT>,
               static_cast<usize>(ShaderType::Count)>
        samplers{};
};

struct EncoderState {
    MTL::RenderPassDescriptor* render_pass{nullptr};
    EncoderRenderState render{};
};

class CommandBuffer final : public ICommandBuffer {
  public:
    // `completions_` runs the work that depends on this buffer finishing
    // (queued guest reports, see Renderer::queueOcclusionReport). Without it
    // nothing can depend on the buffer.
    explicit CommandBuffer(MTL::CommandQueue* command_queue_,
                           std::shared_ptr<CompletionQueue> completions_ = {});
    // Encodes with Metal 4
    explicit CommandBuffer(std::shared_ptr<Mtl4Context> mtl4_context_,
                           std::shared_ptr<CompletionQueue> completions_ = {});
    ~CommandBuffer() override;

    bool usesMetal4() const { return mtl4_context != nullptr; }

    RenderEncoder* getRenderCommandEncoderUnchecked() {
        ASSERT_DEBUG(encoder_type == EncoderType::Render, MetalRenderer,
                     "Render command encoder not active");
        return &render_encoder;
    }
    RenderEncoder*
    getRenderCommandEncoder(MTL::RenderPassDescriptor* render_pass_descriptor);
    RenderEncoder* createRenderCommandEncoder(
        MTL::RenderPassDescriptor* render_pass_descriptor);
    // Encoder for draws that set their state on the encoder directly instead
    // of through the state setters below (e.g. texture blits). When it
    // continues the open pass, the state earlier draws left that such draws
    // do not set is reset to that of a new encoder.
    RenderEncoder* getDirectRenderCommandEncoder(
        MTL::RenderPassDescriptor* render_pass_descriptor,
        MTL::DepthStencilState* depth_stencil_disabled);

    BlitEncoder* getBlitCommandEncoderUnchecked() {
        ASSERT_DEBUG(encoder_type == EncoderType::Blit, MetalRenderer,
                     "Blit command encoder not active");
        return &blit_encoder;
    }
    BlitEncoder* getBlitCommandEncoder();

    ComputeEncoder* getComputeCommandEncoder();

    void endEncoding();

    // Runs once the GPU finished the command buffer, on a Metal thread. Only
    // before the command buffer is committed.
    void addCompletedHandler(GpuCompletionHandler handler);
    // Presents the drawable once the commands before it finished
    void presentDrawable(CA::MetalDrawable* drawable);
    // Ends encoding for work another framework encodes into the native
    // command buffer (MetalFX). With Metal 4, returns the fence that work has
    // to wait for and update; the textures it uses have to be tracked.
    MTL::Fence* beginExternalEncoding();
    // Null with Metal 3
    MTL4::CommandBuffer* getMtl4CommandBuffer() {
        has_commands = true;
        return mtl4_command_buffer;
    }
    // Metal 4: keeps the resource alive and resident until the GPU finished
    // this command buffer
    void track(const MTL::Buffer* buffer);
    void track(const MTL::Texture* texture);
    // Metal 4: copies inline bytes to memory the GPU reads until it finished
    // this command buffer
    MTL::GPUAddress allocateTransient(const void* bytes, usize length);
    // Commits the native command buffer
    void commitNative();
    // Submit, wait and continue with a new command buffer
    f64 debugCommitAndWait();
    // Checkpoints which waited for the host GPU, all threads
    static u64 getCheckpointCount() {
        return checkpoint_total.load(std::memory_order_acquire);
    }
    static u64 getSubmissionCount() {
        return submitted_buffers.load(std::memory_order_acquire);
    }
    // Commands encoded since the last checkpoint on any thread write guest
    // memory (a shader writing a storage buffer, which aliases it). Their
    // results are only visible to the CPU after a checkpoint.
    void noteGuestMemoryWrites() {
        guest_write_epoch.fetch_add(1, std::memory_order_acq_rel);
    }
    static bool hasUnsynchronizedGuestWrites() {
        return guest_write_epoch.load(std::memory_order_acquire) !=
               guest_write_synced.load(std::memory_order_acquire);
    }

    // Completion of the Metal command buffer being encoded. Creating it
    // registers the work that must run once the host finished it, so the
    // command buffer is always committed eventually.
    CompletionToken* getCompletionToken();
    const std::shared_ptr<CompletionToken>& shareCompletionToken();
    // Work depending on this buffer was queued: its completion callbacks have
    // to follow that work
    void noteQueuedCompletionWork() { queued_completion_work = true; }
    void setVisibilityResultBuffer(MTL::Buffer* buffer);
    MTL::Buffer* getVisibilityResultBuffer() const { return visibility_buffer; }
    bool hasRenderPass(MTL::RenderPassDescriptor* descriptor) const {
        return encoder_type == EncoderType::Render &&
               encoder_state.render_pass == descriptor;
    }
    // A render pass on the descriptor would use the open encoder: it is the
    // open pass, or only continues it (see getRenderCommandEncoder)
    bool continuesOpenRenderPass(MTL::RenderPassDescriptor* descriptor) const;
    bool lastSubmissionSucceeded() const { return last_submission_succeeded; }
    void retireUploadBuffer(std::shared_ptr<UploadBufferPool> pool,
                            MTL::Buffer* buffer);

    // Encoder state setting
    void setRenderPipelineState(MTL::RenderPipelineState* pipeline);
    void setDepthStencilState(MTL::DepthStencilState* depth_stencil_state);
    void setCullMode(MTL::CullMode cull_mode);
    void setFrontFaceWinding(MTL::Winding front_face_winding);
    void setDepthBias(float constant, float slope, float clamp);
    void setBlendColor(float r, float g, float b, float a);
    void setStencilReferences(u32 front, u32 back);
    void setViewports(std::span<const MTL::Viewport> viewports);
    void setScissors(std::span<const MTL::ScissorRect> scissors);
    void setBuffer(MTL::Buffer* buffer, u64 offset, ShaderType shader_type,
                   u32 index);
    void setTexture(MTL::Texture* texture, ShaderType shader_type, u32 index);
    void setSampler(MTL::SamplerState* sampler, ShaderType shader_type,
                    u32 index);

  private:
    void commitSubmission();
    void attachGpuCompletionCallbacks();
    // Starts a new native command buffer
    void beginNative();
    // Waits for the committed native command buffer
    GpuCompletion waitNative();
    MTL::Device* getDevice() const;
    MTL::CommandQueue* command_queue{nullptr};
    MTL::CommandBuffer* command_buffer{nullptr};
    MTL::CommandEncoder* command_encoder{nullptr};
    EncoderType encoder_type{EncoderType::None};
    RenderEncoder render_encoder;
    ComputeEncoder compute_encoder;
    BlitEncoder blit_encoder;

    // Metal 4
    // Shared with the submissions in flight, whose completion uses its pools
    std::shared_ptr<Mtl4Context> mtl4_context;
    MTL4::CommandBuffer* mtl4_command_buffer{nullptr};
    MTL4::CommandEncoder* mtl4_encoder{nullptr};
    // What the GPU needs until it finished the native command buffer
    std::shared_ptr<Mtl4Submission> mtl4_submission;
    // Bindings of the render and compute stages. Encoders snapshot them at
    // each draw and dispatch.
    MTL4::ArgumentTable* vertex_table{nullptr};
    MTL4::ArgumentTable* fragment_table{nullptr};
    MTL4::ArgumentTable* compute_table{nullptr};
    void createArgumentTables();

    EncoderState encoder_state{};
    MTL::Buffer* visibility_buffer{nullptr};
    bool last_submission_succeeded{true};
    // Anything was encoded on, or attached to, the current Metal command
    // buffer since it was created
    bool has_commands{false};
    // Commits and completions of every wrapper: when they are equal nothing
    // is in flight and a checkpoint on an untouched command buffer has
    // nothing to wait for
    static inline std::atomic<u64> submitted_buffers{0};
    static inline std::atomic<u64> completed_buffers{0};
    static inline std::atomic<u64> checkpoint_total{0};
    // Encoded work which may write guest memory, and how much of it a
    // checkpoint already waited for
    static inline std::atomic<u64> guest_write_epoch{0};
    static inline std::atomic<u64> guest_write_synced{0};
    std::shared_ptr<CompletionQueue> completions;
    std::shared_ptr<CompletionToken> completion_token;
    bool queued_completion_work{false};
    std::shared_ptr<UploadBufferPool> upload_pool;
    std::vector<MTL::Buffer*> retired_upload_buffers;
    u64 raster_updates{0};
    u64 raster_skips{0};
    u64 render_encoders{0};
    u64 blit_encoders{0};
    // DEBUG: passes of the Metal command buffer whose GPU time is sampled,
    // and the attachments of the previous render pass (HYDRA_PERF_STATS)
    std::vector<GpuPassProfiler::Pass> profiled_passes;
    // Metal 4: start sample of the open encoder's pass, written by the
    // encoder itself
    u32 mtl4_pass_sample{invalid<u32>()};
    void writeMtl4PassTimestamp(bool end);
    std::array<MTL::Texture*, COLOR_TARGET_COUNT + 2> previous_attachments{};
    // DEBUG: the previous render pass, to show why a continuation of it was
    // not merged
    std::string previous_pass_description;
    // What ended the previous render pass: 0 another render pass, 1 a blit,
    // 2 a compute encoder, 3 a submission, 4 a new visibility buffer, 5 an
    // explicit end elsewhere
    u8 render_pass_gap{0};
    void profileRenderPass(MTL::RenderPassDescriptor* descriptor);
    u32 reservePassSamples(GpuPassProfiler::Kind kind, std::string label);
    void attachPassProfiling();

  public:
    // Handlers and drawables attached through this are part of the work
    MTL::CommandBuffer* getCommandBuffer() {
        has_commands = true;
        return command_buffer;
    }

  public:
    // ABI-stable implementations selected by the live module.
    void retireUploadBufferHot0(std::shared_ptr<UploadBufferPool> pool,
                                MTL::Buffer* buffer);
    RenderEncoder* getRenderCommandEncoderHot1(
        MTL::RenderPassDescriptor* render_pass_descriptor);
    RenderEncoder* createRenderCommandEncoderHot2(
        MTL::RenderPassDescriptor* render_pass_descriptor);
    BlitEncoder* getBlitCommandEncoderHot3();
    ComputeEncoder* getComputeCommandEncoderHot4();
    f64 debugCommitAndWaitHot5();
    void endEncodingHot6();
    void setRenderPipelineStateHot7(MTL::RenderPipelineState* pipeline);
    void setDepthStencilStateHot8(MTL::DepthStencilState* depth_stencil_state);
    void setCullModeHot9(MTL::CullMode cull_mode);
    void setFrontFaceWindingHot10(MTL::Winding front_face_winding);
    void setDepthBiasHot11(float constant, float slope, float clamp);
    void setBlendColorHot12(float r, float g, float b, float a);
    void setStencilReferencesHot13(u32 front, u32 back);
    void setViewportsHot14(std::span<const MTL::Viewport> values);
    void setScissorsHot15(std::span<const MTL::ScissorRect> values);
    void setBufferHot16(MTL::Buffer* buffer, u64 offset, ShaderType shader_type,
                        u32 index);
    void setTextureHot17(MTL::Texture* texture, ShaderType shader_type,
                         u32 index);
    void setSamplerHot18(MTL::SamplerState* sampler, ShaderType shader_type,
                         u32 index);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
