# Guest sampler (TSC) behaviour: Python model vs Hydra

`sampler_reference.py` builds random TSC words (the 8-word NVIDIA TEXSAMP layout)
and sends them through Hydra exactly as a draw does: `decodeSampler`,
`specializeShaderSampler` (LOD bias, border emulation) and the Metal sampler.
A guest `TEXS 2D.LL` shader (decoder, MSL, Metal) then samples a mip mapped
RGBA32Float texture with an explicit LOD. The result is compared with a
plain-Python model.

```sh
ninja -C build/swiftui-release -j 6 hydra-maxwell-alu-reference-tests
curl -L --fail https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/3d/clb197tex.h -o /tmp/hydra-nvidia-clb197tex.h
python3 tests/sampler_reference.py build/swiftui-release/bin/hydra-maxwell-alu-reference-tests \
  --header /tmp/hydra-nvidia-clb197tex.h
```

## Covered (20 October 2026: PASS, 3000 samples in 500 random sampler setups)

- 29 TSC fields and enum values audited against `clb197tex.h` (address modes,
  filters, mip filter, LOD bias and clamps, border colour words).
- All eight address modes on each axis (wrap, mirror, clamp to edge, border,
  clamp OGL, mirror once to edge / border / OGL), with point and bilinear taps;
  custom float border colours, including the transparent / opaque black / white
  presets that Metal can express natively and arbitrary ones that Hydra
  emulates in the shader.
- Point and bilinear magnification and minification (chosen by the clamped LOD),
  mip filter none / nearest level / trilinear, sampler min and max LOD clamps,
  textures of 8x8, 8x4, 4x8, 7x5 and 16x2 texels with 3-5 levels.
- Controls: choosing the wrong filter side fails 279 samples; applying the bias
  to explicit LODs fails 386.

## Findings and open questions

- **LOD bias and explicit LOD (open question).** Hydra adds the sampler's
  MIP_LOD_BIAS to implicit LODs, gradients and the TEX.LB bias only; an explicit
  LOD (TEXS.LL, level(x)) is used as given. The header does not say whether
  hardware adds the bias to an explicit LOD (OpenGL's texture-object bias does).
  The test encodes Hydra's behaviour; the flag `APPLY_BIAS_TO_EXPLICIT_LOD` flips
  the model. Needs a hardware trace or documentation to settle.
- **MIRROR_ONCE_BORDER seam (known gap, 44 samples).** Hydra emulates this mode
  by mirroring the continuous coordinate and sampling with a border sampler, so
  a bilinear tap at index -1 beside the mirror seam reads the border colour
  instead of texel 0 (what the per-tap definition of MIRROR_ONCE, and Hydra's own
  mode 5, give). The run reports these samples separately and does not fail on
  them. Only linear taps within about a texel of the seam are affected.

## Conventions, not NVIDIA text

The header has field names but no prose. Used here: LOD clamps are unsigned 4.8
fixed point and the bias a signed 13-bit value over 256; CLAMP_OGL blends with the
border when the active filter is linear and clamps otherwise; MIRROR_ONCE maps
indices -n..-1 to 0..n-1 once; MIRROR_ONCE_CLAMP_OGL behaves like
MIRROR_ONCE_CLAMP_TO_EDGE. Cases with the OGL modes use equal minification and
magnification filters. Bilinear and trilinear weights are compared with a 0.015
tolerance (0.02 with emulated borders) because GPUs quantise the weights.

Not covered: anisotropic filtering, depth compare (see
`maxwell_texture_reference.py`), sRGB border colours, integer textures, implicit
LOD (needs fragment derivatives) and non-2D texture types.
