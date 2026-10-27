#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

// Diagnostic log of what changes the contents of cached textures, one line
// per event, written to the file named by HYDRA_TEXTURE_EVENTS. It is off
// when the variable is unset ("off" also disables it). Every kind of event is
// reported for
// the 1st, 2nd, 4th, 8th... time it hits an address, with the count, so a
// texture that is rewritten again and again stands out without flooding.
inline const char* textureEventsPath() {
    static const char* const path = [] {
        const char* value = std::getenv("HYDRA_TEXTURE_EVENTS");
        return value != nullptr && *value != '\0' &&
                       std::string_view(value) != "off"
                   ? value
                   : nullptr;
    }();
    return path;
}

inline void logTextureEvent(const char* kind,
                            const TextureDescriptor& descriptor,
                            const std::string& detail = {}) {
    const char* const env = textureEventsPath();
    if (env == nullptr)
        return;

    static std::mutex mutex;
    static std::ofstream file;
    static bool opened = false;
    static bool disabled = false;
    static const auto start = std::chrono::steady_clock::now();
    static std::map<std::pair<std::string, uptr>, u64> counts;

    std::scoped_lock lock(mutex);
    if (disabled)
        return;
    if (!opened) {
        opened = true;
        const std::filesystem::path path(env);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        file.open(path, std::ios::app);
        if (!file) {
            disabled = true;
            return;
        }
        file << "# session start\n";
    }
    const auto count = ++counts[{kind, descriptor.ptr}];
    if ((count & (count - 1)) != 0)
        return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    file << fmt::format("{} {} n={} ptr={:#x} {} {} {}x{}x{} lv{} ly{} "
                        "size={:#x} {}\n",
                        ms, kind, count, descriptor.ptr, descriptor.type,
                        descriptor.format, descriptor.width, descriptor.height,
                        descriptor.depth, descriptor.level_count,
                        descriptor.layer_count, descriptor.size, detail)
         << std::flush;
}

} // namespace hydra::hw::tegra_x1::gpu::renderer

// The detail arguments are only evaluated when the log is on: they are
// formatted on texture hot paths
#define LOG_TEXTURE_EVENT(kind, descriptor, ...)                               \
    do {                                                                       \
        if (::hydra::hw::tegra_x1::gpu::renderer::textureEventsPath() !=       \
            nullptr)                                                           \
            ::hydra::hw::tegra_x1::gpu::renderer::logTextureEvent(             \
                kind, descriptor, ##__VA_ARGS__);                              \
    } while (0)
