#include "core/hw/tegra_x1/gpu/renderer/metal/encoder.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

namespace {

MTL::GPUAddress bufferAddress(const MTL::Buffer* buffer, NS::UInteger offset) {
    return buffer != nullptr ? buffer->gpuAddress() + offset : 0;
}

MTL::ResourceID textureId(const MTL::Texture* texture) {
    return texture != nullptr ? texture->gpuResourceID() : MTL::ResourceID{0};
}

MTL::ResourceID samplerId(const MTL::SamplerState* sampler) {
    return sampler != nullptr ? sampler->gpuResourceID() : MTL::ResourceID{0};
}

} // namespace

// Render

void RenderEncoder::setRenderPipelineState(
    const MTL::RenderPipelineState* pipeline) {
    if (mtl4)
        mtl4->setRenderPipelineState(pipeline);
    else
        mtl3->setRenderPipelineState(pipeline);
}

void RenderEncoder::setDepthStencilState(const MTL::DepthStencilState* state) {
    if (mtl4)
        mtl4->setDepthStencilState(state);
    else
        mtl3->setDepthStencilState(state);
}

void RenderEncoder::setCullMode(MTL::CullMode mode) {
    if (mtl4)
        mtl4->setCullMode(mode);
    else
        mtl3->setCullMode(mode);
}

void RenderEncoder::setFrontFacingWinding(MTL::Winding winding) {
    if (mtl4)
        mtl4->setFrontFacingWinding(winding);
    else
        mtl3->setFrontFacingWinding(winding);
}

void RenderEncoder::setDepthBias(float depth_bias, float slope_scale,
                                 float clamp) {
    if (mtl4)
        mtl4->setDepthBias(depth_bias, slope_scale, clamp);
    else
        mtl3->setDepthBias(depth_bias, slope_scale, clamp);
}

void RenderEncoder::setDepthClipMode(MTL::DepthClipMode mode) {
    if (mtl4)
        mtl4->setDepthClipMode(mode);
    else
        mtl3->setDepthClipMode(mode);
}

void RenderEncoder::setTriangleFillMode(MTL::TriangleFillMode mode) {
    if (mtl4)
        mtl4->setTriangleFillMode(mode);
    else
        mtl3->setTriangleFillMode(mode);
}

void RenderEncoder::setBlendColor(float red, float green, float blue,
                                  float alpha) {
    if (mtl4)
        mtl4->setBlendColor(red, green, blue, alpha);
    else
        mtl3->setBlendColor(red, green, blue, alpha);
}

void RenderEncoder::setStencilReferenceValues(u32 front, u32 back) {
    if (mtl4)
        mtl4->setStencilReferenceValues(front, back);
    else
        mtl3->setStencilReferenceValues(front, back);
}

void RenderEncoder::setViewport(MTL::Viewport viewport) {
    if (mtl4)
        mtl4->setViewport(viewport);
    else
        mtl3->setViewport(viewport);
}

void RenderEncoder::setViewports(const MTL::Viewport* viewports,
                                 NS::UInteger count) {
    if (mtl4)
        mtl4->setViewports(viewports, count);
    else
        mtl3->setViewports(viewports, count);
}

void RenderEncoder::setScissorRect(MTL::ScissorRect rect) {
    if (mtl4)
        mtl4->setScissorRect(rect);
    else
        mtl3->setScissorRect(rect);
}

void RenderEncoder::setScissorRects(const MTL::ScissorRect* rects,
                                    NS::UInteger count) {
    if (mtl4)
        mtl4->setScissorRects(rects, count);
    else
        mtl3->setScissorRects(rects, count);
}

void RenderEncoder::setBuffer(MTL4::ArgumentTable* table,
                              const MTL::Buffer* buffer, NS::UInteger offset,
                              NS::UInteger index) {
    owner->track(buffer);
    table->setAddress(bufferAddress(buffer, offset), index);
}

void RenderEncoder::setBytes(MTL4::ArgumentTable* table, const void* bytes,
                             NS::UInteger length, NS::UInteger index) {
    table->setAddress(owner->allocateTransient(bytes, length), index);
}

void RenderEncoder::setTexture(MTL4::ArgumentTable* table,
                               const MTL::Texture* texture,
                               NS::UInteger index) {
    owner->track(texture);
    table->setTexture(textureId(texture), index);
}

void RenderEncoder::setSamplerState(MTL4::ArgumentTable* table,
                                    const MTL::SamplerState* sampler,
                                    NS::UInteger index) {
    table->setSamplerState(samplerId(sampler), index);
}

void RenderEncoder::setVertexBuffer(const MTL::Buffer* buffer,
                                    NS::UInteger offset, NS::UInteger index) {
    if (mtl4)
        setBuffer(vertex_table, buffer, offset, index);
    else
        mtl3->setVertexBuffer(buffer, offset, index);
}

void RenderEncoder::setVertexBytes(const void* bytes, NS::UInteger length,
                                   NS::UInteger index) {
    if (mtl4)
        setBytes(vertex_table, bytes, length, index);
    else
        mtl3->setVertexBytes(bytes, length, index);
}

void RenderEncoder::setVertexTexture(const MTL::Texture* texture,
                                     NS::UInteger index) {
    if (mtl4)
        setTexture(vertex_table, texture, index);
    else
        mtl3->setVertexTexture(texture, index);
}

void RenderEncoder::setVertexSamplerState(const MTL::SamplerState* sampler,
                                          NS::UInteger index) {
    if (mtl4)
        setSamplerState(vertex_table, sampler, index);
    else
        mtl3->setVertexSamplerState(sampler, index);
}

void RenderEncoder::setFragmentBuffer(const MTL::Buffer* buffer,
                                      NS::UInteger offset, NS::UInteger index) {
    if (mtl4)
        setBuffer(fragment_table, buffer, offset, index);
    else
        mtl3->setFragmentBuffer(buffer, offset, index);
}

void RenderEncoder::setFragmentBytes(const void* bytes, NS::UInteger length,
                                     NS::UInteger index) {
    if (mtl4)
        setBytes(fragment_table, bytes, length, index);
    else
        mtl3->setFragmentBytes(bytes, length, index);
}

void RenderEncoder::setFragmentTexture(const MTL::Texture* texture,
                                       NS::UInteger index) {
    if (mtl4)
        setTexture(fragment_table, texture, index);
    else
        mtl3->setFragmentTexture(texture, index);
}

void RenderEncoder::setFragmentSamplerState(const MTL::SamplerState* sampler,
                                            NS::UInteger index) {
    if (mtl4)
        setSamplerState(fragment_table, sampler, index);
    else
        mtl3->setFragmentSamplerState(sampler, index);
}

void RenderEncoder::setVisibilityResultMode(MTL::VisibilityResultMode mode,
                                            NS::UInteger offset) {
    if (mtl4)
        mtl4->setVisibilityResultMode(mode, offset);
    else
        mtl3->setVisibilityResultMode(mode, offset);
}

void RenderEncoder::drawPrimitives(MTL::PrimitiveType type,
                                   NS::UInteger vertex_start,
                                   NS::UInteger vertex_count) {
    if (mtl4)
        mtl4->drawPrimitives(type, vertex_start, vertex_count);
    else
        mtl3->drawPrimitives(type, vertex_start, vertex_count);
}

void RenderEncoder::drawPrimitives(MTL::PrimitiveType type,
                                   NS::UInteger vertex_start,
                                   NS::UInteger vertex_count,
                                   NS::UInteger instance_count) {
    if (mtl4)
        mtl4->drawPrimitives(type, vertex_start, vertex_count, instance_count);
    else
        mtl3->drawPrimitives(type, vertex_start, vertex_count, instance_count);
}

void RenderEncoder::drawPrimitives(MTL::PrimitiveType type,
                                   NS::UInteger vertex_start,
                                   NS::UInteger vertex_count,
                                   NS::UInteger instance_count,
                                   NS::UInteger base_instance) {
    if (mtl4)
        mtl4->drawPrimitives(type, vertex_start, vertex_count, instance_count,
                             base_instance);
    else
        mtl3->drawPrimitives(type, vertex_start, vertex_count, instance_count,
                             base_instance);
}

void RenderEncoder::drawIndexedPrimitives(MTL::PrimitiveType type,
                                          NS::UInteger index_count,
                                          MTL::IndexType index_type,
                                          const MTL::Buffer* index_buffer,
                                          NS::UInteger index_buffer_offset) {
    drawIndexedPrimitives(type, index_count, index_type, index_buffer,
                          index_buffer_offset, 1, 0, 0);
}

void RenderEncoder::drawIndexedPrimitives(MTL::PrimitiveType type,
                                          NS::UInteger index_count,
                                          MTL::IndexType index_type,
                                          const MTL::Buffer* index_buffer,
                                          NS::UInteger index_buffer_offset,
                                          NS::UInteger instance_count) {
    drawIndexedPrimitives(type, index_count, index_type, index_buffer,
                          index_buffer_offset, instance_count, 0, 0);
}

void RenderEncoder::drawIndexedPrimitives(
    MTL::PrimitiveType type, NS::UInteger index_count,
    MTL::IndexType index_type, const MTL::Buffer* index_buffer,
    NS::UInteger index_buffer_offset, NS::UInteger instance_count,
    NS::Integer base_vertex, NS::UInteger base_instance) {
    if (mtl4) {
        owner->track(index_buffer);
        const auto length = index_buffer->length() > index_buffer_offset
                                ? index_buffer->length() - index_buffer_offset
                                : 0;
        mtl4->drawIndexedPrimitives(
            type, index_count, index_type,
            bufferAddress(index_buffer, index_buffer_offset), length,
            instance_count, base_vertex, base_instance);
    } else {
        mtl3->drawIndexedPrimitives(type, index_count, index_type, index_buffer,
                                    index_buffer_offset, instance_count,
                                    base_vertex, base_instance);
    }
}

void RenderEncoder::insertDebugSignpost(const NS::String* string) {
    if (mtl4)
        mtl4->insertDebugSignpost(string);
    else
        mtl3->insertDebugSignpost(string);
}

void RenderEncoder::pushDebugGroup(const NS::String* string) {
    if (mtl4)
        mtl4->pushDebugGroup(string);
    else
        mtl3->pushDebugGroup(string);
}

void RenderEncoder::popDebugGroup() {
    if (mtl4)
        mtl4->popDebugGroup();
    else
        mtl3->popDebugGroup();
}

// Compute

void ComputeEncoder::beforeWork() {
    if (encoded_any)
        mtl4->barrierAfterEncoderStages(MTL4_COMPUTE_STAGES,
                                        MTL4_COMPUTE_STAGES,
                                        MTL4::VisibilityOptionDevice);
    encoded_any = true;
}

void ComputeEncoder::setComputePipelineState(
    const MTL::ComputePipelineState* pipeline) {
    if (mtl4)
        mtl4->setComputePipelineState(pipeline);
    else
        mtl3->setComputePipelineState(pipeline);
}

void ComputeEncoder::setBuffer(const MTL::Buffer* buffer, NS::UInteger offset,
                               NS::UInteger index) {
    if (mtl4) {
        owner->track(buffer);
        table->setAddress(bufferAddress(buffer, offset), index);
    } else {
        mtl3->setBuffer(buffer, offset, index);
    }
}

void ComputeEncoder::setBytes(const void* bytes, NS::UInteger length,
                              NS::UInteger index) {
    if (mtl4)
        table->setAddress(owner->allocateTransient(bytes, length), index);
    else
        mtl3->setBytes(bytes, length, index);
}

void ComputeEncoder::setTexture(const MTL::Texture* texture,
                                NS::UInteger index) {
    if (mtl4) {
        owner->track(texture);
        table->setTexture(textureId(texture), index);
    } else {
        mtl3->setTexture(texture, index);
    }
}

void ComputeEncoder::setSamplerState(const MTL::SamplerState* sampler,
                                     NS::UInteger index) {
    if (mtl4)
        table->setSamplerState(samplerId(sampler), index);
    else
        mtl3->setSamplerState(sampler, index);
}

void ComputeEncoder::dispatchThreads(MTL::Size threads_per_grid,
                                     MTL::Size threads_per_threadgroup) {
    if (mtl4) {
        beforeWork();
        mtl4->dispatchThreads(threads_per_grid, threads_per_threadgroup);
    } else {
        mtl3->dispatchThreads(threads_per_grid, threads_per_threadgroup);
    }
}

void ComputeEncoder::dispatchThreadgroups(MTL::Size threadgroups_per_grid,
                                          MTL::Size threads_per_threadgroup) {
    if (mtl4) {
        beforeWork();
        mtl4->dispatchThreadgroups(threadgroups_per_grid,
                                   threads_per_threadgroup);
    } else {
        mtl3->dispatchThreadgroups(threadgroups_per_grid,
                                   threads_per_threadgroup);
    }
}

// Blit

void BlitEncoder::beforeWork() {
    if (encoded_any)
        mtl4->barrierAfterEncoderStages(MTL4_COMPUTE_STAGES,
                                        MTL4_COMPUTE_STAGES,
                                        MTL4::VisibilityOptionDevice);
    encoded_any = true;
}

void BlitEncoder::copyFromBuffer(const MTL::Buffer* source,
                                 NS::UInteger source_offset,
                                 const MTL::Buffer* destination,
                                 NS::UInteger destination_offset,
                                 NS::UInteger size) {
    if (mtl4) {
        owner->track(source);
        owner->track(destination);
        beforeWork();
        mtl4->copyFromBuffer(source, source_offset, destination,
                             destination_offset, size);
    } else {
        mtl3->copyFromBuffer(source, source_offset, destination,
                             destination_offset, size);
    }
}

void BlitEncoder::copyFromBuffer(
    const MTL::Buffer* source, NS::UInteger source_offset,
    NS::UInteger source_bytes_per_row, NS::UInteger source_bytes_per_image,
    MTL::Size source_size, const MTL::Texture* destination,
    NS::UInteger destination_slice, NS::UInteger destination_level,
    MTL::Origin destination_origin) {
    copyFromBuffer(source, source_offset, source_bytes_per_row,
                   source_bytes_per_image, source_size, destination,
                   destination_slice, destination_level, destination_origin,
                   MTL::BlitOptionNone);
}

void BlitEncoder::copyFromBuffer(
    const MTL::Buffer* source, NS::UInteger source_offset,
    NS::UInteger source_bytes_per_row, NS::UInteger source_bytes_per_image,
    MTL::Size source_size, const MTL::Texture* destination,
    NS::UInteger destination_slice, NS::UInteger destination_level,
    MTL::Origin destination_origin, MTL::BlitOption options) {
    if (mtl4) {
        owner->track(source);
        owner->track(destination);
        beforeWork();
        mtl4->copyFromBuffer(source, source_offset, source_bytes_per_row,
                             source_bytes_per_image, source_size, destination,
                             destination_slice, destination_level,
                             destination_origin, options);
    } else {
        mtl3->copyFromBuffer(source, source_offset, source_bytes_per_row,
                             source_bytes_per_image, source_size, destination,
                             destination_slice, destination_level,
                             destination_origin, options);
    }
}

void BlitEncoder::copyFromTexture(const MTL::Texture* source,
                                  const MTL::Texture* destination) {
    if (mtl4) {
        owner->track(source);
        owner->track(destination);
        beforeWork();
        mtl4->copyFromTexture(source, destination);
    } else {
        mtl3->copyFromTexture(source, destination);
    }
}

void BlitEncoder::copyFromTexture(const MTL::Texture* source,
                                  NS::UInteger source_slice,
                                  NS::UInteger source_level,
                                  const MTL::Texture* destination,
                                  NS::UInteger destination_slice,
                                  NS::UInteger destination_level,
                                  NS::UInteger slice_count,
                                  NS::UInteger level_count) {
    if (mtl4) {
        owner->track(source);
        owner->track(destination);
        beforeWork();
        mtl4->copyFromTexture(source, source_slice, source_level, destination,
                              destination_slice, destination_level,
                              slice_count, level_count);
    } else {
        mtl3->copyFromTexture(source, source_slice, source_level, destination,
                              destination_slice, destination_level,
                              slice_count, level_count);
    }
}

void BlitEncoder::copyFromTexture(
    const MTL::Texture* source, NS::UInteger source_slice,
    NS::UInteger source_level, MTL::Origin source_origin,
    MTL::Size source_size, const MTL::Texture* destination,
    NS::UInteger destination_slice, NS::UInteger destination_level,
    MTL::Origin destination_origin) {
    if (mtl4) {
        owner->track(source);
        owner->track(destination);
        beforeWork();
        mtl4->copyFromTexture(source, source_slice, source_level,
                              source_origin, source_size, destination,
                              destination_slice, destination_level,
                              destination_origin);
    } else {
        mtl3->copyFromTexture(source, source_slice, source_level,
                              source_origin, source_size, destination,
                              destination_slice, destination_level,
                              destination_origin);
    }
}

void BlitEncoder::copyFromTexture(
    const MTL::Texture* source, NS::UInteger source_slice,
    NS::UInteger source_level, MTL::Origin source_origin,
    MTL::Size source_size, const MTL::Buffer* destination,
    NS::UInteger destination_offset, NS::UInteger destination_bytes_per_row,
    NS::UInteger destination_bytes_per_image) {
    copyFromTexture(source, source_slice, source_level, source_origin,
                    source_size, destination, destination_offset,
                    destination_bytes_per_row, destination_bytes_per_image,
                    MTL::BlitOptionNone);
}

void BlitEncoder::copyFromTexture(
    const MTL::Texture* source, NS::UInteger source_slice,
    NS::UInteger source_level, MTL::Origin source_origin,
    MTL::Size source_size, const MTL::Buffer* destination,
    NS::UInteger destination_offset, NS::UInteger destination_bytes_per_row,
    NS::UInteger destination_bytes_per_image, MTL::BlitOption options) {
    if (mtl4) {
        owner->track(source);
        owner->track(destination);
        beforeWork();
        mtl4->copyFromTexture(source, source_slice, source_level,
                              source_origin, source_size, destination,
                              destination_offset, destination_bytes_per_row,
                              destination_bytes_per_image, options);
    } else {
        mtl3->copyFromTexture(source, source_slice, source_level,
                              source_origin, source_size, destination,
                              destination_offset, destination_bytes_per_row,
                              destination_bytes_per_image, options);
    }
}

void BlitEncoder::fillBuffer(const MTL::Buffer* buffer, NS::Range range,
                             u8 value) {
    if (mtl4) {
        owner->track(buffer);
        beforeWork();
        mtl4->fillBuffer(buffer, range, value);
    } else {
        mtl3->fillBuffer(buffer, range, value);
    }
}

void BlitEncoder::generateMipmaps(const MTL::Texture* texture) {
    if (mtl4) {
        owner->track(texture);
        beforeWork();
        mtl4->generateMipmaps(texture);
    } else {
        mtl3->generateMipmaps(texture);
    }
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
