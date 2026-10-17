# Graphics continuation — 2026-10-17

The request covers the entire master backlog. It is not complete. No commits, staging, game launches or GUI actions were performed. Existing changes to the viewport workaround and shader fixtures were preserved.

## Implemented

- FMZ handling is wired through the FMUL/FMUL32I, FFMA/FFMA32I, HMUL2 and HFMA2 operand variants. Zero multiplicands suppress NaN/infinity products; FMA retains the addend. Saturating instructions retain the reference saturation path. Half instructions apply the check independently to both lanes.
- BFE signed offsets >=32 replicate the source sign, including BREV; count zero remains zero. BFE.CC now writes Z/S and clears C/O. IADD32I.PO is recognized by the decoder. Existing u32 IADD3/ISCADD/LEA paths gained overflow result tests.
- Inline index methods 0x57A, 0x57C and 0x4C1 collect full-width, two packed u16 or four packed u8 indices. END consumes them once through the regular indexed-draw implementation, with input type u32 and an inline range instead of guest FIRST/limit addressing. BEGIN resets pending inline data. Discarded draws can return before guest-memory tracking is touched.
- PEXIT tracks nested exit destinations through control-flow blocks. Conditional exits preserve the target on the untaken path. Immediate JMP uses absolute byte addresses, predicates and CC flow tests. Shader state/cache identities retain the program offset, and the decoder accounts for the graphics header when resolving absolute destinations.
- 2D clipping intersects the guest clip rectangle and destination bounds using wide arithmetic. Source coordinates and extents retain fractional scaling and mirror signs. The compressed-alias guest-memory fallback now receives the clipped destination region and only handles integral nonnegative source coordinates. CORNER uses the reference whole-source-texel derivative correction; a derivative of 1 keeps the existing 1:1 alignment.
- Unnormalized TIC coordinates specialize graphics and compute shaders. TEX/gather coordinates and explicit TXD gradients are divided by texture dimensions; TMML uses normalized coordinates for LOD queries. Buffer reads retain integer addressing, and cube samples retain direction coordinates. Shader keys include this state.

## References

The local reference directory recorded in the handoff README is still available. Semantics were checked against:

- NVIDIA `clb197.h`: inline index packing, BEGIN/END and restart method offsets.
- NVIDIA `cl902d.h`: clip rectangle offsets and sample-origin encodings.
- Eden `engines/draw_manager.cpp`: inline accumulation and END submission.
- Eden `engines/fermi_2d.cpp`: signed fixed-point CORNER correction `(du_dx >> 33) << 32`, preserving integral texel alignment.
- Eden shader translators `floating_point_multiply.cpp`, `floating_point_fused_multiply_add.cpp`, `half_floating_point_multiply.cpp`, `half_floating_point_fused_multiply_add.cpp`, `bitfield_extract.cpp`.
- Eden Maxwell `control_flow.cpp` / `instruction.h` and `shader_environment.cpp`: PEXIT tokens, absolute JMP addresses and graphics-program offsets.

## Validation

Final build passed for `hydra` and the nine test targets using `ninja -C build/swiftui-release -j 6`. All suites exited 0 with `MTL_DEBUG_LAYER=1`: gpu-engine (15 PASS groups), shader-library (50), texture-cache (29), raster-state, render-hash, register-write, frame-trace, visual-diagnostic and buffer-snapshot. `git diff --check` passed. Logs are `/tmp/hydra-all-final-build.log` and `/tmp/hydra-all-final-<suite>.log`.

No Metal validation errors were reported. Existing linker warnings about Homebrew dylib deployment targets and duplicate libc++ linkage remain. XCI/key-dependent checks were not rerun.

Added tests check FMZ GPU results, signed BFE edge fields/CC, IADD32I.PO, wrapping ALU, nested/conditional PEXIT, predicated JMP and relocation, unnormalized TEX/TXD GPU results, TMML fragment compilation, shader key changes, inline method packing/lifecycle, restart register offsets, 2D clipping/mirroring/empty rectangles and CORNER 1:1 alignment.

Inline method tests disable rasterization; 2D region tests check coordinate calculations. These are not pixel-coverage or gameplay validations. The rebuilt app must be restarted because engine/shader state layouts changed.

## Explicitly open

See the master backlog for the authoritative list. Major unfinished work includes global memory/SSBO with writable-buffer coherence; completion-ordered report semaphores and real occlusion counts; exact sampler border emulation; remaining draw/restart/alignment modes and transform feedback/DRAW_AUTO; 2D ROP and slice semantics; DMA slice coverage; additional shader flow/gradient variants; buffer-queue/vsync redesign; FP64 arithmetic, video operations, geometry/tessellation, logic ops, MSAA/coverage/sample shading; and expanded G4R4/RGB32 upload/readback.

Broad capability classifications and test limits are recorded in `tests/README-graphics-coverage.md`. Passing the current suites does not establish implementation of the open features.
