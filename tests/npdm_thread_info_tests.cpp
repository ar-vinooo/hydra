#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "common/common.hpp"
#include "core/horizon/loader/npdm.hpp"
#include "ztd/io/memory_stream.hpp"

using namespace hydra;
using namespace hydra::horizon::loader;

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// An NPDM with the ACI0 at 0x80 and its kernel capabilities at ACI0 + 0x40
std::vector<u8> makeNpdm(u32 aci_magic, const std::vector<u32>& caps,
                         NpdmMeta& out_meta) {
    constexpr u32 ACI_OFFSET = 0x80;
    constexpr u32 CAPS_OFFSET = 0x40;
    std::vector<u8> data(ACI_OFFSET + CAPS_OFFSET + caps.size() * 4);
    out_meta = {};
    out_meta.magic = 0x4154454d; // "META"
    out_meta.aci_offset = ACI_OFFSET;
    out_meta.aci_size = static_cast<u32>(CAPS_OFFSET + caps.size() * 4);
    std::memcpy(data.data(), &out_meta, sizeof(out_meta));
    std::memcpy(data.data() + ACI_OFFSET, &aci_magic, 4);
    const u32 caps_size = static_cast<u32>(caps.size() * 4);
    std::memcpy(data.data() + ACI_OFFSET + 0x30, &CAPS_OFFSET, 4);
    std::memcpy(data.data() + ACI_OFFSET + 0x34, &caps_size, 4);
    std::memcpy(data.data() + ACI_OFFSET + CAPS_OFFSET, caps.data(), caps_size);
    return data;
}

constexpr u32 threadInfo(u32 lowest, u32 highest, u32 min_core,
                         u32 max_core) {
    return 0b0111 | (lowest << 4) | (highest << 10) | (min_core << 16) |
           (max_core << 24);
}

} // namespace

int main() {
    constexpr u32 ACI0 = 0x30494341;
    // A syscall mask descriptor (0b1111) comes first, as in games
    {
        NpdmMeta meta;
        auto data = makeNpdm(ACI0, {0x0000ffff, threadInfo(59, 28, 0, 2)}, meta);
        ztd::io::MemoryStream stream(data);
        const auto info = readNpdmThreadInfo(stream, meta);
        check(info.has_value(), "ThreadInfo is found");
        check(info->core_mask == 0b111, "Cores 0-2 make mask 0b111");
        check(info->highest_priority == 28 && info->lowest_priority == 59,
              "Priority range is decoded");
    }
    {
        NpdmMeta meta;
        auto data = makeNpdm(ACI0, {threadInfo(63, 0, 3, 3)}, meta);
        ztd::io::MemoryStream stream(data);
        const auto info = readNpdmThreadInfo(stream, meta);
        check(info && info->core_mask == 0b1000, "A single core is decoded");
    }
    {
        NpdmMeta meta;
        auto data = makeNpdm(0x12345678, {threadInfo(59, 28, 0, 2)}, meta);
        ztd::io::MemoryStream stream(data);
        check(!readNpdmThreadInfo(stream, meta), "A bad ACI0 magic is rejected");
    }
    {
        NpdmMeta meta;
        auto data = makeNpdm(ACI0, {threadInfo(28, 59, 0, 2)}, meta);
        ztd::io::MemoryStream stream(data);
        check(!readNpdmThreadInfo(stream, meta),
              "An inverted priority range is rejected");
    }
    {
        NpdmMeta meta;
        auto data = makeNpdm(ACI0, {threadInfo(59, 28, 0, 4)}, meta);
        ztd::io::MemoryStream stream(data);
        check(!readNpdmThreadInfo(stream, meta), "Core 4 is rejected");
    }
    {
        NpdmMeta meta;
        auto data = makeNpdm(ACI0, {threadInfo(59, 28, 0, 2)}, meta);
        meta.aci_size = 0x1000; // Past the end of the file
        ztd::io::MemoryStream stream(data);
        check(!readNpdmThreadInfo(stream, meta), "A truncated ACI0 is rejected");
    }

    std::cout << "PASS: NPDM ThreadInfo cores, priorities and malformed "
                 "capabilities\n";
    return 0;
}
