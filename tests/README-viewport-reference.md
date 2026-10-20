# Viewport, scissor and window origin: Python model vs Hydra

`viewport_reference.py` writes random guest viewport transform, coordinate
swizzle, window origin, Z clip range, surface clip and scissor registers to
Hydra's 3D engine; `ThreeD::getViewport` and `ThreeD::getScissor` build the Metal
state, which `hydra-viewport-reference-tests` prints. A friend struct
(`ThreeDTestAccess`) gives the test access to these private functions.
The runner now emits viewport/scissor values from `prepareDrawState()` and
checks all 16 snapshot entries against the direct helpers before comparison
with the Python model. The independent model still exercises viewport 0 only.

```sh
ninja -C build/swiftui-release -j 6 hydra-viewport-reference-tests
curl -L --fail https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/3d/clb197.h -o /tmp/hydra-nvidia-clb197.h
python3 tests/viewport_reference.py build/swiftui-release/bin/hydra-viewport-reference-tests \
  --header /tmp/hydra-nvidia-clb197.h
```

## Covered (20 October 2026: PASS, 500 configurations)

Surfaces of 300x200 to 1920x1080, random scales and offsets (X, Y, and Z from a
random depth range), NEG_X / NEG_Y coordinate swizzles, window origin upper left
and lower left, both Z clip ranges (`NEGATIVE_W_TO_POSITIVE_W`, `ZERO_TO_POSITIVE_W`),
enabled and disabled scissors. For 75 normalised device points per
configuration the model's window position (x, row from the top, depth) must equal
the position Metal's viewport transform gives for the viewport Hydra built; the
scissor rectangle must equal the model's pixel range (flipped for a lower-left
origin). 16 offsets and values audited against the header.

Control: a model that ignores the lower-left origin fails every lower-left case.

## Conventions, not NVIDIA text

The header only names the registers. Used here: `x_win = offset_x + scale_x * x`,
rows counted from the bottom with a lower-left window origin, and the depth
formula `z_win = offset_z + scale_z * z`, with `z` in -1..1 or 0..1 depending on
the Z clip range (Hydra remaps the clip space of the other range in the vertex
shader). Depth ranges outside 0..1 are clamped by Hydra and not asserted.

Not covered: viewport swizzles that swap axes (not supported), `FLIP_Y` (only
used for the winding), viewports 1-15, the viewport depth range registers
(`SET_VIEWPORT_CLIP_MIN/MAX_Z`) used when the viewport transform is disabled,
and the rasterised result of a draw.
