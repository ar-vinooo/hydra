#pragma once

#include <bit>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace hydra::video {
// Host1x CDMA packets address methods in words. Engine classes use method
// 0x10 to select a register and 0x11 to write it. State crosses command buffers.
struct Host1xState {
    std::uint32_t class_id = 0;
    std::uint32_t engine_method = 0;
};

template <typename Write>
std::expected<void, std::string>
interpretHost1x(std::span<const std::uint32_t> words, Host1xState& state, Write write) {
    std::size_t cursor = 0;
    auto method = [&](std::uint32_t offset, std::uint32_t value) {
        if (state.class_id != 0xf0 && state.class_id != 0x5d)
            return; // Host1x control/synchronization class, not an engine register.
        if (offset == 0x10)
            state.engine_method = value;
        else if (offset == 0x11)
            write(state.engine_method, value);
    };
    while (cursor < words.size()) {
        const auto header = words[cursor++];
        const auto opcode = header >> 28;
        const auto offset = (header >> 16) & 0xfff;
        const auto count = header & 0xffff;
        if (opcode == 0) {
            state.class_id = (header >> 6) & 0x3ff;
            const auto mask = header & 0x3f;
            if (words.size() - cursor < std::size_t(std::popcount(mask)))
                return std::unexpected("Truncated Host1x SETCLASS packet");
            for (std::uint32_t bit = 0; bit < 6; ++bit)
                if (mask & (1u << bit))
                    method(offset + bit, words[cursor++]);
        } else if (opcode == 1 || opcode == 2) {
            if (words.size() - cursor < count)
                return std::unexpected("Truncated Host1x method packet");
            for (std::uint32_t i = 0; i < count; ++i)
                method(offset + (opcode == 1 ? i : 0), words[cursor++]);
        } else if (opcode == 3) {
            if (words.size() - cursor < std::size_t(std::popcount(count)))
                return std::unexpected("Truncated Host1x MASK packet");
            for (std::uint32_t bit = 0; bit < 16; ++bit)
                if (count & (1u << bit))
                    method(offset + bit, words[cursor++]);
        } else if (opcode == 4) {
            method(offset, count);
        } else {
            return std::unexpected("Unsupported Host1x packet opcode " + std::to_string(opcode));
        }
    }
    return {};
}
} // namespace hydra::video
