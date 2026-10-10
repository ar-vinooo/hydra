#pragma once

#include "core/input/const.hpp"

namespace hydra::input {

using PlayerDevices = std::array<std::vector<std::string>, 8>;

// Each physical device belongs to one player. Copy the selection first so
// callers can also pass a span into an existing player's devices.
inline bool assignPlayerDevices(PlayerDevices& players, u32 player,
                                std::span<const std::string> devices) {
    if (player >= players.size())
        return false;
    std::vector<std::string> selected;
    for (const auto& device : devices)
        if (!device.empty() && std::find(selected.begin(), selected.end(),
                                         device) == selected.end())
            selected.push_back(device);
    for (auto& names : players)
        std::erase_if(names, [&](const auto& name) {
            return std::find(selected.begin(), selected.end(), name) !=
                   selected.end();
        });
    players[player] = std::move(selected);
    return true;
}

} // namespace hydra::input
