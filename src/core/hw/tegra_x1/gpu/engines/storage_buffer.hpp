#pragma once

#include "core/hw/tegra_x1/gpu/gpu.hpp"
#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::engines {

// Resolves the NVN storage buffer descriptor ({u64 GPU address, u32 size}) at
// `byte_offset` of a bound constant buffer to host memory. The range is cut
// to the mapping that holds its start.
inline std::optional<ztd::Range<uptr>>
resolveStorageBuffer(ztd::Range<uptr> const_buffer, u32 byte_offset) {
    if (const_buffer.getBegin() == 0 ||
        u64(byte_offset) + 12 > const_buffer.getSize())
        return std::nullopt;

    u64 address;
    u32 size;
    std::memcpy(&address,
                reinterpret_cast<const void*>(const_buffer.getBegin() +
                                              byte_offset),
                sizeof(address));
    std::memcpy(&size,
                reinterpret_cast<const void*>(const_buffer.getBegin() +
                                              byte_offset + 8),
                sizeof(size));
    if (address == 0 || size == 0)
        return std::nullopt;

    uptr base;
    const auto as = tls_crnt_gmmu->findAddr(address, base);
    if (!as || as->ptr == 0)
        return std::nullopt;
    const u64 delta = address - base;
    return ztd::Range<uptr>::fromSize(
        as->ptr + delta, std::min<u64>(size, as->size - delta));
}

} // namespace hydra::hw::tegra_x1::gpu::engines
