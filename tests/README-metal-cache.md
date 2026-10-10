# Metal disk shader cache

The Metal renderer automatically stores GPU pipeline binaries under
Hydra's app data directory, in `cache/metal/v2-<GPU>-<OS hash>/`.
On macOS this is `~/Library/Application Support/Hydra/cache/metal/`.

Each entry uses SHA-256 of the generated shader code, stage/backend/compiler
options, and explicit Metal pipeline state. Guest addresses and struct padding
are excluded. Metal also verifies the binary with
`PipelineOptionFailOnBinaryArchiveMiss` before reporting a hit.

On a miss, the draw compiles normally. A worker retains a copy of the
Metal descriptor, harvests the pipeline into its own binary archive, then
atomically renames the completed file into place. A serialization or write
failure affects only that entry. Shutdown drains the worker.

This caches Metal pipeline binaries. Guest shader decompilation and
MSL-to-library compilation still run when a shader is first encountered in a
session. New effects or tracks can therefore still stutter on first use.
No draws are skipped and async shader compilation is not enabled.

Set `HYDRA_DISABLE_SHADER_CACHE=1` before launching Hydra to bypass the disk cache
for comparisons. Set `HYDRA_PERF_STATS=1` to enable the existing frame statistics.
Logs report the cache directory, hits/misses, and saved/failed entries.
Worker diagnostics go to stderr, independent of debugger shutdown; capture
both streams with `2>&1` when redirecting logs.

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
```

The tests submit actual Metal draws and inspect every pixel in a 4x4 render
target. They check cross-process cache hits, newly encountered shaders, changed
color-write state, corrupt entries, failed worker writes, recovery, disabled
caching, and an unusable directory. Reported times are synthetic pipeline
creation timings; they do not establish a CTR FPS improvement.
