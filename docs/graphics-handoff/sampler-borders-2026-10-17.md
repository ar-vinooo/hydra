# Sampler border colors (TEX D-9)

Status: IMPLEMENTED_UNVERIFIED. Written without building or running anything (project rule); tests were added but have not been executed.

## Guest semantics and evidence

| Item | Class | Evidence |
|---|---|---|
| Border = 4 raw 32-bit words in the TSC (0x10..0x1C), float for float formats | CONFIRMED | `clb097tex.h`, Eden `TSCEntry::border_color` |
| Separate 8-bit sRGB border (R/G/B at TSC 0x08 bits 24-31, 0x0C bits 12-19 / 20-27), alpha shared; used when `srgb_conversion` is set and the texture is sRGB | STRONGLY INFERRED | Eden `SrgbBorderColor()`, `Sampler::MakeKey` (`srgb_border`), `has_srgb_border_color` |
| Integer textures: border words are raw component bits | STRONGLY INFERRED | Eden keeps the raw words and forces nearest filtering for integer formats (`force_nearest`); no float conversion exists in Eden |
| Depth compare: border depth is the first component | STRONGLY INFERRED | Vulkan custom border (Eden passes the same words); not documented by NVIDIA |
| CLAMP_OGL border vs edge by filter | CONFIRMED (Eden) | already implemented |
| MIRROR_ONCE_BORDER = mirror once (`|u|`), then border (GL `MIRROR_CLAMP_TO_BORDER_EXT` definition) | STRONGLY INFERRED | NVIDIA GL extension is defined for this hardware; Eden has no equivalent and falls back to an edge mode |
| The border color bypasses the TIC component swizzle | HYPOTHESIS | Eden adds `VK_EXT_border_color_swizzle` mapping to compensate Vulkan swizzling the border, which implies hardware does not; the shader blends the unswizzled border |

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

## Tests (not run)

- `texture_format_tests`: `testSamplerBorders` (presets, integer, depth, sRGB, mirror-to-border, unused border).
- `shader_library_tests`: `testTextureBorderEmulation` (decoded TEX with a custom border, linear and nearest, coverage 0 / 0.5 / 1 per axis).

Gameplay validation pending. Header changes (`const.hpp`, `shader_cache.hpp`, `3d.hpp`) require a full `hydra` rebuild and restart (hot-reload ABI).
