# Hydra graphics work: handoff

Status as of 2026-10-18. This covers the graphics-compatibility work done on branch `arvino`: what changed, how it was verified, what is still open, and the rules for continuing. Nothing here is committed.

Read sections 1–3 before touching code. Section 5 lists the design decisions that will bite you if you don't know them.

Files in this folder:
- `occlusion-reports-2026-10-17.md`: native occlusion counts, completion-ordered reports and local conditional rendering.
- `continuation-all-2026-10-17.md`: subsequent implementation, validated scope and explicitly open work.
- `primitive-restart-2026-10-17.md`: indexed primitive restart continuation, references, validation and remaining draw scope.
- `fp64-reports-2026-10-17.md`: FP64 soft float, ATOM.CAS, buffer-cache coherence, report semaphore reduction/acquire, and what was deliberately not started.
- `global-memory-2026-10-17.md`: LDG/STG/ATOM/RED storage buffers (decoder to Metal binding), tests, open coherence work.
- `queue-vsync-slices-2026-10-17.md`: buffer queue state machine, 60 Hz vsync, DMA/2D slices and ROP.
- `video-instructions-2026-10-17.md`: VMAD/VMNMX/VSETP.
- `sampler-borders-2026-10-17.md`: exact sampler border colors (shader emulation), evidence, limits.
- `implementation-2026-10-17.md`: continuation changes, validation status, remaining scope and failed GPU capture evidence.
- `continuation-2026-10-18.md`: RZ register boundaries, DRAW_AUTO fallback,
  compressed alias copies and depth/3D readback.
- `README.md`: this document.
- `backlog.md`: master backlog. `[x]` means done and tested; `[ ]` means open.
- `audit/*.md`: eight subsystem audits written at the start of the work, with capability tables, code quotes, NVIDIA references and secondary implementation comparisons. They describe the code **before** the fixes below, so their line numbers are stale and many of their bugs are now fixed. Section 7 maps which ones.

---

## 1. Goal and ground rules

**Goal.** The user's mandate is broad Nintendo Switch (Tegra X1 / Maxwell GM20B) graphics compatibility in Hydra, a macOS emulator using Metal. It covers the 2D, 3D, DMA and inline engines, textures, the shader decompiler (Maxwell SASS → IR → MSL), compute and memory sync, and presentation. For each feature: compare it with a verified reference, classify it (VERIFIED / IMPLEMENTED_UNVERIFIED / PARTIAL / MISSING), and fix whole feature families rather than one game's symptom. Order of priority: correctness, then broad compatibility, then maintainability, then performance.

Do not:
- add enum values or Metal features without confirmed guest semantics;
- treat one game rendering correctly as proof that a subsystem is complete.
- treat another emulator's implementation as proof of NVIDIA semantics.

**Rules from the user (follow them):**
- **Never commit** unless the user asks. Don't stage files either; the user's tree has many unrelated uncommitted changes.
- **Don't launch or drive games or the GUI.** The user runs the games and reports what they see. Ask them to test.
- **Gather evidence first.** Use logs, captures, dumped shaders and reference code. Fix root causes, not symptoms.
- **Use primary authorities.** NVIDIA documentation/headers define guest state;
  Apple Metal documentation, SDK headers and validation define host behavior.
  Reproducible hardware traces may fill gaps. Emulator code, including Eden,
  is only a hypothesis generator and must be corroborated before a feature is
  marked guest-semantics verified.
- **Make engine-level, generic fixes.** Main titles: Crash Bandicoot 4 and Jump Force (both UE4). Regression titles: Crash Team Racing and Mario Kart 8. A fix for one UE4 title should carry over to the other.
- **The user writes casual Indonesian** ("bg", "bro"); reply in Indonesian.
- **Strip debug code before any commit** (section 9).
- **Match the existing code style.** Comments are sparse, short and explain why. Don't write long comment blocks.

**Evidence hierarchy** (cite primary sources in code reviews or commit messages):
1. **NVIDIA open-gpu-doc**: `classes/3d/clb197.h` (3D class),
   `clb097tex.h` (TIC/TSC), `clb0b5.h` (DMA), `cla140.h`.
2. **Apple Metal** documentation, SDK headers and validation-layer results.
3. Reproducible NVIDIA hardware/NVN traces and focused output comparisons.

Mesa/NAK, nouveau, envytools and other emulators may help locate an encoding
or formulate a test. They are secondary comparators, not authorities. Eden in
particular must never be the sole reason for an implementation or `[x]` status;
agreement with it is not confirmation that its behavior is correct.

A local copy of these sources was at `/private/tmp/claude-501/-Users-arvino-Documents-Project-hydra/0e906531-f45a-4e9b-b0bc-dcb303edaa1b/scratchpad/refs/`. It is session-scoped and may be gone; fetch upstream if so.

---

## 2. Build and test

The machine is an M1 with 8 GB of RAM: keep `-j 6`. The build uses `-Werror` with `-Wconversion -Wsign-conversion -Wshadow -Wold-style-cast` and more, so new code must be warning-clean.

```sh
# App and GPU test suites
ninja -C build/swiftui-release -j 6 hydra hydra-shader-library-tests hydra-texture-cache-tests \
  hydra-raster-state-tests hydra-gpu-engine-tests hydra-render-hash-tests hydra-register-write-tests \
  hydra-frame-trace-tests hydra-visual-diagnostic-tests hydra-buffer-snapshot-tests hydra-xci-tests

# Test binaries live in build/swiftui-release/bin/. Always run with the Metal validation layer:
for t in shader-library texture-cache raster-state gpu-engine render-hash register-write frame-trace visual-diagnostic; do
  MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-$t-tests || echo "FAILED: $t"
done
```

New `.cpp`/`.hpp` files under `src/core` must be added to the explicit source list in `src/core/CMakeLists.txt`. Test targets are defined in the top-level `CMakeLists.txt`.

**Current results (2026-10-17, after all changes below):**

| Suite | Result |
|---|---|
| shader-library | 50 PASS (includes all shader coverage tests) |
| texture-cache | 29 PASS (includes texture format tests) |
| gpu-engine | 17 PASS groups |
| raster-state, render-hash, register-write, frame-trace, visual-diagnostic, buffer-snapshot | PASS |
| metal-cache | needs arguments (`<archive-path> <cold\|warm\|…>`), not run |
| xci | needs a fixture dir and prod.keys; MMU and thread-wait parts pass (section 6) |

**Not validated in gameplay.** Every change below is verified by unit/GPU tests only. Gameplay checks by the user are pending (section 8).

---

## 3. Where I stopped

**Occlusion/report continuation (2026-10-17):** Real visibility counts now replace the approximate per-draw counter. Payload and ZPASS reports synchronize before publishing guest data; local CONDITIONAL/EQ/NE rendering modes read these reports. See `occlusion-reports-2026-10-17.md` for evidence, regression results and limits. The entire master backlog is still not complete.

**Latest continuation (2026-10-17):** Indexed restart, inline indexed methods, FMZ/BFE/IADD32I.PO edges, PEXIT/direct absolute JMP, unnormalized texture coordinates and 2D clip/CORNER calculations have been added. The final build and all nine regression suites pass. The full backlog is **not complete**; see `continuation-all-2026-10-17.md`, the master backlog and `tests/README-graphics-coverage.md` for the tested subsets and remaining families.

**Update (2026-10-17, later):** Codex finished CC flow tests, GL depth funcs, blend factors, sampler bias/anisotropy and omap outputs. Then (Codex paused) I added:
- BFE.BREV, bool NOT, CLAMP_OGL, nearest samplers for integer textures, cross-format texture copies, alpha test;
- the raster batch: raster discard, cull FRONT_AND_BACK, winding, depth bias halving per primitive class, depth clamp, vertex stream bound;
- **Regression found and fixed (2026-10-17):** the viewport batch treated register 0x64B (SET_VIEWPORT_SCALE_OFFSET) = 0 as "window space", but UE4/NVN titles leave it 0 while drawing clip-space vertices with viewport transforms, so Crash Bandicoot 4 rendered blank (every draw collapsed into one pixel). The transform is now always applied (`viewportTransformEnabled()` in `3d.cpp`); the window-space shader path stays tested at shader level only. Found with `HYDRA_TRACE_DIR` frame/visual captures and hot reload.
- the viewport batch: LOWER_LEFT flip by surface height, scissor flip, window-space positions when the viewport transform is off (converted in the vertex shader, `GuestShaderState::window_space_*`), default depth range 0..1 with clamping, signed SSCALED attributes, vertex sizes G8R8/X8B8G8R8/A8, compact draws (0x485/0x486, 0x5F9-0x5FE).

- the next batch: dual-source blending (`color(0), index(1)`), wrapping 32-bit signed IR arithmetic, closest Metal border preset, TIC format cache and other per-draw savings, DMA semaphores/block-linear to block-linear/transfer NONE, control flow (CAL/RET/PRET/PCNT/CONT), TMML, SULD/SURED/SUST.D/SUATOM variants, conditional rendering (later expanded to local CONDITIONAL/EQ/NE by the occlusion/report continuation), puller semaphores and host fences (`HYDRA_GPU_FENCES=1`, **off by default**; with it a fence completes when the Metal command buffer completes and `nvhost_ctrl` waits block on it);
- the 2D batch: signed 32.32 `FixedPoint`, exact du/dx source extents (no truncation, mirrored copies keep their sign), SAMPLE_MODE filter (POINT uses the nearest sampler; origin CORNER is **not** modeled, shifting by du/2 would blur 1:1 copies if the driver uses it differently), integer uint/int blit pipeline variants (point read, `BlitOptions` in `blitTexture`), word 0x90 no longer used as a source layer;
- tooling: `hydra-shader-corpus-tests <dir> [limit]` decompiles and Metal-compiles every dumped guest shader (`HYDRA_CORPUS_VERBOSE=1` prints names); all 2771 dumped Crash 4/Jump Force shaders pass.

**Continuation (2026-10-18):** Sequential shader operands now stop at RZ
instead of wrapping into R0, `DRAW_AUTO` has the verified byte-count/stride
fallback, raw compressed-block alias copies work in both directions, and
packed depth/stencil plus 3D GPU texture writes can be flushed back to guest
memory. The focused shader, engine and texture suites pass with Metal
validation. See `continuation-2026-10-18.md`.

See `backlog.md` for what is left. The raster, viewport and compact draw changes have no engine-level test (they need a `ThreeD` with a GMMU, like `testInlineToMemory`); only the window-space shader path is tested. Verify them in gameplay, or add such a test.

Performance and stability are part of the job, not an afterthought: new code on per-draw paths must not allocate, hash or copy more than the code it replaces (see section 5, items 6 and 9, for the existing cost-saving designs).

The original note follows.

I was about to start **condition codes and flow tests**, the first P1 item.

FSET, ISET, PSET, LOP and the others drop `.CC`, and BRA/EXIT/KIL/SYNC/BRK ignore their `ccc` flow-test field.

Confirmed in Crash 4 shader `0x1b56830`: `FSET.EQ.FTZ.AND.CC RZ, R16, R17, PT` is followed by `@P0 BRA CC.NEU`. Hydra branches whenever `R17==R18`, regardless of `R16==R17`.

References:
- `audit/shader_alu.md` §B2;
- `audit/shader_mem_tex_flow.md` C16;
- A secondary-emulator comparison identified the same CC/flow behavior, but
  does not count as guest-semantics evidence.

Notes for the work:
- Hydra represents the flags as `ir::Value::createConditionFlag(i)`: 0 = Z, 1 = S, 2 = C, 3 = O. See the I2I CC code in `decoder/conversion.cpp`.
- BRA decoding is in `decoder/decoder.cpp` (search `// bra`).
- Add a runAlu/flow test in `tests/shader_coverage_tests.cpp`.

Nothing was edited for this item yet.

---

## 4. What was done

Grouped by area. Each line gives the fix, the main files and the test that covers it. All paths are relative to `src/core/hw/tegra_x1/gpu/` unless they start with `src/` or `tests/`.

### 4.1 Shader decompiler

**Stage I/O and system values**
- **Attribute map** (`renderer/shader_decompiler/const.{hpp,cpp}`, `analyzer/memory_analyzer.*`, `codegen/lang/msl/emitter.*`):
  - Exact attribute address map. Unknown addresses log and are skipped instead of being mis-mapped.
  - Front facing reads as 0xFFFFFFFF/0; it was previously mis-mapped.
  - New: point coord (with origin), primitive ID, layer and viewport index (FS inputs and VS outputs), point size (fixed or program), clip distances (enabled by mask).
  - Instance ID excludes the base instance.
  - Tests: `testFrontFacing`, `testPointSizeAndCoord`, `testClipDistances`, `testViewportAndLayerOutputs`.
- **S2R/CS2R system registers** (`decoder/move.cpp`, MSL emitter): Y direction (from `y_negate`), helper thread, lane ID in FS, invocation defaults, clocks 0. Test: `testSpecialRegisters`.

**Integer and half ALU**
- **New ops** (`decoder/integer_misc.*`, new file):
  - PRMT, all modes;
  - IMUL and IMUL32I (HI word, signedness, CC);
  - P2R;
  - FCHK fast path (predicate = false).
  - Tests: `testPrmt`, `testImul`, `testP2r`.
- **HSET2/HSETP2 rewrite** (`decoder/half_comparison.*`): `.H_AND`, F32 cbuf operand, F32 swizzle. Test: `testHalfCompare`.
- **Half arithmetic** (`decoder/half_arithmetic.*`, `decoder/const.hpp` `getHalfConstMemory`):
  - HADD2/HMUL2/HFMA2 cbuf operands are one F32 used for both halves.
  - HADD2.R negates A with bit 43 (it was bit 38).
  - RZ as a half2 operand no longer emits `INVALID` (`codegen/lang/emitter.hpp` `getConstantStr` gained vector support).
  - Test: `testHalfOperands`.
- **XMAD cbuf form** (`decoder/integer_arithmetic.*`): decodes `.PSL` (bit 55) and `.MRG` (bit 56). This was wrong in 520+ dumped Crash 4 shaders. Test: `testXmadConstantMultiply`.
- **LDC** (`decoder/memory.cpp`): U8/S8/U16/S16 extraction, signed 16-bit register offset. Test: `testLdcSmallIntegers`.

**Conversions (implemented; NVIDIA/hardware validation incomplete)** (`decoder/conversion.{hpp,cpp}`)
- Each conversion is now a backend helper; see the intrinsics bullet below.
- **F2I**: round-half-to-even, saturation to the destination range, NaN → 0, F16 source half (H1), 16-bit results sign-extended into the full register, 64-bit destinations as a register pair.
- **F2F**: H1 source, F16 result in the low half (high half 0), SAT, integer rounding only for equal sizes, exact F64↔F32 conversions.
- **I2I**: SAT is bit 50 (was 51); source sign is bit 13 only; 3-bit byte selector; non-saturating narrow results are truncated and zero-extended. The result tests validate the Metal path, not these guest semantics on NVIDIA hardware.
- **I2F**: selector, 64-bit source pair, abs/neg keep the most negative value.
- **MUFU.RCP64H/RSQ64H** (`decoder/multifunction.cpp`): now produce approximate double seeds. Before, they aborted the host via `LOG_FATAL` on an undefined value.
- **Removed debug code**: the old `HYDRA_NO_BYTESEL` "DEBUG byte select" logging is gone.
- Test: `testConversions`, which checks the results against host references.

**Textures**
- **TXQ** (`decoder/texture.*`, MSL emitter): opcode swap fixed, LOD operand used, levels/dimensions correct.
- **TEXS**: LL.DC operand order.
- **TLDS**: proper encoding table.
- **TLD4S**: F16.
- **TXD**: derivative layout and 1D.
- **Depth compare**: returns `float4(res, res, res, 1)`.
- Tests: `testTextureQuery`, `testTexsVariants`, `testTldsEncodings`, `testTxdGradientOrder`.

**Shared memory** (`decoder/compute.*`)
- ATOMS.CAS is implemented.
- 8/16-bit shared stores are atomic (AND/OR).
- Test: `testSharedCompareExchange`.

**Intrinsics** (`ir/intrinsic.hpp`, `codegen/lang/msl/intrinsics.cpp`)
- This is the mechanism for ops with exact guest semantics.
- Add an enum value, an MSL helper source string and a function name. Call it with `builder.opIntrinsic(op, type, {args})`.
- Arguments are raw u32 words.
- Helpers are emitted only if used.

**Other decompiler fixes**
- `decoder/comparison_helper.hpp` `getLogical`: has a default case (an invalid bool op was undefined behaviour).

### 4.2 MME macros and engine registers

- **MME interpreter rewrite** (`macro/driver_base.*`, `macro/interpreter/driver.*`):
  - Current behavior: per-call state reset, u64 carry (subtract carry = no borrow), signed 18-bit immediates, delay slots, annul, exit after the delay slot. Parts originally came from a secondary emulator and still need NVIDIA/hardware confirmation.
  - Correct ExtractShiftLeft operand order.
  - All arguments after the first are parameters.
  - Nested macro calls execute immediately.
  - `MacroHost` interface.
  - Tests: `testMacroAlu`, `testMacroBitfields`, `testMacroControlFlow`, `testMacroParametersAndNesting`, `testMacroResultOperations` in `tests/gpu_engine_tests.cpp`.
- **Register file and BEGIN** (`engines/3d.{hpp,cpp}`): `Regs3D` gained `mme_scratch[0x100]`, with a static_assert on the size. The BEGIN topology field is 16 bits; the instance controls are decoded.
- **Method table** (`engines/engine_base.hpp`): `getReg`/`writeReg` bounds-check in all builds. Table-dispatched methods also store their argument.

### 4.3 3D engine and renderer state

- **Clears** (`engines/3d.cpp`, `renderer/renderer.hpp`, `renderer/metal/renderer.*`):
  - Clears no longer inherit stale draw state.
  - `setClearState(rect, stencil_write_mask)` is new.
  - Stencil clears respect the layer and the write mask (register 0x43e bit 0, mask in 0x4e7).
  - The layered-clear assert is removed.
  - Test: `testRendererClears`.
- **Instancing**: per-instance vertex streams bind at `base_instance + instance_index / divisor` using a constant step function (`renderer/metal/pipeline.cpp`, `getVertexBuffer`). The Metal base instance equals the guest instance index.
- **Pipeline state**: `topology_class` is passed when the VS writes the layer or viewport (Metal requires `inputPrimitiveTopology`).
- **Shader key** (`renderer/shader_cache.*`): includes Y negate, clip mask, point state and point coord origin.
- **Index buffers** (`engines/3d.cpp` `drawVertexElements`, `renderer/index_cache.*`; `index_decoders.inc` deleted):
  - The bound view now starts at INDEX_BUFFER FIRST, is clamped to the LIMIT register, and the draw uses start 0.
  - Decoded index buffers are checked against a copy of the guest bytes, so rewritten indices decode again, and the descriptor is compared, so hash collisions are safe.
  - The input index type is read correctly; before, u8/u32 quads read garbage.
  - Fan/polygon → triangle list (was a wrong zig-zag), quad strip → triangles, line loop → closed strip.
  - Degenerate draws are skipped.
  - Test: `testIndexDecoding`.

### 4.4 Formats and textures

**Format mapping** (`renderer/const.{hpp,cpp}`, `renderer/metal/maxwell_to_mtl.cpp`)
- **Crash fixes**: no `LOG_FATAL` remains in format conversion.
  - Unknown color, depth and TIC formats return `Invalid`. Engines skip invalid render targets (`getColorTargetTexture`/`getDepthStencilTargetTexture` return null) and 2D copies.
  - Added: RGBA32Sint in the Metal table; ZT X8Z24 (0x15) → `Z24Unorm_X8Uint`; A2R10G10B10 → new `BGR10A2Unorm`; X1R5G5B5 and the Z/O 1555/8888 variants.
- **TIC table**: +26 encodings originally collected from a secondary emulator;
  NVIDIA `clb097tex.h` is the required authority for revalidation:
  - BC6H S/U, BC7 sRGB, BC4 SNORM;
  - RG8 UINT/SINT, RGB10A2 UINT, E5B9G9R9;
  - ETC2/EAC ×10, now mapped to native Metal `ETC2_*`/`EAC_*`;
  - depth alternates, S8Z24, X8Z24, R32B24G8, A5B5G5R1.
  - Still missing: G4R4 and R32G32B32 (both need upload expansion).
- **Z24 layouts**:
  - ZT 0x14 (NVIDIA Z24S8, stencil in the low byte) and TIC Z24S8/G24R8 → new `S8Uint_Z24Unorm`.
  - ZT 0x16 (NVIDIA S8Z24) and TIC S8Z24 → `Z24Unorm_S8Uint`.
  - Each has its own upload decoder (`renderer/metal/texture.cpp`).
  - Both map to Metal `Depth32Float_Stencil8`, share a cache group, and alias each other.
  - Stencil sampling through G (S8Z24 TIC) is remapped by `toSampledSwizzleChannels`.
- **Packed 16-bit TIC formats**:
  - A1B5G5R5 → `RGB5A1Unorm` (Metal `BGR5A1Unorm`), B5G6R5 → `R5G6B5Unorm`, A4B4G4R4 → `RGBA4Unorm`, A5B5G5R1 → `R1G5B5A5Unorm`.
  - Their channel order is fixed with a **value remap** of the swizzle (section 5).

**Converters and cache keys**
- **Converters made total**: invalid sampler filters, swizzles and TIC texture types no longer hit undefined behaviour. The TSC mag filter is 3 bits.
- **Texture shapes legalized for Metal** (`renderer::legalizeTextureShape`, used by `ThreeD::getTexture`; 2D engine descriptors with depth > 1 become 2D arrays):
  - several layers → array types;
  - level count clamped to what the size allows;
  - 1D textures have one level;
  - non-square cubes and sizes over 16384 are rejected.
- **Collision-safe caches**:
  - Texture group keys are exact packed values, not hashes.
  - Storages are probed past colliding keys with a descriptor check (`TextureCache::findStorageKey`/`servesRequest`/`getFreeStorageKey`). Views are probed with `TextureViewDescriptor::operator==`.
  - `CacheBase` gained `compare_descriptors` (section 5), enabled for samplers, pipelines, render passes, depth-stencil states and the clear color/depth pipelines.
  - `Z32Float` has its own group: an illegal Metal view from D32FS8 is no longer possible.

Tests (`tests/texture_format_tests.cpp`, run from texture-cache):
- `testSurfaceFormatConversions`, `testTicFormatConversions`;
- `testHostFormatCoverage`: every one of the 118 reachable guest formats creates a valid Metal texture under the validation layer;
- `testPackedFormatSwizzles`, `testZ24UploadLayouts`, `testBgr10A2RenderTarget`;
- `testTextureShapeLegalization`;
- `testStorageHashCollision`: brute-forces a real 32-bit collision;
- `testAliasQueries`;
- `testCacheCollisions` (in gpu-engine).

### 4.5 DMA, 2D, inline and memory

- **Inline-to-memory rewrite** (`engines/inline_base.*`), for 3D, compute and the A140 class:
  - LAUNCH_DMA starts a transfer and resets the buffer;
  - byte-exact (the last word can be partial);
  - pitch lines use PITCH_OUT;
  - block-linear destinations are swizzled;
  - writes can span several GPU mappings.
  - Test: `testInlineToMemory`.
- **DMA** (`engines/copy.cpp`):
  - the pitch destination flush/invalidate range honours MULTI_LINE_ENABLE;
  - empty multi-line copies no longer underflow.
- **2D** (`engines/2d.cpp`, `renderer/texture_cache.*`): the guest-memory copy workaround for compressed aliases now runs only when a compressed storage overlaps the source or destination and the source has no unflushed GPU writes. New queries: `overlapsCompressedStorage`, `hasUnflushedGpuWrites`.
- **Locks**:
  - `Compute::launch` takes the buffer-cache lock, in the same order as draws (buffer, then texture).
  - The GMMU is locked (`src/core/hw/generic_mmu.hpp`, `gmmu.cpp`; see section 5).
  - Tests: `checkMmuLookup` in `tests/xci_tests.cpp`, including a concurrent remap stress test.
- **nvdrv** (`src/core/horizon/services/nvdrv/ioctl/nvhost_as_gpu.*`):
  - ALLOC_AS_EX argument order fixed;
  - big page size checked;
  - GMMU reservations are zero-filled (`calloc`).
  - Not yet seen in a game.

---

## 5. Design decisions you must know

1. **Format naming.**
   - Hydra `TextureFormat` names list components from the least significant bits (DXGI/Metal style): `Z24Unorm_S8Uint` has depth in the low 24 bits.
   - Maxwell RT/TIC names in NVIDIA headers list them from the most significant bits.
   - Hydra's `DepthSurfaceFormat` names follow Mesa (LSB-first), so `S8Z24Unorm` = 0x14 = NVIDIA "Z24S8".
2. **Adding a `TextureFormat`.**
   - Append it at the end of the enum.
   - Add a row to `texture_format_infos` and update the `static_assert`, which names the last enumerator (currently `R1G5B5A5Unorm`).
   - Add a formatter entry and a Metal table entry.
   - The group key packs the format into 8 bits; a static_assert enforces fewer than 256 formats.
3. **Swizzles.**
   - `PixelFormatInfo::component_indices[guest component]` gives the host channel that holds it. `metal::TextureView` remaps swizzle **values** through it.
   - `SwizzleChannels(format, …)` only remaps G→R for `S8Uint_Z24Unorm`.
   - TIC views must use `toSampledSwizzleChannels(word, srgb)`, which also handles stencil reads of S8Z24 through G.
   - Do not reintroduce position permutations; the old code was wrong for non-identity swizzles.
4. **`CacheBase<…, compare_descriptors = true>`** (`src/common/types.hpp`): the subclass needs `static bool equals(a, b)` comparing **exactly the fields its `hash` uses**, with the same conditions. Otherwise you get spurious duplicates or lingering collisions.
5. **Texture cache storage keys** are hashes of the descriptor of the request that created the storage. A storage serves a request when ptr, layout and dimensions match and it has at least as many levels and layers (`servesRequest`). Use `findStorageKey` to look up and `getFreeStorageKey` to insert. Never call `group.cache.insert(descriptor.getStorageHash())` directly.
6. **GMMU** (`src/core/hw/generic_mmu.hpp`):
   - `findAddrImplRef` is gone. Use `findAddr(addr, base)`, which returns a `std::optional<Impl>` copy, or `findAddrImpl`.
   - Writers take the exclusive lock (`map`, `unmap`, `GMmu::createAddressSpace`/`unbindToSparse`, which call `mapLocked`).
   - Lookups use a 4-entry thread-local cache invalidated by a generation counter, because `unmapAddr` runs tens of thousands of times per frame.
   - Composite operations inside `GMmu` must call `mapLocked`, not `map`, while holding the lock.
7. **Hot reload.**
   - Functions with a `HotN` suffix are reached through trampolines in `src/core/hot/routes.cpp` and `routes.inc`. Changing a hot function's signature means updating both.
   - Any header change needs a full `hydra` rebuild and an app restart, because an ABI hash guards the module.
   - See `tests/README-hot-debug.md`.
8. **Index draws**: `drawIndexed` is called with start 0. The index buffer view already begins at FIRST.
9. **Decoded index buffers** use plain `new`/`delete` ownership (`shared_ptr` with the default deleter). A deleter calling the renderer's virtual `freeTemporaryBuffer` caused a pure-virtual call at shutdown.
10. **Shader tests** use `runAlu` in `tests/shader_coverage_tests.cpp`:
    - it decodes a straight-line program and runs it on Metal;
    - R0–R31, predicates and CC go in and out, with one cbuf slot;
    - it compiles with production options (fast math off).
    - Note that Metal flushes fp32 subnormals in comparisons; avoid subnormal test inputs.

---

## 6. Test status notes

All GPU suites pass (2026-10-17). The buffer-snapshot and thread-wait failures listed in earlier versions of this document were stale test expectations (4 KiB snapshot size classes; CancelSynchronization now needs `beginSyncWait()`), fixed in the tests.

`hydra-xci-tests FIXTURES` still ends with "XCI requires prod.keys" (needs real keys) and `--check-gpu-memory` aborts during system init on `DiskFile("")` (environment). Run it through `script -q log ...` if you need its output: it prints nothing when piped.

---

## 7. Audit findings: fixed vs open

**Fixed:**
- `textures.md`: D-1, D-2, D-3, D-4, D-6, D-7, D-11 (filters, swizzle, TIC type), D-15, D-19. Of P1-5, everything except G4R4 and R32G32B32.
- `3d_backend.md`: B1, B6, B9, B10, B15.
- `3d_frontend.md`: B1–B8, B10–B14, B26, B27.
- `shader_alu.md`: B1, B3–B9, B13–B16.
- `shader_mem_tex_flow.md`: C1–C5, C8, C10–C13, C22 (`reg_t`
  sequential operands stop at RZ).
- `compute_memory_sync.md`: B4–B8, B18.
- `2d_dma_inline.md`: B1–B4.
- `services_presentation.md`: B1, B2.

**Open, in priority order.** See `backlog.md` for the full list.

1. **CC writes and flow tests**: `shader_alu.md` B2, `shader_mem_tex_flow.md` C16. Confirmed in Crash 4.
2. **Sampler completeness** (`textures.md` D-8–D-10, D-21, P1-7):
   - LOD bias is needed by UE4 dynamic resolution (Crash 4 renders 640×360 inside 1280×720);
   - anisotropy;
   - CLAMP_OGL and MIRROR_ONCE_BORDER;
   - nearest filtering for integer textures;
   - border colours.
3. **3D back end**:
   - GL-encoded depth funcs 0x200–0x207 become Always (`3d_backend.md` B2);
   - constant and SRC1 blend factors (B3), blend reset values (B4), no blending on integer RTs (B5);
   - fragment outputs per the SPH omap (B7, and `shader_mem_tex_flow.md` C6); dual source (B8).
4. **3D front end** (`3d_frontend.md` B15–B25):
   - cull FRONT_AND_BACK, winding staleness, depth bias halving;
   - lower-left origin scissors, viewport-transform disable;
   - primitive restart, rasterizer discard, depth clamp;
   - compact/inline draws (B9).
5. **Remaining ALU items** (`shader_alu.md`):
   - PSETP second destination (B10), MOV immediate sign extension (B11), LEA.HI.X cbuf and IADD32I.PO decode masks (B12);
   - `fmin`/`fmax` for FMNMX NaN, `!` instead of `~` on bools, unsigned arithmetic in IADD3/ISCADD/LEA, `extract_bits` guards (§c).
6. **Decoder bugs** (`shader_mem_tex_flow.md`):
   - shuffles outside compute (C23);
   - mixed texture declaration types (C24).
7. **Cross-format texture copies** violate Metal's blit rule (`textures.md` D-5): views for same-size formats, buffers for compressed, compute for depth↔colour.
8. **Larger items**:
   - global memory/SSBO (LDG returns 0, STG/ATOM/RED missing; `compute_memory_sync.md` B3, `shader_mem_tex_flow.md` C7);
   - real GPU fences and syncpoints, plus host methods 0x00–0x3F, which are currently forwarded to engines (`compute_memory_sync.md` B1/B2, `services_presentation.md` B3);
   - remaining semaphore acquire/reduction/awaken/trap and asynchronous report publication (payload/ZPASS completion ordering is implemented);
   - buffer-queue and vsync rework (`services_presentation.md` B9–B14);
   - control flow CONT/PCNT/CAL/RET (C17);
   - SULD and typed SUST.D (D-20, C15).
9. **P2**: FP64 soft-float, video ops, geometry shader/tessellation emulation, logic ops, MSAA, transform feedback.

**Capability classification**: `tests/README-graphics-coverage.md` now records the implemented/tested subsets and the missing or partial families.

---

## 8. Risk areas for gameplay validation

If the user reports a visual regression after these changes, check these first:

1. **Z24 layout split** (section 4.4). If depth or stencil sampling breaks (shadows, SSAO, UE4 stencil copies), check which ZT format the game renders with versus its TIC. Both layouts alias the same storage, so this mainly affects uploads.
2. **Packed 16-bit TIC formats.** B5G6R5 is now `R5G6B5Unorm` and A1B5G5R5 is now `RGB5A1Unorm`, with a value remap. Watch UI and 2D colours.
3. **Index FIRST/limit and decode changes.** Watch for missing geometry or draws dropped by the limit clamp. A one-time warning is logged.
4. **Texture cache keys** (`servesRequest` probing, exact group keys, Z32F group split). Watch for textures that now upload from stale guest memory instead of reusing a render target.
5. **GMMU lookup cache and locks**: performance, and any stale mapping after remaps.
6. **Conversion semantics need NVIDIA/hardware revalidation.** The current F2I maps NaN to 0 even for unsigned results, and non-saturating I2I narrow results zero-extend. Metal result tests only confirm that Hydra implements those choices consistently.
7. **2D block-alias path restricted**: NVN texture streaming that copies BC blocks through RG32/RGBA32.
8. **Formats now unbound instead of aborting**: A8, 0x40–0x47, 0xFF, V8Z24 and the coverage depth formats log `NOT_IMPLEMENTED` once.

---

## 9. Debug code to strip before any commit

Some of this predates this work; see the user's notes. In the tree now:
- **Frame tracer**: `FrameTracer` (`frame_tracer.hpp`), `traceDraw`/clear tracing in `engines/3d.cpp`, `debugDumpTexture` call sites.
- **Unconditional `DEBUG` logs**:
  - `engines/2d.cpp` "DEBUG 2D copy";
  - `engines/copy.cpp` "DEBUG DMA" and the 2-second "DEBUG DMA summary";
  - `engines/3d.cpp` "DEBUG semaphore" and "DEBUG image view level";
  - `engines/compute.cpp` `HYDRA_DUMP_COMPUTE`;
  - `gmmu.cpp` `HYDRA_GMMU_CHECK`/`HYDRA_GMMU_FRESH`.
- **Kernel/loader debug from earlier sessions**: `HYDRA_TRACE_SVC`, `HYDRA_DUMP_NSO`, wait logging, the terminate backtrace hook.
- **Keep (env-gated, zero cost when unset)**: `HYDRA_SHADER_OVERRIDE_DIR`, `HYDRA_DUMP_SHADERS`, hot reload, and visual diagnostics (`gpu/visual_diagnostics.*`, `tools/visual_debug.py`).

Ask the user before removing anything they might still be using. Don't silently delete their tooling.
