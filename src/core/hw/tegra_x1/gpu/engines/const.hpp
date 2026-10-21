#pragma once

#include "core/hw/tegra_x1/gpu/const.hpp"

#include <chrono>
#include <cstdlib>

namespace hydra::hw::tegra_x1::gpu::engines {

// GPU timestamp of semaphore releases: ticks at 614.4 MHz derived from the
// host clock
inline u64 getGpuTimestamp() {
    const u64 ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    return ns / 625 * 384;
}

// Reductions of the DMA semaphores (copy engine NVB0B5 and inline engine
// NVA140 REDUCTION_OP): IMIN, IMAX, IXOR, IAND, IOR, IADD, INC, DEC
inline u32 reduceDmaSemaphoreWord(u32 operation, bool is_signed, u32 old,
                                  u32 payload) {
    switch (operation) {
    case 0:
        return is_signed ? static_cast<u32>(std::min(static_cast<i32>(old),
                                                     static_cast<i32>(payload)))
                         : std::min(old, payload);
    case 1:
        return is_signed ? static_cast<u32>(std::max(static_cast<i32>(old),
                                                     static_cast<i32>(payload)))
                         : std::max(old, payload);
    case 2:
        return old ^ payload;
    case 3:
        return old & payload;
    case 4:
        return old | payload;
    case 6:
        return old >= payload ? 0u : old + 1;
    case 7:
        return (old == 0 || old > payload) ? payload : old - 1;
    default:
        return old + payload;
    }
}

struct Iova {
    u32 hi;
    u32 lo;

    // NOLINTNEXTLINE(cppcoreguidelines-explicit-constructor)
    operator u64() const {
        return static_cast<u64>(hi) << 32 | static_cast<u64>(lo);
    }
};

enum class Winding : u32 {
    Clockwise = 0x900,
    CounterClockwise = 0x901,
};

enum class CullFaceMode : u32 {
    Front = 0x404,
    Back = 0x405,
    FrontAndBack = 0x408,
};

enum class PrimitiveType : u32 {
    Points,
    Lines,
    LineLoop,
    LineStrip,
    Triangles,
    TriangleStrip,
    TriangleFan,
    Quads,
    QuadStrip,
    Polygon,
    LinesAdjacency,
    LineStripAdjacency,
    TrianglesAdjacency,
    TriangleStripAdjacency,
    Patches,
};

enum class IndexType : u32 {
    UInt8,
    UInt16,
    UInt32,
    None,
};

inline u32 getIndexTypeSize(IndexType type) {
    switch (type) {
    case IndexType::UInt8:
        return sizeof(u8);
    case IndexType::UInt16:
        return sizeof(u16);
    case IndexType::UInt32:
        return sizeof(u32);
    default:
        return 0;
    }
}

enum class VertexAttribType : u32 {
    None,
    Snorm,
    Unorm,
    Sint,
    Uint,
    Uscaled,
    Sscaled,
    Float,
};

enum class VertexAttribSize : u32 {
    // One to four 32-bit components
    _1x32 = 0x12,
    _2x32 = 0x04,
    _3x32 = 0x02,
    _4x32 = 0x01,

    // One to four 16-bit components
    _1x16 = 0x1b,
    _2x16 = 0x0f,
    _3x16 = 0x05,
    _4x16 = 0x03,

    // One to four 8-bit components
    _1x8 = 0x1d,
    _2x8 = 0x18,
    _3x8 = 0x13,
    _4x8 = 0x0a,

    // Misc arrangements
    _10_10_10_2 = 0x30,
    _11_11_10 = 0x31,

    // Arrangements fetched like the ones above (packed forms of the same
    // byte order)
    A8B8G8R8 = 0x2f,
    G8R8 = 0x32,
    X8B8G8R8 = 0x33,
    A8 = 0x34,
};

// The fetch size the arrangement is read as
inline VertexAttribSize getFetchSize(VertexAttribSize size) {
    // DEBUG: bisect switch, remove with the other HYDRA_NO_* switches
    static const bool no_a8b8g8r8 =
        std::getenv("HYDRA_NO_VERTEX_A8B8G8R8") != nullptr;
    if (no_a8b8g8r8 && size == VertexAttribSize::A8B8G8R8)
        return size;
    switch (size) {
    case VertexAttribSize::G8R8:
        return VertexAttribSize::_2x8;
    case VertexAttribSize::A8B8G8R8:
    case VertexAttribSize::X8B8G8R8:
        return VertexAttribSize::_4x8;
    case VertexAttribSize::A8:
        return VertexAttribSize::_1x8;
    default:
        return size;
    }
}

struct VertexAttribState {
    u32 buffer_id : 5;
    u32 padding : 1;
    bool is_fixed : 1;
    u32 offset : 14;
    VertexAttribSize size : 6;
    VertexAttribType type : 3;
    u32 padding2 : 1;
    bool bgra : 1; // Bit 31
};
static_assert(sizeof(VertexAttribState) == sizeof(u32));

// SWAP_R_AND_B applies to every attribute format. Metal only has a BGRA
// variant of uchar4 normalized, the shader swaps the others.
inline bool hostVertexFormatSwapsRb(const VertexAttribState& state) {
    return state.type == VertexAttribType::Unorm &&
           state.size == VertexAttribSize::_4x8;
}

enum class ShaderStage : u32 {
    VertexA,
    VertexB,
    TessCtrl,
    TessEval,
    Geometry,
    Fragment,

    Count,

    // Kepler compute, not part of the 3D pipeline
    Compute = 0x10,
};

enum class CompareOp : u32 {
    Invalid = 0,
    Never = 1,
    Less = 2,
    Equal = 3,
    LessEqual = 4,
    Greater = 5,
    NotEqual = 6,
    GreaterEqual = 7,
    Always = 8,
};

enum class TextureType : u32 {
    _1D = 0,
    _2D = 1,
    _3D = 2,
    Cubemap = 3,
    _1DArray = 4,
    _2DArray = 5,
    _1DBuffer = 6,
    _2DNoMipmap = 7,
    CubeArray = 8,
};

enum class MemoryLayout : u32 {
    BlockLinear,
    Pitch,
};

enum class BlockDim : u32 {
    OneGob = 0,
    TwoGobs = 1,
    FourGobs = 2,
    EightGobs = 3,
    SixteenGobs = 4,
    ThirtyTwoGobs = 5,
    QuarterGob = 14,
};

inline i32 getBlockSizeLog2(const BlockDim dim) {
    return signExtend<i32, 4>(static_cast<i32>(dim));
}

enum class ColorWriteMask : u32 {
    None = 0,
    Red = ZTD_BIT(0),
    Green = ZTD_BIT(4),
    Blue = ZTD_BIT(8),
    Alpha = ZTD_BIT(12),
    All = Red | Green | Blue | Alpha,
};
ZTD_ENABLE_ENUM_BITWISE_OPERATORS(ColorWriteMask)

} // namespace hydra::hw::tegra_x1::gpu::engines

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::engines::PrimitiveType, Points,
                       "points", Lines, "lines", LineLoop, "line loop",
                       LineStrip, "line strip", Triangles, "triangles",
                       TriangleStrip, "triangle strip", TriangleFan,
                       "triangle fan", Quads, "quads", QuadStrip, "quad strip",
                       Polygon, "polygon", LinesAdjacency, "lines adjacency",
                       LineStripAdjacency, "line strip adjacency",
                       TrianglesAdjacency, "triangles adjacency",
                       TriangleStripAdjacency, "triangle strip adjacency",
                       Patches, "patches")

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::engines::IndexType, None,
                       "none", UInt8, "u8", UInt16, "u16", UInt32, "u32")

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::engines::VertexAttribType,
                       None, "none", Snorm, "snorm", Unorm, "unorm", Sint,
                       "sint", Uint, "uint", Uscaled, "uscaled", Sscaled,
                       "sscaled", Float, "float")

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::engines::VertexAttribSize,
                       _1x32, "1x32", _2x32, "2x32", _3x32, "3x32", _4x32,
                       "4x32", _1x16, "1x16", _2x16, "2x16", _3x16, "3x16",
                       _4x16, "4x16", _1x8, "1x8", _2x8, "2x8", _3x8, "3x8",
                       _4x8, "4x8", _10_10_10_2, "10_10_10_2", _11_11_10,
                       "11_11_10")

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::engines::ShaderStage, VertexA,
                       "vertex A", VertexB, "vertex B", TessCtrl,
                       "tessellation control", TessEval,
                       "tessellation evaluation", Geometry, "geometry",
                       Fragment, "fragment", Compute, "compute")

ENABLE_ENUM_FORMATTING(hydra::hw::tegra_x1::gpu::engines::CompareOp, Invalid,
                       "invalid", Never, "never", Less, "less", Equal, "equal",
                       LessEqual, "less equal", Greater, "greater", NotEqual,
                       "not equal", GreaterEqual, "greater equal", Always,
                       "always")
