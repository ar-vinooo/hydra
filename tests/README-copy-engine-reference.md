# DMA copy engine (B0B5): Python model vs Hydra

`copy_engine_reference.py` applies the documented fields of the copy class to
random memory images with plain Python loops and compares the result byte for
byte with what Hydra's `Copy` engine leaves behind. `hydra-copy-engine-reference-tests`
drives the engine through `Gpu::subchannelMethod`, the same way a channel does.

```sh
ninja -C build/swiftui-release -j 6 hydra-copy-engine-reference-tests
curl -L --fail https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/dma-copy/clb0b5.h -o /tmp/hydra-nvidia-clb0b5.h
python3 tests/copy_engine_reference.py build/swiftui-release/bin/hydra-copy-engine-reference-tests \
  --header /tmp/hydra-nvidia-clb0b5.h
```

## Covered (20 October 2026: PASS, 12 categories, 592 cases)

- 45 register offsets and bit fields audited against `clb0b5.h`: LAUNCH_DMA
  (transfer type, flush, semaphore, interrupt, layouts, multi-line, remap),
  SET_REMAP_COMPONENTS (selects, constants, NO_WRITE, sizes, counts), offsets,
  pitches, line length/count, and the SET_SRC/DST block size, size, layer and origin.
- Pitch to pitch, plain (60 cases) and remapped (150): every select (source
  components, CONST_A/B, NO_WRITE), component sizes 1-4, 1-4 source and
  destination components, multi-line with different pitches.
- Semaphore release, one and four words, with and without a copy (12).
- Tiled copies pitch to block linear, block linear to pitch and block linear to
  block linear (150 without remap, 75 with an identity remap) with random
  widths, heights, depths 1-4, block heights 1-8 and block depths 1-4 GOBs,
  X/Y origins, starting layers and more lines than the surface holds.
- Tiled copies with a general remap (120): selects, constants, NO_WRITE and
  different source and destination component counts. Hydra used to ignore the
  remap on tiled copies (an `ASSERT_DEBUG` only, so a release build copied
  bytes): all 120 cases failed. `Copy::executeCopy` now remaps element by
  element for any mix of layouts and the cases pass.

Controls: swapping the two remap constants in the model fails 95 of 150 remap cases.

## What is a convention, not NVIDIA text

`clb0b5.h` names the fields and offsets but has no prose. These come from common
driver behaviour and Hydra's own earlier documented tests, not from NVIDIA:

- With REMAP_ENABLE, LINE_LENGTH_IN counts elements of NUM_SRC/DST components of
  COMPONENT_SIZE bytes, and the SET_SRC/DST_WIDTH values count elements as well.
- LINE_COUNT walks the Y rows of a slice and then the Z slices of a block-linear
  surface; the pitch side of a multi-slice copy keeps the full slice height. The
  model only uses a Y origin when the surface has a single slice, so the
  ambiguous case is not asserted.
- A select naming a source component the element does not have, and the bytes of
  a four-word semaphore other than the payload and the timestamp, are not defined
  and not compared (the timestamp is zeroed before comparing).
- Rectangles that do not fit the surface, unmapped or split spans and
  `FLUSH`/interrupt/physical-address bits are covered by `dma_slice_tests`, not here.

Not covered: the 2D blit and inline-to-memory engines, the quarter-GOB block
width, GOB heights other than 8 and semaphore reductions.
