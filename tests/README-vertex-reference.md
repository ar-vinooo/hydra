# Python reference for vertex colors and attribute conversion

The Python oracle computes expected values without calling Hydra conversion
helpers. The C++ executable accepts raw Maxwell `SET_VERTEX_ATTRIBUTE_A` words
and vertex bytes, generates vertex and fragment MSL from IR, creates the actual
Hydra Metal pipeline, draws, and returns RGBA32Float pixels. Python compares the
pixels against normalization, signed conversion, float decoding, component
defaults, and R/B swapping.

## Run

```sh
ninja -C build/swiftui-release -j 6 hydra-vertex-reference-tests
curl -L --fail https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/3d/clb197.h -o /tmp/hydra-clb197.h
python3 tests/vertex_reference.py build/swiftui-release/bin/hydra-vertex-reference-tests --nvidia-header /tmp/hydra-clb197.h
```

The header argument is optional for offline use. When supplied, Python checks
26 encoding fields/enum values against that file before running GPU cases.
Metal API validation is enabled by the Python runner. It also clears the
diagnostic `HYDRA_NO_VERTEX_SWAP` switch for the test subprocess.

## Sources and strength of evidence

- [NVIDIA clb197.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197.h),
  `SET_VERTEX_ATTRIBUTE_A`: stream, source, offset, component widths, numerical
  type, and `SWAP_R_AND_B`. The header confirms the bit layout and enum names.
  Interpreting `SWAP_R_AND_B` as exchanging the resulting R/B components,
  including defaults for narrow inputs, is an inference from the field name.
- [OpenGL 4.6 core specification](https://registry.khronos.org/OpenGL/specs/gl/glspec46.core.pdf),
  sections 2.3.5 and 10.2: normalized fixed-point equations, vertex conversion,
  and component defaults. UNORM is `v / (2**b - 1)`; SNORM is
  `max(-1, v / (2**(b-1) - 1))` after sign extension. This is an independent
  numeric reference, not proof that every GM20B path has identical semantics.
- [Apple MTLVertexFormat](https://developer.apple.com/documentation/metal/mtlvertexformat)
  and [uchar4Normalized](https://developer.apple.com/documentation/metal/mtlvertexformat/uchar4normalized),
  the installed SDK enum definitions, and execution with Metal validation are
  the host references. Values are read back from actual Metal vertex fetch;
  they are not produced by a second Python implementation of Metal.

## Results on Apple M1, 18 October 2026

| Check | Before fix | After fix |
|---|---|---|
| Numeric vertex cases | 1104/1601 match | 1601/1601 match |
| Shader fallback R/B swap | 497 mismatches | 0 mismatches |
| CTR input bytes `(93, 0, 255, 0)`, UNORM8x4, swap disabled | Correct | Correct |

The bug was in `LangEmitter::emitMainFunctionPrologue`: the backend hook swapped
`__in` after the input had already been copied into the guest attribute register
array. Moving the backend assignments before that copy makes the swap visible.
UNORM8x4 uses Metal's BGRA vertex format and already worked; it must not be
swapped a second time by the shader. The existing diagnostic switch remains.

The CTR sample reaches the fragment output as approximately
`(0.3647058904, 0, 1, 0)`, preserving its blue/purple color. That case passed
before the fix as well, so this result does not identify the white glow's cause.

Coverage:

- 1–4 components: SNORM/UNORM 8/16-bit; SINT/UINT and signed/unsigned scaled
  8/16/32-bit; finite Float16/Float32 including negative values and HDR.
- Both R/B swap settings, missing components, nonzero buffer stream/byte offset,
  and a nonzero VS→FS varying location (`locn2`).
- Every possible input byte for UNORM8x4, distinct channel patterns, and signed
  and unsigned normalized 10:10:10:2 boundary values.

Floating comparison allows a few float32 ULPs for normalization and varying
interpolation. All vertices receive identical bytes, so this does not test
interpolation across different colors. The shaders start from IR: Maxwell
instruction decoding, real game shader arithmetic, texture sampling, blending,
instancing, packed aliases such as A8/X8B8G8R8, nonfinite/subnormal floats, and
unsupported normalized 32-bit vertex formats are outside this test.

The app build and `hydra-shader-library-tests` and `hydra-gpu-engine-tests` also
passed with Metal validation. No game or GUI was run.
