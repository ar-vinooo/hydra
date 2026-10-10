# Metal raster state cache

Each render encoder tracks viewport arrays, scissor arrays, blend color, depth
bias and stencil references. Repeated values skip the corresponding Metal call.
Changing a value or creating a new render encoder applies the state again.
Rectangle comparisons inspect fields explicitly rather than comparing padding.
The stencil-clear path updates the same cache to prevent stale references.

Build with `HYDRA_METAL_CACHE_TESTS=ON`, then run:

```sh
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-raster-state-tests
MTL_DEBUG_LAYER=1 HYDRA_DISABLE_RASTER_CACHE=1 ./build/swiftui-release/bin/hydra-raster-state-tests
```

Both modes render and verify an identical red/green target. The tests cover
repeated state, changed viewport/scissor/blend color, and render-blit-render
transitions using the same render-pass descriptor. The test reports 24 applied
and 16 skipped calls with caching, versus 40 applied and none skipped without it.
This measures command elimination in the test, not a gameplay speedup.

`HYDRA_PERF_STATS=1` adds `DRAW` lines reporting CPU binding time per frame,
applied/skipped raster calls, and shader/pipeline creation counts and total
compilation time over the reporting interval. Compilation time includes the
synchronous library/function creation or pipeline creation path. It is not
GPU execution time. Raster counters are aggregated when a command buffer is
submitted, avoiding an atomic counter operation for each raster setter.

For an A/B comparison use the same warmed track, Docked/Handheld mode and display
resolution, without concurrent builds or GPU tests. Use
`HYDRA_DISABLE_RASTER_CACHE=1` for the baseline with identical instrumentation.
The existing pipeline disk cache is retained; this change does not skip draws
or introduce asynchronous shader compilation.
