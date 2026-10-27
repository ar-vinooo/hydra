#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace hydra::video {

struct CommandBuffer {
    std::uint32_t handle;
    std::uint32_t offset;
    std::uint32_t word_count;
};

struct Submission {
    std::vector<CommandBuffer> commands;
    std::uint32_t relocation_count;
    std::uint32_t syncpoint_count;
    std::uint32_t fence_count;
};

// Host1x submit ABI: 16-byte count header followed by command descriptors,
// relocations, relocation shifts, syncpoint increments and fence thresholds.
// Decode words explicitly so malformed or unaligned ioctl buffers are safe.
inline std::expected<Submission, std::string>
parseSubmission(std::span<const std::uint8_t> bytes) {
    constexpr std::size_t MAX_INPUT_BYTES = 4 * 1024 * 1024;
    if (bytes.size() < 16 || bytes.size() > MAX_INPUT_BYTES)
        return std::unexpected("Invalid Host1x submission size");
    auto word = [&](std::size_t offset) {
        return std::uint32_t(bytes[offset]) |
               (std::uint32_t(bytes[offset + 1]) << 8) |
               (std::uint32_t(bytes[offset + 2]) << 16) |
               (std::uint32_t(bytes[offset + 3]) << 24);
    };
    const auto command_count = word(0);
    const auto relocation_count = word(4);
    const auto syncpoint_count = word(8);
    const auto fence_count = word(12);
    if (command_count > 1024 || relocation_count > 65536 ||
        syncpoint_count > 1024 || fence_count > 1024)
        return std::unexpected("Host1x submission counts exceed limits");
    const std::size_t required = 16 + std::size_t(command_count) * 12 +
        std::size_t(relocation_count) * 20 + std::size_t(syncpoint_count) * 20 +
        std::size_t(fence_count) * 4;
    if (bytes.size() < required)
        return std::unexpected("Truncated Host1x submission arrays");
    Submission result{.relocation_count = relocation_count,
                      .syncpoint_count = syncpoint_count,
                      .fence_count = fence_count};
    result.commands.reserve(command_count);
    for (std::size_t i = 0; i < command_count; ++i) {
        const auto offset = 16 + i * 12;
        const auto words = word(offset + 8);
        if (words > 1024 * 1024)
            return std::unexpected("Host1x command buffer exceeds limits");
        result.commands.push_back({word(offset), word(offset + 4), words});
    }
    return result;
}

} // namespace hydra::video
