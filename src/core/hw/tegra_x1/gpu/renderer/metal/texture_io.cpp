#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"

#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/maxwell_to_mtl.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

namespace {

// One memory plane of a subresource
struct Plane {
    MTL::BlitOption option;
    u64 bytes_per_pixel;
};

struct Subresource {
    u32 layer;
    u32 level;
    u64 width;
    u64 height;
    u64 depth;
    // Block rows and row length of the first plane (compressed formats)
    u64 blocks_x;
    u64 blocks_y;
};

// Planes in storage order, empty if the pixel format is not covered
std::vector<Plane> getPlanes(const TextureDescriptor& descriptor,
                             MTL::PixelFormat format) {
    switch (format) {
    case MTL::PixelFormatDepth16Unorm:
        return {{MTL::BlitOptionNone, 2}};
    case MTL::PixelFormatDepth32Float:
        return {{MTL::BlitOptionNone, 4}};
    case MTL::PixelFormatStencil8:
        return {{MTL::BlitOptionNone, 1}};
    case MTL::PixelFormatDepth32Float_Stencil8:
        return {{MTL::BlitOptionDepthFromDepthStencil, 4},
                {MTL::BlitOptionStencilFromDepthStencil, 1}};
    default: {
        // Color formats must be stored the way the guest names them
        if (getHostTextureFormat(descriptor.format) != descriptor.format)
            return {};
        return {{MTL::BlitOptionNone,
                 getTextureFormatInfo(descriptor.format).bytes_per_block}};
    }
    }
}

std::vector<Subresource> getSubresources(const TextureDescriptor& descriptor) {
    const auto& info = getTextureFormatInfo(descriptor.format);
    std::vector<Subresource> result;
    for (u32 layer = 0; layer < descriptor.layer_count; layer++) {
        for (u32 level = 0; level < descriptor.level_count; level++) {
            const auto dims = descriptor.getLevelDimensions(level);
            Subresource sub{layer, level, dims.width, dims.height, dims.depth, 0, 0};
            sub.blocks_x = (sub.width + info.block_width - 1) / info.block_width;
            sub.blocks_y =
                (sub.height + info.block_height - 1) / info.block_height;
            result.push_back(sub);
        }
    }
    return result;
}

// Bytes per row and per image of a plane. Compressed formats are addressed in
// blocks; depth and stencil planes have one texel per pixel.
struct Layout {
    u64 row;
    u64 image;
};
Layout getLayout(const Subresource& sub, const Plane& plane, bool first_plane,
                 bool compressed) {
    const u64 columns = first_plane && compressed ? sub.blocks_x : sub.width;
    const u64 rows = first_plane && compressed ? sub.blocks_y : sub.height;
    return {columns * plane.bytes_per_pixel, columns * rows * plane.bytes_per_pixel};
}

bool supported(const TextureDescriptor& descriptor) {
    return descriptor.level_count != 0 && descriptor.layer_count != 0;
}

} // namespace

// A buffer texture is its backing buffer: one blob of its bytes
bool Renderer::readTextureContents(ITexture* texture,
                                   std::vector<std::vector<u8>>& out) {
    syncDrawQueue();
    const auto& descriptor = texture->getDescriptor();
    if (descriptor.type == MTL::TextureTypeTextureBuffer) {
        auto* backing = static_cast<Texture*>(texture)->getBufferBacking();
        if (backing == nullptr)
            return false;
        const u64 bytes = std::min<u64>(descriptor.size, backing->length());
        out.assign(1, {});
        auto* staging = device->newBuffer(bytes, MTL::ResourceStorageModeShared);
        auto* commands = command_queue->commandBuffer();
        auto* blit = commands->blitCommandEncoder();
        blit->copyFromBuffer(backing, 0, staging, 0, bytes);
        blit->endEncoding();
        commands->commit();
        commands->waitUntilCompleted();
        const bool ok = commands->status() == MTL::CommandBufferStatusCompleted;
        if (ok)
            out[0].assign(static_cast<const u8*>(staging->contents()),
                          static_cast<const u8*>(staging->contents()) + bytes);
        staging->release();
        return ok;
    }
    auto* native = static_cast<Texture*>(texture)->getTexture();
    const auto planes = getPlanes(descriptor, native->pixelFormat());
    if (!supported(descriptor) || planes.empty())
        return false;
    const bool compressed = isTextureFormatCompressed(descriptor.format);
    const auto subs = getSubresources(descriptor);

    struct Pending {
        std::vector<MTL::Buffer*> buffers;
        std::vector<u64> sizes;
    };
    std::vector<Pending> pending(subs.size());
    auto* commands = command_queue->commandBuffer();
    auto* blit = commands->blitCommandEncoder();
    for (usize i = 0; i < subs.size(); i++) {
        const auto& sub = subs[i];
        for (usize p = 0; p < planes.size(); p++) {
            const auto layout = getLayout(sub, planes[p], p == 0, compressed);
            const u64 bytes = layout.image * sub.depth;
            auto* buffer =
                device->newBuffer(bytes, MTL::ResourceStorageModeShared);
            const bool is_3d = descriptor.type == MTL::TextureType3D;
            blit->copyFromTexture(
                native, is_3d ? 0 : sub.layer, sub.level, MTL::Origin(0, 0, 0),
                MTL::Size(sub.width, sub.height, sub.depth), buffer, 0,
                layout.row, layout.image, planes[p].option);
            pending[i].buffers.push_back(buffer);
            pending[i].sizes.push_back(bytes);
        }
    }
    blit->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    const bool ok = commands->status() == MTL::CommandBufferStatusCompleted;
    out.clear();
    for (auto& entry : pending) {
        std::vector<u8> blob;
        for (usize p = 0; p < entry.buffers.size(); p++) {
            const auto* bytes =
                static_cast<const u8*>(entry.buffers[p]->contents());
            blob.insert(blob.end(), bytes, bytes + entry.sizes[p]);
            entry.buffers[p]->release();
        }
        out.push_back(std::move(blob));
    }
    return ok;
}

bool Renderer::readBufferContents(const BufferView& view,
                                  std::vector<u8>& out) {
    syncDrawQueue();
    if (!view.isValid())
        return false;
    auto* native = static_cast<Buffer*>(view.getBase())->getBuffer();
    const u64 offset = view.getOffset();
    const u64 size = view.getSize();
    if (offset + size > native->length())
        return false;
    out.resize(size);
    if (native->storageMode() == MTL::StorageModeShared) {
        std::memcpy(out.data(), static_cast<const u8*>(native->contents()) + offset,
                    size);
        return true;
    }
    auto* staging = device->newBuffer(size, MTL::ResourceStorageModeShared);
    auto* commands = command_queue->commandBuffer();
    auto* blit = commands->blitCommandEncoder();
    blit->copyFromBuffer(native, offset, staging, 0, size);
    blit->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    const bool ok = commands->status() == MTL::CommandBufferStatusCompleted;
    if (ok)
        std::memcpy(out.data(), staging->contents(), size);
    staging->release();
    return ok;
}

bool Renderer::writeTextureContents(
    ITexture* texture, const std::vector<std::vector<u8>>& contents) {
    syncDrawQueue();
    const auto& descriptor = texture->getDescriptor();
    if (descriptor.type == MTL::TextureTypeTextureBuffer) {
        auto* backing = static_cast<Texture*>(texture)->getBufferBacking();
        if (backing == nullptr || contents.size() != 1 ||
            contents[0].size() > backing->length())
            return false;
        auto* staging = device->newBuffer(contents[0].data(), contents[0].size(),
                                          MTL::ResourceStorageModeShared);
        auto* commands = command_queue->commandBuffer();
        auto* blit = commands->blitCommandEncoder();
        blit->copyFromBuffer(staging, 0, backing, 0, contents[0].size());
        blit->endEncoding();
        commands->commit();
        commands->waitUntilCompleted();
        const bool ok = commands->status() == MTL::CommandBufferStatusCompleted;
        staging->release();
        return ok;
    }
    auto* native = static_cast<Texture*>(texture)->getTexture();
    const auto planes = getPlanes(descriptor, native->pixelFormat());
    if (!supported(descriptor) || planes.empty())
        return false;
    const bool compressed = isTextureFormatCompressed(descriptor.format);
    const auto subs = getSubresources(descriptor);
    if (contents.size() != subs.size())
        return false;

    std::vector<MTL::Buffer*> buffers;
    auto* commands = command_queue->commandBuffer();
    auto* blit = commands->blitCommandEncoder();
    bool valid = true;
    for (usize i = 0; i < subs.size() && valid; i++) {
        const auto& sub = subs[i];
        u64 offset = 0;
        for (usize p = 0; p < planes.size(); p++) {
            const auto layout = getLayout(sub, planes[p], p == 0, compressed);
            const u64 bytes = layout.image * sub.depth;
            if (offset + bytes > contents[i].size()) {
                valid = false;
                break;
            }
            auto* buffer = device->newBuffer(contents[i].data() + offset, bytes,
                                             MTL::ResourceStorageModeShared);
            const bool is_3d = descriptor.type == MTL::TextureType3D;
            blit->copyFromBuffer(buffer, 0, layout.row, layout.image,
                                 MTL::Size(sub.width, sub.height, sub.depth),
                                 native, is_3d ? 0 : sub.layer, sub.level,
                                 MTL::Origin(0, 0, 0), planes[p].option);
            buffers.push_back(buffer);
            offset += bytes;
        }
    }
    blit->endEncoding();
    commands->commit();
    commands->waitUntilCompleted();
    const bool ok = valid &&
                    commands->status() == MTL::CommandBufferStatusCompleted;
    for (auto* buffer : buffers)
        buffer->release();
    return ok;
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
