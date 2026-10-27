# 2D blit and inline-to-memory: Python models vs Hydra

Two more engines of stage B, checked like the DMA copy engine
([README-copy-engine-reference.md](README-copy-engine-reference.md)): random
memory images plus register scripts go through Hydra's engines, the memory
afterwards is compared byte for byte with plain Python models of the documented
fields. Registers are audited against NVIDIA's `cl902d.h` and `cla140.h` first.

```sh
ninja -C build/swiftui-release -j 6 hydra-blit-2d-reference-tests hydra-copy-engine-reference-tests
curl -L --fail https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/twod/cl902d.h -o /tmp/hydra-nvidia-cl902d.h
curl -L --fail https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/inline-to-memory/cla140.h -o /tmp/hydra-nvidia-cla140.h
python3 tests/blit_2d_reference.py build/swiftui-release/bin/hydra-blit-2d-reference-tests --header /tmp/hydra-nvidia-cl902d.h
python3 tests/inline_to_memory_reference.py build/swiftui-release/bin/hydra-copy-engine-reference-tests --header /tmp/hydra-nvidia-cla140.h
```

## 2D engine (class 902D): PASS, 500 cases

- 50 register offsets and fields audited (destination and source surfaces,
  clip rectangle, ROP, operation, sample mode, the PIXELS_FROM_MEMORY block).
- Metal blit path (SRCCOPY, one slice): 300 cases over R8, A8B8G8R8 and RGBA16F,
  source and destination pitch or block-linear in all four combinations, scale
  steps of 1, 2, 0.5, -1, -2, 0.25, 1.5 and 0.75 on each axis (up, down, mirrored),
  random destination rectangles, source offsets and optional clip rectangles.
- CPU path (ROP and volume copies): 200 cases with the 16 pattern independent
  ternary ROPs, destination layers of 1-3 slice volumes and source rectangles
  that run off the surface (edges clamp).
- Controls: sampling pixel corners instead of centres in the model fails the
  run; without the fix below 120 of 300 Metal cases failed.

**Fix found by the test:** with point sampling the engine takes the source texel
floor(SRC_X0 + (i + 0.5) * DU_DX). Downscaling by an integer factor puts those
positions exactly on texel boundaries, and the Metal blit (normalised float
coordinates) landed on either side of them: 2x downscaled blits picked the wrong
texel in about 40% of cases. Nearest blits now nudge the source origin by 1/512
of a texel (`Renderer::blitTexture`), which resolves boundary positions like the
exact fixed point arithmetic. 1:1 copies and filtered blits are not affected.
The rounding can differ from exact only for positions that fall within 1/512
texel below a boundary, which needs a step with a large denominator.

**Fix for the harness' sake:** `TextureCache::updateImpl` and the write
protection call dereferenced `GMmu::getMmu()` after only checking the GMMU; they
now check the MMU too (a GMMU without MMU only exists in tests).

Convention, not NVIDIA text: sampling at pixel centres (ORIGIN_CENTER, point
filter). The header has no prose. ORIGIN_CORNER, bilinear filtering, colour keys,
blend operations and the depth-stencil / BC alias copies are not asserted.
Block heights taller than the image needs are not tested (Hydra reduces them as
it does for mip levels).

## Inline-to-memory (class A140 registers at 0x60-0x6d): PASS, 200 cases

Driven through the Maxwell 3D class, where Hydra implements the engine: line
length and count, pitch destinations (107 cases, row pitch larger than the line)
and block-linear destinations (93 cases, random width, height, origin, block
height and depth) with payloads delivered as LOAD_INLINE_DATA words, including a
partial last word. 17 registers and the LAUNCH_DMA layout bit audited. Control:
a model that is one row off fails every block-linear case.

Not asserted: data words beyond LINE_LENGTH_IN * LINE_COUNT, SET_DST_LAYER and
depth (Hydra leaves the slice to the address), LAUNCH_DMA completion types,
semaphores and interrupts (not implemented).
