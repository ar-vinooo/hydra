# Live graphics debugging

Configure and start the development build:

```sh
cmake -S . -B build/swiftui-release -G Ninja -DFRONTEND=SwiftUI -DMACOS_BUNDLE=ON -DCMAKE_BUILD_TYPE=Release -DHYDRA_HOT_RELOAD=ON -DHYDRA_HOT_TESTS=ON
python3 tools/hot_debug.py run
```

The app opens in the library. Start and control the game manually. The watcher
rebuilds only `hydra-hot` after a supported implementation changes; it never
stops, launches, or sends input to the guest game. Session files default to
`<build-dir>/hot-debug`: `run.log`, `override/`, `trace/`, and optional `shaders/`.
Use `--session /path` to choose another directory. Generated shader dumps require
`run --dump-shaders` and add disk I/O. Use `run --perf-stats` to log
aggregate frame, CPU encoding, GPU, upload and compilation timings every two
seconds. This does not enable per-draw GPU waits or frame captures. Guest polling
and mapping diagnostics are opt-in via `HYDRA_TRACE_SLEEP` and `HYDRA_TRACE_SVC`.

A build configured with hot reload also watches its module when opened normally
from Finder. Its default shader override directory is the same session directory.
Attach the source watcher without restarting that app:

```sh
python3 tools/hot_debug.py watch
```

## Runtime coverage

`hot_sources.txt` in the build directory lists the C++ files actually compiled
into the live module. The watcher reads this manifest, rather than maintaining a
separate list of watched renderer files. Entry points are in
`src/core/hot/routes.inc`; stable host trampolines are in `routes.cpp`.
Implementations stay in their original source files, with `HotN` suffixes.

Supported runtime paths include:

- Normalized draw command sealing, resource validation and consumption. Resource
  registry storage, retirement and mapping-capture TLS remain host-owned.
- 3D and compute engine method dispatch, shader/resource binding, draw/clear,
  viewport/scissor, blending, culling, depth bias and depth/stencil state.
- 2D copies, DMA and inline uploads.
- Guest shader decoding, IR analysis and MSL generation, for graphics and compute.
- Metal renderer calls, command encoding, texture and buffer copies, buffer/index
  cache operations, and shader-library compilation.
- Texture cache invalidation, upload, alias synchronization, writeback and view
  creation.
- Pipeline, shader, render-pass, sampler, depth-state and clear/blit pipeline
  creation. New GPU objects use the loaded module's implementations.

Each engine operation pins one immutable dispatch table, including nested calls,
so an in-flight draw does not mix old and new code. Old images stay loaded for
in-flight calls, object vtables and Metal completion callbacks. Code-derived
cache keys include the module revision: shader, pipeline, sampler, depth-state
and texture-view changes take effect without reusing the previous objects.
Guest registers, memory, GPU texture contents and the game process survive reload.
Recompiling gameplay shaders can cause a temporary frame-rate drop.

This does not make arbitrary C++ layouts interchangeable. Header/dispatch-table
changes are blocked by the watcher and rejected by the runtime ABI check.
Changes to class layout, boot-only initialization, CPU/hypervisor code, operating
system services, persistent worker loops and frontend code still require a full
build/restart. Editing constructor code affects newly created objects; it cannot
retroactively migrate existing object state. Installing this expanded host for
the first time requires one app restart. Subsequent supported implementation
changes do not. Old module images and revision-specific objects consume memory
until the debugging session exits.

## Status, manual reload and rollback

```sh
python3 tools/hot_debug.py status
python3 tools/hot_debug.py build
python3 tools/hot_debug.py reload
python3 tools/hot_debug.py rollback
```

`status` reports the host PID, ABI, applied revision and last load result from
`libhydra-hot.so.status`. The runtime only reports `loaded` after registration
succeeds. Failed builds do not replace the module. Failed registration/dlopen
keeps the last working table. `rollback` restores the previous table with a new
cache revision. It rolls back code, not game memory or guest-visible side effects.
The running game is not restarted by these commands.

For a shader-only edit, copy `shaders/gpu_0x<address>_<type>.metal` into
`override/0x<address>_<type>.metal`. The watcher requests reload after additions,
edits and removals. Deleting an override restores generated MSL. Keep bindings
and entry points compatible and check Metal compiler errors in `run.log`.

## Verification

```sh
python3 tests/hot_debug_tests.py
cmake --build build/swiftui-release --target hydra-hot hydra-hot-reload-tests hydra-hot-fixture-a hydra-hot-fixture-b hydra-hot-fixture-rejected -j 4
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-hot-reload-tests \
  build/swiftui-release/libhydra-hot.so \
  build/swiftui-release/libhydra-hot-fixture-a.so \
  build/swiftui-release/libhydra-hot-fixture-b.so \
  build/swiftui-release/libhydra-hot-fixture-rejected.so
```

The integration test loads the production module in a single host process,
executes generated shaders on Metal, changes shader pixels with two fixture
modules, checks pinned calls and cache revisions, rejects an incompatible module,
and checks that rollback restores the prior GPU output. It does not control a
game. Crash 4's specific graphics artifacts still need visual verification and
renderer fixes; hot reload itself is debugging infrastructure.

## Optional frame tracing

```sh
python3 tools/hot_debug.py trace --trace-options 'textures gbuffer=1 post=1'
```

Tracing requires `run` (or an explicit `HYDRA_TRACE_DIR` environment variable).
Frame dumps can be large and temporarily reduce FPS. Each trigger resets capture
options. Image dumps share a budget of 24 per capture, configurable up to 64 with
`dumps=N`. Keep heavy captures disabled during ordinary live debugging.

## Structured visual diagnostics

See [README-visual-debug.md](README-visual-debug.md) for correlated captures,
bounded draw isolation, temporary shader/state overrides and evidence reports.
The initial host integration requires a full build/restart; later diagnostic
requests use the existing live gameplay session.

## Metal GPU capture (.gputrace)

```sh
python3 tools/hot_debug.py run --gpu-capture   # enables MTL_CAPTURE_ENABLED=1
python3 tools/hot_debug.py gpucapture --frames 3
```

The renderer starts/stops the capture on the GPU thread between command
buffers (stopping from the presentation thread crashed Metal's capture layer).
Captures land in `<session>/gputrace/`; open them in Xcode or with
`gpudebug -t <trace>`. While capturing, each draw is wrapped in a debug group
`d<N> vs 0x<guest VS> fs 0x<guest FS> n=<count> rt0=0x<RT0 address>`.
`--gpu-capture` makes Metal retain every resource; on 8 GB machines it can
swap heavily, so leave it off for normal play.

Work lifecycle/admission dan callback submit Metal tetap dimiliki host (`core/execution` dan `gpu/management`); module memakai simbol host. Perubahan layout ownership memerlukan rebuild aplikasi + module dan restart aplikasi. Lihat [execution management](../docs/graphics-handoff/execution-management-2026-10-20.md) untuk tes budget, cancellation dan retirement.
