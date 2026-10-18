# Light-shaft alpha regression

Run after building `hydra-blend-reference-tests`:

```sh
curl -L https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/3d/clb197.h -o /tmp/clb197.h
python3 tests/lightshaft_blend_reference.py \
  build/swiftui-release/bin/hydra-blend-reference-tests \
  --nvidia-header /tmp/clb197.h --report /tmp/lightshaft-blend.json
```

Python checks 24 global/per-target OGL/D3D operation/factor encodings against
the [NVIDIA Maxwell header](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197.h).
It computes the captured occlusion equations independently:

```
RGB = source RGB + destination RGB * (1 - source alpha)
alpha = destination alpha - source alpha
```

The C++ runner resolves the raw registers with the same `getBlendEquation`,
`getBlendOperation`, and `getBlendFactor` functions used by production 3D,
draws a pixel on the Metal GPU, and reads the actual target bytes. Python
compares all four channels after RGBA8Unorm/half-float storage conversion.
The 1536 cases cover global/per-target structures, both enum encodings,
flag values 0/1/2, 16 source-alpha values, and four destination-alpha values.
Metal API validation is enabled; hot reload is disabled for the runner.

The regression was a draw-time selection that replaced explicit alpha
REVERSE_SUBTRACT/ONE/ONE with RGB ADD/ONE/ONE_MINUS_SRC_ALPHA when the
separate-alpha flag was zero. With an empty UNORM mask and positive source
alpha, the former remains zero; the latter incorrectly creates a light source.
The radial blur samples that mask's alpha and spreads it into white shafts.

The header names the separate-alpha flag without documenting its full
side effects. This change rolls back an unsupported draw-time interpretation;
it does not establish register-write propagation behavior of that flag.
The independent [deko3d state emitter](https://github.com/devkitPro/deko3d/blob/master/source/maxwell/gpu_3d_state.cpp)
also programs color and alpha operation/factor registers explicitly in
`dkCmdBufBindBlendStates`. Neither that source nor a passing scalar comparison
substitutes for an NVIDIA hardware capture.

Limits: this is a mathematical NVIDIA-register reference compared to Apple
Metal execution, **not execution on a physical NVIDIA GPU**. It does not replay
the guest shader, stencil occlusion, texture sampling, or the complete blur
chain. The runner constructs the Metal blend descriptor directly, so renderer
pipeline-cache behavior still requires gameplay validation. Existing generic
blend/HDR tests remain in `blend_reference.py`.

For live captures, RGBA8 debug dumps now include `<image>.png.rgba8`: exact
row-major RGBA bytes, width × height × 4, before preview alpha is forced opaque.
Use this sidecar rather than the PNG alpha channel when checking mask values.
