#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "common/config.hpp"
#include "core/hw/tegra_x1/gpu/renderer/const.hpp"

namespace hydra::hw::tegra_x1::gpu::renderer {

// Diagnostic log of what changes the contents of cached textures, one line
// per event, written to <app data>/texture_events.log (HYDRA_TEXTURE_EVENTS
// overrides the path, "off" disables it). Every kind of event is reported for
// the 1st, 2nd, 4th, 8th... time it hits an address, with the count, so a
// texture that is rewritten again and again stands out without flooding.
inline void logTextureEvent(const char* kind,
                            const TextureDescriptor& descriptor,
                            const std::string& detail = {}) {
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
        const char* env = std::getenv("HYDRA_TEXTURE_EVENTS");
        if (env != nullptr && std::string(env) == "off") {
            disabled = true;
            return;
        }
        const std::filesystem::path path =
            env != nullptr
                ? std::filesystem::path(env)
                : std::filesystem::path(CONFIG_INSTANCE.getAppDataPath()) /
                      "texture_events.log";
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
