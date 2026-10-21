# Metal disk shader cache

Hydra keeps three kinds of shader data on disk so a title does not compile the
same shaders on every launch. All of it lives under Hydra's app data directory
(`~/Library/Application Support/Hydra/cache/metal/` on macOS):

| Data | Location | Key |
|---|---|---|
| Pipeline binaries (`.metallib`) | `v3-<GPU>-<OS hash>/` | SHA-256 of shader code, stage/backend/compiler options and explicit Metal pipeline state |
| MSL sources (`.metal`) | `titles/<title ID>/msl/` | SHA-256 of the source |
| Pipeline recipes (`.recipe`) | `titles/<title ID>/pipelines/` | Same key as the pipeline binary |

Guest addresses and struct padding are excluded from every key. Metal verifies
the binary with `PipelineOptionFailOnBinaryArchiveMiss` before reporting a hit.
Sources and recipes do not depend on the GPU or OS, so an OS update only
recompiles the binaries.

## Prewarm

When a title starts (`IRenderer::setTitleId`, called from
`System::loadAndStart`), two background pools work while the guest boots:

- `ShaderLibraryCache` compiles the stored MSL sources into resident
  `MTLLibrary` objects, most recently used first.
- `BinaryArchiveCache` rebuilds the recorded pipelines from their recipes,
  using those libraries and the binary archives.

A draw takes its library or pipeline from there. If the prewarm is building
exactly that library or pipeline, the draw waits for it rather than compiling
it twice. If the prewarm has not reached it yet, the draw compiles it as
before. **No draw is skipped and nothing is approximated.** Prewarmed objects
are created from the same source and descriptor as a draw would use.

`MTLDevice::setShouldMaximizeConcurrentCompilation(true)` lets the Metal
compiler service run these compilations in parallel.

The first session of a title still compiles on first use. Guest shader
decompilation (Maxwell to MSL) and compute pipelines are not prewarmed yet.

## Limits and cleanup

- MSL sources: 8192 files / 256 MiB per title (least recently used evicted),
  4 MiB per source, 4096 resident libraries. The prewarm stops once the
  resident set is full and never evicts.
- Pipeline binaries: deleted when unused for 30 days; least recently used are
  deleted above 1 GiB. Every hit marks the archive as used.
- Sources and recipes unused for 30 days are deleted when the title starts.

## Diagnostics

Set `HYDRA_DISABLE_SHADER_CACHE=1` before launching Hydra to bypass all disk
caches for comparisons. With `HYDRA_PERF_STATS=1` the periodic statistics
include:

- `DRAW: ... compilation shader N (ms), pipeline N (ms)`: compilations that
  draws still had to do.
- `SHADERS: decompiled N (ms) | library hits N | prewarmed pipelines used N`.

At startup the log reports `Shader source cache: N sources`,
`Pipeline recipes: N`, and once done `Shader prewarm: ...` and
`Pipeline prewarm: ...` with their durations. Worker diagnostics go to stderr,
independent of debugger shutdown; capture both streams with `2>&1` when
redirecting logs.

## Integration tests

Use an existing configured macOS build with access to a Metal GPU:

```sh
cmake -S . -B build/swiftui-release -DHYDRA_METAL_CACHE_TESTS=ON
cmake --build build/swiftui-release --target hydra-metal-cache-tests -j 6
```

Choose a new test directory for each run. Each invocation is a separate process:

```sh
test_bin=build/swiftui-release/bin/hydra-metal-cache-tests
test_cache=$(mktemp -d)/cache
"$test_bin" "$test_cache" cold
"$test_bin" "$test_cache" warm
"$test_bin" "$test_cache" changed
"$test_bin" "$test_cache" warm
"$test_bin" "$test_cache" state
"$test_bin" "$test_cache" state-warm
"$test_bin" "$test_cache" warm
"$test_bin" "$test_cache" corrupt
"$test_bin" "$test_cache" warm

error_cache=$(mktemp -d)/cache
"$test_bin" "$error_cache" cold
"$test_bin" "$error_cache" write-failure
"$test_bin" "$error_cache" warm
"$test_bin" "$(mktemp -d)/disabled" disabled
"$test_bin" /dev/null/hydra-test unwritable

recipe_cache=$(mktemp -d)/cache
"$test_bin" "$recipe_cache" recipe-cold
"$test_bin" "$recipe_cache" recipe-warm
```

The tests submit actual Metal draws and inspect every pixel in a 4x4 render
target. They check cross-process cache hits, newly encountered shaders, changed
color-write state, corrupt entries, failed worker writes, recovery, disabled
caching, and an unusable directory. `recipe-cold` records a pipeline recipe
through `Pipeline`; `recipe-warm` checks that the next process prewarms both
libraries and the pipeline, that the draw uses the prewarmed pipeline, and that
it renders the same pixels. Reported times are synthetic pipeline creation
timings; they do not establish an FPS improvement in a game.

`hydra-shader-library-tests` covers source persistence, the warm-session GPU
output, the resident limit and disk eviction with small limits.
