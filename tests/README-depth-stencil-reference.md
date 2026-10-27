# Python depth/stencil reference

`depth_stencil_reference.py` simulates two successive fragments per case with
independent scalar arithmetic. `hydra-depth-stencil-reference-tests` executes
those cases through Hydra's Metal renderer and reads back color, float32 depth,
and 8-bit stencil. Values are compared exactly; the selected depth values are
exactly representable in float32.

```sh
ninja -C build/swiftui-release -j 6 hydra-depth-stencil-reference-tests
curl -L --fail https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/3d/clb197.h -o /tmp/hydra-clb197.h
python3 tests/depth_stencil_reference.py build/swiftui-release/bin/hydra-depth-stencil-reference-tests --nvidia-header /tmp/hydra-clb197.h --report /tmp/hydra-depth-stencil-report.json
```

The header argument is optional for offline runs; when provided it verifies
32 NVIDIA enum values used by the test. The Python runner enables Metal
validation and clears the diagnostic `HYDRA_DEBUG_DEPTH_ALWAYS` override.

## Independent expected results

The oracle applies stencil comparison first, then depth comparison. It selects
the stencil-fail, depth-fail, or pass operation, applies the low eight write-mask
bits, and updates color/depth only when the fragment passes. Depth writes are
suppressed when depth testing is disabled, even if the write-enable bit is set.
The second draw changes depth/color and the stencil reference, exercising
retained GPU state as well as the equations.

The C++ runner uses `Renderer::draw`, actual render-pass and pipeline creation,
the depth/stencil state cache, dynamic reference commands and the command-buffer
encoder transitions. `decodeStencilOp` was extracted unchanged from the engine's
local lambda so the same production NVIDIA→Metal operation decoder is exercised.
It does not reimplement the mapping in the test. Direct renderer input bypasses
the 3D register-selection code and Maxwell shader decoding; test shaders use MSL.

## Evidence and limits

- [NVIDIA clb197.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197.h):
  `SET_DEPTH_FUNC_V_{OGL,D3D}_*` and
  `SET_STENCIL_OP_FAIL_V_{OGL,D3D}_*` provide the guest encodings and names.
- [OpenGL 4.6 core specification](https://registry.khronos.org/OpenGL/specs/gl/glspec46.core.pdf),
  sections 17.3.3 and 17.3.4, provides the independent stencil/depth equations and
  operation ordering. This API reference and the NVIDIA enum names do not replace
  a GM20B hardware trace. Reference values in the final cases lie within 0–255;
  guest register behavior for higher reference values is not inferred from GL's
  API-level clamping rule.
- [Apple MTLDepthStencilDescriptor](https://developer.apple.com/documentation/metal/mtldepthstencildescriptor),
  installed SDK definitions and Metal validation provide host evidence.

On Apple M1, 18 October 2026:

| Group | Result |
|---|---|
| Depth: 8 functions × OGL/D3D encodings, enable/write combinations, equal/near/far/boundary inputs, encoder continuity | 896/896 |
| Stencil: all 8 operations, 3 outcomes, saturation/wrap edges, write masks, both encodings | 2880/2880 |
| Stencil comparison: functions, read/write masks, changed reference, encoder continuity | 2880/2880 |
| Total | **6656/6656** |

Sensitivity check: changing the production increment-wrap decoder to return
increment-clamp caused **180 mismatches**, with 6476/6656 passing and exit status
1. The original decoder was then restored. No new depth/stencil behavior defect
was found in the tested subset.

The target is D32Float+S8 with one sample, equal front/back stencil state and
fixed in-range depth. Different front/back state selection, bias, clipping,
D16/D24 rounding, MSAA, explicit fragment depth, depth texture sampling, shader
arithmetic, reflection, fog and bloom remain outside this test. A passing result
does not identify the cause of the white waterfall effect or certify all 3D.
