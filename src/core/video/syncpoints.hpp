#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <functional>
#include <map>
#include <vector>

namespace hydra::video {
// Video channels own independent Host1x counters; ID zero stays reserved for
// the existing GPU path. Counters wrap exactly as the guest's 32-bit thresholds.
class Syncpoints {
  public:
    std::optional<std::uint32_t> allocate() {
        std::scoped_lock lock(mutex);
        if (next == values.size()) return std::nullopt;
        return next++;
    }
    std::optional<std::uint32_t> read(std::uint32_t id) {
        std::scoped_lock lock(mutex);
        if (!id || id >= next) return std::nullopt;
        return values[id];
    }
    std::optional<std::uint32_t> complete(std::uint32_t id, std::uint32_t increments) {
        std::unique_lock lock(mutex);
        if (!id || id >= next) return std::nullopt;
        values[id] += increments;
        const auto value = values[id];
        reached_cv.notify_all();
        std::vector<std::function<void()>> ready;
        for (auto it = waits.begin(); it != waits.end();) {
            if (it->second.id == id && reached(value, it->second.threshold)) {
                ready.push_back(std::move(it->second.callback));
                it = waits.erase(it);
            } else {
                ++it;
            }
        }
        lock.unlock();
        for (const auto& callback : ready) callback();
        return value;
    }
    std::optional<std::uint64_t> onReached(std::uint32_t id,
                                          std::uint32_t threshold,
                                          std::function<void()> callback) {
        std::unique_lock lock(mutex);
        if (!id || id >= next) return std::nullopt;
        if (reached(values[id], threshold)) {
            lock.unlock();
            callback();
            return 0;
        }
        const auto token = next_wait++;
        waits.emplace(token, Wait{id, threshold, std::move(callback)});
        return token;
    }
    // Blocks the calling thread until the counter reaches the threshold or
    // the time passes. Returns whether it was reached.
    bool waitFor(std::uint32_t id, std::uint32_t threshold,
                 std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        if (!id || id >= next) return false;
        return reached_cv.wait_for(lock, timeout, [&] {
            return reached(values[id], threshold);
        });
    }
    void cancelWait(std::uint64_t token) {
        std::scoped_lock lock(mutex);
        waits.erase(token);
    }
    static bool reached(std::uint32_t value, std::uint32_t threshold) {
        return std::uint32_t(value - threshold) < 0x80000000u;
    }
  private:
    std::mutex mutex;
    std::condition_variable reached_cv;
    std::array<std::uint32_t, 192> values{};
    std::uint32_t next = 1;
    struct Wait {
        std::uint32_t id, threshold;
        std::function<void()> callback;
    };
    std::map<std::uint64_t, Wait> waits;
    std::uint64_t next_wait = 1;
};
} // namespace hydra::video
