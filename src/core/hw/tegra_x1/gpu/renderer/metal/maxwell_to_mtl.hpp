#pragma once

#include "core/hw/tegra_x1/gpu/const.hpp"
#include "core/hw/tegra_x1/gpu/renderer/metal/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::metal {

struct PixelFormatInfo {
    MTL::PixelFormat pixel_format;
    bool has_depth{false};
    bool has_stencil{false};
    // Host channel of each guest component (R, G, B, A)
    simd::uchar4 component_indices;
    // TODO: format decoder
};

const PixelFormatInfo& toMtlPixelFormatInfo(TextureFormat format);
inline MTL::PixelFormat toMtlPixelFormat(TextureFormat format) {
    return toMtlPixelFormatInfo(format).pixel_format;
}

MTL::CullMode toMtlCullMode(const engines::CullFaceMode mode);
MTL::Winding toMtlWinding(const engines::Winding winding);

MTL::PrimitiveType
toMtlPrimitiveType(const engines::PrimitiveType primitive_type);
MTL::IndexType toMtlIndexType(engines::IndexType index_type);

MTL::VertexFormat toMtlVertexFormat(engines::VertexAttribType type,
                                    engines::VertexAttribSize size, bool bgra);

MTL::CompareFunction toMtlCompareFunc(engines::CompareOp depth_compare_op);

MTL::TextureSwizzle toMtlSwizzle(const ImageSwizzle swizzle);

MTL::SamplerMinMagFilter toMtlSamplerMinMagFilter(const SamplerFilter filter);
MTL::SamplerAddressMode
toMtlSamplerAddressMode(const SamplerAddressMode address_mode);

} // namespace hydra::hw::tegra_x1::gpu::renderer::metal
