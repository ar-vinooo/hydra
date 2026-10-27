#include "core/hw/tegra_x1/gpu/renderer/shader_decompiler/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp {

SvAccess getSvAccessFromAddr(u64 addr) {
    ASSERT_ALIGNMENT_DEBUG(addr, 4, ShaderDecompiler, "Address");

    const auto component = static_cast<u8>((addr >> 2) & 0x3);
    const auto vec4 = [&](SvSemantic semantic, u64 base) {
        return SvAccess(Sv(semantic, static_cast<u8>((addr - base) >> 4)),
                        component);
    };
    const auto scalar = [](SvSemantic semantic) {
        return SvAccess(Sv(semantic), 0);
    };

    if (addr >= SV_USER_IN_OUT_BASE && addr < SV_USER_IN_OUT_END)
        return vec4(SvSemantic::UserInOut, SV_USER_IN_OUT_BASE);
    if (addr >= SV_POSITION_BASE && addr < SV_USER_IN_OUT_BASE)
        return vec4(SvSemantic::Position, SV_POSITION_BASE);
    if (addr >= SV_CLIP_DISTANCE_BASE &&
        addr < SV_CLIP_DISTANCE_BASE + CLIP_DISTANCE_COUNT * 4)
        return vec4(SvSemantic::ClipDistance, SV_CLIP_DISTANCE_BASE);
    if (addr >= SV_POINT_COORD_BASE && addr < SV_POINT_COORD_BASE + 8)
        return SvAccess(Sv(SvSemantic::PointCoord), component);
    if (addr >= SV_TESS_COORD_BASE && addr < SV_TESS_COORD_BASE + 8)
        return SvAccess(Sv(SvSemantic::TessCoord), component);

    switch (addr) {
    case SV_PRIMITIVE_ID_BASE:
        return scalar(SvSemantic::PrimitiveID);
    case SV_LAYER_BASE:
        return scalar(SvSemantic::Layer);
    case SV_VIEWPORT_INDEX_BASE:
        return scalar(SvSemantic::ViewportIndex);
    case SV_POINT_SIZE_BASE:
        return scalar(SvSemantic::PointSize);
    case SV_INSTANCE_ID_BASE:
        return scalar(SvSemantic::InstanceID);
    case SV_VERTEX_ID_BASE:
        return scalar(SvSemantic::VertexID);
    case SV_FRONT_FACING_BASE:
        return scalar(SvSemantic::FrontFacing);
    default:
        break;
    }

    SHADER_UNSUPPORTED("SV address 0x{:02x}", addr);

    return {Sv(SvSemantic::Invalid), invalid<u8>()};
}

} // namespace hydra::hw::tegra_x1::gpu::renderer::shader_decomp
