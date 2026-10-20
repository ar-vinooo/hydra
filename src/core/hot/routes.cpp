// Stable host trampolines. Keep implementations in their original .cpp files.
#include "core/hot/table.hpp"

#ifdef ZTD_PLATFORM_APPLE
namespace hydra::hw::tegra_x1::gpu::renderer::metal {
void Renderer::setSurface(void* surface) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_setSurfaceHot0)(surface);
}

ISurfaceCompositor* Renderer::acquireNextSurface() {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_acquireNextSurfaceHot1)();
}

BufferBase* Renderer::createBuffer(u64 size) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_createBufferHot2)(size);
}

BufferBase* Renderer::createCpuBufferSnapshot(ztd::Range<uptr> range) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_createCpuBufferSnapshotHot3)(
        range);
}

BufferBase* Renderer::allocateTemporaryBuffer(const u64 size) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_allocateTemporaryBufferHot4)(
        size);
}

BufferBase* Renderer::allocateTextureUploadBuffer(u64 size) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_allocateTextureUploadBufferHot5)(
        size);
}

void Renderer::retireTextureUploadBuffer(ICommandBuffer* command_buffer,
                                         BufferBase* buffer) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_retireTextureUploadBufferHot6)(
        command_buffer, buffer);
}

void Renderer::freeTemporaryBuffer(BufferBase* buffer) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_freeTemporaryBufferHot7)(buffer);
}

ITexture* Renderer::createTexture(const TextureDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_createTextureHot8)(descriptor);
}

void Renderer::blitTexture(ICommandBuffer* command_buffer, ITextureView* src,
                           simd::float3 src_origin, MTL::Size src_size, u32 src_level,
                           u32 src_layer, ITextureView* dst, simd::float3 dst_origin,
                           MTL::Size dst_size, u32 dst_level, u32 dst_layer,
                           u32 level_count, u32 layer_count, const BlitOptions& options) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_blitTextureHot9)(
        command_buffer, src, src_origin, src_size, src_level, src_layer, dst,
        dst_origin, dst_size, dst_level, dst_layer, level_count, layer_count,
        options);
}

SamplerBase* Renderer::createSampler(const SamplerDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_createSamplerHot10)(descriptor);
}

ICommandBuffer* Renderer::createCommandBuffer() {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_createCommandBufferHot11)();
}

RenderPassBase*
Renderer::createRenderPass(const RenderPassDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_createRenderPassHot12)(
        descriptor);
}

void Renderer::bindRenderPass(const RenderPassBase* render_pass) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_bindRenderPassHot13)(render_pass);
}

void Renderer::clearColor(ICommandBuffer* command_buffer, u32 render_target_id,
                          u32 layer, u8 mask, const simd::uint4 color) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_clearColorHot14)(
        command_buffer, render_target_id, layer, mask, color);
}

void Renderer::clearDepth(ICommandBuffer* command_buffer, u32 layer,
                          const float value) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_clearDepthHot15)(
        command_buffer, layer, value);
}

void Renderer::clearStencil(ICommandBuffer* command_buffer, u32 layer,
                            const u32 value) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_clearStencilHot16)(
        command_buffer, layer, value);
}

ShaderBase* Renderer::createShader(const ShaderDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_createShaderHot17)(descriptor);
}

PipelineBase* Renderer::createPipeline(const PipelineDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_createPipelineHot18)(descriptor);
}

void Renderer::bindPipeline(const PipelineBase* pipeline) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_bindPipelineHot19)(pipeline);
}

void Renderer::setDepthTestEnabled(bool enabled) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_setDepthTestEnabledHot20)(
        enabled);
}

void Renderer::setDepthWriteEnabled(bool enabled) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_setDepthWriteEnabledHot21)(
        enabled);
}

void Renderer::debugDumpTexture(ICommandBuffer* command_buffer,
                                ITextureView* view, const std::string& path) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_debugDumpTextureHot22)(
        command_buffer, view, path);
}

void Renderer::debugDumpTextureLevel(ICommandBuffer* command_buffer,
                                     ITextureView* view, u32 level,
                                     const std::string& path) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_debugDumpTextureLevelHot23)(
        command_buffer, view, level, path);
}

void Renderer::setStencilState(const StencilState& stencil) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_setStencilStateHot24)(stencil);
}

bool Renderer::hasStencilAttachment() const {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_hasStencilAttachmentHot25)();
}

void Renderer::setCullState(bool enabled, engines::CullFaceMode mode,
                            engines::Winding front_face) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_setCullStateHot26)(
        enabled, mode, front_face);
}

void Renderer::setDepthBias(f32 constant, f32 slope, f32 clamp) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_setDepthBiasHot27)(
        constant, slope, clamp);
}

void Renderer::setBlendConstant(const simd::float4 color) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_setBlendConstantHot28)(color);
}

void Renderer::setDepthCompareOp(engines::CompareOp op) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_setDepthCompareOpHot29)(op);
}

void Renderer::setViewport(u32 index, const MTL::Viewport& viewport) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_setViewportHot30)(
        index, viewport);
}

void Renderer::setScissor(u32 index, const MTL::ScissorRect& scissor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_setScissorHot31)(index, scissor);
}

void Renderer::bindVertexBuffer(const BufferView& buffer, u32 index) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_bindVertexBufferHot32)(
        buffer, index);
}

void Renderer::bindIndexBuffer(const BufferView& index_buffer,
                               engines::IndexType index_type) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_bindIndexBufferHot33)(
        index_buffer, index_type);
}

void Renderer::bindUniformBuffer(const BufferView& buffer,
                                 ShaderType shader_type, u32 index) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_bindUniformBufferHot34)(
        buffer, shader_type, index);
}

void Renderer::bindTexture(ITextureView* texture, SamplerBase* sampler,
                           ShaderType shader_type, u32 index) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_bindTextureHot35)(
        texture, sampler, shader_type, index);
}

void Renderer::unbindUniformBuffers(ShaderType shader_type) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_unbindUniformBuffersHot36)(
        shader_type);
}

void Renderer::unbindTextures(ShaderType shader_type) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_unbindTexturesHot37)(shader_type);
}

void Renderer::draw(ICommandBuffer* command_buffer,
                    const engines::PrimitiveType primitive_type,
                    const u32 start, const u32 count, const u32 base_instance,
                    const u32 instance_count) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_drawHot38)(
        command_buffer, primitive_type, start, count, base_instance,
        instance_count);
}

void Renderer::drawIndexed(ICommandBuffer* command_buffer,
                           const engines::PrimitiveType primitive_type,
                           const u32 start, const u32 count,
                           const u32 base_vertex, const u32 base_instance,
                           const u32 instance_count) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_drawIndexedHot39)(
        command_buffer, primitive_type, start, count, base_vertex,
        base_instance, instance_count);
}

f64 Renderer::debugSyncCommandBuffer(ICommandBuffer* command_buffer) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_debugSyncCommandBufferHot40)(
        command_buffer);
}

void Renderer::dispatchCompute(ICommandBuffer* command_buffer,
                               const ComputeDispatch& dispatch) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_dispatchComputeHot41)(
        command_buffer, dispatch);
}

void Renderer::getColorPixelFormats(
    MTL::PixelFormat out_pixel_formats[COLOR_TARGET_COUNT]) const {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_getColorPixelFormatsHot42)(
        out_pixel_formats);
}

MTL::PixelFormat Renderer::getDepthStencilPixelFormat() const {
    ::hydra::hot::Scope hot_scope;
    return (
        (*this).*
        hot_scope.table().renderer_getDepthStencilPixelFormatHot43)();
}

MTL::RenderCommandEncoder*
Renderer::getRenderCommandEncoder(CommandBuffer* command_buffer) const {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_getRenderCommandEncoderHot44)(
        command_buffer);
}

void Renderer::setRenderPipelineState(CommandBuffer* command_buffer) const {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_setRenderPipelineStateHot45)(
        command_buffer);
}

void Renderer::setDepthStencilState(CommandBuffer* command_buffer) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().renderer_setDepthStencilStateHot46)(
        command_buffer);
}

void Renderer::setVertexBuffer(CommandBuffer* command_buffer, u32 index) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_setVertexBufferHot47)(
        command_buffer, index);
}

void Renderer::setUniformBuffer(CommandBuffer* command_buffer,
                                ShaderType shader_type, u32 index) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_setUniformBufferHot48)(
        command_buffer, shader_type, index);
}

void Renderer::setTexture(CommandBuffer* command_buffer, ShaderType shader_type,
                          u32 index) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_setTextureHot49)(
        command_buffer, shader_type, index);
}

void Renderer::beginCapture() {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_beginCaptureHot50)();
}

void Renderer::endCapture() {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_endCaptureHot51)();
}

bool Renderer::canDraw() const {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_canDrawHot52)();
}

void Renderer::bindDrawState(CommandBuffer* command_buffer) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().renderer_bindDrawStateHot53)(
        command_buffer);
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal

#endif

#ifdef ZTD_PLATFORM_APPLE
namespace hydra::hw::tegra_x1::gpu::renderer::metal {
void CommandBuffer::retireUploadBuffer(std::shared_ptr<UploadBufferPool> pool,
                                       MTL::Buffer* buffer) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().command_buffer_retireUploadBufferHot0)(
        pool, buffer);
}

MTL::RenderCommandEncoder* CommandBuffer::getRenderCommandEncoder(
    MTL::RenderPassDescriptor* render_pass_descriptor) {
    ::hydra::hot::Scope hot_scope;
    return (
        (*this).*
        hot_scope.table().command_buffer_getRenderCommandEncoderHot1)(
        render_pass_descriptor);
}

MTL::RenderCommandEncoder* CommandBuffer::createRenderCommandEncoder(
    MTL::RenderPassDescriptor* render_pass_descriptor) {
    ::hydra::hot::Scope hot_scope;
    return (
        (*this).*
        hot_scope.table().command_buffer_createRenderCommandEncoderHot2)(
        render_pass_descriptor);
}

MTL::BlitCommandEncoder* CommandBuffer::getBlitCommandEncoder() {
    ::hydra::hot::Scope hot_scope;
    return (
        (*this).*
        hot_scope.table().command_buffer_getBlitCommandEncoderHot3)();
}

MTL::ComputeCommandEncoder* CommandBuffer::getComputeCommandEncoder() {
    ::hydra::hot::Scope hot_scope;
    return (
        (*this).*
        hot_scope.table().command_buffer_getComputeCommandEncoderHot4)();
}

f64 CommandBuffer::debugCommitAndWait() {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().command_buffer_debugCommitAndWaitHot5)();
}

void CommandBuffer::endEncoding() {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().command_buffer_endEncodingHot6)();
}

void CommandBuffer::setRenderPipelineState(MTL::RenderPipelineState* pipeline) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().command_buffer_setRenderPipelineStateHot7)(
        pipeline);
}

void CommandBuffer::setDepthStencilState(
    MTL::DepthStencilState* depth_stencil_state) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().command_buffer_setDepthStencilStateHot8)(
        depth_stencil_state);
}

void CommandBuffer::setCullMode(MTL::CullMode cull_mode) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().command_buffer_setCullModeHot9)(cull_mode);
}

void CommandBuffer::setFrontFaceWinding(MTL::Winding front_face_winding) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().command_buffer_setFrontFaceWindingHot10)(
        front_face_winding);
}

void CommandBuffer::setDepthBias(float constant, float slope, float clamp) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().command_buffer_setDepthBiasHot11)(
        constant, slope, clamp);
}

void CommandBuffer::setBlendColor(float r, float g, float b, float a) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().command_buffer_setBlendColorHot12)(r, g, b,
                                                                        a);
}

void CommandBuffer::setStencilReferences(u32 front, u32 back) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().command_buffer_setStencilReferencesHot13)(
        front, back);
}

void CommandBuffer::setViewports(std::span<const MTL::Viewport> values) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().command_buffer_setViewportsHot14)(values);
}

void CommandBuffer::setScissors(std::span<const MTL::ScissorRect> values) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().command_buffer_setScissorsHot15)(values);
}

void CommandBuffer::setBuffer(MTL::Buffer* buffer, u64 offset,
                              ShaderType shader_type, u32 index) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().command_buffer_setBufferHot16)(
        buffer, offset, shader_type, index);
}

void CommandBuffer::setTexture(MTL::Texture* texture, ShaderType shader_type,
                               u32 index) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().command_buffer_setTextureHot17)(
        texture, shader_type, index);
}

void CommandBuffer::setSampler(MTL::SamplerState* sampler,
                               ShaderType shader_type, u32 index) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().command_buffer_setSamplerHot18)(
        sampler, shader_type, index);
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal

#endif

#ifdef ZTD_PLATFORM_APPLE
namespace hydra::hw::tegra_x1::gpu::renderer::metal {
ITextureView*
Texture::createView(const TextureViewDescriptor& view_descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().texture_createViewHot0)(view_descriptor);
}

void Texture::copyBufferBytes(ICommandBuffer* command_buffer,
                              const ITexture* src, u64 src_offset,
                              u64 dst_offset, u64 size) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().texture_copyBufferBytesHot1)(
        command_buffer, src, src_offset, dst_offset, size);
}

void Texture::copyFrom(ICommandBuffer* command_buffer, const BufferBase* src,
                       const ztd::Range<u32> dst_levels,
                       const ztd::Range<u32> dst_layers) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().texture_copyFromHot2)(
        command_buffer, src, dst_levels, dst_layers);
}

void Texture::copyDepthStencilFromBuffer(MTL::BlitCommandEncoder* encoder,
                                         const u8* src, const MTL::Size dims,
                                         const u32 layer, const u32 level) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().texture_copyDepthStencilFromBufferHot3)(
        encoder, src, dims, layer, level);
}

bool Texture::copyFrom(ICommandBuffer* command_buffer, const ITexture* src,
                       const u32 src_level, const u32 src_layer,
                       const u32 dst_level, const u32 dst_layer,
                       const u32 level_count, const u32 layer_count) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().texture_copyFromHot4)(
        command_buffer, src, src_level, src_layer, dst_level, dst_layer,
        level_count, layer_count);
}

bool Texture::copyFrom(ICommandBuffer* command_buffer, const ITexture* src,
                       const MTL::Origin src_origin, const u32 src_level,
                       const u32 src_layer, const MTL::Origin dst_origin,
                       const u32 dst_level, const u32 dst_layer,
                       const MTL::Size size, const u32 layer_count) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().texture_copyFromHot5)(
        command_buffer, src, src_origin, src_level, src_layer, dst_origin,
        dst_level, dst_layer, size, layer_count);
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal

#endif

namespace hydra::hw::tegra_x1::gpu::renderer {
ShaderBase* ShaderCache::find(const GuestShaderDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().shader_cache_findHot0)(descriptor);
}

ShaderBase* ShaderCache::create(const GuestShaderDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().shader_cache_createHot1)(descriptor);
}

u32 ShaderCache::hash(const GuestShaderDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    ztd::hash::XxHash32 hash;
    hash.add(::hydra::hot::generation());
    hash.add(hot_scope.table().shader_cache_hashHot2(descriptor));
    return hash.toHashCode();
}
} // namespace hydra::hw::tegra_x1::gpu::renderer

namespace hydra::hw::tegra_x1::gpu::renderer {
PipelineBase* PipelineCache::create(const PipelineDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().pipeline_cache_createHot0)(descriptor);
}

u32 PipelineCache::hash(const PipelineDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    ztd::hash::XxHash32 hash;
    hash.add(::hydra::hot::generation());
    hash.add(hot_scope.table().pipeline_cache_hashHot1(descriptor));
    return hash.toHashCode();
}
} // namespace hydra::hw::tegra_x1::gpu::renderer

namespace hydra::hw::tegra_x1::gpu::renderer {
SamplerBase* SamplerCache::create(const SamplerDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().sampler_cache_createHot0)(descriptor);
}

u32 SamplerCache::hash(const SamplerDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    ztd::hash::XxHash32 hash;
    hash.add(::hydra::hot::generation());
    hash.add(hot_scope.table().sampler_cache_hashHot1(descriptor));
    return hash.toHashCode();
}
} // namespace hydra::hw::tegra_x1::gpu::renderer

namespace hydra::hw::tegra_x1::gpu::renderer {
RenderPassBase*
RenderPassCache::create(const RenderPassDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().render_pass_cache_createHot0)(descriptor);
}

u32 RenderPassCache::hash(const RenderPassDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    ztd::hash::XxHash32 hash;
    hash.add(::hydra::hot::generation());
    hash.add(hot_scope.table().render_pass_cache_hashHot1(descriptor));
    return hash.toHashCode();
}
} // namespace hydra::hw::tegra_x1::gpu::renderer

#ifdef ZTD_PLATFORM_APPLE
namespace hydra::hw::tegra_x1::gpu::renderer::metal {
MTL::DepthStencilState*
DepthStencilStateCache::create(const DepthStencilStateDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().depth_stencil_state_cache_createHot0)(
        descriptor);
}

u32 DepthStencilStateCache::hash(
    const DepthStencilStateDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    ztd::hash::XxHash32 hash;
    hash.add(::hydra::hot::generation());
    hash.add(hot_scope.table().depth_stencil_state_cache_hashHot1(
        descriptor));
    return hash.toHashCode();
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal

#endif

#ifdef ZTD_PLATFORM_APPLE
namespace hydra::hw::tegra_x1::gpu::renderer::metal {
MTL::RenderPipelineState*
BlitPipelineCache::create(const BlitPipelineDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().blit_pipeline_cache_createHot0)(
        descriptor);
}

u32 BlitPipelineCache::hash(const BlitPipelineDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    ztd::hash::XxHash32 hash;
    hash.add(::hydra::hot::generation());
    hash.add(((*this).*hot_scope.table().blit_pipeline_cache_hashHot1)(
        descriptor));
    return hash.toHashCode();
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal

#endif

#ifdef ZTD_PLATFORM_APPLE
namespace hydra::hw::tegra_x1::gpu::renderer::metal {
MTL::RenderPipelineState* ClearColorPipelineCache::create(
    const ClearColorPipelineDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().clear_color_pipeline_cache_createHot0)(
        descriptor);
}

u32 ClearColorPipelineCache::hash(
    const ClearColorPipelineDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    ztd::hash::XxHash32 hash;
    hash.add(::hydra::hot::generation());
    hash.add(hot_scope.table().clear_color_pipeline_cache_hashHot1(
        descriptor));
    return hash.toHashCode();
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal

#endif

#ifdef ZTD_PLATFORM_APPLE
namespace hydra::hw::tegra_x1::gpu::renderer::metal {
MTL::RenderPipelineState* ClearDepthPipelineCache::create(
    const ClearDepthPipelineDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().clear_depth_pipeline_cache_createHot0)(
        descriptor);
}

u32 ClearDepthPipelineCache::hash(
    const ClearDepthPipelineDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    ztd::hash::XxHash32 hash;
    hash.add(::hydra::hot::generation());
    hash.add(hot_scope.table().clear_depth_pipeline_cache_hashHot1(
        descriptor));
    return hash.toHashCode();
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal

#endif

namespace hydra::hw::tegra_x1::gpu::renderer {
bool TextureCache::synchronize2DWith2D(ICommandBuffer* command_buffer,
                                       TextureStorage& storage,
                                       TextureStorage& other_storage) {
    ::hydra::hot::Scope hot_scope;
    return hot_scope.table().texture_cache_synchronize2DWith2DHot0(
        command_buffer, storage, other_storage);
}

bool TextureCache::synchronize3DWith3D(ICommandBuffer* command_buffer,
                                       TextureStorage& storage,
                                       TextureStorage& other_storage) {
    ::hydra::hot::Scope hot_scope;
    return hot_scope.table().texture_cache_synchronize3DWith3DHot1(
        command_buffer, storage, other_storage);
}

void TextureCache::decodeTexture(ICommandBuffer* command_buffer,
                                 TextureStorage& storage) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().texture_cache_decodeTextureHot2)(
        command_buffer, storage);
}
} // namespace hydra::hw::tegra_x1::gpu::renderer

namespace hydra::hw::tegra_x1::gpu::engines {
void ThreeD::method(u32 method, u32 arg) {
    ::hydra::hot::Scope hot_scope;
    return HYDRA_HOT_CALL(*this, engine_three_d)(method, arg);
}
} // namespace hydra::hw::tegra_x1::gpu::engines

namespace hydra::hw::tegra_x1::gpu::engines {
void TwoD::method(u32 method, u32 arg) {
    ::hydra::hot::Scope hot_scope;
    return HYDRA_HOT_CALL(*this, engine_two_d)(method, arg);
}
} // namespace hydra::hw::tegra_x1::gpu::engines

namespace hydra::hw::tegra_x1::gpu::engines {
void Compute::method(u32 method, u32 arg) {
    ::hydra::hot::Scope hot_scope;
    return HYDRA_HOT_CALL(*this, engine_compute)(method, arg);
}
} // namespace hydra::hw::tegra_x1::gpu::engines

namespace hydra::hw::tegra_x1::gpu::engines {
void Copy::method(u32 method, u32 arg) {
    ::hydra::hot::Scope hot_scope;
    return HYDRA_HOT_CALL(*this, engine_copy)(method, arg);
}
} // namespace hydra::hw::tegra_x1::gpu::engines

namespace hydra::hw::tegra_x1::gpu::engines {
void Inline::method(u32 method, u32 arg) {
    ::hydra::hot::Scope hot_scope;
    return HYDRA_HOT_CALL(*this, engine_inline)(method, arg);
}
} // namespace hydra::hw::tegra_x1::gpu::engines

namespace hydra::hw::tegra_x1::gpu::engines {
void ThreeD::flushMacro() {
    ::hydra::hot::Scope hot_scope;
    HYDRA_HOT_CALL(*this, engine_flush_macro)();
}
} // namespace hydra::hw::tegra_x1::gpu::engines

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp {
void decompile(ztd::io::MemoryStream& stream, ShaderType type,
               const GuestShaderState& state, ShaderBackend& backend,
               std::vector<u8>& code, ResourceMapping& mapping,
               const std::function<std::optional<u32>(u32, u32)>& read_const) {
    ::hydra::hot::Scope hot_scope;
    return hot_scope.table().shader_decompile(
        stream, type, state, backend, code, mapping, read_const);
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp
#ifdef ZTD_PLATFORM_APPLE
namespace hydra::hw::tegra_x1::gpu::renderer::metal {
void Buffer::copyFrom(ICommandBuffer* command_buffer, ITextureView* src,
                      const MTL::Origin src_origin, const MTL::Size src_size,
                      const ztd::Range<u32> src_levels,
                      const ztd::Range<u32> src_layers, u64 dst_offset) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().buffer_copyFromHot0)(
        command_buffer, src, src_origin, src_size, src_levels, src_layers,
        dst_offset);
}

void Buffer::copyFromImpl(const uptr data, u64 dst_offset, u64 size_) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().buffer_copyFromImplHot1)(
        data, dst_offset, size_);
}

void Buffer::copyFromImpl(ICommandBuffer* command_buffer, BufferBase* src,
                          u64 dst_offset, u64 src_offset, u64 size_) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().buffer_copyFromImplHot2)(
        command_buffer, src, dst_offset, src_offset, size_);
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal

#endif

namespace hydra::hw::tegra_x1::gpu::renderer {
BufferView BufferCache::get(ICommandBuffer* command_buffer,
                            ztd::Range<uptr> range) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().buffer_cache_getHot0)(
        command_buffer, range);
}

void BufferCache::invalidateMemory(ztd::Range<uptr> range) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*
            hot_scope.table().buffer_cache_invalidateMemoryHot1)(range);
}

void BufferCache::updateRange(ICommandBuffer* command_buffer,
                              BufferEntry& entry, ztd::Range<uptr> range) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().buffer_cache_updateRangeHot2)(
        command_buffer, entry, range);
}

BufferEntry& BufferCache::find(ztd::Range<uptr> range) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().buffer_cache_findHot3)(range);
}
} // namespace hydra::hw::tegra_x1::gpu::renderer

namespace hydra::hw::tegra_x1::gpu::renderer {
BufferView IndexCache::decode(ICommandBuffer* command_buffer,
                              const IndexDescriptor& descriptor,
                              engines::IndexType& out_type,
                              engines::PrimitiveType& out_primitive_type,
                              u32& out_count) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().index_cache_decodeHot0)(
        command_buffer, descriptor, out_type, out_primitive_type, out_count);
}

u32 IndexCache::hash(const IndexDescriptor& descriptor) {
    ::hydra::hot::Scope hot_scope;
    ztd::hash::XxHash32 hash;
    hash.add(::hydra::hot::generation());
    hash.add(hot_scope.table().index_cache_hashHot1(descriptor));
    return hash.toHashCode();
}
} // namespace hydra::hw::tegra_x1::gpu::renderer

#ifdef ZTD_PLATFORM_APPLE
namespace hydra::hw::tegra_x1::gpu::renderer::metal {
MTL::Library* ShaderLibraryCache::compile(std::string_view source,
                                          NS::Error** error) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().shader_library_cache_compileHot0)(
        source, error);
}

MTL::Library* ShaderLibraryCache::get(std::string_view source,
                                      NS::Error** error) {
    ::hydra::hot::Scope hot_scope;
    return ((*this).*hot_scope.table().shader_library_cache_getHot1)(
        source, error);
}
} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
#endif

