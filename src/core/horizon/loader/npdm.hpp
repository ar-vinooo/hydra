#pragma once

#include <optional>

#include "ztd/io/stream.hpp"

namespace hydra::horizon::loader {

enum class NpdmFlags : u8 {
    None = 0,
    Is64BitInstruction = ZTD_BIT(0),
    AddressSpace32Bit = 0x0 << 1,
    AddressSpace64BitOld = 0x1 << 1,
    AddressSpace32BitNoReserved = 0x2 << 1,
    AddressSpace64Bit = 0x3 << 1,
    OptimizeMemoryAllocation = ZTD_BIT(4),       // 7.0.0+
    DisableDeviceAddressSpaceMerge = ZTD_BIT(5), // 11.0.0+
    EnableAliasRegionExtraSize = ZTD_BIT(6),     // 18.0.0+
    PreventCodeReads = ZTD_BIT(7),               // 19.0.0+
};
ZTD_ENABLE_ENUM_BITWISE_OPERATORS(NpdmFlags)

struct NpdmMeta {
    u32 magic;
    u32 signature_key_generation; // 9.0.0+
    u32 _reserved_x8;
    NpdmFlags flags;
    u8 _reserved_xd;
    u8 main_thread_priority;
    u8 main_thread_core_number;
    u32 _reserved_x10;
    u32 system_resource_size; // 3.0.0+
    u32 version;
    u32 main_thread_stack_size;
    char name[0x10];
    u8 product_code[0x10];
    u8 _reserved_x40[0x30];
    u32 aci_offset;
    u32 aci_size;
    u32 acid_offset;
    u32 acid_size;
};

// ThreadInfo kernel capability: the cores and priorities a process may use
struct NpdmThreadInfo {
    u64 core_mask;
    u8 highest_priority; // Numerically lowest
    u8 lowest_priority;
};

// Reads the ThreadInfo capability from the ACI0 of an NPDM. Layouts from
// switchbrew (NPDM, Kernel capabilities): ACI0 holds the kernel capability
// offset and size at 0x30; a ThreadInfo descriptor ends in 0b0111 and holds
// the lowest priority in bits 4-9, the highest in 10-15 and the core range
// in 16-23 and 24-31.
inline std::optional<NpdmThreadInfo>
readNpdmThreadInfo(ztd::io::IStream& stream, const NpdmMeta& meta) {
    constexpr u32 ACI0_MAGIC = 0x30494341; // "ACI0"
    constexpr u64 ACI0_KERNEL_CAPS_OFFSET = 0x30;
    const u64 size = stream.getSize();
    if (meta.aci_size < ACI0_KERNEL_CAPS_OFFSET + 8 ||
        meta.aci_offset + u64(meta.aci_size) > size)
        return std::nullopt;
    stream.seekTo(meta.aci_offset);
    if (stream.read<u32>() != ACI0_MAGIC)
        return std::nullopt;
    stream.seekTo(meta.aci_offset + ACI0_KERNEL_CAPS_OFFSET);
    const u32 caps_offset = stream.read<u32>();
    const u32 caps_size = stream.read<u32>();
    if (u64(caps_offset) + caps_size > meta.aci_size)
        return std::nullopt;
    stream.seekTo(meta.aci_offset + caps_offset);
    for (u32 i = 0; i < caps_size / sizeof(u32); i++) {
        const u32 descriptor = stream.read<u32>();
        if ((descriptor & 0xf) != 0b0111)
            continue;
        const u32 lowest_priority = (descriptor >> 4) & 0x3f;
        const u32 highest_priority = (descriptor >> 10) & 0x3f;
        const u32 min_core = (descriptor >> 16) & 0xff;
        const u32 max_core = (descriptor >> 24) & 0xff;
        if (highest_priority > lowest_priority || min_core > max_core ||
            max_core >= 4)
            return std::nullopt;
        u64 core_mask = 0;
        for (u32 core = min_core; core <= max_core; core++)
            core_mask |= 1ull << core;
        return NpdmThreadInfo{core_mask, static_cast<u8>(highest_priority),
                              static_cast<u8>(lowest_priority)};
    }
    return std::nullopt;
}

} // namespace hydra::horizon::loader
