#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <simd/simd.h>

#pragma GCC diagnostic pop

namespace MTL {

constexpr std::string_view format_as(TextureType type) {
    switch (type) {
    case TextureType1D:
        return "1D";
    case TextureType1DArray:
        return "1D array";
    case TextureTypeTextureBuffer:
        return "1D buffer";
    case TextureType2D:
        return "2D";
    case TextureType2DArray:
        return "2D array";
    case TextureType3D:
        return "3D";
    case TextureTypeCube:
        return "cube";
    case TextureTypeCubeArray:
        return "cube array";
    default:
        return "unknown";
    }
}

// Metal's size and origin structs have no comparison operators
inline bool operator==(const Size& a, const Size& b) {
    return a.width == b.width && a.height == b.height && a.depth == b.depth;
}
inline bool operator!=(const Size& a, const Size& b) { return !(a == b); }
inline bool operator==(const Origin& a, const Origin& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
inline bool operator!=(const Origin& a, const Origin& b) { return !(a == b); }

} // namespace MTL
