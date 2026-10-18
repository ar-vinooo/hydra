#pragma once

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

// A draw/dispatch (and every nested hot call) stays on one immutable image.
// Old images remain loaded for in-flight CPU calls and Metal callbacks.
// The thread-local is reached once per scope: on macOS every access is a
// _tlv_get_addr call, and hot functions run for every draw. With
// HYDRA_HOT_RELOAD=0 there is only one image, so scopes skip the thread-local
// entirely (slot is null).
class Scope {
  public:
    Scope();
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
};
u64 generation();

} // namespace hydra::hot

#define HYDRA_HOT_CALL(object, name) ((object).*(hot_scope.table().name))
