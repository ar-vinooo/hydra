#pragma once

#include "core/hw/tegra_x1/gpu/renderer/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class UploadBufferPool;

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
    explicit CommandBuffer(MTL::CommandQueue* command_queue);
    ~CommandBuffer() override;

    MTL::RenderCommandEncoder* getRenderCommandEncoderUnchecked() {
        ASSERT_DEBUG(encoder_type == EncoderType::Render, MetalRenderer,
                     "Render command encoder not active");
        return static_cast<MTL::RenderCommandEncoder*>(command_encoder);
    }
    MTL::RenderCommandEncoder*
    getRenderCommandEncoder(MTL::RenderPassDescriptor* render_pass_descriptor);
    MTL::RenderCommandEncoder* createRenderCommandEncoder(
        MTL::RenderPassDescriptor* render_pass_descriptor);

    MTL::BlitCommandEncoder* getBlitCommandEncoderUnchecked() {
        ASSERT_DEBUG(encoder_type == EncoderType::Blit, MetalRenderer,
                     "Blit command encoder not active");
        return static_cast<MTL::BlitCommandEncoder*>(command_encoder);
    }
    MTL::BlitCommandEncoder* getBlitCommandEncoder();

    MTL::ComputeCommandEncoder* getComputeCommandEncoder();

    void endEncoding();
    // Submit, wait and continue with a new command buffer
    f64 debugCommitAndWait();
    void setVisibilityResultBuffer(MTL::Buffer* buffer);
    MTL::Buffer* getVisibilityResultBuffer() const { return visibility_buffer; }
    bool hasRenderPass(MTL::RenderPassDescriptor* descriptor) const {
        return encoder_type == EncoderType::Render &&
               encoder_state.render_pass == descriptor;
    }
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
    MTL::CommandQueue* command_queue{nullptr};
    MTL::CommandBuffer* command_buffer{nullptr};
    MTL::CommandEncoder* command_encoder{nullptr};
    EncoderType encoder_type{EncoderType::None};

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
    std::shared_ptr<UploadBufferPool> upload_pool;
    std::vector<MTL::Buffer*> retired_upload_buffers;
    u64 raster_updates{0};
    u64 raster_skips{0};
    u64 render_encoders{0};
    u64 blit_encoders{0};

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
    MTL::RenderCommandEncoder* getRenderCommandEncoderHot1(
        MTL::RenderPassDescriptor* render_pass_descriptor);
    MTL::RenderCommandEncoder* createRenderCommandEncoderHot2(
        MTL::RenderPassDescriptor* render_pass_descriptor);
    MTL::BlitCommandEncoder* getBlitCommandEncoderHot3();
    MTL::ComputeCommandEncoder* getComputeCommandEncoderHot4();
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
