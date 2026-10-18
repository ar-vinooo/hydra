# Sampler border colors (TEX D-9)

Status: IMPLEMENTED, HOST-VERIFIED by focused Metal result tests on 2026-10-18.
Exact guest semantics are still PARTIAL pending NVIDIA hardware evidence.

## Guest semantics and evidence

| Item | Class | Evidence |
|---|---|---|
| Four 32-bit TSC border fields and separate packed sRGB R/G/B fields | CONFIRMED ENCODING | NVIDIA `clb097tex.h`: `TEXSAMP2..7` border fields |
| BORDER, CLAMP_OGL and MIRROR_ONCE_BORDER address-mode values | CONFIRMED ENCODING | NVIDIA `clb097tex.h`: `TEXSAMP0_ADDRESS_{U,V,P}` |
| Integer textures interpret the border words as raw component bits | UNVERIFIED GUEST SEMANTICS | Current behavior passes Hydra's Metal tests; NVIDIA header names the fields but does not define the conversion rule |
| Depth compare uses the first border component | UNVERIFIED GUEST SEMANTICS | Current Metal implementation is tested; not established by NVIDIA documentation |
| CLAMP_OGL selects border or edge according to filtering | UNVERIFIED GUEST SEMANTICS | Encoding is documented; exact transition behavior still needs an NVIDIA trace |
| MIRROR_ONCE_BORDER mirrors once (`|u|`) and then uses the border | PARTIALLY CONFIRMED | NVIDIA documents the mode name; coordinate-boundary behavior needs hardware confirmation |
| Border color bypasses the TIC component swizzle | UNVERIFIED GUEST SEMANTICS | Current shader behavior is tested only against its host-side oracle |

## Implementation

Metal only has transparent black, opaque black and opaque white. Anything else is emulated in the shader.

- `renderer/const.hpp`: `SamplerDescriptor::integer_texture`, `isSrgbTextureFormat`, `getSamplerBorderPreset` (exact presets only; integer: zero only; compare: first component 0 or 1.0), `samplerNeedsBorderEmulation` (custom color on a border axis, or any mirror-to-border axis), `getEffectiveSamplerAnisotropy`.
- `engines/3d.cpp` `decodeSampler(tsc, format)`: resolves the integer/sRGB border, applies the integer nearest overrides, zeroes the color for samplers that never address the border (shared samplers and shader variants). `getSampler` memoizes per integer/sRGB variant. `specializeShaderSampler` (used by graphics and compute) records the LOD bias and `GuestShaderState::border_samplers` for emulated bindings; both feed the shader cache key.
- `metal/sampler.cpp`: exact presets only, otherwise transparent black. Old nearest-preset heuristic removed.
- `msl/emitter.cpp`: for an emulated binding the guest sampler is baked into `constexpr sampler hydra_bs<N>_{p0,pw,c0}` (filters, mip filter, LOD clamp, anisotropy, addressing, compare). Mirrored axes use `abs()` on the coordinate with an edge/border mode.
  - Float color: `sample(p0)` (transparent border) and `sample(pw)` (white border) differ by `1 - coverage` in every channel; result = `p0 + coverage * border`. Exact for any filter, LOD, anisotropy, 1D/2D/3D, arrays and offsets because Metal's filtering is linear in the texels.
  - Depth compare: `sample_compare(c0)` plus the plain depth samples for coverage; `compared + coverage * (cmp(ref, border) - cmp(ref, 0))`.
  - Gathers: same per lane (component border for color, compare delta for depth).
  - Integer: nearest, `inside ? sample : border bits` with the coordinate range test.

## Limitations (not silently dropped)

- Integer gathers with a border, and texel offsets on integer border samples, are logged `NOT_IMPLEMENTED` and fall back to the bound sampler / ignore the offset in the border test.
- Mirror-to-border combined with texel offsets: the offset is applied after the mirroring (logged).
- Coverage is `max` over channels of `white - base`; with very large (HDR) texel values float cancellation reduces accuracy.
- Cube maps are seamless on Metal; USE_WRAP (non-seamless) remains unmodeled (TEX F20).
- Compare sampling without a comparing TSC sampler combined with a border falls back to the bound sampler (logged).
- Each distinct border color produces a distinct shader variant; animated border colors would recompile shaders.
- Metal's depth border preset values (0 / 1) are assumed for depth textures with presets.

## Tests

- `texture_format_tests`: `testSamplerBorders` (presets, integer, depth, sRGB, mirror-to-border, unused border).
- `shader_library_tests`: `testTextureBorderEmulation` (decoded TEX with a custom border, linear and nearest, coverage 0 / 0.5 / 1 per axis).

Both suites pass with `MTL_DEBUG_LAYER=1` as part of the 2026-10-18
continuation. These tests validate the Metal implementation against Hydra's
expected values; they do not substitute for NVIDIA hardware-semantic evidence.

Gameplay validation pending. Header changes (`const.hpp`, `shader_cache.hpp`, `3d.hpp`) require a full `hydra` rebuild and restart (hot-reload ABI).
