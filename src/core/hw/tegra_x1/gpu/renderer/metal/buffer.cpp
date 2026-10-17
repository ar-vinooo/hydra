#include "core/hw/tegra_x1/gpu/renderer/metal/buffer.hpp"
#include "core/hot/table.hpp"

#include <map>
#include <memory>
#include <mutex>

#include "core/hw/tegra_x1/gpu/renderer/metal/command_buffer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/renderer.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/texture_view.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

namespace {
MTL::ComputePipelineState* packedReadbackPipeline(MTL::Device* device) {
    static std::mutex mutex;
    static std::map<std::pair<MTL::Device*, u64>,
                    std::shared_ptr<MTL::ComputePipelineState>> pipelines;
    std::scoped_lock lock(mutex);
    const auto key = std::pair{device, ::hydra::hot::generation()};
    if (const auto it = pipelines.find(key); it != pipelines.end())
        return it->second.get();
    const char* source = R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void pack_pixels(device const uchar* src [[buffer(0)]],
                                device uchar* dst [[buffer(1)]],
                                constant ulong* p [[buffer(2)]],
                                uint tid [[thread_position_in_grid]]) {
            const ulong i = tid;
            if (i >= p[0]) return;
            if (p[2] == 1) {
                const uint r = (uint(src[i * 2]) * 15 + 127) / 255;
                const uint g = (uint(src[i * 2 + 1]) * 15 + 127) / 255;
                dst[p[1] + i] = uchar(r | (g << 4));
            } else {
                dst[p[1] + i] = src[(i / 12) * 16 + i % 12];
            }
        })";
    NS::Error* error = nullptr;
    auto* library = device->newLibrary(
        NS::String::string(source, NS::UTF8StringEncoding), nullptr, &error);
    ASSERT(library != nullptr, MetalRenderer, "Cannot compile packed texture readback");
    auto* function = library->newFunction(
        NS::String::string("pack_pixels", NS::UTF8StringEncoding));
    auto* pipeline = device->newComputePipelineState(function, &error);
    function->release();
    library->release();
    ASSERT(pipeline != nullptr, MetalRenderer, "Cannot create packed texture readback pipeline");
    return pipelines.emplace(key, std::shared_ptr<MTL::ComputePipelineState>(
        pipeline, [](auto* value) { value->release(); })).first->second.get();
}
} // namespace

Buffer::Buffer(MTL::Device* device, u64 size)
    : BufferBase(size),
      buffer{device->newBuffer(size, MTL::ResourceStorageModePrivate)} {}

Buffer::Buffer(MTL::Buffer* buffer_)
    : BufferBase(buffer_->allocatedSize()), buffer{buffer_} {}

Buffer::~Buffer() { buffer->release(); }

void Buffer::copyFromHot0(ICommandBuffer* command_buffer, ITextureView* src,
                      const uint3 src_origin, const uint3 src_size,
                      const ztd::Range<u32> src_levels,
                      const ztd::Range<u32> src_layers, u64 dst_offset) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);
    auto src_impl = static_cast<TextureView*>(src);

    const auto format = src_impl->getDescriptor().format;
    const auto host_format = getHostTextureFormat(format);
    // TODO: bytes per image
    // TODO: calculate the stride for the Metal pixel format
    for (u32 layer = src_layers.getBegin(); layer < src_layers.getEnd();
         layer++) {
        for (u32 level = src_levels.getBegin(); level < src_levels.getEnd();
             level++) {
            auto blit_encoder = command_buffer_impl->getBlitCommandEncoder();
            if (host_format != format) {
                const u64 host_stride = getTextureFormatStride(host_format, src_size.x());
                const u64 host_slice = host_stride * src_size.y();
                const u64 host_size = host_slice * src_size.z();
                const u64 guest_size = static_cast<u64>(
                    getTextureFormatStride(format, src_size.x())) * src_size.y() * src_size.z();
                ASSERT(dst_offset <= size && guest_size <= size - dst_offset,
                       MetalRenderer, "Packed texture readback exceeds destination");
                auto* staging = buffer->device()->newBuffer(
                    host_size, MTL::ResourceStorageModePrivate);
                blit_encoder->copyFromTexture(
                    src_impl->getTexture(), layer, level,
                    MTL::Origin(src_origin.x(), src_origin.y(), src_origin.z()),
                    MTL::Size(src_size.x(), src_size.y(), src_size.z()),
                    staging, 0, host_stride, host_slice);
                auto* compute = command_buffer_impl->getComputeCommandEncoder();
                const u64 params[]{guest_size, dst_offset,
                                   format == TextureFormat::RG4Unorm ? 1ull : 0ull};
                compute->setComputePipelineState(packedReadbackPipeline(buffer->device()));
                compute->setBuffer(staging, 0, 0);
                compute->setBuffer(buffer, 0, 1);
                compute->setBytes(params, sizeof(params), 2);
                compute->dispatchThreads(MTL::Size(guest_size, 1, 1), MTL::Size(64, 1, 1));
                staging->release();
                continue;
            }
            blit_encoder->copyFromTexture(
                src_impl->getTexture(), layer, level,
                MTL::Origin::Make(src_origin.x(), src_origin.y(),
                                  src_origin.z()),
                MTL::Size::Make(src_size.x(), src_size.y(), src_size.z()),
                buffer, dst_offset,
                getTextureFormatStride(src_impl->getDescriptor().format,
                                       src_size.x()),
                0);
        }
    }
}

void Buffer::copyFromImplHot1(const uptr data, u64 dst_offset, u64 size_) {
    memcpy(reinterpret_cast<u8*>(buffer->contents()) + dst_offset,
           reinterpret_cast<void*>(data), size_);
}

void Buffer::copyFromImplHot2(ICommandBuffer* command_buffer, BufferBase* src,
                          u64 dst_offset, u64 src_offset, u64 size_) {
    const auto command_buffer_impl =
        static_cast<CommandBuffer*>(command_buffer);
    auto src_impl = static_cast<Buffer*>(src);

    auto blit_encoder = command_buffer_impl->getBlitCommandEncoder();
    blit_encoder->copyFromBuffer(src_impl->getBuffer(), src_offset, buffer,
                                 dst_offset, size_);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
