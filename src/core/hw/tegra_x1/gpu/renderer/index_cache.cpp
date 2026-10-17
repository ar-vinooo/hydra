#include "core/hw/tegra_x1/gpu/renderer/index_cache.hpp"

#include <cstddef>
#include <cstring>

#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/buffer_base.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

namespace {

constexpr usize MAX_CACHED_INDEX_BUFFERS = 4096;

// Primitives Metal draws natively, with indices it supports
bool needsDecoding(const IndexDescriptor& descriptor) {
    switch (descriptor.primitive_type) {
    case engines::PrimitiveType::Quads:
    case engines::PrimitiveType::QuadStrip:
    case engines::PrimitiveType::TriangleFan:
    case engines::PrimitiveType::Polygon:
    case engines::PrimitiveType::LineLoop:
        return true;
    default:
        return descriptor.mem_range &&
               descriptor.type == engines::IndexType::UInt8;
    }
}

engines::PrimitiveType getDecodedPrimitiveType(engines::PrimitiveType type) {
    switch (type) {
    case engines::PrimitiveType::Quads:
    case engines::PrimitiveType::QuadStrip:
    case engines::PrimitiveType::TriangleFan:
    case engines::PrimitiveType::Polygon:
        return engines::PrimitiveType::Triangles;
    case engines::PrimitiveType::LineLoop:
        return engines::PrimitiveType::LineStrip;
    default:
        return type;
    }
}

u32 getDecodedCount(engines::PrimitiveType type, u32 count) {
    switch (type) {
    case engines::PrimitiveType::Quads:
        return count / 4 * 6;
    case engines::PrimitiveType::QuadStrip:
        return count >= 4 ? (count - 2) / 2 * 6 : 0;
    case engines::PrimitiveType::TriangleFan:
    case engines::PrimitiveType::Polygon:
        return count >= 3 ? (count - 2) * 3 : 0;
    case engines::PrimitiveType::LineLoop:
        return count > 1 ? count + 1 : count;
    default:
        return count;
    }
}

// Calls emit with the position of each vertex of the decoded primitives,
// keeping the winding of the guest primitives
template <typename Emit>
void forEachDecodedVertex(engines::PrimitiveType type, u32 count, Emit emit) {
    switch (type) {
    case engines::PrimitiveType::Quads:
        for (u32 base = 0; base + 3 < count; base += 4) {
            for (const u32 offset : {0u, 1u, 2u, 0u, 2u, 3u})
                emit(base + offset);
        }
        break;
    case engines::PrimitiveType::QuadStrip:
        // Quad i is made of the vertices 2i, 2i + 1, 2i + 3 and 2i + 2
        for (u32 base = 0; base + 3 < count; base += 2) {
            for (const u32 offset : {0u, 1u, 3u, 0u, 3u, 2u})
                emit(base + offset);
        }
        break;
    case engines::PrimitiveType::TriangleFan:
    case engines::PrimitiveType::Polygon:
        for (u32 i = 1; i + 1 < count; i++) {
            emit(0);
            emit(i);
            emit(i + 1);
        }
        break;
    case engines::PrimitiveType::LineLoop:
        for (u32 i = 0; i < count; i++)
            emit(i);
        if (count > 1)
            emit(0);
        break;
    default:
        for (u32 i = 0; i < count; i++)
            emit(i);
        break;
    }
}

u32 readIndex(uptr in, engines::IndexType type, u32 position) {
    switch (type) {
    case engines::IndexType::UInt8:
        return reinterpret_cast<const u8*>(in)[position];
    case engines::IndexType::UInt16:
        return reinterpret_cast<const u16*>(in)[position];
    default:
        return reinterpret_cast<const u32*>(in)[position];
    }
}

bool isSameDescriptor(const IndexDescriptor& a, const IndexDescriptor& b) {
    if (a.type != b.type || a.primitive_type != b.primitive_type ||
        a.count != b.count || a.mem_range.has_value() != b.mem_range.has_value())
        return false;
    return !a.mem_range ||
           (a.mem_range->getBegin() == b.mem_range->getBegin() &&
            a.mem_range->getEnd() == b.mem_range->getEnd());
}

} // namespace

IndexCache::~IndexCache() = default;

BufferView IndexCache::decodeHot0(ICommandBuffer* command_buffer,
                              const IndexDescriptor& descriptor,
                              engines::IndexType& out_type,
                              engines::PrimitiveType& out_primitive_type,
                              u32& out_count) {
    out_type = descriptor.type;
    out_primitive_type = descriptor.primitive_type;
    out_count = descriptor.count;
    if (!needsDecoding(descriptor)) {
        if (descriptor.mem_range)
            return renderer.getBufferCache().get(command_buffer,
                                                 *descriptor.mem_range);
        return BufferView();
    }

    out_primitive_type = getDecodedPrimitiveType(descriptor.primitive_type);
    out_count = getDecodedCount(descriptor.primitive_type, descriptor.count);
    // Generated indices are vertex positions, below the vertex count
    if (descriptor.mem_range)
        out_type = descriptor.type == engines::IndexType::UInt32
                       ? engines::IndexType::UInt32
                       : engines::IndexType::UInt16;
    else
        out_type = descriptor.count > 0x10000 ? engines::IndexType::UInt32
                                              : engines::IndexType::UInt16;
    if (out_count == 0)
        return BufferView();

    const auto* source =
        descriptor.mem_range
            ? reinterpret_cast<const u8*>(descriptor.mem_range->getBegin())
            : nullptr;
    const usize source_size =
        descriptor.mem_range ? descriptor.mem_range->getSize() : 0;

    // Buffers in use stay alive: Metal retains what commands reference
    if (cache.size() >= MAX_CACHED_INDEX_BUFFERS)
        cache.clear();
    const auto hash_value = hash(descriptor);
    auto& entry = cache[hash_value];
    if (entry.buffer != nullptr &&
        isSameDescriptor(entry.descriptor, descriptor) &&
        entry.source.size() == source_size &&
        (source_size == 0 ||
         std::memcmp(entry.source.data(), source, source_size) == 0))
        return BufferView{entry.buffer};

    // Deleted directly: views can outlive the renderer's backend
    const auto index_size = getIndexTypeSize(out_type);
    std::shared_ptr<BufferBase> index_buffer(renderer.allocateTemporaryBuffer(
        static_cast<u64>(out_count) * static_cast<u64>(index_size)));
    entry.descriptor = descriptor;
    entry.source.assign(source, source + source_size);
    entry.buffer = index_buffer;

    auto out = index_buffer->getPtr();
    const auto write = [&](u32 index) {
        if (out_type == engines::IndexType::UInt32) {
            *reinterpret_cast<u32*>(out) = index;
            out += sizeof(u32);
        } else {
            *reinterpret_cast<u16*>(out) = static_cast<u16>(index);
            out += sizeof(u16);
        }
    };
    if (descriptor.mem_range) {
        const uptr in = descriptor.mem_range->getBegin();
        forEachDecodedVertex(descriptor.primitive_type, descriptor.count,
                             [&](u32 position) {
                                 write(readIndex(in, descriptor.type, position));
                             });
    } else {
        forEachDecodedVertex(descriptor.primitive_type, descriptor.count, write);
    }

    return BufferView{index_buffer};
}

u32 IndexCache::hashHot1(const IndexDescriptor& descriptor) {
    ztd::hash::XxHash32 hash;
    hash.add(descriptor.type);
    hash.add(descriptor.primitive_type);
    if (descriptor.mem_range) {
        hash.add(descriptor.mem_range->getBegin());
        hash.add(descriptor.mem_range->getEnd());
    }
    hash.add(descriptor.count);
    return hash.toHashCode();
}

} // namespace hydra::hw::tegra_x1::gpu::renderer
