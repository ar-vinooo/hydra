#pragma once

#include "core/hw/tegra_x1/gpu/renderer/geometry.hpp"

// Tessellation emulation. Metal tessellates with factors a compute (here: a
// rasterization-off vertex function) writes, and runs the post-tessellation
// vertex function per generated vertex. A draw of patches is four draws in
// the same render pass, ordered by buffer barriers:
// 1. The vertex shader writes its output records (geometry.hpp capture).
// 2. The control shader, rasterization off, runs per patch and output control
//    point: it reads the input control points' records, writes its own record
//    to the control point buffer and per patch attributes straight to the
//    patch buffer. Without a control shader a generated function copies the
//    input control points.
// 3. A generated function converts each patch's levels (patch words 0-5, or
//    the fixed function registers) to Metal tessellation factors.
// 4. drawPatches runs the evaluation shader as the post-tessellation vertex
//    function, which reads the control points and patch attributes by patch
//    id, and the fragment shader.
namespace hydra::hw::tegra_x1::gpu::renderer::tessellation {

// Per patch attribute words (yuzu's IR::Patch): levels outer 0-3 (left, top,
// right, bottom), inner 0-1, padding, then the generic components
constexpr u32 PATCH_WORDS = 128;
constexpr u32 PATCH_SIZE = PATCH_WORDS * 4;
constexpr u32 LEVEL_WORDS = 6;

// Buffer slots (no vertex buffers in these functions). The control shader
// reads input records from geometry::INPUT_BUFFER and indices from
// geometry::INDEX_DATA_BUFFER, with parameters in geometry::PARAMS_BUFFER.
constexpr u32 CONTROL_POINT_BUFFER = geometry::OUTPUT_BUFFER;
constexpr u32 PATCH_BUFFER = geometry::OUTPUT_INDEX_BUFFER;
constexpr u32 FACTOR_BUFFER = 25;
// The evaluation shader reads the control points here
constexpr u32 EVALUATION_CONTROL_POINT_BUFFER = geometry::INPUT_BUFFER;

// SET_TESSELLATION_PARAMETERS (clb197.h)
enum class Domain : u32 {
    Isoline = 0,
    Triangle = 1,
    Quad = 2,
};
enum class Spacing : u32 {
    Integer = 0,
    FractionalOdd = 1,
    FractionalEven = 2,
};
enum class OutputPrimitives : u32 {
    Points = 0,
    Lines = 1,
    TrianglesCw = 2,
    TrianglesCcw = 3,
};
inline Domain getDomain(u32 parameters) {
    return static_cast<Domain>(parameters & 0x3);
}
inline Spacing getSpacing(u32 parameters) {
    return static_cast<Spacing>((parameters >> 4) & 0x3);
}
inline OutputPrimitives getOutputPrimitives(u32 parameters) {
    return static_cast<OutputPrimitives>((parameters >> 8) & 0x3);
}

// MTLTriangleTessellationFactorsHalf and MTLQuadTessellationFactorsHalf
inline u32 getFactorSize(Domain domain) {
    return domain == Domain::Quad ? 12 : 8;
}

// Metal tessellates triangles and quads into triangles only
inline bool isSupported(u32 parameters) {
    const auto domain = getDomain(parameters);
    const auto output = getOutputPrimitives(parameters);
    return (domain == Domain::Triangle || domain == Domain::Quad) &&
           (output == OutputPrimitives::TrianglesCw ||
            output == OutputPrimitives::TrianglesCcw);
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::tessellation
