#pragma once

#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

class CommandBuffer;

// Stages a Metal 4 barrier can wait for and block
constexpr MTL::Stages MTL4_RENDER_STAGES =
    MTL::StageVertex | MTL::StageFragment | MTL::StageTile;
constexpr MTL::Stages MTL4_COMPUTE_STAGES = MTL::StageDispatch | MTL::StageBlit;
constexpr MTL::Stages MTL4_ALL_STAGES = MTL4_RENDER_STAGES | MTL4_COMPUTE_STAGES;

// Encoders of a CommandBuffer. They have the methods of the Metal 3 encoders
// and encode them with the API of the command buffer: with Metal 4, resources
// are bound through argument tables, inline bytes are copied to transient
// memory, and everything encoded is retained and made resident by the command
// buffer. An encoder is only valid until its command buffer opens another one.

class RenderEncoder {
  public:
    void setRenderPipelineState(const MTL::RenderPipelineState* pipeline);
    void setDepthStencilState(const MTL::DepthStencilState* state);
    void setCullMode(MTL::CullMode mode);
    void setFrontFacingWinding(MTL::Winding winding);
    void setDepthBias(float depth_bias, float slope_scale, float clamp);
    void setDepthClipMode(MTL::DepthClipMode mode);
    void setTriangleFillMode(MTL::TriangleFillMode mode);
    void setBlendColor(float red, float green, float blue, float alpha);
    void setStencilReferenceValues(u32 front, u32 back);
    void setViewport(MTL::Viewport viewport);
    void setViewports(const MTL::Viewport* viewports, NS::UInteger count);
    void setScissorRect(MTL::ScissorRect rect);
    void setScissorRects(const MTL::ScissorRect* rects, NS::UInteger count);

    void setVertexBuffer(const MTL::Buffer* buffer, NS::UInteger offset,
                         NS::UInteger index);
    void setVertexBytes(const void* bytes, NS::UInteger length,
                        NS::UInteger index);
    void setVertexTexture(const MTL::Texture* texture, NS::UInteger index);
    void setVertexSamplerState(const MTL::SamplerState* sampler,
                               NS::UInteger index);
    void setFragmentBuffer(const MTL::Buffer* buffer, NS::UInteger offset,
                           NS::UInteger index);
    void setFragmentBytes(const void* bytes, NS::UInteger length,
                          NS::UInteger index);
    void setFragmentTexture(const MTL::Texture* texture, NS::UInteger index);
    void setFragmentSamplerState(const MTL::SamplerState* sampler,
                                 NS::UInteger index);

    void setVisibilityResultMode(MTL::VisibilityResultMode mode,
                                 NS::UInteger offset);
    // Buffers vertex functions wrote are visible to the vertex functions of
    // the following draws
    void vertexBufferBarrier();
    // Tessellation (Metal 3 only: Metal 4 encoders do not tessellate)
    bool supportsTessellation() const { return mtl3 != nullptr; }
    void setTessellationFactorBuffer(const MTL::Buffer* buffer,
                                     NS::UInteger offset,
                                     NS::UInteger instance_stride);
    void drawPatches(NS::UInteger control_points, NS::UInteger patch_start,
                     NS::UInteger patch_count, NS::UInteger instance_count,
                     NS::UInteger base_instance);

    void drawPrimitives(MTL::PrimitiveType type, NS::UInteger vertex_start,
                        NS::UInteger vertex_count);
    void drawPrimitives(MTL::PrimitiveType type, NS::UInteger vertex_start,
                        NS::UInteger vertex_count, NS::UInteger instance_count);
    void drawPrimitives(MTL::PrimitiveType type, NS::UInteger vertex_start,
                        NS::UInteger vertex_count, NS::UInteger instance_count,
                        NS::UInteger base_instance);
    void drawIndexedPrimitives(MTL::PrimitiveType type, NS::UInteger index_count,
                               MTL::IndexType index_type,
                               const MTL::Buffer* index_buffer,
                               NS::UInteger index_buffer_offset);
    void drawIndexedPrimitives(MTL::PrimitiveType type, NS::UInteger index_count,
                               MTL::IndexType index_type,
                               const MTL::Buffer* index_buffer,
                               NS::UInteger index_buffer_offset,
                               NS::UInteger instance_count);
    void drawIndexedPrimitives(MTL::PrimitiveType type, NS::UInteger index_count,
                               MTL::IndexType index_type,
                               const MTL::Buffer* index_buffer,
                               NS::UInteger index_buffer_offset,
                               NS::UInteger instance_count,
                               NS::Integer base_vertex,
                               NS::UInteger base_instance);

    void insertDebugSignpost(const NS::String* string);
    void pushDebugGroup(const NS::String* string);
    void popDebugGroup();

    // Null with Metal 4
    MTL::RenderCommandEncoder* getMtl3() const { return mtl3; }

  private:
    friend class CommandBuffer;

    CommandBuffer* owner{nullptr};
    MTL::RenderCommandEncoder* mtl3{nullptr};
    MTL4::RenderCommandEncoder* mtl4{nullptr};
    MTL4::ArgumentTable* vertex_table{nullptr};
    MTL4::ArgumentTable* fragment_table{nullptr};

    void setBuffer(MTL4::ArgumentTable* table, const MTL::Buffer* buffer,
                   NS::UInteger offset, NS::UInteger index);
    void setBytes(MTL4::ArgumentTable* table, const void* bytes,
                  NS::UInteger length, NS::UInteger index);
    void setTexture(MTL4::ArgumentTable* table, const MTL::Texture* texture,
                    NS::UInteger index);
    void setSamplerState(MTL4::ArgumentTable* table,
                         const MTL::SamplerState* sampler, NS::UInteger index);
};

class ComputeEncoder {
  public:
    void setComputePipelineState(const MTL::ComputePipelineState* pipeline);
    void setBuffer(const MTL::Buffer* buffer, NS::UInteger offset,
                   NS::UInteger index);
    void setBytes(const void* bytes, NS::UInteger length, NS::UInteger index);
    void setTexture(const MTL::Texture* texture, NS::UInteger index);
    void setSamplerState(const MTL::SamplerState* sampler, NS::UInteger index);
    void dispatchThreads(MTL::Size threads_per_grid,
                         MTL::Size threads_per_threadgroup);
    void dispatchThreadgroups(MTL::Size threadgroups_per_grid,
                              MTL::Size threads_per_threadgroup);

    // Null with Metal 4
    MTL::ComputeCommandEncoder* getMtl3() const { return mtl3; }

  private:
    friend class CommandBuffer;

    CommandBuffer* owner{nullptr};
    MTL::ComputeCommandEncoder* mtl3{nullptr};
    MTL4::ComputeCommandEncoder* mtl4{nullptr};
    MTL4::ArgumentTable* table{nullptr};
    // Metal 4 dispatches of an encoder run concurrently: one waits for the
    // ones before it, as with a serial Metal 3 encoder
    bool encoded_any{false};

    void beforeWork();
};

// With Metal 4, copies are encoded on a compute encoder
class BlitEncoder {
  public:
    void copyFromBuffer(const MTL::Buffer* source, NS::UInteger source_offset,
                        const MTL::Buffer* destination,
                        NS::UInteger destination_offset, NS::UInteger size);
    void copyFromBuffer(const MTL::Buffer* source, NS::UInteger source_offset,
                        NS::UInteger source_bytes_per_row,
                        NS::UInteger source_bytes_per_image,
                        MTL::Size source_size,
                        const MTL::Texture* destination,
                        NS::UInteger destination_slice,
                        NS::UInteger destination_level,
                        MTL::Origin destination_origin);
    void copyFromBuffer(const MTL::Buffer* source, NS::UInteger source_offset,
                        NS::UInteger source_bytes_per_row,
                        NS::UInteger source_bytes_per_image,
                        MTL::Size source_size,
                        const MTL::Texture* destination,
                        NS::UInteger destination_slice,
                        NS::UInteger destination_level,
                        MTL::Origin destination_origin,
                        MTL::BlitOption options);
    void copyFromTexture(const MTL::Texture* source,
                         const MTL::Texture* destination);
    void copyFromTexture(const MTL::Texture* source, NS::UInteger source_slice,
                         NS::UInteger source_level,
                         const MTL::Texture* destination,
                         NS::UInteger destination_slice,
                         NS::UInteger destination_level,
                         NS::UInteger slice_count, NS::UInteger level_count);
    void copyFromTexture(const MTL::Texture* source, NS::UInteger source_slice,
                         NS::UInteger source_level, MTL::Origin source_origin,
                         MTL::Size source_size, const MTL::Texture* destination,
                         NS::UInteger destination_slice,
                         NS::UInteger destination_level,
                         MTL::Origin destination_origin);
    void copyFromTexture(const MTL::Texture* source, NS::UInteger source_slice,
                         NS::UInteger source_level, MTL::Origin source_origin,
                         MTL::Size source_size, const MTL::Buffer* destination,
                         NS::UInteger destination_offset,
                         NS::UInteger destination_bytes_per_row,
                         NS::UInteger destination_bytes_per_image);
    void copyFromTexture(const MTL::Texture* source, NS::UInteger source_slice,
                         NS::UInteger source_level, MTL::Origin source_origin,
                         MTL::Size source_size, const MTL::Buffer* destination,
                         NS::UInteger destination_offset,
                         NS::UInteger destination_bytes_per_row,
                         NS::UInteger destination_bytes_per_image,
                         MTL::BlitOption options);
    void fillBuffer(const MTL::Buffer* buffer, NS::Range range, u8 value);
    void generateMipmaps(const MTL::Texture* texture);

    // Null with Metal 4
    MTL::BlitCommandEncoder* getMtl3() const { return mtl3; }

  private:
    friend class CommandBuffer;

    CommandBuffer* owner{nullptr};
    MTL::BlitCommandEncoder* mtl3{nullptr};
    MTL4::ComputeCommandEncoder* mtl4{nullptr};
    bool encoded_any{false};

    void beforeWork();
};

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
