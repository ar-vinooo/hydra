#pragma once

#include <atomic>

#include "core/hot/route_headers.hpp"
#include "core/hw/tegra_x1/gpu/renderer/texture_cache.hpp"

// Hot reloading (HYDRA_HOT_RELOAD): functions listed here are called through
// a table that a rebuilt hydra-hot module replaces while the game runs. Their
// definitions are compiled both into hydra-core and into the module.
namespace hydra::hot {

using hw::tegra_x1::gpu::renderer::TextureCache;

#define HYDRA_HOT_FUNCTIONS(X)                                                 \
    X(texture_cache_invalidate_memory, &TextureCache::invalidateMemoryImpl)    \
    X(texture_cache_mark_gpu_writes_in_guest,                                  \
      &TextureCache::markGpuWritesInGuestImpl)                                 \
    X(texture_cache_update, &TextureCache::updateImpl)                         \
    X(texture_cache_flush_memory, &TextureCache::flushMemoryImpl)              \
    X(texture_cache_encode_texture, &TextureCache::encodeTextureImpl)

struct Table {
    u64 revision{0};
#define HYDRA_HOT_DECLARE(name, function) decltype(function) name;
    HYDRA_HOT_FUNCTIONS(HYDRA_HOT_DECLARE)
#undef HYDRA_HOT_DECLARE
#define HYDRA_HOT_ROUTE(name, method) decltype(method) name;
#include "core/hot/routes.inc"
#undef HYDRA_HOT_ROUTE
};

// Fills the table with the functions of the image it is compiled into
void fillTable(Table& table);

// Hash of the headers the image was built with
const char* getAbi();

// The functions of the latest loaded module, or the built-in ones
const Table& getTable();

namespace detail {
// Set once a module replaced the built-in functions. Until then there is only
// one image, and scopes need no pinning.
extern std::atomic<bool> pinning;
extern std::atomic<const Table*> latest_table;
const Table& initialTable();
} // namespace detail

// A draw/dispatch (and every nested hot call) stays on one immutable image.
// Old images remain loaded for in-flight CPU calls and Metal callbacks.
// Pinning uses a thread-local: on macOS every access is a _tlv_get_addr
// call, and scopes run for every register write. While no module was loaded
// (always, unless one is being developed) a scope is one atomic load.
class Scope {
  public:
    Scope() {
        if (!detail::pinning.load(std::memory_order_acquire)) [[likely]] {
            slot = nullptr;
            previous = nullptr;
            const auto* table =
                detail::latest_table.load(std::memory_order_acquire);
            current = table != nullptr ? table : &detail::initialTable();
            return;
        }
        pin();
    }
    ~Scope() {
        if (slot != nullptr)
            *slot = previous;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    const Table& table() const { return *current; }

  private:
    const Table** slot;
    const Table* previous;
    const Table* current;

    void pin();
};
u64 generation();

} // namespace hydra::hot

#define HYDRA_HOT_CALL(object, name) ((object).*(hot_scope.table().name))
