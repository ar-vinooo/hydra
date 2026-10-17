# Crash Bandicoot 4 rendering regressions

The Metal regression tests cover the following renderer fixes:

- Reuse unchanged small CPU buffer snapshots and pool replaced snapshots only after the last CPU view releases them and the current ordered GPU command buffer completes. Native Metal readback checks old/new data before reuse and a later allocation reuses the completed buffer. The ready pool is bounded to 64 MiB / 8192 buffers.
- Default omitted vertex position/output components to `(0, 0, 0, 1)`, while preserving explicit guest writes.
- Honor fragment shader depth exports, including the packed register after color components and the reserved sample-mask slot. Native Metal depth readback covers shaders with zero, one and six color exports, plus the ordinary interpolated-depth path.
- Preserve requested depth writes with the `Always` comparison. The depth readback regression uses Hydra's depth-state cache so a dropped depth-copy pass is caught as well as missing shader exports.
- Correct FSWZADD subtraction signs for guest quad derivatives. A 4×4 Metal fragment render checks positive X/Y differences in every quad lane and preserves add/move modes. The original helper fails this regression. Gameplay impact remains unverified.
- Decode ICMP/FCMP operand variants, predicates, ordered/unordered NaN comparisons and FCMP comparisons with subnormal inputs/FTZ.
- Preserve SHL/SHR clamp and wrap semantics, signed right shifts and bit reversal. Extended modes and condition codes remain unsupported.
- Decode BFI bitfield insertion and SHF funnel shifts with bounded shift counts. Condition-code outputs and SHF extended modes remain unsupported.
- Keep predicate-only instructions from shadowing general-purpose registers during bindless texture tracking, and track every word loaded by LDC.B128.
- Decode bounded BRX switches consisting of an unsigned IMNMX clamp, optional immediate SHL and optional indexed LDC jump table. Enumerate validated targets and include the table words in guest shader cache keys. Other indirect branch patterns remain unsupported.
- Sample GPU-written Z24S8 depth through its G24R8 texture alias.
- Synchronize overlapping buffer texture subranges through Metal buffer copies, including cross-format R16/R32 aliases, byte-offset copies, aliases created after a GPU write and CPU uploads followed by newer GPU writes. Apply overlapping GPU writes in time order.
- Preserve source and destination mip/layer indices when synchronizing overlapping 2D array textures. A Metal readback regression checks an alias starting at source mip 1, layer 1 and verifies that destination layer 1 remains untouched.
- Sample the stencil plane (X32_Stencil8 view, uint shader texture) when a Z24S8 TIC swizzle only reads R. UE4 copies the scene stencil this way; returning depth produced depth-dependent stripes.
- Write GPU-rendered textures back to guest memory before DMA copies read them (readbacks such as UE4's sky light average brightness). Linear textures only write back the requested bytes, so CPU data sharing the memory is not clobbered.
- Replay overlapping GPU texture aliases in write order during guest readback. A Metal regression deliberately puts the newest alias first in cache order and verifies that the older alias cannot overwrite it.
- Distinguish texture storages and mip views by guest tiling and pitch, including address reuse with changed block height, linear stride, or linear/tiled layout. Metal readback checks every decoded byte for each layout at the same guest address. This fixes a cache bug; a subsequent handheld screenshot still showed black stippling, so it does not resolve that symptom on its own.
- Decode and encode 3D block linear surfaces with all slices of a block (block depth > 1), as used by UE4's volumetric lightmap.
- Block linear byte offsets for DMA copies of sub rectangles, layers and partial line counts.

Not covered by tests (verified in game): culling keeps the guest front face for FLIP_Y, 2D engine copies of RG32/RGBA32 compressed-block aliases are also applied to guest memory (texture streaming mip chains), and small RGBA32F/RG32F or pitch 2D copy destinations are written back for CPU readbacks.

Build and run on macOS with a Metal GPU:

```sh
cmake --build build/swiftui-release --target hydra hydra-shader-library-tests hydra-texture-cache-tests hydra-raster-state-tests hydra-render-hash-tests -j 6
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-shader-library-tests
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-texture-cache-tests
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-raster-state-tests
build/swiftui-release/bin/hydra-render-hash-tests
```

The shader and texture tests execute actual Metal commands and inspect results. Passing these tests does not establish correct rendering for every game shader. Crash 4 gameplay still needs a visual check on the combined build; capture stdout/stderr when launching Hydra and keep heavy per-draw tracing disabled for that check.

## Mixed buffer/2D alias correction (build passed; runtime validation pending)

A live Crash 4 capture reports overlapping single-level RG32Float 2D surfaces
and R32Uint buffer textures. Previously the cache logged `Cannot synchronize
textures`, then advanced the destination's update timestamp without copying data.
The new path transfers raw bytes on the ordered GPU stream: a native texture blit
provides packed texels, a byte kernel maps pitch or block-linear guest offsets,
and a destination blit restores the 2D texture when required. Partial copies retain
unaffected texels, buffer neighbors and pitch padding. No float conversion, guest
memory writeback or CPU/GPU wait is introduced. The aligned buffer-to-buffer fast
path remains available. Mixed kernels have a separate device/revision cache so
objects holding an old buffer-only kernel remain safe across live reload.

Scope is single-level, single-layer, uncompressed, non-depth `_2D` surfaces.
Unsupported mixed aliases remain reported and are not marked fresh. This does not
claim that the original dark stippling is fixed: the warning establishes a missing
coherency path, not pixel-level causality. The format-90 descriptor warning is a
separate unresolved observation and has not been mapped to a guessed format.

Prepared Metal cases in `texture_cache_tests.cpp` exercise both directions, a
nonzero surface offset inside a larger raw buffer, a byte-unaligned partial update,
linear row padding, tiled offsets, preserved NaN payload bits and untouched guest
memory. The `hydra` and `hydra-hot` targets built successfully on 16 October 2026.
These regression cases have **not** been built or run. Use the build/test commands above,
then compare a reproducible gameplay scene with the baseline and a second game.

One experiment using the pre-existing `reload_sampled` flag retained the original
stippling and added green artifacts. The flag was removed, but cached contents did
not immediately return to baseline. Refresh is now restricted to ranges without
GPU-write history, including aliases. Reload the game/scene before visual
validation; deleting a diagnostic flag cannot restore previous GPU texture bytes.
Evidence and the experiment outcome are recorded in
`build/hang-debug-20261016-172325/inspection.json`.

## Native unscaled color copies (live patch; tests not run)

`Renderer::blitTextureHot9` now uses a native Metal texture copy for distinct,
same-format, uncompressed 2D/2D-array views with integral origins, matching
sizes, one mip and one layer, after checking mip/layer and rectangle bounds.
This preserves alpha and raw bits (including float NaN payloads). Previously
these copies used the display blit pipeline, whose RGB-only write mask retained
the destination alpha. Scaled copies and format conversions still use the old
render path; their alpha behavior remains a separate limitation. No descriptor
or live object layout changes are required. Resources remain owned by Metal's
ordered command buffer.

A GPU readback case in `texture_cache_tests.cpp` checks all four components of a
partial RGBA8 copy and unchanged neighboring pixels. It is prepared only: no
test build or execution was performed. The hot module built and loaded during
gameplay on 16 October 2026. Dark stippling remains visible; this establishes a
copy-path correctness defect, not the root cause of the visual symptom.

Solid diagnostic coverage now bypasses guest fragment execution when the shader
has no explicit depth export. Capture `capture-549275540011833` contains magenta
pixels in `p5614-d2634-after-rt0.png` and records a successful Metal pipeline.
The override expires automatically and was explicitly reset. Depth-exporting
shaders retain the output-only fallback with an explicit limitation event.

## Scaled color copy alpha (17 October 2026)

Guest render-path color copies now write RGBA and select the pipeline using the
destination format. Display composition retains its RGB-only write mask; the
pipeline cache keys distinguish the two uses. Previously scaled copies retained
stale destination alpha and format conversions selected an incompatible pipeline.

Built `hydra`, `hydra-hot` and `hydra-texture-cache-tests`; the complete texture
cache test executable passed with `MTL_DEBUG_LAYER=1`. New Metal readbacks cover
1x1-to-2x2 scaling to RGBA8 and RGBA16Unorm, every color component, untouched
neighbors and separate display/copy pipeline cache entries. The existing native
subrectangle copy regression passed as well. Output is in
`build/swiftui-release/ghosting-texture-tests.log`.

The descriptor change requires a full app restart, not a live module reload.
Crash 4 edge ghosting, reported during movement and with a stationary camera,
has not been reproduced or visually validated after this correction. No live
Hydra process was available during this check; existing hot-debug status was stale.


Further sampler/decoder and partial tiled readback corrections, their tested
coverage and remaining limitations are documented in
[README-texture-sampling.md](README-texture-sampling.md). Crash 4 gameplay
validation remains pending; no complete visual repair is claimed.
