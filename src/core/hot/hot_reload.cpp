#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "core/hot/table.hpp"

// Hot reloading (HYDRA_HOT_RELOAD=0 turns it off and removes its per-call
// overhead): with HYDRA_HOT_MODULE=<path to the hydra-hot module>, the
// module is loaded whenever it changes (e.g. after `ninja hydra-hot`). Old
// modules stay loaded, threads may still be running their code.
namespace hydra::hot {

namespace detail {
std::atomic<bool> pinning{false};
std::atomic<const Table*> latest_table{nullptr};
} // namespace detail

namespace {

auto& crnt_table = detail::latest_table;

using register_fn_t = bool (*)(Table*, std::uint64_t, const char*);

thread_local const Table* pinned_table = nullptr;

// The watcher thread isn't a guest or debugger thread, the logger can't be
// used from it
template <typename... Args>
void report(fmt::format_string<Args...> format, Args&&... args) {
    std::fprintf(stderr, "[hydra hot reload] %s\n",
                 fmt::format(format, std::forward<Args>(args)...).c_str());
}

const Table* builtinTable() {
    static const Table table = [] {
        Table t{};
        fillTable(t);
        return t;
    }();
    return &table;
}

void status(const std::filesystem::path& path, const char* state,
            u64 revision) {
    const auto temporary = path.string() + ".status.tmp";
    {
        std::ofstream out(temporary);
        out << "pid=" << getpid() << "\nabi=" << getAbi()
            << "\nrevision=" << revision << "\nstate=" << state << '\n';
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path.string() + ".status", ec);
}

bool loadModule(const std::filesystem::path& path, u64 generation) {
    // dlopen returns the already loaded image for a known path, load a copy
    std::error_code ec;
    const auto dir = path.parent_path() / ".hot";
    std::filesystem::create_directories(dir, ec);
    const auto copy = dir / fmt::format("{}.{}.{}{}", path.stem().string(),
                                        getpid(), generation,
                                        path.extension().string());
    std::filesystem::copy_file(
        path, copy, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        report("Hot reload: failed to copy {}: {}", path.string(),
               ec.message());
        return false;
    }

    void* handle = dlopen(copy.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        report("Hot reload: {}", dlerror());
        return false;
    }
    auto register_fn =
        reinterpret_cast<register_fn_t>(dlsym(handle, "hydra_hot_register"));
    auto* table = new Table{};
    if (register_fn == nullptr ||
        !register_fn(table, sizeof(Table), getAbi())) {
        report("Hot reload: module {} doesn't match this build, rebuild "
               "Hydra",
               copy.string());
        delete table;
        dlclose(handle);
        return false;
    }
    table->revision = generation;
    // From now on there are several images: scopes pin theirs
    detail::pinning.store(true, std::memory_order_release);
    crnt_table.store(table, std::memory_order_release);
    report("Hot reload: loaded module #{} ({})", generation, path.string());
    return true;
}

void watch(std::filesystem::path path) {
    // A module built before the start can be stale, only load new builds
    std::error_code start_ec;
    std::filesystem::file_time_type loaded_time =
        std::filesystem::last_write_time(path, start_ec);
    std::filesystem::file_time_type seen_time = loaded_time;
    u64 generation = 0;
    std::vector<const Table*> history;
    status(path, "watching", 0);
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::error_code ec;
        if (std::filesystem::remove(path.string() + ".rollback", ec)) {
            if (!history.empty()) {
                auto* table = new Table(*history.back());
                history.pop_back();
                table->revision = ++generation;
                crnt_table.store(table, std::memory_order_release);
                report("Hot reload: rolled back at revision {}", generation);
                status(path, "rolled_back", generation);
            } else {
                report("Hot reload: no previous module to restore");
            }
        }
        ec.clear();
        const bool requested =
            std::filesystem::remove(path.string() + ".reload", ec);
        const auto time = std::filesystem::last_write_time(path, ec);
        if (ec || (!requested && time == loaded_time))
            continue;
        // Wait for the linker to finish writing
        if (!requested && time != seen_time) {
            seen_time = time;
            continue;
        }
        loaded_time = time;
        const auto* previous = crnt_table.load(std::memory_order_acquire);
        if (loadModule(path, ++generation)) {
            history.push_back(previous);
            status(path, "loaded", generation);
        } else {
            status(path, "rejected", previous->revision);
        }
    }
}

} // namespace

namespace {

const Table& getLatestTable() {
    const Table* table = crnt_table.load(std::memory_order_acquire);
    if (table != nullptr) [[likely]]
        return *table;

    static std::once_flag once;
    std::call_once(once, [] {
        const Table* builtin = builtinTable();
        const Table* expected = nullptr;
        crnt_table.compare_exchange_strong(expected, builtin);
        const char* disabled = std::getenv("HYDRA_HOT_RELOAD");
        // HYDRA_HOT_RELOAD=0: no module is ever loaded
        if (disabled != nullptr && std::strcmp(disabled, "0") == 0)
            return;
        const char* path = std::getenv("HYDRA_HOT_MODULE");
#ifdef HYDRA_DEFAULT_HOT_MODULE
        // Unit tests also link hydra-core. They must not watch the app's
        // module or overwrite its PID/revision status unless explicitly asked.
        if (path == nullptr && std::strcmp(getprogname(), "Hydra") == 0)
            path = HYDRA_DEFAULT_HOT_MODULE;
#endif
        if (path != nullptr && *path != '\0') {
            // First use can come from the shader cache's background worker,
            // which has no guest/debugger thread context.
            report("Hot reload: watching {}", path);
            std::thread(watch, std::filesystem::path(path)).detach();
        }
    });
    return *crnt_table.load(std::memory_order_acquire);
}

} // namespace

const Table& getTable() {
    if (pinned_table != nullptr)
        return *pinned_table;
    return getLatestTable();
}

namespace detail {
const Table& initialTable() { return getLatestTable(); }
} // namespace detail

void Scope::pin() {
    slot = &pinned_table;
    previous = *slot;
    current = previous != nullptr ? previous : &getLatestTable();
    *slot = current;
}
u64 generation() { return getTable().revision; }

} // namespace hydra::hot
