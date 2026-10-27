# Render cache key overhead

CTR live CPU samples after the texture range filter still showed work in
shader/pipeline hashing and Metal render encoder setup. The generic enum
overload of `XxHash32::add` feeds each byte as a separate word. The two render
cache keys now hash enum values as explicit `u32` words, keeping the existing
field list and avoiding struct padding. These are in-memory cache keys; the
Metal disk archive key implementation is unchanged.

The new `hydra-render-hash-tests` target under `HYDRA_METAL_CACHE_TESTS` checks
shader key changes for vertex formats, fixed inputs, clip depth, fragment
inputs, sampled code bytes, color types and buffer texture formats. It also
checks pipeline target/write/depth formats and reports microbenchmarks against
the prior hash implementations. One local run measured vertex hashing at
511 to 130 ns/hash and pipeline hashing at 255 to 129 ns/hash. These are isolated
CPU timings and do not establish an equivalent FPS improvement.

Metal encoder creation now resets cached state once rather than twice. The
existing real Metal pixel test checks repeated and changed raster state, plus
render/blit/render transitions, with the raster cache enabled and disabled.
Shader dump configuration is read once instead of querying the process
environment on each shader lookup.

Gameplay performance, audio and rendering must still be checked on the same
CTR track, mode and resolution. CPU sampling and aggregate Metal command-buffer
times do not identify individual expensive GPU shaders. The attempted xctrace
template discovery exited with signal 9, so no detailed GPU capture was obtained.
