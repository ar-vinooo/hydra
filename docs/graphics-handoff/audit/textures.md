# Hydra texture & image system audit (Maxwell TIC/TSC → Metal)

Audit date: 2026-10-17. Repository: `/Users/arvino/Documents/Project/hydra`, branch `arvino` with uncommitted work. This was a read-only audit: nothing in the repo was modified, built or run.

**Line numbers.** All `file:line` citations reflect the working tree at about 10:20 local time. `engines/3d.cpp`, `renderer/const.hpp` and the MSL emitter changed during the audit (another session is editing them), so numbers can drift by a few dozen lines. The quoted code is authoritative.

**References.**
- `REFS` = `/private/tmp/claude-501/-Users-arvino-Documents-Project-hydra/0e906531-f45a-4e9b-b0bc-dcb303edaa1b/scratchpad/refs`.
- `clb097tex.h` / `clb197.h` = NVIDIA open-gpu-doc.
- `Eden/...` = `REFS/yuzu/src/video_core/...`.

**How claims were verified.**
1. Every Hydra claim comes from reading the code. Call paths were followed through the `HotN` implementations.
2. Hydra's TIC/TSC bitfield structs were copied into a scratch program and compiled with clang. It prints the bit range the compiler assigns to every field (`scratchpad/audit/bitcheck/bitcheck.cpp`). All ranges were compared against `clb097tex.h`.
3. Metal behaviour that the findings rely on was measured with scratch Swift probes on this host (Apple M1, macOS 27.0.1, `MTL_DEBUG_LAYER=1`). The probes are in `scratchpad/audit/metalprobe/probe*.swift`, and the results are in the appendix. They confirm:
   - the packed 16-bit bit layouts;
   - that blit copies require equal pixel formats;
   - that a `Depth32Float` view of `Depth32Float_Stencil8` is not castable;
   - that 1D textures cannot be mipmapped;
   - the restrictions on unnormalized samplers;
   - that min/max sampler reduction is unsupported on M1;
   - the mip-count and non-square-cube asserts.

**Class legend.**
- **VERIFIED**: implemented, and either a repo test checks the results or a `tests/README-*.md` documents it as verified in game.
- **IMPLEMENTED_UNVERIFIED**: implemented, but no test checks it.
- **PARTIAL**: implemented with semantic gaps or bugs.
- **MISSING**: not implemented or ignored.
- **UNKNOWN**: cannot be decided from code plus references alone.

---

## Summary

| Class | Count |
|---|---|
| VERIFIED | 22 |
| IMPLEMENTED_UNVERIFIED | 21 |
| PARTIAL | 42 |
| MISSING | 47 |
| UNKNOWN | 1 |
| **Total capabilities (rows A1–J10)** | **133** |

Many MISSING rows are quality or performance hints (sector promotion, trilinear optimization, aniso spread, LOD quality, coherency hash) with no visible effect; they are marked "Hint" in the notes.

**TIC/TSC bit layouts.** Every field Hydra decodes sits at the bit position `clb097tex.h` specifies. The deviations are naming differences, reserved bits Hydra borrows from later hardware (as Eden does), and three real discrepancies:
- the TSC `MAG_FILTER` field is 2 bits wide in Hydra but 3 in the doc;
- the TIC address low bits (`GOB_DEPTH_OFFSET`/reserved) are folded into the address;
- `SET_SAMPLER_BINDING` and QMD `LINKED_TSC` are not decoded.

**Formats.** Eden maps 111 TIC (format × component types × sRGB) encodings. Hydra maps 83 of them, all agreeing with Eden's choice of format, and is missing 28. Several mapped ones are still wrong on the host side:
- RGBA32 SINT crashes (no Metal LUT entry);
- A1B5G5R5 uses the wrong Metal bit layout;
- swizzle composition is wrong for the ABGR4 and B5G6R5 host formats.

**Sampler.** Only these TSC fields reach Metal: filters, the 8 address modes (5 with exact Metal equivalents, 3 approximated, of which MIRROR_ONCE_BORDER is approximated wrongly), depth compare, min/max LOD and a 3-colour border approximation. LOD bias, anisotropy, reduction, unnormalized coordinates and sRGB border are dropped.

---

## (a) Capability table

### A. Descriptor pools, handles and binding

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| A1 | TIC pool base and maximum index (SET_TEX_HEADER_POOL_A/B/C) | PARTIAL | `engines/3d.hpp:427-429`. `3d.cpp:1600-1610` bounds-checks only when `max_index != 0`. `3d.cpp:1078` (buffer/integer specialization in `getShader`) indexes `pool[getImageHandle(*handle)]` with no check. `compute.cpp:129` | `clb197.h:2790-2797` (MAXIMUM_INDEX 21:0). Eden passes `regs.tex_header.limit` (`shader_environment.cpp:381`) | `max_index==0` means "only entry 0 valid", but Hydra treats it as unlimited. The unchecked read in `getShader` can read up to 32 MiB past the pool |
| A2 | TSC pool base and maximum index | PARTIAL | `3d.hpp:421-423`, `3d.cpp:1604-1605`. `compute.cpp:240` `tsc_pool[getSamplerHandle(*handle)]` has no limit check | `clb197.h:2773-2780` (19:0) | Compute ignores `tsc_limit` completely |
| A3 | Bound textures through TEXTURE_BUFFER_INDEX / SET_BINDLESS_TEXTURE (0x982) | IMPLEMENTED_UNVERIFIED | `3d.hpp:544`; `3d.cpp:2038-2070` `readTextureHandle` (word index = TEX immediate); compute `compute.hpp:42`, `compute.cpp:37-67` | `clb197.h:4105-4106`; Eden `maxwell_3d.h:3022`, `kepler_compute.h:120` | Shader-side mapping is tested; the engine read path is not |
| A4 | Handle split TIC[19:0] / TSC[31:20] | IMPLEMENTED_UNVERIFIED | `3d.cpp:32-33`, `compute.cpp:17-18` | Eden `textures/texture.h:150-156` | |
| A5 | Separate texture and sampler handle words OR'ed together | IMPLEMENTED_UNVERIFIED | `3d.cpp` `readTextureHandle` (`*handle \|= *sampler`); `compute.cpp:59-65` | Eden `vk_compute_pipeline.cpp:188` (`lhs_raw \| rhs_raw`) | |
| A6 | Bindless handles (statically tracked cbuf words or constants) | PARTIAL | `renderer/const.hpp:397-416, 448-463`; tracking tests `tests/shader_library_tests.cpp:40-118` and the bindless binding test (`:2147`) | `tests/README-texture-sampling.md` (Bindless row) | Arbitrary dynamic indexing is unsupported. The word field is 13 bits wide and 0x1fff is the "no sampler" sentinel. Constant handles keep only 26 bits, so the TSC index is truncated to 0..63 (bug D-18) |
| A7 | SET_SAMPLER_BINDING = VIA_HEADER_BINDING (3D linked TSC) | MISSING | Register 0x48D falls inside `u32 padding_0x48d[0x26]` (`3d.hpp:366`). The sampler index is always `getSamplerHandle(handle)` (`3d.cpp:1601, 1673`) | `clb197.h:2054-2057`; Eden `texture.h:159-166`, `maxwell_3d.h:1102-1105, 2815`, `shader_environment.cpp:379-391` | When VIA_HEADER_BINDING is set, TSC index = TIC index. Used by GL-driver ports and homebrew |
| A8 | Compute QMD LINKED_TSC | MISSING | `ComputeQmd` (`compute.hpp:47-67`) never reads word 0xB. `compute.cpp:240` | Eden `kepler_compute.h:137` (`BitField<30,1> linked_tsc` in word 0xB), `shader_environment.cpp:437, 446` | |
| A9 | Picking up TIC/TSC changes (INVALIDATE_TEXTURE_HEADER/SAMPLER_CACHE) | IMPLEMENTED_UNVERIFIED | TIC/TSC are re-read from guest memory every draw; the memos compare raw words (`3d.cpp:1521-1533` sampler, `~1655-1670` texture memo) | `clb197.h:1957-1961, 2232-2242` | No invalidation method is needed |
| A10 | SET_SELECT_MAXWELL_TEXTURE_HEADERS (legacy TEXHEAD V1/V2/V3 layouts) | MISSING | 0x3C4 is padding (`3d.hpp:314`). Maxwell headers are assumed | `clb197.h:1210-1213`; `clb097tex.h:33, 1501, 1756` | NVN always selects Maxwell headers. Eden also ignores this. Low risk |

### B. TIC header decode

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| B1 | TIC bit layout, words 0-7 | IMPLEMENTED_UNVERIFIED | `3d.hpp:24-98`, `gpu/const.hpp:588-599`. The audit's compiled bit dump matches (§b) | `clb097tex.h:266-543` | Only `sizeof==32` is asserted in the repo |
| B2 | HEADER_VERSION dispatch | PARTIAL | `3d.cpp:1384-1394`: only Pitch and BlockLinear are handled; everything else hits `default:` and is decoded as block linear | `clb097tex.h:415-420` | See B5 and B7 |
| B3 | ONE_D_BUFFER header (32-bit width, full 32-bit low address) | IMPLEMENTED_UNVERIFIED | `3d.cpp:1328-1370` (`width_minus_one_16_31 << 16`, `_1DBuffer` storage) | `clb097tex.h:817, 960, 972, 974` | Buffer aliasing at cache level is VERIFIED (I8). TIC extraction is untested |
| B4 | PITCH header (PITCH_BITS20TO5) | VERIFIED | `3d.cpp:1385-1387`. Linear decode test `tests/texture_cache_tests.cpp:81-123` | `clb097tex.h:1150, 1162`; Eden `texture.h:248-253` | The test drives descriptors directly; the TIC field extraction is trivial |
| B5 | PITCH_COLOR_KEY header | MISSING | Goes to `default:` with `is_linear=false`, so a pitch surface is decoded as block linear (bug D-12) | `clb097tex.h:1263-1494` | Colour-key compare is also missing |
| B6 | BLOCKLINEAR header (GOB swizzle, block height/depth) | VERIFIED | `memory_util.cpp:21-211`; tests `texture_cache_tests.cpp:81-123, 1059-1124` | `clb097tex.h:424-439`; Eden `textures/decoders.cpp` | Swizzle constants match Eden's GOB formula |
| B7 | BLOCKLINEAR_COLOR_KEY header | PARTIAL | Decoded as block linear but logs NOT_IMPLEMENTED on every lookup. Word 7 is COLOR_KEY_VALUE, yet image views read `view_mip_min_level` from it (`3d.cpp:1489-1490`) | `clb097tex.h:782, 810` | Rare on NVN |
| B8 | Address bits 47:0 | PARTIAL | `makeAddr(addr_lo, addr_hi)` uses all of word 1 (`3d.hpp:29-32`, `common/functions.hpp:71-73`) | BL: `clb097tex.h:409-412` (RESERVED1Y 36:32, GOB_DEPTH_OFFSET 38:37, address 63:41). Pitch: `:1149-1150`. Eden does the same (`texture.h:244-246`) | Non-zero GOB_DEPTH_OFFSET or reserved bits would corrupt the address. Same as Eden |
| B9 | GOBS_PER_BLOCK_HEIGHT / DEPTH | VERIFIED | Passed to the descriptor (`3d.cpp:1409-1412`), adjusted per level (`renderer/const.cpp:782-817`); tests `texture_cache_tests.cpp:81-123, 1088-1124` | `clb097tex.h:426-439` | |
| B10 | GOBS_PER_BLOCK_WIDTH | IMPLEMENTED_UNVERIFIED | Ignored (`3d.hpp:44`) | `clb097tex.h:424-425` (only ONE_GOB is defined) | Correct to ignore |
| B11 | TILE_WIDTH_IN_GOBS (tile-width spacing) | PARTIAL | Used as `block_width_gobs_log2` (`3d.cpp:1412`); row alignment per level (`texture_cache.cpp:776`, `const.cpp:802-817`). **Not** applied to layer-stride alignment (`const.cpp:821-840`, bug D-16) | `clb097tex.h:441`; Eden `util.cpp:188-229` (StrideAlignment/NumGobs with `is_small`), `:296-303` (AlignLayerSize) | Hydra's shrink heuristic differs from Eden's `IsSmallerThanGobSize`. No test. The title-logo fix is recorded only in user memory notes |
| B12 | GOB_DEPTH_OFFSET | MISSING | not decoded | `clb097tex.h:410` | |
| B13 | GOB3D | MISSING | decoded (`3d.hpp:49`), never used | `clb097tex.h:448` | Assumed 0, as in Eden |
| B14 | USE_ARRAY_TILE_ALIGNMENT | MISSING | unnamed padding bit 110 | `clb097tex.h:449` | Affects the layer stride |
| B15 | SPARSE_ENABLE / sparse residency | MISSING | `3d.hpp:47` (padding), `:74` `is_sparse` unused | `clb097tex.h:440`; Eden `image_info.cpp:48` | |
| B16 | Width / height / depth extents | PARTIAL | `3d.cpp:1396-1412`. No height=1 forcing for 1D. No promotion of 1D/2D/Cube to arrays when depth>1. No clamping to Metal limits (bug D-15) | `clb097tex.h:467, 493-494`; Eden `image_info.cpp:54-123` | |
| B17 | View base layer (`view_layer_base_*`) | PARTIAL | Never decoded (`3d.hpp:33, 37, 64`). The TIC address already points at the base layer, so data is right implicitly; the cache resolves it as a layer view or a separate storage | Eden `texture.h:270-272`, `texture_cache.h:1187-1188` (`image_gpu_addr = Address() - BaseLayer()*layer_stride`), `image_info.cpp:55-68` (array promotion) | Without array promotion a 2D TIC with base layer > 0 stays non-array. That is harmless only while depth == 1 |
| B18 | MAX_MIP_LEVEL → level count | VERIFIED | `3d.cpp:1408`; mip storage and views tested (`texture_cache_tests.cpp:65-75, 129-209`) | `clb097tex.h:466`; Eden `image_info.cpp:37` | Not clamped to the dimensions. Metal asserts when the count is too large (probe) |
| B19 | RES_VIEW_MIN/MAX_MIP_LEVEL | PARTIAL | Images use one level at `view_mip_min_level`. Sampled views always cover `[0, level_count)` (`3d.cpp:1485-1507`). The code comment says honouring the range "broke streamed textures" | `clb097tex.h:525-526`; Eden `image_view_info.cpp:38-42` | Wrong base level for mip filter NONE, TXQ, and LOD clamps relative to the base (bug D-22) |
| B20 | TIC MIN_LOD_CLAMP | MISSING | only printed in traces (`3d.hpp:94`) | `clb097tex.h:543` | |
| B21 | TIC MIP_LOD_BIAS | MISSING | `3d.hpp:80` unused | `clb097tex.h:499` | |
| B22 | TIC MAX_ANISOTROPY, ANISO_*, LOD_*_QUALITY, USE_HEADER_OPT_CONTROL, TRILIN_OPT | MISSING | `3d.hpp:53-58, 65, 79-85` unused | `clb097tex.h:451-465, 469, 498-524` | Quality hints. Eden also ignores them |
| B23 | DEPTH_TEXTURE bit | MISSING | `3d.hpp:59` unused (depth is inferred from the format and the instruction's DC flag) | `clb097tex.h:465` | |
| B24 | S_R_G_B_CONVERSION | PARTIAL | sRGB cases exist only for A8B8G8R8, DXT1/23/45 and ASTC (`renderer/const.cpp:247-311`). BC7 sRGB and ETC2 sRGB are missing. sRGB on any other format returns `Invalid` | `clb097tex.h:470`; Eden `format_lookup_table.cpp:56, 168-206, 217-272` | |
| B25 | TEXTURE_TYPE decode | PARTIAL | `toTextureType` (`3d.cpp:35-55`) has no default, so values 9-15 are UB | `clb097tex.h:471-481` | |
| B26 | SECTOR_PROMOTION | MISSING | `3d.hpp:68` unused | `clb097tex.h:482-486` | Cache hint with no visible effect |
| B27 | BORDER_SIZE | MISSING | `3d.hpp:69` unused | `clb097tex.h:487-492` | NVN uses BORDER_SAMPLER_COLOR. Negligible |
| B28 | NORMALIZED_COORDS = 0 (rectangle textures) | MISSING | `3d.hpp:75` unused | `clb097tex.h:496`; Eden `image_view_info.cpp:73` (Rect), `shader_environment.cpp:48-49` (Color2DRect) | Coordinates are treated as normalized |
| B29 | MULTI_SAMPLE_COUNT | MISSING | `3d.hpp:93` unused; no multisample support in the GPU tree (grep) | `clb097tex.h:527-542`; Eden `image_info.cpp:36, 125-128`, `samples_helper.h` | |
| B30 | COLOR_KEY_OP / VALUE | MISSING | `3d.hpp:78, 89` unused | `clb097tex.h:782, 810` | |
| B31 | RESOURCE_VIEW_COHERENCY_HASH, load/store hint | MISSING | `3d.hpp:35-36` unused | `clb097tex.h:421-422` | Hints |

### C. Texture types and queries

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| C1 | ONE_D | PARTIAL | Created as `MTLTextureType1D` with `mipmapLevelCount=level_count` (`metal/texture.cpp:25`) and declared `texture1d<T>` (`msl/emitter.cpp:56`) | `clb097tex.h:472` | Metal asserts when a 1D texture has more than one mip (probe). MSL `texture1d` sampling accepts no level/bias arguments. MoltenVK emulates 1D as 2D for this reason |
| C2 | TWO_D | VERIFIED | `texture_cache_tests.cpp:81-123` | | |
| C3 | THREE_D | VERIFIED | Decode/encode with slices per block: `memory_util.cpp:48-94`; tests `:1088-1124`; README-crash4-render.md:22 | | Never read back to the guest (I12) |
| C4 | CUBEMAP (6 × depth layers) | IMPLEMENTED_UNVERIFIED | `3d.cpp:1402-1406`, `metal/texture.cpp:33-36` | Eden `image_info.cpp:98-104` | Cube-level tests use 2D-array storage, so the cube view itself is untested. Metal asserts on non-square cubes (probe) |
| C5 | ONE_D_ARRAY | IMPLEMENTED_UNVERIFIED | `3d.cpp:1424-1431` | | Same 1D mip restriction as C1 |
| C6 | TWO_D_ARRAY | VERIFIED | `texture_cache_tests.cpp:214-267` | | |
| C7 | ONE_D_BUFFER | VERIFIED | `metal/texture.cpp:63-74`; raw alias tests `texture_cache_tests.cpp:402-660` (README-texture-sampling.md:62-70, run 17 Oct) | | |
| C8 | TWO_D_NO_MIPMAP | IMPLEMENTED_UNVERIFIED | mapped to 2D (`3d.cpp:43-45`) | Eden treats it like 2D | |
| C9 | CUBEMAP_ARRAY | IMPLEMENTED_UNVERIFIED | `metal/texture.cpp:37-41` | | |
| C10 | Reinterpreting TIC type vs shader type (array ↔ non-array, cube ↔ 2D array) | IMPLEMENTED_UNVERIFIED | `3d.cpp:1418-1478` | Eden promotes types (`image_view_info.cpp:44-57`) | A 2D TIC with layer_count > 1 viewed as 2D asks Metal for a view with N slices of a 2D texture (bug D-15) |
| C11 | TXQ dimension queries | PARTIAL | `decoder/texture.cpp:267-294` (`// TODO: src`, only `Dimensions`); `msl/emitter.cpp:1057+` queries the view's level 0 | | The LOD operand is ignored and other query types are not implemented. Shader-side, but it depends on B19 |

### D. Formats and per-component data types (details in §c)

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| D1 | Plain colour formats R8 to RGBA32 in UNORM/SNORM/UINT/SINT/FLOAT | PARTIAL | `renderer/const.cpp:244-344`; LUT `metal/maxwell_to_mtl.cpp:46-84` | Eden `format_lookup_table.cpp:48-139` | Missing: G8R8 UINT/SINT, A2B10G10R10 UINT, R32G32B32 FLOAT. RGBA32 SINT crashes (bug D-1) |
| D2 | Packed 16-bit: A4B4G4R4, A5B5G5R1, A1B5G5R5, B5G6R5, B6G5R5, G4R4 | PARTIAL | `const.cpp:265-267, 707-711`; `maxwell_to_mtl.cpp:96, 160, 163` | Eden `maxwell_to_vk.cpp:130-136, 193-194`; `vk_texture_cache.cpp:743-765` | A1B5G5R5 has the wrong host layout (bug D-3). A5B5G5R1, G4R4 and B6G5R5 are unmapped. Swizzle composition is broken (bug D-4) |
| D3 | E5B9G9R9_SHAREDEXP, BF10GF11RF11 | PARTIAL | B10G11R11 at `const.cpp:268`. `E5BGR9Float` and its Metal entry `RGB9E5Float` exist (`maxwell_to_mtl.cpp:103`) but no TIC case maps to them | Eden `format_lookup_table.cpp:118-119, 140-141` | The Metal RGB9E5 layout matches Maxwell bit for bit, so the "HACK" comment is unnecessary |
| D4 | DXT1 / DXT23 / DXT45, plus sRGB | IMPLEMENTED_UNVERIFIED | `const.cpp:257-262`; native Metal BC1-3 (`maxwell_to_mtl.cpp:104-111`) | Eden `:166-177` | Storage isolation is VERIFIED (`texture_cache_tests.cpp:47-75`). No pixel-decode test |
| D5 | DXN1 / DXN2 (BC4/BC5) UNORM/SNORM | PARTIAL | `const.cpp:263-264, 312` | Eden `:178-185` | DXN1 SNORM is missing even though `BC4_RSnorm` exists in the LUT |
| D6 | BC6H_SF16 / BC6H_UF16 | MISSING | No TIC case; the LUT maps both to `Invalid` (`maxwell_to_mtl.cpp:118-119`) | Eden `:190-193` | Metal on macOS supports `BC6H_RGBFloat` and `BC6H_RGBUfloat` natively |
| D7 | BC7U, plus sRGB | PARTIAL | `const.cpp:323` (UNORM only) | Eden `:186-189` | `BC7_RGBAUnorm_sRGB` is in the LUT but no TIC case reaches it |
| D8 | ETC2_RGB / RGB_PTA / RGBA, plus sRGB | MISSING | LUT `Invalid` (`maxwell_to_mtl.cpp:168-178`) | Eden `:195-206` | Native on Apple GPUs (Metal ETC2_*) |
| D9 | EAC / EACX2 UNORM/SNORM | MISSING | as D8 | Eden `:208-215` | Native on Apple GPUs (EAC_R11*, EAC_RG11*) |
| D10 | ASTC_2D, all 14 block sizes, LDR, plus sRGB | IMPLEMENTED_UNVERIFIED | `const.cpp:270-311`; `maxwell_to_mtl.cpp:132-159` | Eden `:217-272`, `maxwell_to_vk.cpp:248-265` (fallback) | No fallback when the device lacks ASTC (J3) |
| D11 | Depth/stencil sampling formats: Z16, ZF32, Z24S8, G24R8, X8Z24, S8Z24, ZF32_X24S8, R32B24G8 | PARTIAL | `const.cpp:316-320` | Eden `:142-165` (12 encodings) | 5 present, 7 missing (§c) |
| D12 | Sampling the depth plane through G and the stencil plane through RRRR for Z24S8/G24R8 | VERIFIED | `const.cpp:201-218, 694-702`; `metal/texture_view.cpp:27-40`; tests `texture_cache_tests.cpp:663-818`; README-crash4-render.md:15, 18 | Eden `vk_texture_cache.cpp:300-323` | ZF32_X24S8 stencil sampling is not routed to a stencil view |
| D13 | Coverage/VCAA depth formats (X4V4Z24…, ZF32_X16V8…) | MISSING | enum only | Eden also has none | Not used by NVN titles |
| D14 | Y8_VIDEO, R1, G8B8G8R8, B8G8R8G8, X8B8G8R8, G8R24 | MISSING | enum only | Eden also has none | Low priority |
| D15 | Per-component data types (exact 4-tuple match) | PARTIAL | `IMAGE_FORMAT_CASE_IMPL` matches all four types exactly (`const.cpp:222-231`) | Eden matches exactly too, but includes mixed tuples for depth formats | Missing mixed tuples become `Invalid`, so the texture is left unbound |
| D16 | SNORM_FORCE_FP16 / UNORM_FORCE_FP16 | MISSING | enum only (`gpu/const.hpp:568-576`) | `clb097tex.h:344-375` (data type values 5, 6) | Could normalize to SNORM/UNORM before lookup |
| D17 | Behaviour for unknown formats | PARTIAL | `LOG_ERROR`, return `Invalid` (`const.cpp:345-352`); `getTexture` returns nullptr and the texture is unbound | Eden falls back to `A8B8G8R8_UNORM` (`format_lookup_table.cpp:274-277`) | Unbound textures sample as black and fail the debug layer |

### E. Component swizzle

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| E1 | X/Y/Z/W sources R, G, B, A, ZERO | PARTIAL | `SwizzleChannels` (`const.cpp:557-718`) → Metal view swizzle (`metal/texture_view.cpp:14-23`, `maxwell_to_mtl.cpp:714-730`) | Eden value remaps `vk_texture_cache.cpp:550-593, 743-765` | Correct for identity host formats. Wrong for ABGR4 and B5G6R5 because positions are permuted instead of sources being remapped (bug D-4) |
| E2 | ONE_INT vs ONE_FLOAT | PARTIAL | Both map to `MTL::TextureSwizzleOne` (`maxwell_to_mtl.cpp:726-728`) | Eden does the same (`vk_texture_cache.cpp:340-342`) | The bit-exact 0x00000001 / 0x3F800000 distinction is lost |
| E3 | Depth-format swizzle (G→R for Z24S8) | VERIFIED | `const.cpp:694-702`; test `texture_cache_tests.cpp:736` | Eden `ConvertGreenRed` (`vk_texture_cache.cpp:550-557, 2517`) | |
| E4 | Invalid swizzle value 1 | PARTIAL | `toMtlSwizzle` has no default, so the result is UB | `clb097tex.h:376-383` | Only reached with garbage descriptors |

### F. Sampler (TSC)

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| F1 | TSC bit layout | IMPLEMENTED_UNVERIFIED | `3d.hpp:100-148`; compiled dump matches (§b) | `clb097tex.h:1962-2046` | MAG_FILTER width and CUBEMAP_INTERFACE_FILTERING naming differ |
| F2 | WRAP / MIRROR / CLAMP_TO_EDGE | IMPLEMENTED_UNVERIFIED | `maxwell_to_mtl.cpp:813-818` | `clb097tex.h:1962-1966`; Eden `maxwell_to_vk.cpp:53-63` | |
| F3 | BORDER | PARTIAL | `ClampToBorderColor` with only Metal's 3 colours (`metal/sampler.cpp:23-51`) | Eden custom border colours (`vk_texture_cache.cpp:2804-2806`) | See F18 |
| F4 | CLAMP_OGL | PARTIAL | Always `ClampToEdge`, and `LOG_NOT_IMPLEMENTED` on every sampler creation (`maxwell_to_mtl.cpp:821-823`) | Eden: Nearest → CLAMP_TO_EDGE, Linear → CLAMP_TO_BORDER (`maxwell_to_vk.cpp:65-74`) | |
| F5 | MIRROR_ONCE_CLAMP_TO_EDGE | IMPLEMENTED_UNVERIFIED | → `MirrorClampToEdge` (`:824-825`) | Eden `:75` | |
| F6 | MIRROR_ONCE_BORDER | PARTIAL | → `ClampToBorderColor`, so mirroring is lost (`:826-828`) | Eden → MIRROR_CLAMP_TO_EDGE (`:76-78`) | Negative coordinates return border instead of a mirrored texel |
| F7 | MIRROR_ONCE_CLAMP_OGL | PARTIAL | → `MirrorClampToEdge` (`:829-831`) | Eden is the same (`:79-80`) | |
| F8 | Depth compare enable and function | IMPLEMENTED_UNVERIFIED | `3d.cpp:1542-1544` (`+1` maps onto `CompareOp` correctly); `metal/sampler.cpp:20-21` | `clb097tex.h:1989-1998`; Eden `maxwell_to_vk.cpp:89-110` | Shader `gather_compare` tests use raw Metal samplers. Integer-coordinate compare is not emulated (`msl/emitter.cpp:976`) |
| F9 | MAG/MIN filter POINT and LINEAR | VERIFIED | `metal/sampler.cpp:12-13`; Hydra sampler path exercised by `shader_library_tests.cpp:1720-1896` (nearest) | `clb097tex.h:2011-2022` | Linear min/mag is untested |
| F10 | MIN_FILTER MIN_ANISO, MAG VCAA_4/8_TAP | MISSING | `mag_filter` is 2 bits (`3d.hpp:114`). `toMtlSamplerMinMagFilter` has no case for 3 or 0, so the result is UB (`maxwell_to_mtl.cpp:790-797`) | `clb097tex.h:2011-2022` | Bug D-11 |
| F11 | MIP filter NONE / POINT / LINEAR | VERIFIED | `maxwell_to_mtl.cpp:799-808`; LINEAR checked by the colour-mip test (`shader_library_tests.cpp:1893`) | `clb097tex.h:2023-2026`; Eden `maxwell_to_vk.cpp:36-51` | Value 0 is UB (D-11). NONE samples view level 0, which in Hydra is always storage level 0 (B19) |
| F12 | MIN/MAX LOD clamp (4.8 fixed point) | VERIFIED | `3d.cpp:1547-1548`; `metal/sampler.cpp:15-16`; test `shader_library_tests.cpp:1720-1896` | `clb097tex.h:2037-2038`; Eden `texture.h:388-394` | |
| F13 | TSC MIP_LOD_BIAS (signed 13-bit, 1/256) | MISSING | No field in `SamplerDescriptor` (`renderer/const.hpp:310-322`) | `clb097tex.h:2032`; Eden `texture.h:396-400`, `vk_texture_cache.cpp:2835` | `MTLSamplerDescriptor.lodBias` exists and works on M1 (probe) |
| F14 | MAX_ANISOTROPY | MISSING | `setMaxAnisotropy` is never called | `clb097tex.h:2002-2010` (1, 2, 4, 6, 8, 10, 12, 16); Eden `texture.cpp:40-68` | |
| F15 | Reduction filter (min/max) | MISSING | `3d.hpp:120` unused | Eden `vk_texture_cache.cpp:2795`, `maxwell_to_vk.cpp:852` | Metal `reductionMode` exists, but min/max "requires device support" and is unavailable on M1 (probe), so a shader fallback is needed |
| F16 | FLOAT_COORD_NORMALIZATION (force unnormalized) | MISSING | `3d.hpp:122` unused | `clb097tex.h:2033-2035` | Metal unnormalized samplers require NotMipmapped and clamp-only addressing (probe), so shader-side scaling is needed |
| F17 | sRGB border conversion (SRGB_CONVERSION + 8-bit sRGB border RGB) | MISSING | `3d.hpp:107, 129-134` unused | `clb097tex.h:1999, 2039-2042`; Eden `texture.cpp:35-38`, `vk_texture_cache.cpp:2802, 2855` | |
| F18 | Arbitrary RGBA border colour | PARTIAL | Snapped to TransparentBlack, OpaqueBlack or OpaqueWhite with a lossy heuristic (`metal/sampler.cpp:24-50`, bug D-9) | Eden VK_EXT_custom_border_color | |
| F19 | Border colour for integer formats | PARTIAL | Always reinterpreted as float (`std::bit_cast<float4>`) | Eden `NeedsExplicitBorderColorFormat` (`vk_texture_cache.cpp:2533`) | An integer (1,1,1,1) border becomes transparent black |
| F20 | Seamless cube (CUBEMAP_INTERFACE_FILTERING) | PARTIAL | Ignored; Metal cubes are always seamless | `clb097tex.h:2027-2031` | USE_WRAP (non-seamless) cannot be emulated. Same as Eden |
| F21 | TRILIN_OPT | MISSING | unused | `clb097tex.h:2036` | Hint |
| F22 | MIN_LOD_CLAMP_BEHAVIOR_FOR_NEAREST_MIP | MISSING | Not decoded (bit 3 sits inside an unnamed `u32 : 2`) | `clb097tex.h:2016-2018` | Negligible |
| F23 | Linear filtering on integer textures (force nearest) | MISSING | Logs "Filtered integer texture sample" and samples with the TSC's filter (`msl/emitter.cpp:874-877`) | Eden `key.force_nearest` (`vk_texture_cache.cpp:2852, 2920`) | Undefined on Metal |
| F24 | Sampler cache keying | PARTIAL | `CacheBase::find` keys on a `u32` XxHash with no equality check (`common/types.hpp:352-367`, `sampler_cache.cpp:13-27`) | | A collision returns the wrong sampler (bug D-7) |

### G. Images (surfaces: SULD/SUST/SUATOM/SURED)

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| G1 | Compute image binding (TIC format specialization, view at `view_mip_min_level`) | IMPLEMENTED_UNVERIFIED | `compute.cpp:179-186, 249-265`; `3d.cpp:1485-1507` | Eden image views | The HZB fix is documented only in user memory notes |
| G2 | Images in graphics stages (VS/FS) | MISSING | `configureShaderStage` binds `resource_mapping.textures` only; `images` is never bound in 3D | | UE4 and NVN pixel-shader UAVs |
| G3 | SUST formatted (.P with component mask) | IMPLEMENTED_UNVERIFIED | `decoder/compute.cpp:378-421`; `msl/emitter.cpp:1267-1285` | | |
| G4 | SUST sized/raw (.D 8–128 bit) | PARTIAL | `// TODO: convert raw data to the image format`; raw register bits are written as typed components (`decoder/compute.cpp:405-415`) | Eden binds a typeless/compatible view | Wrong for every image whose format is not a matching 32-bit-per-component format (D-20) |
| G5 | SULD (.P / .D) | MISSING | `COMMENT_NOT_IMPLEMENTED("suld")` (`decoder/decoder.cpp:284-286`) | | Compute post-processing reads are lost |
| G6 | SUATOM | PARTIAL | 32-bit only (`decoder/compute.cpp:438`); INC/DEC wrap approximated (`msl/emitter.cpp:1304`); two encodings not implemented (`decoder.cpp:287-291`) | | |
| G7 | SURED | MISSING | `decoder.cpp:280-282` | | |
| G8 | Image atomics usage flag | PARTIAL | `MTLTextureUsageShaderAtomic` is set only when the storage is created as R32Uint/R32Sint (`metal/texture.cpp:54-61`) | | Atomics through a format view of another storage are invalid |

### H. MSAA

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| H1 | Multisample textures (TIC `msaa_mode`, sample count, size scaling, 2DMultisample) | MISSING | No MSAA anywhere in `gpu/` (grep) | `clb097tex.h:527-542`; Eden `image_info.cpp:36, 125-128`, `samples_helper.h` | Unity titles often enable MSAA |
| H2 | Sample positions, TLD.MS sample index, TXQ sample queries | MISSING | `decoder/texture.cpp:827` `// TODO: multisample index` | Eden `samples_helper.h` | |

### I. Texture cache

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| I1 | Storage keyed by guest layout (address reuse with a changed pitch, tiling or block height) | VERIFIED | `const.cpp:739-758`; test `texture_cache_tests.cpp:81-123`; README-crash4-render.md:21 | | |
| I2 | Format aliasing through Metal pixel-format views (same-bpp group) | PARTIAL | Group = (type class, bpp, block dims, depth flag, format if compressed) (`const.cpp:720-737`). No compatibility table | Eden `compatible_formats.cpp:273-285` | Most same-size colour views are legal (probe). `Z32Float` and `Z24Unorm_S8Uint` share a group, which yields a `Depth32Float` view of `Depth32Float_Stencil8` that is not castable (bug D-6) |
| I3 | Isolating compressed formats | VERIFIED | `const.cpp:725-730`; test `texture_cache_tests.cpp:47-75` | | |
| I4 | Mip/layer views, storage level growth, layered RT levels | VERIFIED | `texture_cache.cpp:350-428`; tests `:129-209` | | |
| I5 | RT ↔ texture sharing, including padded small-level RT widths | VERIFIED | `texture_cache.cpp:359-378`; test `:191-209` | | |
| I6 | Same-format alias sync with mip/layer offsets | VERIFIED | `texture_cache.cpp:555-608`; test `:214-267` | | |
| I7 | Cross-format alias sync (different formats, depth ↔ colour, compressed ↔ uncompressed) | PARTIAL | `copyFrom` calls `copyFromTexture` on raw base textures whatever their formats (`texture_cache.cpp:494-497, 593-607`; `metal/texture.cpp:342-383`) | Eden `IsCopyCompatible` (`compatible_formats.cpp:287-294`) plus conversion passes | Metal requires equal pixel formats for texture blits (probe), so this is bug D-5 |
| I8 | Buffer ↔ 2D raw-byte alias sync | VERIFIED | `texture_cache_hot.cpp:263-290`; `metal/texture.cpp:100-239`; tests `:402-660`; README-texture-sampling.md:62-70 | | Limited to single-level, non-depth, uncompressed 2D surfaces |
| I9 | Compressed-block alias copies (2D engine RG32/RGBA32 → guest memory) | VERIFIED | `engines/2d.cpp:79-99, 108-145` | README-crash4-render.md:25 ("verified in game") | Restricted to RG32/RGBA32 formats at layer 0, scale 1 |
| I10 | CPU invalidation → re-upload | VERIFIED | `texture_cache_hot.cpp:81-115, 170-196`; test `:330-342` | | |
| I11 | GPU → guest readback (colour, partial, ordered by write time) | VERIFIED | `texture_cache_hot.cpp:348-536`; tests `:920-1050` | | |
| I12 | Depth/stencil and 3D readback | MISSING | `texture_cache_hot.cpp:389-394` returns early (`ONCE(LOG_NOT_IMPLEMENTED ...)`) | | CPU and DMA readers see stale depth/3D data (D-23) |
| I13 | Texture combining (a new texture that starts before an existing overlapping one) | MISSING | `texture_cache.cpp:465-471` | Eden merges overlaps (`texture_cache.h`) | Both storages stay; correctness relies on sync |
| I14 | Collision-safe keys | PARTIAL | `SmallCache<u32,…>` keyed by XxHash32 of the descriptor with no equality check on hit (`texture_cache.cpp:316-338`; `common/small_cache.hpp:117-185`) | | Bug D-7 |
| I15 | Eviction / destruction | MISSING | `texture_cache.hpp:105` "TODO: destroy textures"; `texture_cache.cpp:410, 500` | | The cache grows without bound, which also raises the collision odds |
| I16 | CPU upload conversion Z24 → D32FS8 | UNKNOWN | `metal/texture.cpp:289-339` assumes depth in bits 0-23 and stencil in 31-24 for every Z24 format | Eden distinguishes `S8_UINT_D24_UNORM` (R = stencil, the low byte) from `D24_UNORM_S8_UINT` (`vk_texture_cache.cpp:308-314`, `maxwell_to_gl.h:123-124`, `surface.cpp:80-83`) | Likely wrong for TIC Z24S8 / RT 0x14 (D-19). Needs a hardware or game check |

### J. Host Metal mapping and fallbacks

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| J1 | TextureFormat → MTLPixelFormat LUT | PARTIAL | `maxwell_to_mtl.cpp:46-179, 358-368` | | `RGBA32Sint` and `RGB5A1Unorm` have no entry, and BC6H/ETC2/EAC map to `Invalid` |
| J2 | D24S8 → `Depth32Float_Stencil8` (Apple silicon) | PARTIAL | `maxwell_to_mtl.cpp:87-93`; `metal/texture.cpp:255-339` | Probe: `D24S8 supported: false` | Upload is converted on the CPU; there is no readback path |
| J3 | ASTC fallback when the device lacks ASTC | MISSING | No capability checks in `renderer/metal` (grep) | Eden `maxwell_to_vk.cpp:248-265`, `textures/astc.h:8` | The x86_64 macOS build (CMakeLists.txt:60-66) would hit `ASSERT(texture != nullptr)` and abort |
| J4 | BCn fallback (iOS GPUs without BC) | MISSING | | Eden `maxwell_to_vk.cpp:266-282`, `texture_cache/decode_bc.h:16` | The iOS target exists (CMakeLists.txt:66-68) |
| J5 | ETC2/EAC (native on Apple GPUs, fallback elsewhere) | MISSING | LUT `Invalid` | Eden `maxwell_to_vk.cpp:283-298` | |
| J6 | 16-bit packed formats on non-Apple GPUs (ABGR4, B5G6R5, A1BGR5, BGR5A1) | MISSING | no conversion path | | These Metal formats are Apple-GPU only |
| J7 | 96-bit RGB32 emulation | MISSING | `RGB32*` → `RGBA32*` "HACK" (`maxwell_to_mtl.cpp:71-73`) while the format info says 12 bytes per pixel (`const.cpp:61-63`) | Eden maps R32G32B32_FLOAT | Latent: the staging stride would be wrong once a TIC case exists |
| J8 | Clamping to Metal limits (mip count, 16384 maximum, square cube, no 1D mips) | MISSING | `metal/texture.cpp:16-85` passes descriptor values straight through | Probe: asserts for too many mips, non-square cube and 1D mips | Bug D-15 |
| J9 | Swizzle composition in Metal views | PARTIAL | `metal/texture_view.cpp:14-23` | | Bug D-4 |
| J10 | Texture-buffer views with a TIC swizzle | IMPLEMENTED_UNVERIFIED | `3d.cpp:1359-1367` | Probe: Metal accepts a swizzled view of a texture buffer | |

---

## (b) TIC/TSC bitfield check against `clb097tex.h`

**Method.** The Hydra structs were copied verbatim into `scratchpad/audit/bitcheck/bitcheck.cpp` and compiled with clang. Each field was filled with ones and the resulting bit range was printed. Results:

- **TIC word 0** (`ImageFormatWord`). COMPONENTS 6:0, R/G/B/A types 9:7, 12:10, 15:13, 18:16, X/Y/Z/W 21:19 … 30:28, PACK 31. All match `NVB097_TEXHEAD_BL_*` (`clb097tex.h:266-408`). Enum values for formats, component types, swizzle sources, header version and texture type all match the doc.
- **TIC words 2-7 (BL/PITCH/1D layouts).** Every decoded field matches its bit range: `addr_hi` 79:64, `hdr_version` 87:85, GOBS 98:96 / 101:99 / 104:102, TILE_WIDTH 108:106, GOB3D 109, LOD_ANISO_QUALITY2 112 … MAX_MIP_LEVEL 127:124, WIDTH 143:128, sRGB 150, TEXTURE_TYPE 154:151, SECTOR_PROMOTION 156:155, BORDER_SIZE 159:157, HEIGHT 175:160, DEPTH 189:176, NORMALIZED 191, TRILIN 197:193, MIP_LOD_BIAS 210:198, ANISO_BIAS 214:211, MAX_ANISO 221:219, view min/max mip 227:224 / 231:228, MSAA 235:232, MIN_LOD_CLAMP 247:236. The 1D buffer width parts 111:96 and 143:128 and the pitch 111:96 match.
- **TSC words 0-7.** address U/V/P, DEPTH_COMPARE 9, function 12:10, sRGB 13, MAX_ANISOTROPY 22:20, MIN 37:36, MIP 39:38, LOD_BIAS 56:44, NORMALIZATION 57, TRILIN 62:58, min/max LOD 75:64 / 87:76, sRGB border R/G/B 95:88, 115:108, 123:116, and the border colours in words 4-7 all match.

**Mismatches and divergences.**

| Field | `clb097tex.h` | Hydra | Impact |
|---|---|---|---|
| TIC w1[8:0] (BL) / w1[4:0] (Pitch) | BL: RESERVED1Y 36:32, **GOB_DEPTH_OFFSET 38:37**, RESERVED1X 40:39, address 63:41 (`:409-412`). Pitch: RESERVED1A 36:32 (`:1149`) | `u32 addr_lo` takes all 32 bits as address (`3d.hpp:29`) | A non-zero GOB depth offset corrupts the address. Eden does the same |
| TIC w2[20:16], w2[31:29], w4[18:16] | RESERVED_ADDRESS, RESERVED2A, RESERVED4A (`:414, 423, 468`) | `view_layer_base_3_7/8_10/0_2` | Borrowed from later docs, as Eden does. Unused by Hydra (B17) |
| TIC w2[24] | RESERVED_HEADER_VERSION (`:421`) | `load_store_hint_maybe` | Naming only |
| TIC w3[9] | SPARSE_ENABLE (`:440`) | unnamed padding | Not decoded |
| TIC w3[14] | USE_ARRAY_TILE_ALIGNMENT (`:449`) | unnamed padding | Not decoded; affects array layer stride |
| TIC w3[2:0] / [12:10] | GOBS_PER_BLOCK_WIDTH / TILE_WIDTH_IN_GOBS | named `tile_width_gobs_log2` (ignored) and `sparse_tile_width_gobs_log2` (used as the block width) | Misleading names, correct bits |
| TIC w5[30] | RESERVED5A (`:495`) | `is_sparse` | Eden also calls it `is_sparse`. Unused |
| TIC w6[0] | RESERVED6Y in BL/PITCH (`:497`); COLOR_KEY_OP in BLCK/PITCHCK (`:782`) | `color_key_op` | OK |
| TIC w7 | BL/PITCH: view mip/MSAA/min LOD. BLCK/PITCHCK: COLOR_KEY_VALUE 255:224 (`:810, 1494`) | union | OK, but image views read `view_mip_min_level` even in colour-key mode (B7) |
| Legacy TEXHEAD0-7, V2, V3 | Selected by SET_SELECT_MAXWELL_TEXTURE_HEADERS=FALSE (`clb197.h:1210`) | not supported | Acceptable on NVN |
| **TSC w1 MAG_FILTER** | **2:0, 3 bits** (POINT=1, LINEAR=2, VCAA_4_TAP=3, VCAA_8_TAP=4; `:2011-2015`) | `u32 mag_filter : 2` plus `u32 : 2` (`3d.hpp:114-115`) | VCAA_8_TAP (4) reads as 0 and VCAA_4_TAP reads as 3; both are UB in the converter. Eden also uses 2 bits |
| TSC w1[3] | MIN_LOD_CLAMP_BEHAVIOR_FOR_NEAREST_MIP (`:2016`) | inside unnamed padding | Ignored |
| TSC w1[9:8] | CUBEMAP_INTERFACE_FILTERING, a 2-bit enum USE_WRAP/OVERRIDE_WRAP/AUTO_SPAN_SEAM/AUTO_CROSS_SEAM (`:2027-2031`) | split into `cubemap_anisotropy:1` (bit 8) and `cubemap_interface_filtering:1` (bit 9), Eden's naming | Unused either way |
| TSC w1[11:10] | not listed in clb097tex.h (gap) | `reduction_filter` (Eden: WeightedAverage/Min/Max) | From later docs; fine |
| TSC w0[16:14], [19:17] | RESERVED0A/0B (`:2000-2001`) | `font_filter_width/height` | Naming only |
| TSC MAX_ANISOTROPY values | 0..7 = 1, 2, 4, 6, 8, 10, 12, 16 (`:2002-2010`) | unused | Note that Eden computes `1 << n` (`texture.cpp:67`), which disagrees with the doc for 3, 5 and 6 |
| Register SET_SAMPLER_BINDING | method 0x1234, register 0x48D (`clb197.h:2054`) | `padding_0x48d` (`3d.hpp:366`) | Linked TSC missing (A7) |
| Register SET_TEX_*_POOL_C | MAXIMUM_INDEX 21:0 / 19:0 | full `u32` | OK, but 0 is treated as unlimited (A1) |
| QMD word 0xB bit 30 | LINKED_TSC (Eden `kepler_compute.h:137`) | not read | A8 |

---

## (c) Format mapping: gaps and errors

**Notation.** "Eden" is the `PixelFormatFromTextureInfo` case (`format_lookup_table.cpp`). "Hydra TIC" is the `IMAGE_FORMAT_CASE` in `renderer/const.cpp:244-344`. "Metal" is the active LUT entry in `maxwell_to_mtl.cpp:46-179`. Types written as one word mean all four components share it; mixed tuples are R,G,B,A.

**Packed bit layouts measured on Apple M1 (appendix, probe 1).**
- `A1BGR5Unorm`: A = bit 0, B = 1-5, G = 6-10, R = 11-15.
- `BGR5A1Unorm`: B = 0-4, G = 5-9, R = 10-14, A = 15.
- `B5G6R5Unorm`: B = 0-4, G = 5-10, R = 11-15.
- `ABGR4Unorm`: A = 0-3, B = 4-7, G = 8-11, R = 12-15.

Maxwell names are MSB-first, so R is in the low bits for A8B8G8R8, A2B10G10R10, B5G6R5, A1B5G5R5 and A4B4G4R4, as Eden's Vulkan mappings confirm.

| TIC code | Eden encodings → PixelFormat | Hydra TIC | Hydra Metal host | Status / issue |
|---|---|---|---|---|
| 0x01 R32G32B32A32 | FLOAT, UINT, SINT | FLOAT ✓ UINT ✓ SINT ✓ | RGBA32Float ✓, RGBA32Uint ✓, **no RGBA32Sint entry** | **SINT crashes** (D-1) |
| 0x02 R32G32B32 | FLOAT | ✗ | (`RGB32*` → RGBA32 "HACK" with the wrong staging stride) | MISSING; needs 12→16 byte expansion on upload or a buffer-only path |
| 0x03 R16G16B16A16 | FLOAT, UNORM, SNORM, UINT, SINT | all ✓ | RGBA16* ✓ | OK |
| 0x04 R32G32 | FLOAT, UINT, SINT | all ✓ | RG32* ✓ | OK |
| 0x05 R32_B24G8 | (FLOAT, UINT, UNORM, UNORM) → D32_FLOAT_S8_UINT | ✗ | | MISSING (map to Z32Float_X24S8Uint) |
| 0x06 ETC2_RGB | UNORM, UNORM+sRGB | ✗ | `ETC2_RGB` → Invalid | MISSING (Metal `ETC2_RGB8`, `_sRGB` on Apple GPUs) |
| 0x07 X8B8G8R8 | — | ✗ | (RGBX8Unorm → RGBA8 exists) | Eden also lacks it; trivial (force A = 1 via swizzle) |
| 0x08 A8B8G8R8 | UNORM, SNORM, UINT, SINT, UNORM+sRGB | all ✓ | RGBA8* ✓ | OK |
| 0x09 A2B10G10R10 | UNORM, UINT | UNORM ✓, **UINT ✗** | RGB10A2Uint exists in the LUT | MISSING case |
| 0x0A ETC2_RGB_PTA | UNORM, sRGB | ✗ | Invalid | MISSING (Metal `ETC2_RGB8A1*`) |
| 0x0B ETC2_RGBA | UNORM, sRGB | ✗ | Invalid | MISSING (Metal `EAC_RGBA8*`) |
| 0x0C R16G16 | FLOAT, UNORM, SNORM, UINT, SINT | all ✓ | ✓ | OK |
| 0x0D G8R24 | — | ✗ | | Eden also lacks it |
| 0x0E G24R8 | (UINT, UNORM, UNORM, UNORM) → S8_UINT_D24_UNORM | ✓ → Z24Unorm_S8Uint | Depth32Float_Stencil8 | OK (stencil/depth plane selection VERIFIED) |
| 0x0F R32 | FLOAT, UINT, SINT | all ✓ | ✓ | OK |
| 0x10 BC6H_SF16 | FLOAT → BC6H_SFLOAT | ✗ | Invalid | MISSING (Metal `BC6H_RGBFloat`) |
| 0x11 BC6H_UF16 | FLOAT → BC6H_UFLOAT | ✗ | Invalid | MISSING (Metal `BC6H_RGBUfloat`) |
| 0x12 A4B4G4R4 | UNORM | ✓ → RGBA4Unorm | ABGR4Unorm with component indices {3,2,1,0} | Identity swizzle correct; **non-identity swizzles wrong** (D-4) |
| 0x13 A5B5G5R1 | UNORM → R5G5B5A1 + SwapSpecial | ✗ | | MISSING (Metal `A1BGR5Unorm` + source remap R↔A, G↔B) |
| 0x14 A1B5G5R5 | UNORM → A1R5G5B5 + SwapBlueRed (`maxwell_to_vk.cpp:135`) | ✓ → A1BGR5Unorm | **A1BGR5Unorm (A in bit 0)** | **Wrong layout** (D-3). Should be `BGR5A1Unorm` + R/B source swap |
| 0x15 B5G6R5 | UNORM → B5G6R5_UNORM (R in the low bits) | ✓ → B5G6R5Unorm | B5G6R5Unorm (B in bits 0-4) plus position swizzle (z,y,x,ONE) | Correct for permutation swizzles; **wrong for replicated swizzles and ignores W** (D-4). Should use a source R/B swap |
| 0x16 B6G5R5 | — | ✗ | | Eden also lacks it |
| 0x17 BC7U | UNORM, UNORM+sRGB | UNORM ✓, **sRGB ✗** | BC7_RGBAUnorm_sRGB exists | MISSING case |
| 0x18 G8R8 | UNORM, SNORM, UINT, SINT | UNORM ✓ SNORM ✓ **UINT ✗ SINT ✗** | RG8Uint/Sint exist | MISSING cases |
| 0x19 EAC | UNORM, SNORM | ✗ | Invalid | MISSING (Metal `EAC_R11Unorm/Snorm`) |
| 0x1A EACX2 | UNORM, SNORM | ✗ | Invalid | MISSING (Metal `EAC_RG11*`) |
| 0x1B R16 | FLOAT, UNORM, SNORM, UINT, SINT | all ✓ (FLOAT appears twice, at :244 and :327) | ✓ | OK |
| 0x1C Y8_VIDEO | — | ✗ | | Eden also lacks it |
| 0x1D R8 | UNORM, SNORM, UINT, SINT | all ✓ | ✓ | OK |
| 0x1E G4R4 | UNORM → R4G4 + SwapGreenRed | ✗ | | MISSING (no 8-bit packed Metal format; expand to RG8Unorm on upload) |
| 0x1F R1 | — | ✗ | | Eden also lacks it |
| 0x20 E5B9G9R9 | FLOAT | ✗ | RGB9E5Float entry exists and has an identical bit layout | MISSING case (trivial) |
| 0x21 BF10GF11RF11 | FLOAT | ✓ | RG11B10Float ✓ | OK |
| 0x22/0x23 G8B8G8R8 / B8G8R8G8 | — | ✗ | | Eden also lacks them (Metal GBGR422/BGRG422 exist) |
| 0x24 DXT1 | UNORM, sRGB | ✓ ✓ (BC1_RGB → Metal BC1_RGBA) | ✓ | OK; 1-bit alpha preserved |
| 0x25 DXT23 | UNORM, sRGB | ✓ ✓ | ✓ | OK |
| 0x26 DXT45 | UNORM, sRGB | ✓ ✓ | ✓ | OK |
| 0x27 DXN1 | UNORM, SNORM | UNORM ✓, **SNORM ✗** | BC4_RSnorm exists | MISSING case |
| 0x28 DXN2 | UNORM, SNORM | ✓ ✓ | ✓ | OK |
| 0x29 Z24S8 | (UINT, UNORM, UNORM, UNORM), (UINT, UNORM, UINT, UINT) | first ✓, **second ✗** | D32FS8 | MISSING alternate encoding; upload layout suspect (D-19) |
| 0x2A X8Z24 | UNORM; (UNORM, UINT, UINT, UINT) | **✗ ✗** | Z24Unorm_X8Uint → D32FS8 exists | MISSING (also the RT format 0x15 → LOG_FATAL, D-2) |
| 0x2B S8Z24 | (UNORM, UINT, UINT, UINT) → D24_UNORM_S8_UINT | **✗** | | MISSING; needs a different upload layout from Z24S8 |
| 0x2C-0x2E, 0x31-0x39, 0x3B-0x3E | — (coverage/VCAA) | ✗ | | Not needed |
| 0x2F ZF32 | FLOAT; (FLOAT, UINT, UINT, UINT) | first ✓, **second ✗** | Depth32Float | MISSING alternate (shadow maps) |
| 0x30 ZF32_X24S8 | (FLOAT, UINT, UNORM, UNORM) | ✓ | D32FS8 (8 bytes: float plus stencil low byte) | OK |
| 0x3A Z16 | UNORM; (UNORM, UINT, UINT, UINT) | first ✓, **second ✗** | Depth16Unorm | MISSING alternate |
| 0x40-0x57 ASTC 2D (14 sizes) | UNORM, sRGB each (28) | all 28 ✓ | ASTC_*_LDR / `_sRGB` | OK on Apple GPUs; no fallback (J3) |

**Totals.** Eden has 111 encodings. Hydra has 83 unique TIC cases (86 matches, 2 macro definitions, 1 duplicate), all present in Eden. The 28 missing ones:

- A2B10G10R10 UINT
- G4R4
- A5B5G5R1
- G8R8 UINT, SINT
- R32G32B32 FLOAT
- E5B9G9R9
- Z32 (F, U, U, U)
- Z16 (UN, U, U, U)
- X8Z24 ×2
- Z24S8 (U, UN, U, U)
- S8Z24
- R32B24G8
- DXN1 SNORM
- BC7 sRGB
- BC6H S and U
- ETC2 ×6
- EAC ×4

A script cross-check of the two tables is reproduced in the audit transcript.

**Further host-side mapping problems.**
- `TextureFormat::RGB5A1Unorm` has no Metal LUT entry (unreachable today).
- `RGB5Unorm`, `BGR5Unorm` and `BGRX8*` map to formats with alpha (HACK). RT-only.
- The `Z24Unorm_S8Uint`, `Z24Unorm_X8Uint` and `Z32Float_X24S8Uint` hacks all become `Depth32Float_Stencil8` with CPU conversion on upload and no readback.
- Packed 16-bit, ASTC and ETC2 formats exist only on Apple GPUs, and no capability query is made.

---

## (d) Confirmed bugs, with code quotes

Severity: **S1** = crash or abort, or a texture definitely wrong in common use. **S2** = wrong rendering in identifiable situations. **S3** = edge case, latent, or robustness.

### D-1 (S1) RGBA32 SINT has no Metal pixel-format entry: dereference of `end()`

```cpp
// renderer/const.cpp:344 and :367
IMAGE_FORMAT_CASE(RGBA32, Sint, Sint, Sint, Sint, RGBA32Sint)
COLOR_SURFACE_FORMAT_CASE(RGBA32Sint, RGBA32Sint)
// metal/maxwell_to_mtl.cpp:83-85 (active LUT; RGBA32Sint appears only in the commented-out block at :187)
COLOR_PIXEL_FORMAT_ENTRY_RGBA(RGBA32Float, RGBA32Float),
COLOR_PIXEL_FORMAT_ENTRY_RGBA(RGBA32Uint, RGBA32Uint),
DS_PIXEL_FORMAT_ENTRY(S8Uint, Stencil8, false, true),
// :358-367
auto it = pixel_format_lut.find(format);
ASSERT_DEBUG(it != pixel_format_lut.end(), ...);   // (void) in release (common/log.hpp:74)
const auto& info = it->second;                        // UB when not found
```

Any RGBA32 SINT texture, image or render target (colour surface 0xC1) is undefined behaviour in release and aborts in debug.

Fix: add `COLOR_PIXEL_FORMAT_ENTRY_RGBA(RGBA32Sint, RGBA32Sint)`. Then make `toMtlPixelFormatInfo` return an "unsupported" sentinel instead of dereferencing.

### D-2 (S1) Depth surface format X8Z24 (0x15) aborts the process

```cpp
// gpu/const.hpp:674
Z24X8Unorm = 0x15,
// renderer/const.cpp:438-452 — no case for Z24X8Unorm
DEPTH_SURFACE_FORMAT_CASE(S8Z24Unorm, Z24Unorm_S8Uint) // TODO
DEPTH_SURFACE_FORMAT_CASE(Z24S8Unorm, Z24Unorm_S8Uint)
...
default:
    LOG_FATAL(Gpu, "Depth surface format {}", depth_surface_format);   // abort()
```

NVN `DEPTH24` (X8Z24) depth buffers terminate Hydra. `TextureFormat::Z24Unorm_X8Uint` already exists and is mapped in the Metal LUT (`maxwell_to_mtl.cpp:87`). Eden: `surface.cpp:92-93`. This touches the render-target path but is a format-mapping bug.

### D-3 (S2) TIC A1B5G5R5 uses Metal `A1BGR5Unorm`, whose bit layout is incompatible

```cpp
// renderer/const.cpp:267
IMAGE_FORMAT_CASE(A1BGR5, Unorm, Unorm, Unorm, Unorm, A1BGR5Unorm)
// metal/maxwell_to_mtl.cpp:163 (identity component indices)
COLOR_PIXEL_FORMAT_ENTRY_RGBA(A1BGR5Unorm, A1BGR5Unorm),
```

| | R | G | B | A |
|---|---|---|---|---|
| Maxwell A1B5G5R5 | bits 0-4 | 5-9 | 10-14 | 15 |
| Metal `A1BGR5Unorm` (measured) | 11-15 | 6-10 | 1-5 | 0 |

The Maxwell layout follows Eden: VK A1R5G5B5 + SwapBlueRed, `maxwell_to_vk.cpp:135`, `vk_texture_cache.cpp:746-747`.

Every channel is misread. Alpha comes from Maxwell R's least significant bit, and no swizzle can repair this. Correct mapping: `MTLPixelFormatBGR5A1Unorm` (measured B 0-4, G 5-9, R 10-14, A 15) with an R↔B source remap. The NvColorFormat path even admits it: `NV_COLOR_FORMAT_CASE(R5G5B5A1, A1BGR5Unorm) // Doesn't work` (`const.cpp:191`).

### D-4 (S2) Swizzle composition permutes positions instead of remapping sources

```cpp
// metal/texture_view.cpp:14-23
MTL::TextureSwizzle swizzle_components[] = {
    toMtlSwizzle(descriptor.swizzle_channels.r), ... g, b, a };
MTL::TextureSwizzleChannels swizzle_channels_mtl(
    swizzle_components[pixel_format_info.component_indices[0]], ...[1], ...[2], ...[3]);
// metal/maxwell_to_mtl.cpp:96
COLOR_PIXEL_FORMAT_ENTRY(RGBA4Unorm, ABGR4Unorm, ZTD_PASS({3, 2, 1, 0})),
// renderer/const.cpp:707-711 (TIC B5G6R5 lands here)
case TextureFormat::B5G6R5Unorm: ...
    SWIZZLE(z, y, x, ImageSwizzle::OneFloat);
```

The correct composition is `out[i] = host_channel_of(guest_source[i])`. Hydra computes `out[i] = guest_source[perm[i]]`. The two agree only for identity or symmetric swizzles.

Example: an ABGR4 TIC with swizzle (R,R,R,ONE), i.e. luminance.
- Expected: Metal (A,A,A,1), because guest R lives in Metal A.
- Hydra produces red = toMtl(swizzle.a = ONE) → 1, green = toMtl(swizzle.b = R) → Metal red (guest A), and so on.

B5G6R5 with (R,R,R,ONE) yields Metal R (guest B) in every channel, and any TIC W component is replaced by ONE.

Fix: remap swizzle *values* through the host channel map, as Eden's `SwapBlueRed`, `SwapSpecial` and `SwapGreenRed` do (`vk_texture_cache.cpp:550-593`).

### D-5 (S2) Texture-to-texture copies between different pixel formats break Metal's blit rule

```cpp
// texture_cache.cpp:494-497 (new storage absorbs overlapping storages of the same group, which may have another format)
storage.base->copyFrom(command_buffer, other_base, 0, 0, overlapping_storage.level, ...);
// texture_cache.cpp:593-597 (padded-layout path; reached across groups with equal row bytes, e.g. RGBA16F vs RGBA8)
storage.base->copyFrom(command_buffer, other_storage.base, uint3({0, 0, 0}), other_level, ...);
// metal/texture.cpp:356-359
encoder->copyFromTexture(mtl_src, src_layer + i, src_level + j, ..., texture, dst_layer + i, ...);
```

Measured with the Metal debug layer: `[sourceTexture pixelFormat](RGBA8Unorm) must equal [destinationTexture pixelFormat](R32Uint)`. The same assert fires for Depth32Float→R32Float, BC1→RG32Uint and D32FS8→RGBA8.

A group holds storages of different formats with the same bytes per pixel. `updateImpl` syncs sources from every group (`texture_cache_hot.cpp:229-262`). In release builds these copies are undefined.

Fix:
- Same-size formats: blit from a `newTextureView(dst_format)` of the source.
- Size-compatible compressed ↔ uncompressed: route through a buffer.
- Depth ↔ colour: use a compute or render pass.
- Otherwise refuse the copy and mark the alias unsupported.

### D-6 (S3) `Z32Float` and `Z24Unorm_S8Uint` share a cache group, which requests an illegal view

```cpp
// renderer/const.cpp:720-737 (group = type class, bpp, block dims, is_depth_stencil; format only for compressed)
// metal/maxwell_to_mtl.cpp:89-91
DS_PIXEL_FORMAT_ENTRY(Z32Float, Depth32Float, true, false),
DS_PIXEL_FORMAT_ENTRY(Z24Unorm_S8Uint, Depth32Float_Stencil8, true, true), // HACK
```

Both are 4-bpp depth formats, so a ZF32 TIC over a D24S8 render target resolves to a view of the D32FS8 storage with `Depth32Float`. The debug layer reports that `Depth32Float` is "not castable" from `Depth32Float_Stencil8`. In release the view is nil, and `TextureView` does not check for nil (`metal/texture_view.cpp:42-46`).

Fix: put the host pixel format, or a host-compatibility class, into the group hash.

### D-7 (S2, latent) Texture storages, views and samplers are found by 32-bit hash only

```cpp
// texture_cache.cpp:316-338
const auto group_hash = descriptor.getGroupHash();
const auto storage_hash = descriptor.getStorageHash();
auto storage_opt = group.cache.find(storage_hash);
if (storage_opt) { ... return getTextureView(...); }       // no descriptor equality check
// common/small_cache.hpp:178-185 — find(KeyT key) compares only the u32 key
// common/types.hpp:352-364 (SamplerCache) — std::map<u32, T> keyed by XxHash32
```

Storages are never destroyed (`texture_cache.hpp:105`, `texture_cache.cpp:410, 500`), and merged memories can hold thousands of storages. By the birthday bound, a group of 30k storages has roughly a 10% chance of a collision. A collision silently returns another texture's storage or another sampler. The resulting wrong textures would vary from run to run, because guest addresses change.

Fix: keep the full descriptor in each entry and compare on hit (or key a `std::unordered_map` by the descriptor).

### D-8 (S2) The TSC→Metal sampler drops LOD bias, anisotropy, reduction, normalization and sRGB border

```cpp
// engines/3d.cpp:1532-1549 — only these TSC fields are consumed
.min_filter = ..., .mag_filter = ..., .mip_filter = ..., .address_mode_s/t/r = ...,
.depth_compare_op = ..., .border_color_u = ..., .min_lod = ..., .max_lod = ...,
// renderer/const.hpp:310-322
struct SamplerDescriptor { ...; f32 min_lod = 0.0f; f32 max_lod = 15.0f; // TODO: more };
```

`mip_lod_bias`, `max_anisotropy`, `reduction_filter`, `float_coord_normalization`, `srgb_conversion` and the sRGB border bytes are ignored. Metal supports `lodBias` and `maxAnisotropy` on M1 (probe), but min/max reduction is not supported there. UE4 with dynamic resolution (Crash 4 renders at 640×360 inside 1280×720) relies on a negative sampler mip bias.

### D-9 (S2) The border-colour approximation snaps wrongly

```cpp
// metal/sampler.cpp:27-31
if (border_color.w() == 1.0f) {
    if (border_color.x() == 1.0f || border_color.y() == 1.0f || border_color.z() == 1.0f) {
        mtl_border_color = MTL::SamplerBorderColorOpaqueWhite;
```

- (1,0,0,1) becomes opaque white, and (0,0,0.99,1) becomes opaque black.
- Integer-format border colours are reinterpreted as floats.
- sRGB border conversion is ignored.
- For depth-compare samplers, a border such as (1,0,0,0) (alpha 0, depth 1) becomes transparent black (depth 0), so shadow-map edges flip.
- A `LOG_WARN` fires on every sampler creation.

### D-10 (S3) MIRROR_ONCE_BORDER loses mirroring; CLAMP_OGL ignores the filter

```cpp
// metal/maxwell_to_mtl.cpp:821-831
case SamplerAddressMode::Clamp:            LOG_NOT_IMPLEMENTED(..., "Clamp");  return ClampToEdge;
case SamplerAddressMode::MirrorClampToBorder: LOG_NOT_IMPLEMENTED(...);        return ClampToBorderColor;
```

Eden maps MirrorOnceBorder to MIRROR_CLAMP_TO_EDGE, and Clamp with Linear filtering to CLAMP_TO_BORDER (`maxwell_to_vk.cpp:65-80`). Both cases also log every time a sampler is created.

### D-11 (S3) Undefined behaviour on unhandled enum values

```cpp
// metal/maxwell_to_mtl.cpp:790-797 — no case for 0 or 3 (MIN_ANISO, clb097tex.h:2022) and no return after the switch
MTL::SamplerMinMagFilter toMtlSamplerMinMagFilter(const SamplerFilter filter) {
    switch (filter) { case Nearest: ...; case Linear: ...; } }
// :799-808 toMtlSamplerMipFilter — value 0 is unhandled; :714-730 toMtlSwizzle — value 1 is unhandled
// engines/3d.cpp:35-55 toTextureType — values 9..15 are unhandled
// engines/3d.hpp:114 — u32 mag_filter : 2;  (doc: 3 bits, so VCAA_8_TAP=4 truncates to 0)
```

A zero-filled TSC slot, or MIN_ANISO, returns garbage into `MTLSamplerDescriptor`.

### D-12 (S3) PITCH_COLOR_KEY textures are decoded as block linear

```cpp
// engines/3d.cpp:1382-1394
bool is_linear = false; u32 linear_stride = 0;
switch (tic.hdr_version) {
case TicHdrVersion::Pitch: is_linear = true; linear_stride = static_cast<u32>(tic.pitch_5_20) << 5u; break;
case TicHdrVersion::BlockLinear: break;
default: LOG_NOT_IMPLEMENTED(Engines, "TIC HDR version {}", tic.hdr_version); break;  // PitchColorKey lands here
}
```

### D-13 (S2) Linked TSC (VIA_HEADER_BINDING / QMD LINKED_TSC) is unsupported

```cpp
// engines/3d.cpp:1600-1601, 1673
const auto image_handle = getImageHandle(texture_handle);
const auto sampler_handle = getSamplerHandle(texture_handle);
const auto& tsc = tex_sampler_pool[sampler_handle];
// engines/3d.hpp:366 — register 0x48D (SET_SAMPLER_BINDING) is padding; compute.cpp:240 uses getSamplerHandle too
```

Eden: `TexturePair(raw, via_header_index)` returns `{raw, raw}` (`texture.h:159-166`).

### D-14 (S3) Missing or inconsistent TIC/TSC bounds checks

```cpp
// engines/3d.cpp:1078 (getShader, buffer/integer specialization) — no check against tex_header_pool_max_index
const auto& tic = pool[getImageHandle(*handle)];
// compute.cpp:240 — no tsc_limit check
three_d.getSampler(tsc_pool[getSamplerHandle(*handle)]);
// engines/3d.cpp:1602-1605, compute.cpp:129 — max index 0 is treated as "no limit"
if ((regs.tex_header_pool_max_index != 0 && image_handle > regs.tex_header_pool_max_index) || ...
```

### D-15 (S1/S2) Texture descriptors are not legalized for Metal

```cpp
// engines/3d.cpp:1396-1412 — 2D/1D/Cube with depth>1 keep their non-array type but get layer_count = depth
if (type != renderer::TextureType::_3D) { layer_count = depth; depth = 1; ... }
const u32 level_count = tic.mip_max_levels + 1;            // not clamped to the dimensions
// metal/texture.cpp:25, 43-47, 82-84
desc->setMipmapLevelCount(descriptor.level_count);         // 1D with >1 level asserts in Metal
default: ASSERT_DEBUG(descriptor.layer_count == 1, ...);   // release: 2D texture, but uploads target slices > 0
ASSERT(texture != nullptr, MetalRenderer, "Failed to create {} texture ...");  // abort
```

Measured: Metal asserts for "MTLTextureType1D requires that mipmapLevelCount is 1", for "requests 5 mipmap levels … (4,4,1) can only support … 3", and for "width must equal height for cube textures".

Eden promotes 1D/2D/Cube to array types when `Depth()>1 || BaseLayer()!=0` (`image_info.cpp:54-68`).

### D-16 (S3) Layer-stride alignment ignores tile-width spacing

```cpp
// renderer/const.cpp:821-840 — no tile_width_spacing parameter
u32 alignLayerSize(u32 layer_size, u32 height, u32 depth, u32 block_height,
                   u32 block_height_gobs_log2, u32 block_depth_gobs_log2) { ... }
```

Eden (`util.cpp:296-303`):

```cpp
if (tile_width_spacing > 0) {
    const u32 alignment_log2 = GOB_SIZE_SHIFT + tile_width_spacing + block.height + block.depth;
    return Common::AlignUpLog2(size_bytes, alignment_log2);
}
```

Array or cube textures with TILE_WIDTH_IN_GOBS > 0 get the wrong layer stride, so every layer after the first is garbage.

### D-17 (S3) A shift by −1 is evaluated before the zero guard

```cpp
// renderer/const.cpp:773-778
while (dim <= (gob_dim << (size_log2 - 1)) && size_log2 != 0)   // size_log2==0 → shift by 0xFFFFFFFF (UB)
```

This works by accident on ARM64 and x86 because shift counts wrap modulo 32. Eden checks `shift == 0` first (`util.cpp:77-91`).

### D-18 (S3) Bindless index encoding truncates handles

```cpp
// renderer/const.hpp:397-399, 450-451
constexpr u32 TRACKED_TEXTURE_WORD_MASK = 0x1fff;        // cbuf word offsets ≥ 0x1fff unrepresentable (0x1fff = NO_SAMPLER)
constexpr u32 CONSTANT_TEXTURE_HANDLE_MASK = (1u << 26) - 1;   // constant handles keep TSC bits 20..25 only → TSC index ≥ 64 lost
```

### D-19 (S2, needs verification) The Z24 depth upload assumes depth in the low 24 bits for every Z24 format

```cpp
// metal/texture.cpp:312-319
default: { // Z24Unorm_S8Uint, Z24Unorm_X8Uint
    depth[i] = static_cast<f32>(word & 0xffffffu) / static_cast<f32>(0xffffffu);
    stencil[i] = descriptor.format == TextureFormat::Z24Unorm_S8Uint ? static_cast<u8>(word >> 24) : 0;
// renderer/const.cpp:316-317, 442-443 — TIC Z24S8/G24R8 and RT 0x14 and 0x16 all map to Z24Unorm_S8Uint
```

Two Maxwell layouts are collapsed into one host format with a single decoder:
- Eden `surface.cpp:80-83`: RT 0x14 → S8_UINT_D24_UNORM, RT 0x16 → D24_UNORM_S8_UINT.
- Eden `vk_texture_cache.cpp:312-314`: "S8_UINT_D24_UNORM: R = stencil".
- Eden `maxwell_to_gl.h:124`: GL_UNSIGNED_INT_24_8 puts depth in the high 24 bits.
- Hydra itself treats R as stencil when sampling (test `:818`).

At least one of the two is decoded wrongly. This only matters when guest memory holds depth that was not rendered by the host: DMA or 2D copies, re-uploads, or CPU-generated depth.

### D-20 (S2) SULD is missing and SUST.D writes raw bits into typed images

```cpp
// decoder/decoder.cpp:284-286
INST(0xeb00000000000000, 0xffe0000000000000) { COMMENT_NOT_IMPLEMENTED("suld"); }
// decoder/compute.cpp:405-415
// TODO: convert raw data to the image format
ONCE(LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Sized surface store"));
for (u32 i = 0; i < count; i++) values[i] = getRegOffset(data, i);
```

The emitter then writes `vec<T,4>(as_type<T>(uint(r)), …)` into the TIC-typed image (`msl/emitter.cpp:1267-1285`). For SUST.D.32 into an RGBA8Unorm image this stores the raw word, reinterpreted as a float, into the red channel.

### D-21 (S3) Integer textures are sampled with the TSC's linear filter

```cpp
// msl/emitter.cpp:874-877
if (is_integer && !any(flags & TextureSampleFlags::IntCoords))
    LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Filtered integer texture sample");
```

Eden forces a nearest-filter sampler variant (`vk_texture_cache.cpp:2852`).

### D-22 (S2) Sampled views ignore the TIC view mip range

```cpp
// engines/3d.cpp:1485-1507
// Sampled views keep every level: limiting them to the TIC's view range broke streamed textures.
const auto view_levels = is_image ? ztd::Range<u32>::fromSize(view_min_level, 1)
                                  : ztd::Range<u32>(0, level_count);
```

Mip filter NONE, TXQ and LOD clamps all use storage level 0 instead of `RES_VIEW_MIN_MIP_LEVEL` (Eden `image_view_info.cpp:38-42`). The earlier attempt to honour the range corrupted streamed textures. That regression should be root-caused (candidates: interaction with storage level growth, or LOD clamps that are view-relative) rather than worked around.

### D-23 (S2) Depth/stencil and 3D textures are never written back to the guest

```cpp
// hot/texture_cache_hot.cpp:389-394
if (getTextureFormatInfo(descriptor.format).is_depth_stencil || descriptor.type == TextureType::_3D) {
    ONCE(LOG_NOT_IMPLEMENTED(Gpu, "Flushing {} texture with format {}", ...)); return; }
```

---

## (e) Prioritized implementation backlog

Testing conventions follow the existing Metal tests:
- `tests/texture_cache_tests.cpp`: a real `metal::Renderer` and `TextureCache`, Metal readback, run under `MTL_DEBUG_LAYER=1`.
- `tests/shader_library_tests.cpp`: decoder → MSL → GPU execution, as in `testTextureLodSelection`.

Running every new test under `MTL_DEBUG_LAYER=1` turns API misuse (D-5, D-6, D-15) into failures instead of silent UB.

| P | ID | What | Why (games) | How (Hydra files/functions) | Test | Risk / effort |
|---|---|---|---|---|---|---|
| **P0** | 1 | Crash-class format fixes: add `RGBA32Sint` to the LUT; map `DepthSurfaceFormat::Z24X8Unorm` to `Z24Unorm_X8Uint`; make `toMtlPixelFormatInfo` and every `to*` converter total (default cases; treat MIN_ANISO as Linear; mip 0 as NotMipmapped; swizzle 1 as Zero) | Any title using RGBA32I images/RTs or NVN DEPTH24 aborts today | `metal/maxwell_to_mtl.cpp:46-179, 358-368, 714-833`; `renderer/const.cpp:433-455`; `engines/3d.cpp:35-55` | Unit test: iterate every `TextureFormat` and create a 4×4 storage through `TextureCache::find`. Assert a non-null native texture or a clean "unsupported" result. Exhaustive TSC enum sweep through `SamplerCache::find` | Very low / hours |
| **P0** | 2 | Legalize texture descriptors for Metal: promote 1D/2D/Cube to arrays when depth > 1 (Eden `image_info.cpp:54-68`); emulate 1D (and 1D arrays) as 2D with height 1, in both the texture cache and the MSL declarations (`texture2d` with y = 0); clamp `level_count` to `floor(log2(max dim)) + 1`; reject or clamp non-square cubes and sizes > 16384 | Removes `ASSERT(texture != nullptr)` aborts and debug-layer asserts | `engines/3d.cpp:1396-1474` (`getTexture`); `metal/texture.cpp:16-85`; `msl/emitter.cpp:51-75` plus 1D sampling paths | Metal tests: TIC 1D with 3 levels sampled at level 1; a 2D TIC with depth 3 bound to a 2D-array shader; a 4×4 TIC with 8 levels. All under the debug layer | Low-medium / 1-2 days |
| **P0** | 3 | Collision-safe cache keys: store the descriptor in `TextureStorage` (it already has `base->getDescriptor()`) and in group entries; compare on hit; on mismatch probe or rehash. Same for `CacheBase` (SamplerCache) and the view cache | Wrong textures that vary between runs; matches the open "garbage textures" symptom class | `texture_cache.cpp:316-338` (`addToMemory`), `getTextureView`, `mergeMemories`; `common/types.hpp:352-364`; `common/small_cache.hpp` | Inject a test-only hash override (or construct two descriptors with equal `getStorageHash`) and assert distinct storages. Sampler variant likewise | Low / day |
| **P1** | 4 | Fix cross-format copies (D-5) and grouping (D-6): add host-format classes to the group hash; in `Texture::copyFrom`, blit through `newTextureView(dst_format)` when bpp matches; compressed ↔ uncompressed via buffer; depth ↔ colour via a compute kernel (D32FS8 → R32F/RGBA8 and back); otherwise `reportUnsupportedAlias` | UE4 aliases G-buffers/depth across formats; current behaviour is undefined in release and asserts in debug | `metal/texture.cpp:342-383`; `renderer/texture_cache.cpp:430-504, 555-651`; `renderer/const.cpp:720-737` | Debug-layer test: RGBA16F RT and RGBA8 sampled alias with equal row bytes; R32F storage absorbing an RGBA8 overlap; readback of the converted bytes | Medium / 2-3 days; touches the hot sync path |
| **P1** | 5 | Complete the TIC format table (28 Eden encodings): depth alternates (Z16/ZF32/Z24S8 with UINT unused channels, X8Z24 ×2, S8Z24, R32B24G8); BC6H S/U (Metal BC6H_RGBFloat/Ufloat); BC7 sRGB; BC4 SNORM; RG8 UINT/SINT; A2B10G10R10 UINT; E5B9G9R9; ETC2/EAC (native `ETC2_*`/`EAC_*` on Apple GPUs); R32G32B32 (expand 12→16 bytes in `decodeTextureHot2` or use texture_buffer); A5B5G5R1 and G4R4 (D-4 remap / RG8 expansion). Normalize FORCE_FP16 to UNORM/SNORM; drop the sRGB bit for formats that have no sRGB variant (with a log) instead of `Invalid` | Shadow maps/depth textures with alternate encodings are currently unbound (black); BC6H for HDR cubemaps/lightmaps; ETC2 in mobile-ported Unity titles | `renderer/const.cpp:220-355` (consider a table keyed like Eden's `Hash()`), `renderer/const.hpp:30-162`, `metal/maxwell_to_mtl.cpp`; S8Z24 needs its own `TextureFormat` and upload converter | Table-driven test over all 111 Eden encodings asserting `toTextureFormat != Invalid`. Per-family Metal readback against a CPU reference (BC6H block, ETC2 block, E5B9G9R9 word, Z16 alternate) | Low (formats previously unbound start drawing) / 2-3 days |
| **P1** | 6 | Packed formats and swizzle composition: TIC A1B5G5R5 → `BGR5A1Unorm` with R↔B source swap; B5G6R5 → source swap instead of the position permutation; RGBA4 → value remap; compose `out[i] = host(guest_src[i])` in `metal::TextureView` and `SwizzleChannels` (replace `component_indices` with a source-remap table) | Wrong colours in UI and old-style 16-bit textures (common in 2D/indie titles) | `metal/texture_view.cpp:14-23`; `metal/maxwell_to_mtl.cpp:96-99, 160-163`; `renderer/const.cpp:557-718` | Metal readback: for each packed format, upload 0x001F/0x03E0/0x7C00/0x8000 (or analogous) and sample with swizzles (R,G,B,A), (R,R,R,1), (A,A,A,A), (B,G,R,A). Compare with a reference decode of the Maxwell layout | Low / day |
| **P1** | 7 | Sampler completeness: add `lod_bias` (sign-extended 13-bit / 256 → `setLodBias`); `max_anisotropy` (doc table 1,2,4,6,8,10,12,16 → `setMaxAnisotropy`, only for linear min/mip; MIN_ANISO → linear plus anisotropy); CLAMP_OGL (Linear → ClampToBorder) and MIRROR_ONCE_BORDER (→ MirrorClampToEdge) per Eden; force a nearest variant for integer-format views; hash all new fields | UE4 dynamic resolution mip bias (Crash 4, Jump Force); texture sharpness on oblique surfaces | `renderer/const.hpp:310-322`; `engines/3d.cpp:1521-1552`; `metal/sampler.cpp`; `sampler_cache.cpp:13-27`; integer variant chosen at bind time in `ThreeD::configureShaderStage` / `Renderer::bindTexture` | Extend `testTextureLodSelection` with a TSC bias of ±1 (expect the mip colour shift). Wrap tests that sample at u = −0.25/1.25. Unit test of TSC decode → `SamplerDescriptor` | Low / day |
| **P1** | 8 | Linked TSC and bounds: decode register 0x48D in `Regs3D` and QMD word 0xB bit 30; route through a shared `resolveTexturePair(handle, via_header)`; fix `max_index` semantics (0 means one entry) and check every pool index (`getShader` and compute TSC) | GL-driver/homebrew titles use VIA_HEADER_BINDING; garbage handles can read 32 MiB past the pool | `engines/3d.hpp:366`, `engines/3d.cpp:1066-1090, 1590-1680`, `engines/compute.hpp:47-67`, `engines/compute.cpp:121-132, 225-247` | Engine-level test: fake guest TIC/TSC pools; with sampler_binding=1 assert the bound sampler equals TSC[tic_index]; out-of-range handles are skipped | Very low / hours |
| **P2** | 9 | Border colour emulation: keep native when the colour is exactly one of the 3 Metal colours (per format class: float vs integer); otherwise emulate in the shader. A per-binding sampler-parameter buffer holds border RGBA, address-mode bits and texture size. The sample is computed with ClampToZero and then `T + border * (1 - w_inbounds)`, where `w` is the analytic bilinear in-bounds weight (exact for bilinear, nearest-correct). Also sRGB border via `SrgbBorderColor` when the view is sRGB, and depth-compare border | Shadow-map edges, decals and UI clamped textures | `msl/emitter.cpp` (sampling wrappers), `GuestShaderState` specialization keys, `metal/renderer.cpp` (bind the parameter buffer), `metal/sampler.cpp` | Shader test: 2×2 texture, border (0.25,0.5,0.75,1) sampled at u = 1.5 (nearest and linear at the half-texel band); compare-sampler border depth | Medium / 3-4 days |
| **P2** | 10 | Unnormalized coordinates (TIC NORMALIZED_COORDS=0, TSC FORCE_UNNORMALIZED): shader-side `coord *= 1/size` using `get_width/height(lod)`, keyed per binding like `integer_texture_formats` (Metal's unnormalized samplers forbid mips and wrap, per probe) | Rectangle textures in GL-ported titles | `engines/3d.cpp` `getShader` (specialization), `msl/emitter.cpp` sampling, `renderer/shader_cache.hpp` | Shader test sampling texel (3,1) of an 8×4 texture with unnormalized coordinates (3.5, 1.5) | Low-medium / 1-2 days |
| **P2** | 11 | TIC view mip range and TIC MIN_LOD_CLAMP for sampled textures: find why the earlier attempt corrupted streamed textures (likely the TSC LOD clamp being view-relative, plus storage regrowth rebuilding views), then honour `[res_min, res_max]` and apply the TIC min LOD via MSL `min_lod_clamp` or a sampler variant. Fix TXQ to use the LOD operand | UE4 texture streaming (correct mips while streaming); mip filter NONE base level | `engines/3d.cpp:1485-1507`; `decoder/texture.cpp:267-294`; `msl/emitter.cpp:1057+` | Metal test: 4-level texture with TIC view min = 1; mip-NONE sample returns the level-1 colour; TXQ returns level-1 dimensions | Medium-high (known regression area) / 2-3 days |
| **P2** | 12 | Images: implement SULD (.P typed and .D raw), SUST.D via an integer view (R32Uint/RG32Uint/RGBA32Uint pixel-format view of the storage, legal for same-size formats per probe), SURED, SUATOM sizes; bind images in VS/FS | UE4 compute post-processing (reads), NVN fragment UAVs | `decoder/compute.cpp:378-445`, `decoder/decoder.cpp:280-294`, `msl/emitter.cpp:1267-1330`, `engines/3d.cpp` `configureShaderStage` (images) | Compute test: SUST.D.32 into an RGBA8 image then readback bytes; SULD.P of an R16F image; FS image store | Medium / 3-5 days |
| **P2** | 13 | Depth/stencil correctness: separate `S8Z24` from `Z24S8` host converters (stencil-low vs stencil-high, per D-19, after a hardware/game check); implement depth readback (D32FS8 → guest Z24S8/S8Z24/Z32_X24S8 layout via blit-to-buffer plus CPU pack); 3D readback (slice-aware tiling in `encodeTextureImpl`) | CPU depth readbacks; 2D/DMA copies of depth; correctness of re-uploads | `metal/texture.cpp:289-339`; `hot/texture_cache_hot.cpp:383-536`; `renderer/const.cpp:433-455` | Upload known 32-bit words, render-sample depth and stencil, compare. GPU-clear D32FS8, flush, check guest bytes | Medium / 2 days |
| **P2** | 14 | Tile-width spacing for layer stride (D-16) and USE_ARRAY_TILE_ALIGNMENT; replace the shift-UB in `adjustBlockSizeLog2` (D-17); align per-level stride with Eden's `IsSmallerThanGobSize` | Arrays/cubes with tile spacing (garbage layers) | `renderer/const.cpp:773-874`, `texture_cache.cpp:713-816` | CPU test comparing `getLevelOffset` and `layer_size` against an Eden-formula port for a matrix of (bpp, dims, block height/depth, tws, levels, layers) | Low / day |
| **P3** | 15 | MSAA: decode `msaa_mode`; scale storage size by samples X/Y (Eden `image_info.cpp:125-128`); Metal 2DMultisample textures for RTs plus resolve paths; TLD.MS sample index; sample positions | Unity titles with MSAA, some first-party titles | Throughout: `engines/3d.cpp` (RT and TIC), texture cache, renderer, decoder | Render 4× MSAA RT, sample via TLD.MS, resolve via 2D engine | High / weeks |
| **P3** | 16 | Capability-gated fallbacks: query `supportsFamily`/`supportsBCTextureCompression`; software ASTC (port Eden `astc.cpp`), BCn (`decode_bc.cpp`), ETC2 decode to RGBA8/RGBA16F; packed 16-bit → RGBA8 for non-Apple GPUs | Intel Mac and older iOS builds abort on these formats today | `metal/maxwell_to_mtl.cpp`, `metal/renderer.cpp` (device caps), `texture_cache.cpp` `decodeTextureHot2` (decode hook) | Force the fallback via an env flag on M1; readback-compare against native Metal decode | Medium / 3-5 days |
| **P3** | 17 | Min/max reduction fallback: use `setReductionMode` when the device supports it; otherwise emit a `gather` plus `min`/`max` (4-texel footprint) shader variant | HZB/depth-pyramid sampling in some engines | `msl/emitter.cpp`, sampler state specialization | Probe-style test: 2×2 R8 at centre gives 0.2 for min and 0.8 for max | Low-medium / day |
| **P3** | 18 | Texture cache lifecycle: implement texture combining (I13) and storage eviction/destruction (I15) with GPU-fence-safe release | Long sessions: VRAM growth and collision probability | `renderer/texture_cache.cpp:430-504`, `texture_cache.hpp` | Stress test creating and discarding 10k storages; memory bounded; views of live storages unaffected | Medium-high / days |
| **P3** | 19 | Remaining small items: PITCH_COLOR_KEY as pitch; colour-key words; TIC LOD bias/aniso under USE_HEADER_OPT_CONTROL; sRGB on BGR5/BGRX hacks; ZF32_X24S8 stencil view routing; texture-buffer swizzle validation | Rare | as cited in §a | Unit tests per item | Low |

### Testing notes
- Add a table-driven `TicFormatTest` harness in `tests/texture_cache_tests.cpp` that:
  1. writes guest bytes for a TIC (format word plus swizzle);
  2. runs `ThreeD::getTexture`-equivalent logic (factor the TIC → descriptor/view conversion out of `ThreeD` into a free function so tests do not need a full `Gpu`);
  3. samples it with a compute kernel (`texture2d<float/uint/int, access::sample>` plus a nearest sampler);
  4. compares against a CPU reference decoder of the Maxwell bit layout.

  This one harness covers D-1, D-3, D-4, P1-5 and P1-6.
- Factor TSC → `SamplerDescriptor` into a pure function and unit-test every field (bias sign extension, aniso table, wrap/filter enums, border float/int).
- For engine-level binding tests (linked TSC, bounds), build a minimal `Regs3D` plus fake pools in a host vector, mirroring how `texture_cache_tests.cpp` fakes guest memory.

---

## Appendix: Metal probe results (Apple M1, macOS 27.0.1)

The probe sources are `scratchpad/audit/metalprobe/probe.swift`, `probe2.swift`, `probe3.swift` and `probe4.swift`. They are scratch programs, not part of Hydra.

```
supportsBC: true   apple7: true mac2: true   D24S8 supported: false
A1BGR5Unorm 0x0001 -> a=1 | 0x003e -> b=1 | 0x07c0 -> g=1 | 0xf800 -> r=1
BGR5A1Unorm 0x001f -> b=1 | 0x03e0 -> g=1 | 0x7c00 -> r=1 | 0x8000 -> a=1
B5G6R5Unorm 0x001f -> b=1 | 0x07e0 -> g=1 | 0xf800 -> r=1
ABGR4Unorm  0x000f -> a=1 | 0x00f0 -> b=1 | 0x0f00 -> g=1 | 0xf000 -> r=1
1D mip3 texture: "MTLTextureType1D requires that mipmapLevelCount is 1" (assert)
view Depth32Float of Depth32Float_Stencil8: "not castable" (assert)
views RGBA8->BGRA8, RG11B10F->R32U, RGBA8->R32F, RGB10A2->R32U, ABGR4->R16U, RGB9E5->R32U: ok
blit RGBA8->R32U, D32F->R32F, BC1->RG32U, D32FS8->RGBA8: "[source pixelFormat] must equal [destination pixelFormat]" (assert)
sampler normalizedCoordinates=false + mipFilter linear: "require MTLSamplerMipFilterNotMipmapped" (assert)
sampler normalizedCoordinates=false + repeat: "require ClampToEdge, ClampToZero or ClampToBorderColor" (assert)
sampler maxAnisotropy=16 + lodBias=-1.5: ok
sampler reductionMode minimum: "requires device support" (assert on M1)
too many mips (5 for 4x4): assert;  non-square cube: assert;  swizzled texture-buffer view: ok
```
