# TIC colour formats: Python decoder vs Hydra + Metal

`texture_format_reference.py` decodes raw guest texel bytes with exact Python
arithmetic and compares them with what `hydra-texture-format-reference-tests`
reads back from Metal after the bytes pass through Hydra's TIC format table,
texture cache and swizzled texture view (`toTextureFormat`,
`toSampledTextureFormat`, `toSampledSwizzleChannels`, `TextureCache::find`).

```sh
ninja -C build/swiftui-release -j 6 hydra-texture-format-reference-tests
curl -L --fail https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/3d/clb197tex.h -o /tmp/hydra-nvidia-clb197tex.h
python3 tests/texture_format_reference.py build/swiftui-release/bin/hydra-texture-format-reference-tests \
  --header /tmp/hydra-nvidia-clb197tex.h --report /tmp/hydra-texture-format-report.json
```

## What is covered

17 uncompressed colour formats (R8, R16, R32, G8R8, R16_G16, R32_G32, R32_G32_B32,
R16_G16_B16_A16, R32_G32_B32_A32, A8B8G8R8, X8B8G8R8, A2B10G10R10, B5G6R5,
A4B4G4R4, A1B5G5R5, A5B5G5R1, G4R4) plus BF10GF11RF11 and E5B9G9R9, with every
component type that makes sense for the field widths (Unorm, Snorm, Sint, Uint,
Float) and sRGB for the 8-bit RGBA formats. Each combination runs 10 byte
patterns (zeros, ones, 0x80, 0x7f, six random) through 4 swizzles (identity,
reversed, R,R,R,One, Zero,B,One,G): 32,640 component values per run.

Layouts come from the NVIDIA names in `clb197tex.h` (audited against the enum
values): packed names list components from the most significant bits, so
A8B8G8R8 is bytes R, G, B, A in little-endian memory.

Compressed formats (4x4 blocks, one block per case): BC1 (DXT1), BC2 (DXT23),
BC3 (DXT45), BC4 (DXN1, Unorm/Snorm), BC5 (DXN2, Unorm/Snorm) with Python
decoders, and BC7U (Unorm/sRGB), BC6H_SF16, BC6H_UF16 decoded by the independent
C decoder [bcdec](https://github.com/iOrange/bcdec) (MIT or Unlicense). The
runner `tests/bcdec_oracle.c` wraps it; `bcdec.h` is downloaded to
`/tmp/hydra-bcdec` (`--bcdec` to override) and is not vendored. 40 random blocks
plus zero/ones/0x80 per format, tolerance 2.5/255 (BC1-3, BC7), 3/127 (Snorm),
2e-3 relative (BC6H). A tight tolerance of 0.3/255 fails, so the margin is about
one 8-bit step of interpolation rounding; breaking the Python BC1 or BC4 decoder,
or scaling the oracle, fails the run.

## Results (20 October 2026)

- **PASS: 10,289,296/10,289,296** component values: 186,752 for linear texels and
  single BC blocks, 2,058,960 for block-linear layout, 8,043,584 for mip chains
  and array layers. Metal API validation on.
- **Found and fixed:** sampling the alpha of X8B8G8R8 (`XBGR8`) returned the
  unused X byte, because the format is stored as RGBA8 on the host (marked
  HACK). `toSampledSwizzleChannels` now turns an `A` source into `OneFloat` for
  formats with an unused fourth component (RGBX8 and RGBX16 families). Only the
  sampled TIC path changes; internal views used by copies keep the X byte.
  Negative control: with the fix disabled the runner fails 144 values.
- **Convention, not in the header:** a swizzle source naming a component the
  format does not have reads 0 for G/B and 1 for alpha. The runner asserts
  this; NVIDIA's header does not define it.
- **Unmapped combinations** (reported as GAPs, not failures): XBGR8 with
  Uint/Snorm/Sint, and Snorm/Sint/Uint on the 16-bit and 8-bit packed formats
  (B5G6R5, A4B4G4R4, A1B5G5R5, A5B5G5R1, G4R4). Most are not meaningful on
  hardware. XBGR8 integer/Snorm is the only one that may matter.

- **Reserved BC7 modes** (first byte zero): D3D defines the block as all zero,
  Apple GPUs return alpha 1. Not compared; not a meaningful encoding.
- BC6H blocks use only the 14 defined modes; reserved modes decode to
  undefined values.

## Block-linear layout

The second sweep writes guest memory in Tegra X1 block-linear order with an
independent Python implementation (64x8 byte GOBs of 512 bytes, 16-byte sectors
interleaved as x[3:0], y[0], x[4], y[2:1], x[5]; GOBs of a block stacked along
Y, blocks along X) and reads every texel back through Hydra's texture cache:
R8, G8R8, A8B8G8R8, R16G16B16A16, R32G32B32A32 (Uint, byte exact) and DXT1/DXT45
(block grid), 8 sizes from 7x3 to 200x24 and block heights of 1 to 16 GOBs.
Swapping two sector bits in the Python layout fails the run. The layout comes
from public reverse engineering (nouveau, Ryujinx, yuzu), not an NVIDIA
document; Hydra's `unswizzleGobCoords` and this implementation agree.

Only block heights that the image needs are tested (`2**log2 <= next power of two
of the GOB rows`). Hydra shrinks larger block heights the way it does for mip
levels; that is a convention, not something NVIDIA documents, and NVN does not
emit such descriptors. 3D textures, array layers, mip chains and block widths
above one GOB are not covered.

## Mip chains and array layers

Same formats (R8, G8R8, A8B8G8R8, RGBA16, RGBA32 Uint; DXT1, DXT45), five sizes
(16x16 to 128x32, 3 to 8 levels, including non-square and non-power-of-two),
1 or 3 layers, level-0 block heights of 1 to 8 GOBs. The Python layout follows
the usual convention (yuzu, Ryujinx): level N is max(1, size >> N) texels
(rounded up to 4x4 blocks for BC), its block height shrinks while the level has
no more GOB rows than half the block, levels follow each other, and a layer is
padded to a multiple of the level-0 block size. Every level of every layer is
read back. Control runs: not padding the layer stride, or not shrinking block
heights for small levels, each fail the run.

The layer padding and block-height shrinking are conventions from other
emulators and driver behaviour, not NVIDIA documentation; the test shows Hydra
agrees with them.

## Limits

Apple's GPU, not an NVIDIA GPU. Not covered: ETC/EAC/ASTC, depth/stencil,
video formats, `ForceFp16` component types, filtered sampling (texel `read()`
only), 3D textures, cube maps, block widths above one GOB, and linear BC textures
larger than one block.
The runner sets `HYDRA_GPU_SYNC=1` because production command buffers are
committed without waiting.
