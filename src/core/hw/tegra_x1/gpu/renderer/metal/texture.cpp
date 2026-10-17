#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"
#include "core/hot/table.hpp"

#include <map>
#include <memory>
#include <mutex>

#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

namespace {

// Copies a region between textures. Metal blits need equal pixel formats:
// formats of the same texel size go through a view, single plane depth and
// color formats through a buffer.
void copyRegion(MTL::BlitCommandEncoder* encoder, MTL::Texture* src,
                TextureFormat src_format, u32 src_slice, u32 src_level,
                MTL::Origin src_origin, MTL::Texture* dst,
                TextureFormat dst_format, u32 dst_slice, u32 dst_level,
                MTL::Origin dst_origin, MTL::Size size) {
    if (src->pixelFormat() == dst->pixelFormat()) {
        encoder->copyFromTexture(src, src_slice, src_level, src_origin, size,
                                 dst, dst_slice, dst_level, dst_origin);
        return;
    }

    const auto& src_info = getTextureFormatInfo(src_format);
    const auto& dst_info = getTextureFormatInfo(dst_format);
    const bool same_texel = src_info.bytes_per_block ==
                                dst_info.bytes_per_block &&
                            src_info.block_width == dst_info.block_width &&
                            src_info.block_height == dst_info.block_height;
    const auto& src_host = toMtlPixelFormatInfo(src_format);
    const auto& dst_host = toMtlPixelFormatInfo(dst_format);
    const bool src_ds = src_host.has_depth || src_host.has_stencil;
    const bool dst_ds = dst_host.has_depth || dst_host.has_stencil;
    // Packed depth stencil (stored as D32FS8) and RGB32 (stored as RGBA32)
    // have host texels of another size
    const auto host_matches_guest = [](TextureFormat format,
                                       const PixelFormatInfo& host) {
        return !(host.has_depth && host.has_stencil) &&
               getHostTextureFormat(format) == format;
    };
    const bool sizes_match = same_texel &&
                             host_matches_guest(src_format, src_host) &&
                             host_matches_guest(dst_format, dst_host);
    if (!sizes_match) {
        ONCE(LOG_NOT_IMPLEMENTED(MetalRenderer, "Texture copy from {} to {}",
                                 src_format, dst_format));
        return;
    }

    if (!src_ds && !dst_ds && src->buffer() == nullptr) {
        auto* view = src->newTextureView(dst->pixelFormat());
        if (view != nullptr) {
            encoder->copyFromTexture(view, src_slice, src_level, src_origin,
                                     size, dst, dst_slice, dst_level,
                                     dst_origin);
            // Encoded commands retain the view
            view->release();
            return;
        }
    }

    // Through a buffer, in texel blocks
    const u64 blocks_x =
        (size.width + src_info.block_width - 1) / src_info.block_width;
    const u64 blocks_y =
        (size.height + src_info.block_height - 1) / src_info.block_height;
    const u64 row_bytes = blocks_x * src_info.bytes_per_block;
    const u64 image_bytes = row_bytes * blocks_y;
    auto* buffer = src->device()->newBuffer(image_bytes * size.depth,
                                            MTL::ResourceStorageModePrivate);
    encoder->copyFromTexture(src, src_slice, src_level, src_origin, size,
                             buffer, 0, row_bytes, image_bytes);
    encoder->copyFromBuffer(buffer, 0, row_bytes, image_bytes, size, dst,
                            dst_slice, dst_level, dst_origin);
    buffer->release();
}

} // namespace

Texture::Texture(MTL::Device* device, const TextureDescriptor& descriptor)
    : ITexture(descriptor) {
    const auto type = toMtlTextureType(descriptor.type);

    MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
    desc->setTextureType(type);
    desc->setWidth(descriptor.width);
    desc->setHeight(descriptor.height);
    desc->setDepth(descriptor.depth);
    desc->setMipmapLevelCount(descriptor.level_count);
    desc->setStorageMode(MTL::StorageModePrivate);

    switch (descriptor.type) {
    case TextureType::_1DArray:
    case TextureType::_2DArray:
        desc->setArrayLength(descriptor.layer_count);
        break;
    case TextureType::Cube:
        ASSERT_DEBUG(descriptor.layer_count == 6, MetalRenderer,
                     "Invalid cube layer count {}", descriptor.layer_count);
        break;
    case TextureType::CubeArray:
        ASSERT_DEBUG(descriptor.layer_count % 6 == 0, MetalRenderer,
                     "Invalid cube array layer count {}",
                     descriptor.layer_count);
        desc->setArrayLength(descriptor.layer_count / 6);
        break;
    default:
        ASSERT_DEBUG(descriptor.layer_count == 1, MetalRenderer,
                     "Invalid {} layer count {}", descriptor.type,
                     descriptor.layer_count);
        break;
    }

    const auto& pixel_format_info = toMtlPixelFormatInfo(descriptor.format);
    desc->setPixelFormat(pixel_format_info.pixel_format);

    // Color textures can also be written by compute shaders
    MTL::TextureUsage write_usage = 0;
    if (!pixel_format_info.has_depth && !pixel_format_info.has_stencil &&
        !isTextureFormatCompressed(descriptor.format)) {
        write_usage |= MTL::TextureUsageShaderWrite;
        if (pixel_format_info.pixel_format == MTL::PixelFormatR32Uint ||
            pixel_format_info.pixel_format == MTL::PixelFormatR32Sint)
            write_usage |= MTL::TextureUsageShaderAtomic;
    }

    if (descriptor.type == TextureType::_1DBuffer) {
        const auto alignment =
            device->minimumLinearTextureAlignmentForPixelFormat(
                pixel_format_info.pixel_format);
        const u64 host_size = getTextureFormatStride(
            getHostTextureFormat(descriptor.format), descriptor.width);
        const auto bytes = align(std::max(static_cast<u64>(descriptor.size), host_size),
                                 static_cast<u64>(alignment));
        desc->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView | write_usage);
        buffer_backing =
            device->newBuffer(bytes, MTL::ResourceStorageModePrivate);
        texture = buffer_backing->newTexture(desc, 0, bytes);
    } else {
        // Guest textures can be sampled, rendered to and reinterpreted
        desc->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsageRenderTarget |
                       MTL::TextureUsagePixelFormatView | write_usage);
        texture = device->newTexture(desc);
    }
    desc->release();
    ASSERT(texture != nullptr, MetalRenderer,
           "Failed to create {} texture (width: {}, format: {})",
           descriptor.type, descriptor.width, descriptor.format);
}

Texture::~Texture() {
    if (byte_copy_pipeline)
        byte_copy_pipeline->release();
    texture->release();
    if (buffer_backing)
        buffer_backing->release();
}

ITextureView*
Texture::createViewHot0(const TextureViewDescriptor& view_descriptor) {
    return new TextureView(this, view_descriptor);
}

void Texture::copyBufferBytesHot1(ICommandBuffer* command_buffer,
                              const ITexture* src, u64 src_offset,
                              u64 dst_offset, u64 size) {
    const auto* source = static_cast<const Texture*>(src);
    ASSERT(src_offset <= source->descriptor.size &&
               size <= source->descriptor.size - src_offset &&
               dst_offset <= descriptor.size && size <= descriptor.size - dst_offset,
           MetalRenderer, "Texture byte copy exceeds guest range");
    if (size == 0)
        return;
    auto* commands = static_cast<CommandBuffer*>(command_buffer);
    if (buffer_backing && source->buffer_backing &&
        (src_offset | dst_offset | size) % 4 == 0) {
        commands->getBlitCommandEncoder()->copyFromBuffer(
            source->buffer_backing, src_offset, buffer_backing, dst_offset,
            size);
        return;
    }
    const auto create_pipeline = [&] {
        const char* code = R"(
            #include <metal_stdlib>
            using namespace metal;
            kernel void copy_bytes(device const uchar* src [[buffer(0)]],
                                   device uchar* dst [[buffer(1)]],
                                   constant ulong* p [[buffer(2)]],
                                   uint i [[thread_position_in_grid]]) {
                if (p[3] == 0) {
                    if (ulong(i) < p[2]) dst[p[1] + i] = src[p[0] + i];
                    return;
                }
                if (ulong(i) >= p[3] * p[4]) return;
                const ulong x = ulong(i) % p[3];
                const ulong y = ulong(i) / p[3];
                ulong guest;
                if (p[5] != 0) {
                    guest = y * p[5] + x;
                } else {
                    const ulong pitch_align = 64ul << p[6];
                    const ulong gobs_x = ((p[3] + pitch_align - 1) / pitch_align) * (pitch_align / 64ul);
                    const ulong block_rows = 8ul << p[7];
                    const ulong block_size = 512ul << (p[7] + p[8]);
                    const ulong gx = x % 64ul, gy = y % 8ul;
                    guest = (y / block_rows) * gobs_x * block_size +
                            (x / 64ul) * block_size +
                            ((y % block_rows) / 8ul) * 512ul +
                            (gx / 32ul) * 256ul + (gy / 2ul) * 64ul +
                            ((gx % 32ul) / 16ul) * 32ul + (gy % 2ul) * 16ul + gx % 16ul;
                }
                const ulong begin = p[9] != 0 ? p[1] : p[0];
                if (guest < begin || guest - begin >= p[2]) return;
                if (p[9] != 0) dst[i] = src[p[0] + guest - begin];
                else dst[p[1] + guest - begin] = src[i];
            }
        )";
        NS::Error* error = nullptr;
        NS_STACK_SCOPED auto library =
            texture->device()->newLibrary(toNsString(code), nullptr, &error);
        ASSERT(library, MetalRenderer, "Failed to compile texture byte copy: {}",
               error ? error->localizedDescription()->utf8String() : "unknown error");
        NS_STACK_SCOPED auto function =
            library->newFunction(toNsString("copy_bytes"));
        auto* pipeline = texture->device()->newComputePipelineState(function, &error);
        ASSERT(pipeline, MetalRenderer,
               "Failed to create texture byte copy pipeline: {}",
               error ? error->localizedDescription()->utf8String() : "unknown error");
        return pipeline;
    };
    const bool mixed = !buffer_backing || !source->buffer_backing;
    MTL::ComputePipelineState* copy_pipeline = byte_copy_pipeline;
    if (mixed) {
        // Old live textures may still own the buffer-only kernel. Keep mixed
        // kernels separate and revision-specific without changing object layout.
        static std::mutex pipeline_mutex;
        static std::map<std::pair<MTL::Device*, u64>,
                        std::shared_ptr<MTL::ComputePipelineState>> pipelines;
        std::scoped_lock lock(pipeline_mutex);
        const auto key = std::pair{texture->device(), ::hydra::hot::generation()};
        auto it = pipelines.find(key);
        if (it == pipelines.end())
            it = pipelines.emplace(key, std::shared_ptr<MTL::ComputePipelineState>(
                create_pipeline(), [](auto* pipeline) { pipeline->release(); })).first;
        copy_pipeline = it->second.get();
    } else if (!copy_pipeline) {
        byte_copy_pipeline = create_pipeline();
        copy_pipeline = byte_copy_pipeline;
    }

    u64 params[10] = {src_offset, dst_offset, size};
    auto* src_buffer = source->buffer_backing;
    auto* dst_buffer = buffer_backing;
    MTL::Buffer* packed = nullptr;
    u64 thread_count = size;
    if (mixed) {
        ASSERT(bool(src_buffer) != bool(dst_buffer), MetalRenderer,
               "A mixed byte copy requires one buffer and one 2D texture");
        const auto* surface = src_buffer ? this : source;
        const auto& d = surface->descriptor;
        ASSERT(d.type == TextureType::_2D && d.level_count == 1 && d.layer_count == 1 &&
                   d.depth == 1 && !isTextureFormatCompressed(d.format) &&
                   !isTextureFormatDepthOrStencil(d.format), MetalRenderer,
               "Mixed byte copy requires a single-level non-depth 2D surface");
        const u64 row_bytes = getTextureFormatStride(d.format, d.width);
        const auto blocks = d.getLevelBlockSizeLog2(0);
        ASSERT(row_bytes != 0 && d.height != 0 && row_bytes * d.height <= UINT32_MAX &&
                   blocks.x() <= 5 && blocks.y() <= 5 && blocks.z() <= 5 &&
                   (!d.is_linear || d.linear_stride >= row_bytes), MetalRenderer,
               "Invalid mixed byte copy surface layout");
        thread_count = row_bytes * d.height;
        packed = texture->device()->newBuffer(thread_count, MTL::ResourceStorageModePrivate);
        ASSERT(packed, MetalRenderer, "Cannot allocate mixed byte copy staging buffer");
        // The destination's untouched texels must survive a subrange update.
        commands->getBlitCommandEncoder()->copyFromTexture(
            surface->texture, 0, 0, MTL::Origin(0, 0, 0),
            MTL::Size(d.width, d.height, 1), packed, 0, row_bytes, thread_count);
        params[3] = row_bytes;
        params[4] = d.height;
        params[5] = d.is_linear ? d.linear_stride : 0;
        params[6] = blocks.x();
        params[7] = blocks.y();
        params[8] = blocks.z();
        params[9] = src_buffer ? 1 : 0;
        if (src_buffer) dst_buffer = packed;
        else src_buffer = packed;
    }
    auto* encoder = commands->getComputeCommandEncoder();
    encoder->setComputePipelineState(copy_pipeline);
    encoder->setBuffer(src_buffer, 0, 0);
    encoder->setBuffer(dst_buffer, 0, 1);
    encoder->setBytes(params, sizeof(params), 2);
    encoder->dispatchThreads(MTL::Size(thread_count, 1, 1), MTL::Size(64, 1, 1));
    if (mixed && buffer_backing == nullptr) {
        const u64 row_bytes = params[3];
        commands->getBlitCommandEncoder()->copyFromBuffer(
            packed, 0, row_bytes, thread_count,
            MTL::Size(descriptor.width, descriptor.height, 1), texture, 0, 0,
            MTL::Origin(0, 0, 0));
    }
    // Metal command buffers retain encoded resources; no allocation is reused here.
    if (packed) packed->release();
}

void Texture::copyFromHot2(ICommandBuffer* command_buffer, const BufferBase* src,
                       const ztd::Range<u32> dst_levels,
                       const ztd::Range<u32> dst_layers) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);
    const auto mtl_src = static_cast<const Buffer*>(src)->getBuffer();

    auto encoder = command_buffer_impl->getBlitCommandEncoder();

    const bool expanded = getHostTextureFormat(descriptor.format) != descriptor.format;
    if (buffer_backing && !expanded) {
        encoder->copyFromBuffer(mtl_src, 0, buffer_backing, 0, descriptor.size);
        return;
    }

    const bool is_packed_depth_stencil =
        texture->pixelFormat() == MTL::PixelFormatDepth32Float_Stencil8;

    u32 offset = 0;
    for (u32 layer = dst_layers.getBegin(); layer < dst_layers.getEnd();
         layer++) {
        for (u32 level = dst_levels.getBegin(); level < dst_levels.getEnd();
             level++) {
            // Calculate sizes
            const auto dims = descriptor.getLevelDimensions(level);
            const auto stride =
                getTextureFormatStride(descriptor.format, dims.x());
            const auto slice_stride = getTextureFormatSliceStride(
                descriptor.format, dims.x(), dims.y());

            // Copy
            if (expanded) {
                const auto host_format = getHostTextureFormat(descriptor.format);
                const auto host_stride = getTextureFormatStride(host_format, dims.x());
                const u64 host_slice = static_cast<u64>(host_stride) * dims.y();
                const u64 host_size = host_slice * dims.z();
                ASSERT(mtl_src->contents() != nullptr, MetalRenderer,
                       "Expanded texture uploads require mapped source bytes");
                auto* staging = texture->device()->newBuffer(
                    host_size, MTL::ResourceStorageModeShared);
                expandTexturePixels(descriptor.format,
                    static_cast<const u8*>(mtl_src->contents()) + offset,
                    static_cast<u8*>(staging->contents()),
                    static_cast<usize>(dims.x()) * dims.y() * dims.z());
                if (buffer_backing) {
                    encoder->copyFromBuffer(staging, 0, buffer_backing, 0, host_size);
                } else {
                    encoder->copyFromBuffer(staging, 0, host_stride, host_slice,
                                            MTL::Size(dims.x(), dims.y(), dims.z()),
                                            texture, layer, level,
                                            MTL::Origin(0, 0, 0));
                }
                staging->release();
            } else if (is_packed_depth_stencil) {
                copyDepthStencilFromBuffer(
                    encoder,
                    reinterpret_cast<const u8*>(mtl_src->contents()) + offset,
                    dims, layer, level);
            } else {
                encoder->copyFromBuffer(mtl_src, offset, stride, slice_stride,
                                        MTL::Size(dims.x(), dims.y(), dims.z()),
                                        texture, layer, level,
                                        MTL::Origin(0, 0, 0));
            }

            // Add offset
            offset += dims.z() * slice_stride;
        }
    }
}

void Texture::copyDepthStencilFromBufferHot3(MTL::BlitCommandEncoder* encoder,
                                         const u8* src, const uint3 dims,
                                         const u32 layer, const u32 level) {
    // Metal can only upload depth and stencil of a packed depth stencil
    // texture separately, so split the guest data into 2 planes
    const usize pixel_count = static_cast<usize>(dims.x()) * dims.y() * dims.z();
    auto device = texture->device();
    auto depth_buffer = device->newBuffer(pixel_count * sizeof(f32),
                                          MTL::ResourceStorageModeShared);
    auto stencil_buffer =
        device->newBuffer(pixel_count, MTL::ResourceStorageModeShared);
    auto depth = reinterpret_cast<f32*>(depth_buffer->contents());
    auto stencil = reinterpret_cast<u8*>(stencil_buffer->contents());

    for (usize i = 0; i < pixel_count; i++) {
        switch (descriptor.format) {
        case TextureFormat::Z32Float_X24S8Uint: {
            u32 words[2];
            std::memcpy(words, src + i * sizeof(words), sizeof(words));
            std::memcpy(&depth[i], &words[0], sizeof(f32));
            stencil[i] = static_cast<u8>(words[1]);
            break;
        }
        case TextureFormat::S8Uint_Z24Unorm: {
            u32 word;
            std::memcpy(&word, src + i * sizeof(word), sizeof(word));
            depth[i] =
                static_cast<f32>(word >> 8) / static_cast<f32>(0xffffffu);
            stencil[i] = static_cast<u8>(word);
            break;
        }
        default: { // Z24Unorm_S8Uint, Z24Unorm_X8Uint
            u32 word;
            std::memcpy(&word, src + i * sizeof(word), sizeof(word));
            depth[i] = static_cast<f32>(word & 0xffffffu) /
                       static_cast<f32>(0xffffffu);
            stencil[i] = descriptor.format == TextureFormat::Z24Unorm_S8Uint
                             ? static_cast<u8>(word >> 24)
                             : 0;
            break;
        }
        }
    }

    const auto size = MTL::Size(dims.x(), dims.y(), dims.z());
    encoder->copyFromBuffer(depth_buffer, 0, dims.x() * sizeof(f32),
                            static_cast<usize>(dims.x()) * dims.y() *
                                sizeof(f32),
                            size, texture, layer, level, MTL::Origin(0, 0, 0),
                            MTL::BlitOptionDepthFromDepthStencil);
    encoder->copyFromBuffer(stencil_buffer, 0, dims.x(),
                            static_cast<usize>(dims.x()) * dims.y(), size,
                            texture, layer, level, MTL::Origin(0, 0, 0),
                            MTL::BlitOptionStencilFromDepthStencil);

    // The command buffer keeps the buffers alive until it completes
    depth_buffer->release();
    stencil_buffer->release();
}

// TODO: make sure source and destination sizes match
void Texture::copyFromHot4(ICommandBuffer* command_buffer, const ITexture* src,
                       const u32 src_level, const u32 src_layer,
                       const u32 dst_level, const u32 dst_layer,
                       const u32 level_count, const u32 layer_count) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);
    const auto mtl_src = static_cast<const Texture*>(src)->getTexture();

    auto encoder = command_buffer_impl->getBlitCommandEncoder();

    for (u32 i = 0; i < layer_count; i++) {
        for (u32 j = 0; j < level_count; j++) {
            const u32 crnt_dst_level = dst_level + j;
            const auto dims = descriptor.getLevelDimensions(crnt_dst_level);
            copyRegion(encoder, mtl_src, src->getDescriptor().format,
                       src_layer + i, src_level + j, MTL::Origin(0, 0, 0),
                       texture, descriptor.format, dst_layer + i,
                       crnt_dst_level, MTL::Origin(0, 0, 0),
                       MTL::Size(dims.x(), dims.y(), dims.z()));
        }
    }
}

void Texture::copyFromHot5(ICommandBuffer* command_buffer, const ITexture* src,
                       const uint3 src_origin, const u32 src_level,
                       const u32 src_layer, const uint3 dst_origin,
                       const u32 dst_level, const u32 dst_layer,
                       const uint3 size, const u32 layer_count) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);
    const auto mtl_src = static_cast<const Texture*>(src)->getTexture();

    auto encoder = command_buffer_impl->getBlitCommandEncoder();

    for (u32 i = 0; i < layer_count; i++) {
        copyRegion(encoder, mtl_src, src->getDescriptor().format,
                   src_layer + i, src_level,
                   MTL::Origin(src_origin.x(), src_origin.y(), src_origin.z()),
                   texture, descriptor.format, dst_layer + i, dst_level,
                   MTL::Origin(dst_origin.x(), dst_origin.y(), dst_origin.z()),
                   MTL::Size(size.x(), size.y(), size.z()));
    }
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
