# 1D, 2D array, 3D and cube textures: Python model vs Hydra

`texture_type_reference.py` uses the sampler path of
[README-sampler-reference.md](README-sampler-reference.md) (random TSC words,
`decodeSampler`, the Metal sampler, a decoded guest `TEXS`) with texture types a
TIC can describe besides plain 2D.

```sh
ninja -C build/swiftui-release -j 6 hydra-maxwell-alu-reference-tests
python3 tests/texture_type_reference.py build/swiftui-release/bin/hydra-maxwell-alu-reference-tests
```

## Covered (20 October 2026: PASS, 2400 samples)

| Type | TEXS target | Samples | What is checked |
|---|---|---|---|
| 1D | 1D.LZ | 600 | point and linear taps, wrap / mirror / clamp, widths 3-16 |
| 2D array | 2D array.LZ | 600 | layer chosen from an integer register, point and bilinear, 2-4 layers |
| 3D | 3D.LZ | 600 | point and trilinear taps, the three address modes U, V and P, 2-5 slices |
| cube | Cube LL | 600 | face selection by the major axis, texel inside the face |

Controls: flipping one axis of the +Y cube face in the model fails 98 cube samples.

## Bug found and fixed

Sampling a **1D texture** with `TEXS 1D.LZ` generated MSL that did not compile:
the coordinate is an IR vector of one component (`vec<f32,1>`) and
`texture1d::sample` takes a `float`. The emitter now passes `(coords).x` for 1D
textures (`MslEmitter::emitTextureSample`). Any shader that sampled a 1D texture
failed to build before. 2D, array, 3D and cube textures were unaffected.

## Conventions, not NVIDIA text

Models are the textbook ones: samples at texel centres, the usual cube face table
(+X: sc=-z tc=-y; -X: sc=z tc=-y; +Y: sc=x tc=z; -Y: sc=x tc=-z; +Z: sc=x tc=-y;
-Z: sc=-x tc=-y) with row 0 at the top, bilinear and trilinear weights within a
0.02 tolerance (GPUs quantise weights). Cube cases use point sampling away from
face edges and texel boundaries; seam filtering (TSC `CUBEMAP_INTERFACE_FILTERING`)
is not asserted. Array and cube layers are in range. Border colours are not used
for these types (Hydra emulates borders for 2D only).

Not covered: cube arrays, 2D array and 3D mip chains, 3D arrays, texel offsets
on these types, implicit LOD, depth compare on cubes and arrays, and the TLD4
(gather) and TLDS (texel fetch) instructions on non-2D types.
