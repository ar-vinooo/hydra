#pragma once

#include "core/hw/tegra_x1/gpu/engines/const.hpp"

// Geometry shader emulation. Metal has no geometry stage, so a draw with a
// geometry shader is three draws in the same render pass:
// 1. The vertex shader, rasterization off, writes the output record of each
//    vertex to a buffer (slot = vertex id - min_vertex).
// 2. The geometry shader runs as a vertex function, rasterization off, one
//    thread per input primitive and invocation. It reads its input vertices
//    from the records of step 1 and writes the vertices it emits to fixed
//    slots (keeping the primitive order) along with list indices.
// 3. A passthrough vertex function draws those vertices with the fragment
//    shader. Index entries no primitive used point at a vertex outside the
//    clip volume.
// Buffer barriers order the steps (vertex to vertex stage).
namespace hydra::hw::tegra_x1::gpu::renderer::geometry {

// Output attribute words of a vertex: from 0x60 (layer, viewport index,
// point size) through the position to the end of the generic attributes
constexpr u32 RECORD_BASE = 0x60;
constexpr u32 RECORD_END = 0x280;
constexpr u32 RECORD_WORDS = (RECORD_END - RECORD_BASE) / 4;
constexpr u32 RECORD_SIZE = RECORD_WORDS * 4;

// Buffer slots of the geometry and passthrough vertex functions, which have
// no vertex buffers
constexpr u32 INPUT_BUFFER = 30;
constexpr u32 INDEX_DATA_BUFFER = 29;
constexpr u32 OUTPUT_BUFFER = 28;
constexpr u32 OUTPUT_INDEX_BUFFER = 27;
constexpr u32 PARAMS_BUFFER = 26;

// Header output topology
enum class Output : u32 {
    None = 0,
    Points = 1,
    LineStrip = 6,
    TriangleStrip = 7,
};

// Per draw parameters of the geometry vertex function (PARAMS_BUFFER)
struct Params {
    u32 primitive_count;
    // engines::IndexType, None for vertex arrays
    u32 index_type;
    // First index or vertex of the draw
    u32 first;
    i32 base_vertex;
    // Lowest vertex id of the draw: the capture slot of a vertex is its id
    // minus this
    u32 min_vertex;
    u32 vertex_count;
};

// Per draw parameter of the capturing vertex shader
struct CaptureParams {
    u32 min_vertex;
};

// Vertices per input primitive, 0 if a geometry shader cannot take it
inline u32 getInputVertexCount(engines::PrimitiveType primitive) {
    switch (primitive) {
    case engines::PrimitiveType::Points:
        return 1;
    case engines::PrimitiveType::Lines:
    case engines::PrimitiveType::LineLoop:
    case engines::PrimitiveType::LineStrip:
        return 2;
    case engines::PrimitiveType::Triangles:
    case engines::PrimitiveType::TriangleStrip:
    case engines::PrimitiveType::TriangleFan:
        return 3;
    case engines::PrimitiveType::LinesAdjacency:
    case engines::PrimitiveType::LineStripAdjacency:
        return 4;
    case engines::PrimitiveType::TrianglesAdjacency:
    case engines::PrimitiveType::TriangleStripAdjacency:
        return 6;
    default:
        return 0;
    }
}

// Input primitives a draw of `count` vertices assembles
inline u32 getPrimitiveCount(engines::PrimitiveType primitive, u32 count) {
    const auto at_least = [&](u32 minimum, u32 value) {
        return count >= minimum ? value : 0u;
    };
    switch (primitive) {
    case engines::PrimitiveType::Points:
        return count;
    case engines::PrimitiveType::Lines:
        return count / 2;
    case engines::PrimitiveType::LineLoop:
        return at_least(2, count);
    case engines::PrimitiveType::LineStrip:
        return at_least(2, count - 1);
    case engines::PrimitiveType::Triangles:
        return count / 3;
    case engines::PrimitiveType::TriangleStrip:
    case engines::PrimitiveType::TriangleFan:
        return at_least(3, count - 2);
    case engines::PrimitiveType::LinesAdjacency:
        return count / 4;
    case engines::PrimitiveType::LineStripAdjacency:
        return at_least(4, count - 3);
    case engines::PrimitiveType::TrianglesAdjacency:
        return count / 6;
    case engines::PrimitiveType::TriangleStripAdjacency:
        return at_least(6, (count - 4) / 2);
    default:
        return 0;
    }
}

// Index entries one invocation writes, for the most primitives it can emit
inline u32 getMaxOutputIndices(Output output, u32 max_vertices) {
    switch (output) {
    case Output::Points:
        return max_vertices;
    case Output::LineStrip:
        return max_vertices >= 2 ? (max_vertices - 1) * 2 : 0;
    case Output::TriangleStrip:
        return max_vertices >= 3 ? (max_vertices - 2) * 3 : 0;
    default:
        return 0;
    }
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::geometry
