# Metal texture upload pool

Texture uploads use shared Metal buffers grouped into power-of-two capacities
from 4 KiB to 16 MiB. Larger uploads are allocated at their requested size and
are not cached. The ready pool holds at most 64 buffers and 64 MiB; in-flight
uploads are separate and remain unavailable until GPU completion.

Only texture upload staging uses this pool. Readback buffers, index buffers and
other temporary allocations retain their existing allocation path.

`retireTextureUploadBuffer` transfers the upload to its command buffer. One
completion handler per command buffer returns successful uploads to the pool;
failed submissions release the buffers. The callback owns the pool independently
of the renderer, so it does not access a destroyed renderer.

Build with `HYDRA_METAL_CACHE_TESTS=ON`, then run:

```sh
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-texture-cache-tests
MTL_DEBUG_LAYER=1 HYDRA_DISABLE_UPLOAD_POOL=1 ./build/swiftui-release/bin/hydra-texture-cache-tests
```

The regression test exercises real Metal copies of different byte patterns,
prevents reuse before submission, checks reuse after completion, checks the pool
budget and capacity rounding, and exercises pool ownership across pending work.
It also retains the compressed-format and contained-mip regression checks.

For gameplay comparisons, start Hydra with `HYDRA_PERF_STATS=1`. `UPLOAD` lines
report CPU time spent staging texture uploads, uploaded MiB/s, and buffer
allocation/reuse counts. `PERF` lines report frame rate and CPU/GPU timing.
Use the same mode, resolution and track, and compare warmed sessions without
concurrent builds or GPU tests. Set `HYDRA_DISABLE_UPLOAD_POOL=1` for a baseline
with identical instrumentation. FPS gains require a gameplay comparison; passing
the regression test alone does not establish a speedup.
