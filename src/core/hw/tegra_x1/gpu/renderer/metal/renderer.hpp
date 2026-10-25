#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/binary_archive_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/shader_library_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/frame_presenter.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/spatial_upscaler.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/blit_pipeline_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/clear_color_pipeline_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/clear_depth_pipeline_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/completion_queue.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/depth_stencil_state_cache.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/mtl4_context.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/occlusion.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/upload_buffer_pool.hpp"
#include "core/hw/tegra_x1/gpu/renderer/renderer.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {
class DrawQueue;
}

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class CommandBuffer;
class Shader;
class RenderEncoder;
class TextureView;
class Sampler;
class RenderPass;
class Pipeline;

struct CombinedTextureSampler {
    const TextureView* texture_view{nullptr};
    const Sampler* sampler{nullptr};
    std::shared_ptr<void> texture_owner;
    std::shared_ptr<void> sampler_owner;
};

struct State {
    const RenderPass* render_pass{nullptr};
    const Pipeline* pipeline{nullptr};
    std::shared_ptr<void> render_pass_owner;
    std::shared_ptr<void> pipeline_owner;
    bool depth_test_enabled;
    bool depth_write_enabled;
    engines::CompareOp depth_compare_op;
    simd::float4 blend_constant{};
    f32 depth_bias_constant{0.0f};
    f32 depth_bias_slope{0.0f};
    f32 depth_bias_clamp{0.0f};
    StencilState stencil{};
    ClearState clear{};
    MTL::CullMode cull_mode{MTL::CullModeNone};
    MTL::Winding front_face_winding{MTL::WindingClockwise};
    MTL::DepthClipMode depth_clip_mode{MTL::DepthClipModeClip};
    MTL::TriangleFillMode triangle_fill_mode{MTL::TriangleFillModeFill};
    MTL::Viewport viewports[VIEWPORT_COUNT];
    MTL::ScissorRect scissors[VIEWPORT_COUNT];
    BufferView index_buffer;
    engines::IndexType index_type{engines::IndexType::None};
    std::array<BufferView, VERTEX_ARRAY_COUNT> vertex_buffers{};
    std::array<std::array<BufferView, CONST_BUFFER_BINDING_COUNT>,
               static_cast<usize>(ShaderType::Count)>
        uniform_buffers{};
    std::array<std::array<BufferView, CONST_BUFFER_BINDING_COUNT>,
               static_cast<usize>(ShaderType::Count)>
        storage_buffers{};
    std::array<std::array<CombinedTextureSampler, TEXTURE_BINDING_COUNT>,
               static_cast<usize>(ShaderType::Count)>
        textures{};
    // TODO: images
};

class Renderer : public IRenderer {
  public:
    Renderer();
    ~Renderer() override;

    // Surface
    void setSurface(void* surface) override;
    ISurfaceCompositor* acquireNextSurface() override;

    // Buffer
    BufferBase* createBuffer(u64 size) override;
    BufferBase* createCpuBufferSnapshot(ztd::Range<uptr> range) override;
    BufferBase* allocateTemporaryBuffer(const u64 size) override;
    void freeTemporaryBuffer(BufferBase* buffer) override;
    BufferBase* allocateTextureUploadBuffer(u64 size) override;
    void retireTextureUploadBuffer(ICommandBuffer* command_buffer,
                                   BufferBase* buffer) override;

    // Texture
    ITexture* createTexture(const TextureDescriptor& descriptor) override;
    void blitTexture(ICommandBuffer* command_buffer, ITextureView* src,
                     simd::float3 src_origin, MTL::Size src_size, u32 src_level,
                     u32 src_layer, ITextureView* dst, simd::float3 dst_origin,
                     MTL::Size dst_size, u32 dst_level, u32 dst_layer,
                     u32 level_count, u32 layer_count, const BlitOptions& options) override;

    // Sampler
    SamplerBase* createSampler(const SamplerDescriptor& descriptor) override;

    // Command buffer
    ICommandBuffer* createCommandBuffer() override;

    // Render pass
    RenderPassBase*
    createRenderPass(const RenderPassDescriptor& descriptor) override;
    void bindRenderPass(const RenderPassBase* render_pass) override;

    // Clear
    void setClearState(const ClearState& clear_state) override {
        syncDrawQueue();
        state.clear = clear_state;
    }
    void clearColor(ICommandBuffer* command_buffer, u32 render_target_id,
                    u32 layer, u8 mask, const simd::uint4 color) override;
    void clearDepth(ICommandBuffer* command_buffer, u32 layer,
                    const float value) override;
    void clearStencil(ICommandBuffer* command_buffer, u32 layer,
                      const u32 value) override;

    // Shader
    u64 getAllocatedResourceBytes() const override { return device->currentAllocatedSize(); }
    DrawQueue* getDrawQueue() override { return draw_queue.get(); }
    void setTitleId(u64 title_id) override;
    void logPassProfile(f64 frames) override;
    ShaderBase* createShader(const ShaderDescriptor& descriptor) override;

    // Pipeline
    PipelineBase* createPipeline(const PipelineDescriptor& descriptor) override;
    void bindPipeline(const PipelineBase* pipeline) override;

    // Depth stencil
    void setDepthTestEnabled(bool enabled) override;
    void setDepthWriteEnabled(bool enabled) override;
    void setDepthCompareOp(engines::CompareOp op) override;
    void setBlendConstant(const simd::float4 color) override;
    void setDepthBias(f32 constant, f32 slope, f32 clamp) override;
    void setStencilState(const StencilState& stencil) override;
    void setCullState(bool enabled, engines::CullFaceMode mode,
                      engines::Winding front_face) override;
    void setDepthClamp(bool enabled) override {
        syncDrawQueue();
        state.depth_clip_mode =
            enabled ? MTL::DepthClipModeClamp : MTL::DepthClipModeClip;
    }
    void setPolygonLines(bool enabled) override {
        syncDrawQueue();
        state.triangle_fill_mode =
            enabled ? MTL::TriangleFillModeLines : MTL::TriangleFillModeFill;
    }
    void debugDumpTexture(ICommandBuffer* command_buffer, ITextureView* view,
                          const std::string& path) override;
    void debugDumpTextureLevel(ICommandBuffer* command_buffer,
                               ITextureView* view, u32 level,
                               const std::string& path);

    // Viewport and scissor
    void setViewport(u32 index, const MTL::Viewport& viewport) override;
    void setScissor(u32 index, const MTL::ScissorRect& scissor) override;

    // Resource binding
    void bindVertexBuffer(const BufferView& buffer, u32 index) override;
    void bindIndexBuffer(const BufferView& index_buffer,
                         engines::IndexType index_type) override;
    void bindUniformBuffer(const BufferView& buffer, ShaderType shader_type,
                           u32 index) override;
    BufferView getGuestMemoryBuffer(ztd::Range<uptr> range) override;
    bool readTextureContents(ITexture* texture,
                             std::vector<std::vector<u8>>& out) override;
    bool writeTextureContents(
        ITexture* texture,
        const std::vector<std::vector<u8>>& contents) override;
    bool readBufferContents(const BufferView& view,
                            std::vector<u8>& out) override;
    void bindStorageBuffer(const BufferView& buffer, ShaderType shader_type,
                           u32 index) override;
    void bindTexture(ITextureView* texture, SamplerBase* sampler,
                     ShaderType shader_type, u32 index) override;

    // Resource unbinding
    void unbindUniformBuffers(ShaderType shader_type) override;
    void unbindStorageBuffers(ShaderType shader_type) override;
    void unbindTextures(ShaderType shader_type) override;
    std::vector<TextureBinding>
    getTextureBindings(ShaderType shader_type) const override;

    f64 debugSyncCommandBuffer(ICommandBuffer* command_buffer) override;
    bool synchronize(ICommandBuffer* command_buffer) override;
    bool synchronizeGuestWrites(ICommandBuffer* command_buffer) override;
    void setOcclusionCounterEnabled(bool enabled) override {
        syncDrawQueue();
        occlusion->setEnabled(enabled);
    }
    void resetOcclusionCounter() override {
        syncDrawQueue();
        occlusion->reset();
    }
    void queueOcclusionReport(ICommandBuffer* command_buffer,
                              ReportPublisher publish,
                              std::vector<ztd::Range<uptr>> writes) override;
    void queueGuestWrite(ICommandBuffer* command_buffer,
                         std::function<void(bool succeeded)> write,
                         std::vector<ztd::Range<uptr>> writes) override;
    void flushReports(ICommandBuffer* command_buffer,
                      ztd::Range<uptr> range) override;
    void drainReports() override;

    // Compute
    bool dispatchCompute(ICommandBuffer* command_buffer,
                         const ComputeDispatch& dispatch) override;

    // Draw
    void draw(ICommandBuffer* command_buffer,
              const engines::PrimitiveType primitive_type, const u32 start,
              const u32 count, const u32 base_instance,
              const u32 instance_count) override;
    void drawIndexed(ICommandBuffer* command_buffer,
                     const engines::PrimitiveType primitive_type,
                     const u32 start, const u32 count, const u32 base_vertex,
                     const u32 base_instance,
                     const u32 instance_count) override;

    // Helpers
    RenderEncoder*
    getRenderCommandEncoder(CommandBuffer* command_buffer) const;
    void getColorPixelFormats(
        MTL::PixelFormat out_pixel_formats[COLOR_TARGET_COUNT]) const;
    MTL::PixelFormat getDepthStencilPixelFormat() const;
    bool hasStencilAttachment() const;

    // Encoder state setting
    void setRenderPipelineState(CommandBuffer* command_buffer) const;
    void setDepthStencilState(CommandBuffer* command_buffer);
    void setVertexBuffer(CommandBuffer* command_buffer, u32 index);
    void setUniformBuffer(CommandBuffer* command_buffer, ShaderType shader_type,
                          u32 index);
    void setTexture(CommandBuffer* command_buffer, ShaderType shader_type,
                    u32 index);
    void setStorageBuffer(CommandBuffer* command_buffer, ShaderType shader_type,
                          u32 index, bool written);

  protected:
    // Capture
    void beginCapture() override;
    void endCapture() override;

  private:
    MTL::Device* device;
    MTL::CommandQueue* command_queue;
    // Null when encoding with Metal 3
    std::shared_ptr<Mtl4Context> mtl4_context;
    std::shared_ptr<UploadBufferPool> upload_buffer_pool;

    // Buffers wrapping guest memory pages, by first page
    struct GuestMemoryBuffer {
        std::shared_ptr<BufferBase> buffer;
        uptr end;
    };
    std::mutex guest_memory_buffers_mutex;
    std::map<uptr, GuestMemoryBuffer> guest_memory_buffers;

    // Work which runs once the GPU finished the command buffer it depends on
    std::shared_ptr<CompletionQueue> completions;
    std::unique_ptr<OcclusionCounter> occlusion;
    std::optional<u64> prepareOcclusionQuery(CommandBuffer* command_buffer);

    // Encodes draws on a second thread, see DrawQueue
    std::unique_ptr<DrawQueue> draw_queue;

    // Caches
    BinaryArchiveCache binary_archive_cache;
    ShaderLibraryCache shader_library_cache;
    SpatialUpscaler spatial_upscaler;
    FramePresenter frame_presenter;
    DepthStencilStateCache depth_stencil_state_cache;
    BlitPipelineCache blit_pipeline_cache;
    ClearColorPipelineCache clear_color_pipeline_cache;
    ClearDepthPipelineCache clear_depth_pipeline_cache;

    // CA
    CA::MetalLayer* ca_layer{nullptr};
    CA::MetalDrawable* ca_drawable{nullptr};

    // Resources

    // Depth stencil states
    MTL::DepthStencilState* depth_stencil_state_always_and_write;
    MTL::DepthStencilState* depth_stencil_state_clear_stencil;
    MTL::DepthStencilState* depth_stencil_state_disabled{nullptr};
    // Raster state of clears: the clear area only, no depth bias or culling
    void beginClear(CommandBuffer* command_buffer,
                    MTL::DepthStencilState* depth_stencil_state);

    // Samplers
    MTL::SamplerState* nearest_sampler;
    MTL::SamplerState* linear_sampler;

    // Compute pipelines (null if creation failed)
    std::map<MTL::Function*, MTL::ComputePipelineState*> compute_pipelines;

    // State
    State state;
    [[maybe_unused]] u32
        padding[0x100]; // HACK: for some reason, writing to some fields of the
                        // encoder_state corrupts the state

    // Helpers
    bool canDraw() const;
    void bindDrawState(CommandBuffer* command_buffer);
    // Binds the resources `shader` declares from the bindings of `state_type`
    // to the `host_type` stage (a geometry shader runs as a vertex function)
    void bindStageResources(CommandBuffer* command_buffer, const Shader* shader,
                            ShaderType state_type, ShaderType host_type);
    // The vertex ids a draw uses, which index the vertex shader's captured
    // records from the lowest (geometry.hpp). False if they cannot be read.
    struct VertexRange {
        u32 min_vertex;
        u32 slots;
        MTL::Buffer* index_buffer;
        u64 index_offset;
    };
    std::optional<VertexRange> getDrawVertexRange(bool indexed, u32 start,
                                                  u32 count, u32 base_vertex);
    // Records the vertex shader's outputs for the draw (geometry.hpp)
    void encodeVertexCapture(CommandBuffer* command_buffer,
                             const VertexRange& range, MTL::Buffer* records,
                             MTL::Buffer* params, bool indexed, u32 start,
                             u32 count, u32 base_vertex, u32 base_instance);
    // A draw with a geometry shader (see geometry.hpp)
    void drawGeometry(CommandBuffer* command_buffer,
                      engines::PrimitiveType primitive_type, u32 start,
                      u32 count, bool indexed, u32 base_vertex,
                      u32 base_instance, u32 instance_count);
    // A draw of patches with tessellation (see tessellation.hpp)
    void drawTessellation(CommandBuffer* command_buffer, u32 start, u32 count,
                          bool indexed, u32 base_vertex, u32 base_instance,
                          u32 instance_count);

  public:
    GETTER(device, getDevice);
    REF_GETTER(blit_pipeline_cache, getBlitPipelineCache);
    GETTER(nearest_sampler, getNearestSampler);
    GETTER(linear_sampler, getLinearSampler);
    SpatialUpscaler& getSpatialUpscaler() { return spatial_upscaler; }
    FramePresenter& getFramePresenter() { return frame_presenter; }
    Mtl4Context* getMtl4Context() const { return mtl4_context.get(); }

  public:
    // ABI-stable implementations selected by the live module.
    void setSurfaceHot0(void* surface);
    ISurfaceCompositor* acquireNextSurfaceHot1();
    BufferBase* createBufferHot2(u64 size);
    BufferBase* createCpuBufferSnapshotHot3(ztd::Range<uptr> range);
    BufferBase* allocateTemporaryBufferHot4(const u64 size);
    BufferBase* allocateTextureUploadBufferHot5(u64 size);
    void retireTextureUploadBufferHot6(ICommandBuffer* command_buffer,
                                       BufferBase* buffer);
    void freeTemporaryBufferHot7(BufferBase* buffer);
    ITexture* createTextureHot8(const TextureDescriptor& descriptor);
    void blitTextureHot9(ICommandBuffer* command_buffer, ITextureView* src,
                         simd::float3 src_origin, MTL::Size src_size, u32 src_level,
                         u32 src_layer, ITextureView* dst, simd::float3 dst_origin,
                         MTL::Size dst_size, u32 dst_level, u32 dst_layer,
                         u32 level_count, u32 layer_count, const BlitOptions& options);
    SamplerBase* createSamplerHot10(const SamplerDescriptor& descriptor);
    ICommandBuffer* createCommandBufferHot11();
    RenderPassBase*
    createRenderPassHot12(const RenderPassDescriptor& descriptor);
    void bindRenderPassHot13(const RenderPassBase* render_pass);
    void clearColorHot14(ICommandBuffer* command_buffer, u32 render_target_id,
                         u32 layer, u8 mask, const simd::uint4 color);
    void clearDepthHot15(ICommandBuffer* command_buffer, u32 layer,
                         const float value);
    void clearStencilHot16(ICommandBuffer* command_buffer, u32 layer,
                           const u32 value);
    ShaderBase* createShaderHot17(const ShaderDescriptor& descriptor);
    PipelineBase* createPipelineHot18(const PipelineDescriptor& descriptor);
    void bindPipelineHot19(const PipelineBase* pipeline);
    void setDepthTestEnabledHot20(bool enabled);
    void setDepthWriteEnabledHot21(bool enabled);
    void debugDumpTextureHot22(ICommandBuffer* command_buffer,
                               ITextureView* view, const std::string& path);
    void debugDumpTextureLevelHot23(ICommandBuffer* command_buffer,
                                    ITextureView* view, u32 level,
                                    const std::string& path);
    void setStencilStateHot24(const StencilState& stencil);
    bool hasStencilAttachmentHot25() const;
    void setCullStateHot26(bool enabled, engines::CullFaceMode mode,
                           engines::Winding front_face);
    void setDepthBiasHot27(f32 constant, f32 slope, f32 clamp);
    void setBlendConstantHot28(const simd::float4 color);
    void setDepthCompareOpHot29(engines::CompareOp op);
    void setViewportHot30(u32 index, const MTL::Viewport& viewport);
    void setScissorHot31(u32 index, const MTL::ScissorRect& scissor);
    void bindVertexBufferHot32(const BufferView& buffer, u32 index);
    void bindIndexBufferHot33(const BufferView& index_buffer,
                              engines::IndexType index_type);
    void bindUniformBufferHot34(const BufferView& buffer,
                                ShaderType shader_type, u32 index);
    void bindTextureHot35(ITextureView* texture, SamplerBase* sampler,
                          ShaderType shader_type, u32 index);
    void unbindUniformBuffersHot36(ShaderType shader_type);
    void unbindTexturesHot37(ShaderType shader_type);
    void drawHot38(ICommandBuffer* command_buffer,
                   const engines::PrimitiveType primitive_type, const u32 start,
                   const u32 count, const u32 base_instance,
                   const u32 instance_count);
    void drawIndexedHot39(ICommandBuffer* command_buffer,
                          const engines::PrimitiveType primitive_type,
                          const u32 start, const u32 count,
                          const u32 base_vertex, const u32 base_instance,
                          const u32 instance_count);
    f64 debugSyncCommandBufferHot40(ICommandBuffer* command_buffer);
    bool dispatchComputeHot41(ICommandBuffer* command_buffer,
                              const ComputeDispatch& dispatch);
    void getColorPixelFormatsHot42(
        MTL::PixelFormat out_pixel_formats[COLOR_TARGET_COUNT]) const;
    MTL::PixelFormat getDepthStencilPixelFormatHot43() const;
    RenderEncoder*
    getRenderCommandEncoderHot44(CommandBuffer* command_buffer) const;
    void setRenderPipelineStateHot45(CommandBuffer* command_buffer) const;
    void setDepthStencilStateHot46(CommandBuffer* command_buffer);
    void setVertexBufferHot47(CommandBuffer* command_buffer, u32 index);
    void setUniformBufferHot48(CommandBuffer* command_buffer,
                               ShaderType shader_type, u32 index);
    void setTextureHot49(CommandBuffer* command_buffer, ShaderType shader_type,
                         u32 index);
    void beginCaptureHot50();
    void endCaptureHot51();
    bool canDrawHot52() const;
    void bindDrawStateHot53(CommandBuffer* command_buffer);
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
