#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

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

    if (descriptor.type == TextureType::_1DBuffer) {
        const auto alignment =
            device->minimumLinearTextureAlignmentForPixelFormat(
                pixel_format_info.pixel_format);
        const auto bytes = align(static_cast<u64>(descriptor.size),
                                 static_cast<u64>(alignment));
        desc->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
        buffer_backing =
            device->newBuffer(bytes, MTL::ResourceStorageModePrivate);
        texture = buffer_backing->newTexture(desc, 0, bytes);
    } else {
        // Guest textures can be sampled, rendered to and reinterpreted
        desc->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsageRenderTarget |
                       MTL::TextureUsagePixelFormatView);
        texture = device->newTexture(desc);
    }
    desc->release();
    ASSERT(texture != nullptr, MetalRenderer,
           "Failed to create {} texture (width: {}, format: {})",
           descriptor.type, descriptor.width, descriptor.format);
}

Texture::~Texture() {
    texture->release();
    if (buffer_backing)
        buffer_backing->release();
}

ITextureView*
Texture::createView(const TextureViewDescriptor& view_descriptor) {
    return new TextureView(this, view_descriptor);
}

void Texture::copyFrom(ICommandBuffer* command_buffer, const BufferBase* src,
                       const ztd::Range<u32> dst_levels,
                       const ztd::Range<u32> dst_layers) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);
    const auto mtl_src = static_cast<const Buffer*>(src)->getBuffer();

    auto encoder = command_buffer_impl->getBlitCommandEncoder();

    if (buffer_backing) {
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
            if (is_packed_depth_stencil) {
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

void Texture::copyDepthStencilFromBuffer(MTL::BlitCommandEncoder* encoder,
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
void Texture::copyFrom(ICommandBuffer* command_buffer, const ITexture* src,
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
            encoder->copyFromTexture(
                mtl_src, src_layer + i, src_level + j, MTL::Origin(0, 0, 0),
                MTL::Size(dims.x(), dims.y(), dims.z()), texture, dst_layer + i,
                crnt_dst_level, MTL::Origin(0, 0, 0));
        }
    }
}

void Texture::copyFrom(ICommandBuffer* command_buffer, const ITexture* src,
                       const uint3 src_origin, const u32 src_level,
                       const u32 src_layer, const uint3 dst_origin,
                       const u32 dst_level, const u32 dst_layer,
                       const uint3 size, const u32 layer_count) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);
    const auto mtl_src = static_cast<const Texture*>(src)->getTexture();

    auto encoder = command_buffer_impl->getBlitCommandEncoder();

    for (u32 i = 0; i < layer_count; i++) {
        encoder->copyFromTexture(
            mtl_src, src_layer + i, src_level,
            MTL::Origin(src_origin.x(), src_origin.y(), src_origin.z()),
            MTL::Size(size.x(), size.y(), size.z()), texture, dst_layer + i,
            dst_level,
            MTL::Origin(dst_origin.x(), dst_origin.y(), dst_origin.z()));
    }
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
