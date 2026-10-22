# Shader lookup scratch storage

The `DRAW PREP` profile in the CTR session spent 5.5–5.9 ms/frame in pipeline
lookup. The pipeline path looks up both guest shaders, including a second
lookup for texture/sampler specialization when needed.

Previously every shader lookup copied the full guest descriptor, including its
maps and constant-reader callback, before searching resident compiled versions.
It also allocated a temporary vector for the exact lookup key. Warm lookups now
build that same normalized key in a per-cache vector protected by the existing
lookup mutex. Stored cache entries keep their own key copies. An owned descriptor
is constructed only for a miss or selected visual diagnostics.

CPU sampling also identified the key builder itself: generic scalar encoding
expanded each byte into a separate 32-bit word, including eight words for every
empty map's 64-bit count. Resident lookup keys now pack typed scalar values into
one or two words, preserve floating-point bits with `bit_cast`, and encode
sampler colors by component. The normal compilation/hash key retains its
existing encoding. Both resident-key paths include the same specialization
fields; no instruction or constant validation is skipped.

This changes allocation/copy work. It retains exact key equality, comparison of
the mapped instruction bytes actually decoded by the shader, branch-constant
checks, address-space identity, hot-module/override revisions, invalidation and
the lifetime of older compiled artifacts. It does not enable the separately
gated batching/coalescing optimizations.

For the previous allocation/copy path, set
`HYDRA_DISABLE_SHADER_LOOKUP_REUSE=1` before starting the process. The switch
supports controlled pixel and timing comparisons using the same executable.

```sh
cmake --build build/swiftui-release --target hydra \
  hydra-shader-cache-reference-tests hydra-render-hash-tests \
  hydra-shader-library-tests hydra-resource-registry-tests \
  hydra-raster-state-tests hydra-gpu-engine-tests -j 6

python3 tests/shader_cache_reference.py \
  build/swiftui-release/bin/hydra-shader-cache-reference-tests \
  --nvidia-3d build/nvidia-headers/clb197.h \
  --nvidia-compute build/nvidia-headers/clb1c0.h

HYDRA_DISABLE_SHADER_LOOKUP_REUSE=1 python3 tests/shader_cache_reference.py \
  build/swiftui-release/bin/hydra-shader-cache-reference-tests \
  --nvidia-3d build/nvidia-headers/clb197.h \
  --nvidia-compute build/nvidia-headers/clb1c0.h
```

The reference runner enables Metal validation and checks exact RGBA32Float
readback across CPU/GPU code changes, remaps, fragmented mappings, address-space
switches, truncated code, invalidation methods, old shaders and restored branch
targets. Additional cases check caller immutability, normalized revision/branch
metadata and alternating keys. The warm-lookup timing is the median of five
20,000-lookup samples; it is a CPU microbenchmark, not a gameplay FPS result.

Gameplay comparison must use the same CTR track, camera, mode and resolution,
with FG off, a warm shader cache and no concurrent build or GPU test. Compare
`DRAW PREP` pipeline time, `GPU THREAD` work, native FPS and rendered appearance.
The user controls gameplay. Do not infer a gameplay speedup from the isolated
lookup benchmark alone.

Routine kernel waits with an infinite timeout are traced only when
`HYDRA_TRACE_SVC` is enabled. Such timeouts are normal for display/event waits.
Warnings for actual waits longer than two seconds remain active. File logging
was already buffered; error/fatal messages still flush immediately. This removes
routine formatting/debugger-callback work without hiding warnings or errors.
