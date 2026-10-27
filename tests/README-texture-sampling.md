# Texture sampling modes on Metal

`TextureSampleFlags` is a 32-bit IR mask. A flag is accompanied by its data
operands; setting a bit alone does not supply coordinates, gradients or offsets.
The IR builder rejects conflicting modes and missing gradient/offset operands.

| Flag | Implementation |
| --- | --- |
| `IntCoords` | Integer texel reads; coordinate bits remain integers. |
| `DepthCompare` | Depth sampling or depth gather using the comparison reference. |
| `Lod` | Explicit `level`, or the minimum LOD clamp accompanying TXD gradients. |
| `Bias` | Fragment TEX.LB/LBA sampling with `bias`. |
| `Gather` | Four texels from one component; 2D, array and cube resource types retain their type. |
| `Derivatives` | Explicit gradient operands passed to Metal's gradient sampling. TXD/TXD.B decoding currently covers 2D and 2D-array instructions. |
| `Offset` | Signed texel offsets passed to native filtered sampling/gather; integer reads add the offset to integer coordinates. |
| `Offsets` | TLD4.PTP consumes two packed registers containing four signed six-bit x/y pairs. Each result lane comes from its corresponding offset. A following depth reference is read after both registers. |
| `Bindless` | Handles resolved by the existing bounded constant-buffer/constant handle tracker are marked in IR and mapped to native resource slots. Arbitrary GPU-computed descriptor indexing is not implemented. |
| `LodZero` | Explicit level zero for TEX/TLD and scalar variants, preserving the distinction from implicit LOD. Sampler LOD clamps still apply. |

Sampler minimum and maximum LOD are decoded from the TSC's unsigned 4.8 fixed
point fields, stored in the sampler descriptor and included in cache keys.
Metal samplers receive both bounds. TSC/TIC descriptor LOD bias is still a
separate unimplemented setting; instruction TEX bias is supported.

TLD4S uses the same native single-offset path. The former coordinate-shift
approximation is no longer used for filtered texel offsets. A standalone runtime
compiler probe accepted dynamic native offsets on the current macOS 27.0.1 host;
older Metal compiler/device support has not been validated.

## Regression cases

`hydra-shader-library-tests` executes generated shaders on Metal with colored
mip levels and independently numbered grid texels. Cases cover TEX.LB/LBA,
positive/negative bias, explicit zero LOD, TXD gradients, fractional sampler
bounds, separate sampler cache entries, resolved bindless bindings, signed
integer/normalized offsets, ordinary gathers, decoded PTP pairs, array layers
and depth-reference placement after PTP words. GPU readbacks verify the colors
and every gather lane. Unsupported TXD dimensions, full warp/quad texture modes,
sparse residency predicates, multisample fetch and arbitrary dynamic bindless
indexing remain outside this implementation.

`hydra-texture-cache-tests` also covers repeated partial tiled readbacks followed
by a complete readback. It checks requested GPU bytes while retaining unrelated
CPU bytes and tile padding. Partial readback no longer marks the entire storage
flushed.

```sh
cmake --build build/swiftui-release --target hydra hydra-hot hydra-shader-library-tests hydra-texture-cache-tests -j 4
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-shader-library-tests
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-texture-cache-tests
```

Header, descriptor and emitter interface changes require a full host restart.
Do not bypass the hot-reload ABI check. Passing these cases does not establish
that Crash 4 edge noise, overlapping effects, square footprints or water ripples
are fixed; those symptoms still require gameplay comparison on the new host.

The instruction encoding checks use the original envytools GM107 decoder:
https://github.com/envytools/envytools/blob/master/envydis/gm107.c

## Validation on 17 October 2026

The final `hydra`, `hydra-hot`, `hydra-shader-library-tests` and
`hydra-texture-cache-tests` targets built successfully. Both complete test
executables passed with `MTL_DEBUG_LAYER=1`, including decoded PTP gathers,
array layer selection, depth reference placement, TXD, LOD bias/clamps and
repeated partial/full tiled readbacks with preserved padding. Logs are in
`build/ghosting-capture-20261017/texture-flags-tests.log`,
`partial-readback-tests.log` and `final-build.log`.

The local Release Swift flags put the Command Line Tools SDK library directories
before stale Xcode SDK directories to resolve the SDK/linker mismatch after the
Xcode update. No Xcode license agreement was accepted by the agent.

A fresh host was launched through `tools/hot_debug.py run --gpu-capture --perf-stats`
with session `build/ghosting-capture-20261017/live`. Gameplay comparison and a
new `.gputrace` are pending the user's gameplay notification. Do not treat the
passing synthetic tests as validation of the reported Crash 4 visual symptoms.
