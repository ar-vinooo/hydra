# Graphics continuation — 2026-10-18

No commit, staging, GUI or game launch was performed. Existing debug toggles
and the in-progress compressed-alias work were preserved.

## Implemented

- Sequential shader register operands use one `offsetRegister` rule. RZ and
  registers that reach it no longer wrap through the `u8` register type into
  R0. The conversion, FP64, shared/global memory, surface and texture paths use
  it for both sources and destinations.
- NVB197 `DRAW_AUTO` (method 0x48F) uses the 12-bit stride at 0x4C6 and submits
  `byte_count / stride` vertices from vertex zero. A zero stride is ignored
  safely and the ordinary vertex-start register is restored after the draw.
- Copies between compressed blocks and equal-byte RGBA32/RG32 texels preserve
  raw rows through a private Metal buffer. Extents are bounded in all three
  dimensions.
- Packed depth/stencil readback splits the Metal D32FS8 planes and reconstructs
  Z24S8, S8Z24, X8Z24 or D32FS8 guest words. Z24 conversion is checked within
  one UNORM integer step and stencil bytes are exact.
- Texture readback now supplies `bytesPerImage`, advances packed destinations
  across levels/layers and includes every volume slice. Cache flushes retile
  all 3D slices into the guest block-linear layout instead of rejecting 3D and
  depth/stencil storage.

The DRAW_AUTO fallback is derived from NVIDIA's `DRAW_AUTO_BYTE_COUNT` and
12-bit `SET_DRAW_AUTO_STRIDE` definitions in
[`clb197.h`](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197.h).
Metal requires an explicit vertex count, so the fallback converts bytes to
vertices with `byte_count / stride`. No Eden behavior is used as evidence.

## Validation

- `hydra-shader-library-tests`: pass with Metal validation, including the new
  LDG.128 R254 boundary regression.
- `hydra-gpu-engine-tests`: pass with Metal validation, including DRAW_AUTO.
- `hydra-texture-cache-tests`: pass with Metal validation, including raw block
  alias copies, four packed depth layouts, a rendered Z24S8 flush and a
  GPU-written three-slice volume round trip.
- Warning-clean `-Werror` builds and `git diff --check` pass.

## Still open

The remaining master-backlog items need either semantics that are not present
in the available primary/reference implementations, or new architectural
emulation: restart topology-change and inline alignment, report awaken/trap and
additional counters, the unverified video-op family, geometry/tessellation,
logic operations, multisampling/coverage/sample shading and transform
feedback. DRAW_AUTO accepts an explicit byte count now; producing that count
through emulated transform feedback remains open.
