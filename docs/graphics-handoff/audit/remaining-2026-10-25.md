# Remaining Maxwell → Metal gaps (re-verified 2026-10-25)

Re-check of `3d_backend.md`, `3d_frontend.md`, `textures.md`, `shader_alu.md`
and `shader_mem_tex_flow.md` against the working tree. Offsets are clb197.h
byte offsets; paths are relative to `src/core/hw/tegra_x1/gpu/`.

Implemented since the re-check (2026-10-25): compact draw topology is 4 bits
(0x1214), CT_SELECT count clamped to 8, ZT_SIZE_C ARRAY_SIZE_IS_ONE, global
render enable (0x130), DRAW_ZERO_INDEX (0x304), zeta V8Z24 / ZF32_X16V8X8 /
ZF32_X16V8S8, stencil/polygon/point/line power-on defaults, surface clip on
draws and clears (0xff4), polygon mode LINE (0xdac), API_MANDATED_EARLY_Z
(0x210), GS/TCS records carry clip distances, primitive restart expanded for
GS/tessellation input, triangle strip adjacency GS input; earlier the same day:
conditional render mode 2, SET_SAMPLER_BINDING, SINGLE_CT_WRITE_CONTROL,
VertexA, geometry shaders and tessellation (geometry.hpp, tessellation.hpp).

## Direct (Metal has the feature)

| Feature | Register | Gap |
|---|---|---|
| sRGB write toggle | SET_SRGB_WRITE 0x15b8 | not read; needs an sRGB / linear view of the target |
| RT layer offsets | SET_RT_LAYER 0x15cc, COLOR_TARGET_LAYER 0x820+, ZT_LAYER 0x179c | base layer only read by debug code |
| Window clip | WINDOW_CLIP 0x194c | not decoded |
| Polygon mode POINT | FRONT/BACK_POLYGON_MODE 0xdac/0xdb0 | lines done; points need emulation, per-face modes |
| Zeta X8Z24_X16V8S8 | 0x1d | no host format (24-bit depth in 64-bit pixels) |
| D3D cull mode | D3D_SET_CULL_MODE 0x1308 | not decoded (precedence over SET_CULL unspecified) |
| Sample shading | HYBRID_ANTI_ALIAS_CONTROL 0x754 | not decoded |
| Programmable sample positions | ANTI_ALIAS_SAMPLE_POSITIONS 0x11e0 | fixed tables only |
| Compute linked TSC | QMD word 0xB bit 30 | compute.cpp always uses handle bits 20-31 |
| Sampled view base level / LOD | TIC RES_VIEW_MIN_MIP, MIN_LOD_CLAMP, MIP_LOD_BIAS | views always start at level 0 |
| Primitive topology control | 0x1948 / 0x1970 | separate topology state not read |
| Vertex ID base / copy | 0x1118, 0x161c, 0x164c | not read |
| PrimitiveID load | 0x1620 / 0x1624 | not read |
| D24 depth bias units | | no rescale for D24 emulated as D32F |

## Emulation (Metal lacks the feature)

| Feature | Register | Emulation idea |
|---|---|---|
| Logic op | LOGIC_OP 0x19c4/0x19c8 | framebuffer fetch in the fragment shader |
| Advanced (iterated) blend | ITERATED_BLEND 0xdd0 | framebuffer fetch |
| Depth bounds test | 0x19bc, 0xf9c | read depth (fetch) and discard |
| Blend float option | 0x19c0 | 0 × Inf = 0 in the shader |
| Sample mask / AA enable | SAMPLE_MASK 0xfa4, ANTI_ALIAS_ENABLE 0x1534 | AND into [[sample_mask]] |
| Coverage modes | 0x11f8, 0x1138, 0x16b4 | shader |
| Color clamp / PS saturate | 0x13a8, 0x2600 | shader saturate |
| Feedback loops, pixel shader barrier | 0xde0 | split the pass / copy the attachment |
| Transform feedback | 0x380+, 0x744, 0x740; DRAW_AUTO source | vertex shader writes buffers |
| Viewport swizzle Z/W, negative X | 0x0a00+ | vertex epilogue |
| User clip cull | USER_CLIP_OP 0x1940 | discard |
| Point sprite coordinate replacement | 0x1520 | fragment shader |
| Lines: width, AA, stipple, provoking vertex, conservative raster | 0x13b0.., 0x1570, 0x166c, 0x1684, 0x1148 | lines as quads, flat attribute rotation |
| Adjacency topologies without a GS | | drop adjacency in the index cache |
| Restart for vertex arrays | 0xde8 | index cache |
| 10_10_10_2 integer/scaled, 32-bit norm vertex formats | | fetch as uint, unpack in the shader |
| Disabled stream reads | | bind a zero buffer |
| Vertex stream substitute | 0xf84 | address substitution |
| Indirect draws | | HLE macro + drawPrimitives(indirectBuffer:) |
| Report counters (primitives, invocations), statistics | 0x1530, 0xd68 | shader counters |
| Tessellation isolines, point mode; GS after tessellation | | manual tessellation / chained passes |
| MME shadow RAM | 0x124 | shadow copy |
| Texture color key | TIC COLOR_KEY | compare after sampling |
| Min/max reduction filter | TSC | gather + min/max |
| Images in vertex/fragment stages | | ShaderWrite textures per stage |
| SUATOM.CAS, non-32-bit surface atomics | 0xeac0, 0xea80 | atomic CAS on R32 views |
| Copies between formats of different size | | compute reinterpretation |

## Shader decompiler

- Rounding modes and FTZ for FADD/FMUL/FFMA/FMNMX/F2F/I2F; .CC variants.
- `.X` (extended) forms: IMNMX, ISET/ISETP, LOP/LOP3, IADD3, SHL/SHR/SHF; SHF .HI.
- LEA predicate output; MOV lane mask; FCHK (always false); HSETP2 cbuf |b|;
  half .F32 operands rounded to f16 first; video ops' .CC/scale/sat.
- LDC .IL/.IS/.ISL; LDG/STG/ATOM without a resolved storage buffer; 64-bit
  ATOMS beyond EXCH; local memory size from the QMD.
- IPA .CONSTANT/.SC/CENTROID/OFFSET; legacy attributes (colors 0x280-0x2bc,
  fog, fixed function texcoords, viewport mask) in vertex/fragment stages.
- Indexed ALD/AST outside geometry/tessellation; indexed bindless handles.
- TEX/TLD .LC/.CL and sparse predicate; TXQ type/sample position/filter/wrap
  queries; TXD 3D array and cube with offsets; SHFL outside fragment quads;
  BAR variants; flow structurization.
- Unsupported opcodes: LD/ST (generic), STP, AL2P, TXA, IMADSP, IMAD32I,
  VADD/VSET/VSHL/VSHR/VABSDIFF/VABSDIFF4, BPT/IDE/RTT, LMEM/CRS pointer ops,
  LEPC, R2B/B2R, cbuf-target branches beyond bounded jump tables,
  LONGJMP/PLONGJMP.
