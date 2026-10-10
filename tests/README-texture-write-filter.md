# Texture synchronization range filter

Live sampling of CTR found the GPU command thread spending substantial time in
`TextureCache::update`, while the main thread waited for PFIFO completion.
Merged guest-memory intervals can contain many sampled textures. A write to one
render target previously made every older texture scan every storage in the
interval, even when the written address range did not overlap it.

Each memory interval now records conservative GPU-written ranges and their
latest timestamps. A disjoint texture can skip the storage scan. Overlapping
textures still use the existing synchronization path. CPU invalidation takes
precedence over this filter. History survives memory merges and does not retain
storage pointers. Ranges sharing a start retain their maximum end, which may
cause an extra scan after a render target shrinks but cannot omit a needed scan.

Run `MTL_DEBUG_LAYER=1 bin/hydra-texture-cache-tests` from the build directory.
The added hardware test checks a merge after a GPU write, an overlapping padded
alias receiving actual rendered red pixels, and CPU invalidation uploading a
disjoint texture. Repeat with `HYDRA_DISABLE_TEXTURE_WRITE_FILTER=1` to verify
the same pixel results through the original scan path.

With `HYDRA_PERF_STATS=1`, `TEXTURE` logs count full synchronization scans and
skipped scans. CTR performance and audible recovery after Alt-Tab still require
gameplay validation; the optimization does not itself change audio or display
timing.
