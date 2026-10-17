# Hydra audit: Maxwell B (0xB197) 3D back end, output merger and framebuffer

Read-only audit of the Hydra working tree (HEAD `3e8c4cc2` plus uncommitted changes), 2026-10-17 at about 10:20 WIB. Nothing in the repo was modified, built or run.

**Concurrency note:** other agents edited files during the audit (`engines/3d.cpp`, `renderer/metal/pipeline.cpp`, `renderer/pipeline_cache.cpp`, `renderer/shader_cache.hpp`, `shader_decompiler/const.hpp`, `codegen/lang/msl/emitter.cpp`). Those edits were vertex, topology, clip-distance and point-size work, outside this scope. Every line number below was re-checked at write time. The MSL emitter kept moving while I worked, so citations into it name the function and give an approximate line (`≈`).

## Path abbreviations

| Prefix | Location |
|---|---|
| `E/` | `src/core/hw/tegra_x1/gpu/engines/` |
| `G/` | `src/core/hw/tegra_x1/gpu/` (only `G/const.hpp`) |
| `R/` | `src/core/hw/tegra_x1/gpu/renderer/` |
| `M/` | `R/metal/` |
| `SD/` | `R/shader_decompiler/` |
| `T/` | `tests/` |
| `clb197.h:L` | `$REFS/opengpudoc/clb197.h`, line L |
| `VC/` | `$REFS/yuzu/src/video_core/` (Eden) |
| `SR/` | `$REFS/yuzu/src/shader_recompiler/` (Eden) |

## Class legend

| Class | Meaning |
|---|---|
| **VERIFIED** | Implemented, and a test checks GPU results, or a README records in-game verification. |
| **IMPLEMENTED_UNVERIFIED** | Code exists. No test checks its results. |
| **PARTIAL** | Some encodings, modes or fields are missing or wrong. |
| **MISSING** | Not decoded, or ignored, stubbed or a no-op where hardware has an effect. |
| **UNKNOWN** | The correct semantics could not be established from the references. |

## How 3D methods dispatch

`ThreeD::method` calls `HYDRA_HOT_CALL(engine_three_d)` (`src/core/hot/routes.cpp:755-758`). That reaches `ThreeD::methodHot`, which `DEFINE_METHOD_TABLE` generates (`E/engine_base.hpp:10-22`, table at `E/3d.cpp:224-233`).

Only these methods have handlers:

| Method | Handler |
|---|---|
| `0x674` CLEAR_SURFACE | `clearBuffer` |
| `0x54c` CLEAR_REPORT_VALUE | `resetCounter` |
| `0x6c3` SET_REPORT_SEMAPHORE_D | `setReportSemaphore` |
| draws, MME, constant buffers, bind groups | their own handlers |

Every other method in this scope only writes `regs_raw[method]` through the `default: writeReg` path.

Counts over the capability table in section (a): **VERIFIED 5 · IMPLEMENTED_UNVERIFIED 24 · PARTIAL 21 · MISSING 44 · UNKNOWN 1** (95 rows).

---

## (a) Capability table

### a.0 Register offsets: Hydra `Regs3D` against clb197.h

The register index is the method offset divided by 4. I counted every padding block from 0x200 to 0x7e0 by hand. **All offsets in scope match clb197.h.** I found no offset bug.

| Hydra field (`E/3d.hpp`) | Reg idx | clb197 method (offset) | Bitfield check |
|---|---|---|---|
| `color_targets[j]` (`:150-171`, 16 words) | 0x200+16j | SET_COLOR_TARGET_A..MARK (0x800-0x824 + 64j), clb197.h:659-779 | MEMORY 3:0, 7:4, 11:8, LAYOUT 12, 3D 16 (clb197.h:743-765) all match `tile_mode` (`:155-162`). THIRD_DIMENSION is 27:0 but Hydra reads only `u16 layers` (`:164`). |
| `viewport_z_clip` (`:281`) | 0x35f | SET_VIEWPORT_Z_CLIP 0xd7c | OK |
| `clear_color` (`:284`) | 0x360-0x363 | SET_COLOR_CLEAR_VALUE(i) 0xd80, clb197.h:956 | OK, raw 32-bit words |
| `clear_depth` (`:285`) | 0x364 | SET_Z_CLEAR_VALUE 0xd90, clb197.h:959 | OK, float |
| `clear_stencil` (`:291`) | 0x368 | SET_STENCIL_CLEAR_VALUE 0xda0, clb197.h:975 | V 7:0 |
| `advanced_blend_enabled` (`:296`) | 0x374 | SET_ITERATED_BLEND 0xdd0, clb197.h:1049 | name only |
| `depth_target_addr/format/tile_mode/layer_stride` (`:318-326`) | 0x3f8-0x3fc | SET_ZT_A/B/FORMAT/BLOCK_SIZE/ARRAY_PITCH 0xfe0-0xff0, clb197.h:1350-1384 | OK |
| `screen_scissor` (`:329-338`) | 0x3fd-0x3fe | SET_SURFACE_CLIP_H/V 0xff4/0xff8, clb197.h:1386 | OK |
| `color_target_control` (`:349-354`) | 0x487 | SET_CT_SELECT 0x121c, clb197.h:2003-2012 | COUNT 3:0, TARGETn 3 bits from bit 4, matches `getMap` |
| `depth_target_width/height/array_mode` (`:359-364`) | 0x48a-0x48c | SET_ZT_SIZE_A/B/C 0x1228-0x1230, clb197.h:2042-2052 | CONTROL bit 16 is ignored, see B11 |
| `depth_test_enabled` (`:369`) | 0x4b3 | SET_DEPTH_TEST 0x12cc | OK |
| `independent_blend_enabled` (`:374`), `depth_write_enabled` (`:375`) | 0x4b9 / 0x4ba | SET_BLEND_STATE_PER_TARGET 0x12e4 / SET_DEPTH_WRITE 0x12e8 | OK |
| `depth_compare_op` (`:380`) | 0x4c3 | SET_DEPTH_FUNC 0x130c | typed as D3D-only `CompareOp`, see B2 |
| `blend_constant` (`:385`) | 0x4c7-0x4ca | SET_BLEND_CONST_RED..ALPHA 0x131c-0x1328 | OK |
| `blend_state` (`:390-399`) | 0x4cf-0x4d6 | SET_BLEND_SEPARATE_FOR_ALPHA..ALPHA_DEST_COEFF 0x133c-0x1358 | OK |
| `single_rop_control`, `color_blend_enabled[8]` (`:400-401`) | 0x4d7, 0x4d8-0x4df | SET_SINGLE_ROP_CONTROL 0x135c, SET_BLEND(i) 0x1360 | OK |
| `regs_raw[0x4e0..0x4e7]` (`E/3d.cpp:2375-2383`) | 0x4e0-0x4e7 | SET_STENCIL_TEST..SET_STENCIL_MASK 0x1380-0x139c, clb197.h:2456-2545 | OK |
| `regs_raw[0x565..0x569]`, `[0x3d5..0x3d7]` (`E/3d.cpp:2385-2394`) | | SET_TWO_SIDED_STENCIL_TEST..SET_BACK_STENCIL_FUNC 0x1594-0x15a4; SET_BACK_STENCIL_FUNC_REF/MASK/FUNC_MASK 0xf54-0xf5c | ref = 0x3d5, write mask = 0x3d6, func mask = 0x3d7: OK |
| `depth_target_enabled` (`:417`) | 0x54e | SET_ZT_SELECT 0x1538 | OK |
| `color_write_masks[8]` (`:477`) | 0x680-0x687 | SET_CT_WRITE(i) 0x1a00, clb197.h:3580-3592 | R/G/B/A at bits 0/4/8/12 match `ColorWriteMask` (`E/const.hpp:170-177`) |
| `report_semaphore_*` (`:482-484`) | 0x6c0-0x6c3 | SET_REPORT_SEMAPHORE_A-D 0x1b00-0x1b0c | OK |
| `independent_blend_state[8]` (`:502-511`) | 0x780+8j | SET_BLEND_PER_TARGET_* 0x1e00+32j, clb197.h:3732-3760 | OK |
| `ClearBufferData` (`:633-639`) | method 0x674 | CLEAR_SURFACE, clb197.h:3550-3570 | Z 0, S 1, RGBA 2-5, MRT 9:6 match. RT_ARRAY_INDEX is 25:10 (16 bits); Hydra reads 11 bits (B12). |

**In scope but never decoded.** These indices sit in `Regs3D` padding:

| Register | Index |
|---|---|
| ALPHA_TEST / REF / FUNC | 0x4bb / 0x4c4 / 0x4c5 |
| LOGIC_OP / FUNC | 0x671 / 0x672 |
| DEPTH_BOUNDS_TEST / MIN / MAX | 0x66f / 0x3e7 / 0x3e8 |
| SRGB_WRITE | 0x56e |
| ANTI_ALIAS / _ENABLE / _ALPHA_CONTROL | 0x574 / 0x54d / 0x54f |
| SAMPLE_MASK | 0x3e9 |
| SAMPLE_MASK_Xn_Yn | 0x3ef-0x3f2 |
| SINGLE_CT_WRITE_CONTROL | 0x3e4 |
| CLEAR_SURFACE_CONTROL | 0x43e |
| CLEAR_RECT | 0x35b / 0x35c |
| RENDER_ENABLE_A-C | 0x554-0x556 |
| RENDER_ENABLE_OVERRIDE | 0x651 |
| ZPASS_PIXEL_COUNT | 0x545 |
| PIXEL_SHADER_BARRIER | 0x378 |
| API_MANDATED_EARLY_Z | 0x84 |
| VIEWPORT_CLIP_CONTROL | 0x64f |
| ZT_LAYER | 0x5e7 |
| RT_LAYER | 0x573 |
| PS_SATURATE | 0x4ea |
| COLOR_CLAMP | 0x980 |
| INVALIDATE_TEXTURE_DATA_CACHE | 0x4ce |

### a.1 Render targets

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| R1 | RT address A/B | IMPLEMENTED_UNVERIFIED | `E/3d.hpp:151` (`Iova{hi,lo}`), `E/3d.cpp:664-716` | clb197.h:659-663; `VC/texture_cache/texture_cache.h:1904-1919` | `addr==0` gives no attachment, same as Eden. |
| R2 | RT width/height, linear pitch | PARTIAL | `E/3d.cpp:690-705`. Linear: `width = width_or_stride / bpp` (`:695`). Block-linear uses the `getMinimumWidth` heuristic, which can shrink the width toward the surface clip (`:205-220`, `:702`). | `VC/texture_cache/image_info.cpp:151-164`: Eden uses `ct.width` as is. | The heuristic can create two storages with different widths for the same memory when the surface clip changes (dynamic resolution). Risk, not a confirmed bug. |
| R3 | RT format to host format | PARTIAL | `R/const.cpp:357-430`, `M/maxwell_to_mtl.cpp:46-179` | clb197.h:671-741; `VC/surface.cpp:100-206` | See section (c): 0xC1 hits UB, 0xDF has R/B swapped, A8 and X1R5G5B5 are Invalid, 0x40-0x47 abort. |
| R4 | RT memory layout (block W/H/D, pitch/blocklinear, 3D control) | IMPLEMENTED_UNVERIFIED | `E/3d.hpp:155-162`, `E/3d.cpp:706-713` | clb197.h:743-765 | Bits verified. |
| R5 | RT third dimension (array layers or 3D depth) | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:677-689`: 3D gives an MTL 3D texture, layers >1 gives a 2D array. | clb197.h:767-768; `VC/texture_cache/image_info.cpp:173-182` | `layers==0` is not guarded and creates a zero-layer descriptor. The `volume` bit is unused. |
| R6 | RT array pitch | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:712`, `render_target.layer_stride * 4` | clb197.h:770; `VC/texture_cache/image_info.cpp:165` | Matches Eden. |
| R7 | SET_COLOR_TARGET_LAYER (base layer) | MISSING | Stored at `E/3d.hpp:168`. Only debug traces read it. | clb197.h:773-774 | Eden ignores it too. Low priority. |
| R8 | CT_SELECT count and map | IMPLEMENTED_UNVERIFIED | `E/3d.hpp:349-354`; `E/3d.cpp:767-772` (pass), `:1032-1042` (shader types), `:1230-1238` (pipeline) | clb197.h:2003-2012; `VC/renderer_opengl/gl_texture_cache.cpp:1336-1346`, `VC/texture_cache/texture_cache.h:507` | Slot i is bound to `rt[Map(i)]` and fragment output i writes slot i, as in Eden GL. `count` (4 bits, up to 15) is not clamped to 8 when indexing 8-entry arrays (`E/3d.cpp:767`, B12). |
| R9 | CT_WRITE per-target channel masks | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:251-252` (default All), `:1240`; `M/pipeline.cpp:149-159` | clb197.h:3580-3592; `VC/engines/maxwell_3d.cpp:111-117` | `T/metal_cache_tests.cpp` checks write masks on raw MTL descriptors, not through Hydra's `ColorTargetState` path. |
| R10 | SINGLE_CT_WRITE_CONTROL (common mask) | MISSING | Not decoded (reg 0x3e4). | clb197.h:1266-1269; `VC/renderer_vulkan/fixed_pipeline_state.cpp:267`, `vk_rasterizer.cpp:1779` | With common mask on, RT1-7 should use mask[0]. Hydra uses their own (default All). |
| R11 | SET_CT_MRT_ENABLE | MISSING | Not decoded. | clb197.h:1292-1295 | Eden only sets it to 1 at boot (`VC/engines/maxwell_3d.cpp:125`). No-op. |
| R12 | Surface clip as a hardware clip | PARTIAL | Only a width hint (`E/3d.cpp:699-704`). Not applied to draws or clears. | clb197.h:1386-1392; `VC/renderer_vulkan/vk_texture_cache.cpp:2952-2957` (render area = surface clip), `vk_rasterizer.cpp:484` (clear clamp) | Clears cover the whole attachment (C11). |
| R13 | SET_RT_LAYER | MISSING | Not decoded. | clb197.h:2907-2911 | Eden ignores it too (`VC/engines/maxwell_3d.h:1963-1973`). |
| R14 | COLOR_TARGET_MARK / ZT_MARK (IEEE clean) | MISSING | Stored only. | clb197.h:776-779, 1016-1019 | Harmless hint. |
| U1 | Semantics of a non-identity CT_SELECT map (clear MRT_SELECT and per-target state) | UNKNOWN | Hydra indexes masks, blend and clears by slot (`E/3d.cpp:1240-1272`, `M/renderer.cpp:386-389`). | Eden GL routes draw buffer i to attachment Map(i) (`VC/renderer_opengl/gl_texture_cache.cpp:1342`). Eden VK binds `rt[i]` and ignores Map (`VC/texture_cache/texture_cache.h:1904-1913`). | The two Eden back ends disagree. NVN appears to use identity maps. |

### a.2 Zeta (depth/stencil) buffer

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| Z1 | ZT address, block size, array pitch | IMPLEMENTED_UNVERIFIED | `E/3d.hpp:318-326`; `E/3d.cpp:718-742` | clb197.h:1350-1384 | Always block-linear (`false` at `E/3d.cpp:733`). clb197 has no layout bit for ZT. |
| Z2 | ZT format to host format | PARTIAL | `R/const.cpp:433-452`, `M/maxwell_to_mtl.cpp:85-93` | clb197.h:1356-1368; `VC/surface.cpp:78-98` | **0x15 X8Z24 calls `LOG_FATAL` and aborts.** 0x18 and 0x1D-0x1F map to Invalid, which is UB at texture creation. See (c). |
| Z3 | ZT size A/B/C, including the CONTROL bit | PARTIAL | `E/3d.hpp:359-364`; `E/3d.cpp:726-735` use `layers` unconditionally. | clb197.h:2042-2052; `VC/texture_cache/image_info.cpp:217-224` | CONTROL=ARRAY_SIZE_IS_ONE is ignored (B11). |
| Z4 | ZT select (enable) | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:775-781`, `:1274-1279` | clb197.h:2748-2749; `VC/texture_cache/texture_cache.h:1922-1925` | |
| Z5 | SET_ZT_LAYER | MISSING | Not decoded. | clb197.h:3180-3181 | Eden ignores it as well. |
| Z6 | SET_ZT_READ_ONLY | MISSING | Not decoded. | clb197.h:651-657 | Hint. It would matter for read-only depth sampled while bound. |
| Z7 | D24 emulation and Z24S8/S8Z24 packing | PARTIAL | D24S8 and X8D24 map to `Depth32Float_Stencil8` with a "HACK" comment (`M/maxwell_to_mtl.cpp:87-93`). 0x14 and 0x16 both map to `Z24Unorm_S8Uint` (`R/const.cpp:442-443`). The CPU decoder assumes depth in the low 24 bits (`M/texture.cpp:312-318`). | `VC/surface.cpp:80-83` (0x14 → S8_UINT_D24_UNORM, 0x16 → D24_UNORM_S8_UINT); `VC/texture_cache/format_lookup_table.cpp:154-161` | Apple silicon has no D24S8, so D32F is the correct host choice. 0x14 surfaces uploaded from guest memory decode with depth and stencil swapped (B15). Depth-bias units differ for D24 shown as D32F; Eden rescales per title (`VC/renderer_vulkan/vk_rasterizer.cpp:1289-1310`). |
| Z8 | Depth/stencil RT sampled as a texture (G24R8 alias, stencil plane) | **VERIFIED** | `T/texture_cache_tests.cpp:662-742` (G24R8 reads the GPU-written Z24S8 depth 0.625), `:743-823` (RRRR swizzle reads stencil 0x5a); `T/README-crash4-render.md` | `VC/texture_cache/format_lookup_table.cpp:158-159` | Readback via Metal compute. |

### a.3 Clears

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| C1 | CLEAR_SURFACE decode | PARTIAL | `E/3d.hpp:633-639`; `E/3d.cpp:496-535` | clb197.h:3550-3570 | Layout is right under the Itanium ABI. `layer_id` is 11 bits where the field has 16. `target_id` up to 15 indexes 8-entry arrays (B12). |
| C2 | Clear color value, raw bits including integer RTs | **VERIFIED** (pipeline level) | `M/clear_color_pipeline_cache.cpp:50-91` (uint4/int4/float4 output by host format); test `T/shader_library_tests.cpp:739-786` (R32Uint/R32Sint/R32Float on attachment 1, readback of exact bits) | clb197.h:956-957; Eden instead converts float to int (`VC/renderer_vulkan/vk_rasterizer.cpp:506-517`) | The test exercises the Metal clear pipeline directly, not `ThreeD::clearBuffer`. Raw bits are semantically plausible: the NVN integer clear writes integer bits. Eden differs. |
| C3 | Clear RGBA channel mask | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:523-526`; `M/clear_color_pipeline_cache.cpp:111-120` | clb197.h:3557-3568; `VC/renderer_vulkan/vk_rasterizer.cpp:535-541` | No readback of a partially masked multi-channel clear. |
| C4 | Clear MRT_SELECT to attachment | IMPLEMENTED_UNVERIFIED | `M/renderer.cpp:386-396`: slot `render_target_id`. A null slot is skipped. | `VC/renderer_vulkan/vk_rasterizer.cpp:504-505` | See U1. |
| C5 | Clear depth value | PARTIAL | `M/renderer.cpp:420-461`; shader `M/clear_depth_pipeline_cache.cpp:32` (`position.z = value`) | `VC/renderer_vulkan/vk_rasterizer.cpp:550-575` | Inherits the encoder's viewport depth range, depth bias and scissor (B1). |
| C6 | Clear stencil value | PARTIAL | `M/renderer.cpp:463-497`, `:56-70` | `VC/renderer_vulkan/vk_rasterizer.cpp:558-566` | Layer forced to 0. Write mask forced to 0xff, ignoring SET_STENCIL_MASK (B9). |
| C7 | Layered clears (RT_ARRAY_INDEX) | PARTIAL | Color and depth route the layer through `render_target_array_index` (`M/clear_color_pipeline_cache.cpp:27-28`). `ASSERT_DEBUG(layer == 0)` (`M/renderer.cpp:398-400`) aborts debug builds. Stencil always uses layer 0. | `VC/renderer_vulkan/vk_rasterizer.cpp:442`; `VC/macro.cpp:187-198` (HLE_MultiLayerClear) | NVN's multi-layer clear macro issues one clear per layer (B10). |
| C8 | Clears isolated from draw state (viewport, scissor, depth-stencil, bias) | MISSING | `M/renderer.cpp:402-417` (`// TODO: set viewport and scissor`, no `setDepthStencilState`); encoder reuse `M/command_buffer.cpp:77-78` | `VC/renderer_vulkan/vk_rasterizer.cpp:524-532, 578-581` (`vkCmdClearAttachments` is state independent) | **Confirmed bug B1.** |
| C9 | SET_CLEAR_SURFACE_CONTROL (respect stencil mask, clear rect, scissor0, viewport clip0) | MISSING | Not decoded (0x43e). | clb197.h:1560-1572; `VC/renderer_vulkan/vk_rasterizer.cpp:440-441` (USE_SCISSOR0) | Scissored clears clear the whole target, or worse, the previous draw's scissor. |
| C10 | SET_CLEAR_RECT | MISSING | Not decoded (0x35b/0x35c). | clb197.h:937-943 | Eden does not use it either. |
| C11 | Clears clamped to the surface clip | MISSING | Clear triangles cover the full attachment. | `VC/renderer_vulkan/vk_rasterizer.cpp:447-486` | |
| C12 | Deferred (load-action) and zero-bandwidth clears | MISSING | `RenderTargetDescriptor::load_action_clear` (`R/const.hpp:354-366`) is never set; TODO at `E/3d.cpp:513`. | `VC/renderer_vulkan/vk_rasterizer.cpp:492-501` (`DeferColorClear`) | Performance only. Apple TBDR GPUs benefit a lot from load-action clears. |
| C13 | Clears gated by conditional rendering | MISSING | | `VC/engines/draw_manager.cpp:69-73` | |

### a.4 Depth and stencil

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| D1 | Depth test enable | **VERIFIED** (DS-cache level) | `E/3d.cpp:2325-2328`; `M/renderer.cpp:1232-1247`; `M/depth_stencil_state_cache.cpp:52-56`; test `T/shader_library_tests.cpp:1045-1080` | clb197.h:2109-2112 | The readback uses Hydra's `DepthStencilStateCache` with test, write and Always. The engine decode is untested. |
| D2 | Depth write | **VERIFIED** (DS-cache level) | `M/depth_stencil_state_cache.cpp:58-61` (write only when the test is on, GL/VK semantics); `T/README-crash4-render.md` ("Preserve requested depth writes with the Always comparison") | clb197.h:2150-2153 | |
| D3 | Depth func, GL and D3D encodings | PARTIAL | `E/3d.hpp:380` (D3D-typed); `E/3d.cpp:2331` passes the raw value; `M/maxwell_to_mtl.cpp:689-711` returns Always for anything that is not 1-8. | clb197.h:2176-2193; `VC/engines/maxwell_3d.h:940-958`; `VC/renderer_vulkan/maxwell_to_vk.cpp:611-639` | **B2.** GL values 0x200-0x207 give Always. |
| D4 | Depth bounds test | MISSING | Not decoded (0x66f, 0x3e7/0x3e8). | clb197.h:3511-3514, 1275-1279; `VC/renderer_vulkan/vk_rasterizer.cpp:1481-1493` (disabled if unsupported) | Metal has no depth bounds. Ignoring is the standard fallback. |
| D5 | Depth clamp (VIEWPORT_CLIP_CONTROL) | MISSING | Not decoded (0x64f). Metal's default clip mode applies. | clb197.h:3372-3399; `VC/renderer_vulkan/vk_rasterizer.cpp:1652-1667`, `fixed_pipeline_state.cpp:368-370` | Eden enables clamp unless the geometry clip is PASSTHRU, FRUSTUM_XYZ or FRUSTUM_Z. That includes the default WZERO_CLIP=0, so most NVN draws expect clamping. |
| D6 | Stencil test enable | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:2375`; `M/renderer.cpp:1240` (disabled without a stencil attachment) | clb197.h:2456-2459 | |
| D7 | Stencil ops, GL and D3D | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:2336-2364`; `M/depth_stencil_state_cache.cpp:10-29` | clb197.h:2461-2477; `VC/renderer_vulkan/maxwell_to_vk.cpp:655+` | All 16 encodings are mapped. Unknown values give Keep. |
| D8 | Stencil func, GL and D3D | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:2366-2372` | clb197.h:2518-2535 | Unknown values give Always. |
| D9 | Stencil ref, func mask, write mask | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:2381-2383`; `M/renderer.cpp:1381-1382` (ref & 0xff); `M/depth_stencil_state_cache.cpp:38-39` | clb197.h:2537-2545 | |
| D10 | Two-sided stencil (back state) | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:2385-2397` (back = front when disabled) | clb197.h:2802-2880, 1246-1254 | |
| D11 | Depth/stencil disabled without an attachment | IMPLEMENTED_UNVERIFIED | `M/renderer.cpp:1233-1243` | Metal requirement | |
| D12 | Reset values of the depth-stencil registers | PARTIAL | `regs{}` is zero-initialized (`E/engine_base.hpp:61-64`). Ops 0 decode as OGL_ZERO (`E/3d.cpp:2341-2342`); masks are 0. | `VC/engines/maxwell_3d.cpp:87-102` (Keep, Always_GL, masks 0xFFFFFFFF, depth func Always_GL) | Only matters if a title enables stencil without writing state. |

### a.5 Alpha test

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| A1 | ALPHA_TEST enable, ref, func (fixed function on Maxwell) | MISSING | Registers fall in padding (`E/3d.hpp:377`, `:382`). `GuestShaderState` has no alpha-test fields (`R/shader_cache.hpp:11-46`). | clb197.h:2155-2158, 2195-2215; `VC/renderer_vulkan/fixed_pipeline_state.cpp:137-139, 182`; `vk_pipeline_cache.cpp:248-250`; `SR/backend/spirv/emit_spirv_special.cpp:67-91` | Cutout foliage, fences and UI written for alpha test render as opaque quads. |

### a.6 Blending

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| BL1 | Per-RT blend enable SET_BLEND(i) | PARTIAL | `E/3d.cpp:1241-1244`; `M/pipeline.cpp:161-176` | clb197.h:2451; `VC/renderer_vulkan/vk_rasterizer.cpp:1798-1813` | Not disabled for integer RTs (B5). |
| BL2 | Common vs per-target state | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:1246-1272` | clb197.h:2145-2148; `VC/renderer_vulkan/fixed_pipeline_state.cpp:290-306` | |
| BL3 | Blend equations, GL (0x8006-0x800B) and D3D (1-5) | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:57-98` | clb197.h:2255-2266; `VC/renderer_vulkan/maxwell_to_vk.cpp:686-706` | All 10 encodings covered. Unknown values give Add. |
| BL4 | Blend factors, GL (0x4000.., 0xC0xx, 0xC9xx) and D3D (1-0x13) | PARTIAL | `E/3d.cpp:100-201` | clb197.h:2268-2310; `VC/engines/maxwell_3d.h:1007-1047`; `VC/renderer_vulkan/maxwell_to_vk.cpp:708-770` | **B3.** Missing D3D 0x0C-0x0F and GL 0xC001-0xC004 / 0xC900-0xC903; they fall back to One. |
| BL5 | Blend constant color | PARTIAL | `E/3d.cpp:2332`; `M/renderer.cpp:1390-1392`. The Metal-level `setBlendColor` cache has a pixel readback (`T/raster_state_tests.cpp:41-129`). | clb197.h:2220-2232 | Plumbed but unreachable: no guest factor decodes to `ConstColor`/`ConstAlpha` (`R/const.hpp:348-351`). |
| BL6 | SEPARATE_FOR_ALPHA | MISSING | Ignored (`E/3d.hpp:391`, `:503`). | clb197.h:2250-2253 | Eden ignores it too (`VC/renderer_vulkan/fixed_pipeline_state.cpp:280-288`). Low priority. |
| BL7 | Dual-source blending | PARTIAL | D3D SRC1 maps to MTL Source1 (`E/3d.cpp:187-194`, `M/maxwell_to_mtl.cpp:771-778`). The shader never declares `[[color(0), index(1)]]` (`SD/codegen/lang/msl/emitter.cpp` stage outputs ≈`:462-474`, qualifier ≈`:1432-1434`). | `VC/renderer_vulkan/fixed_pipeline_state.cpp:60-90, 133`; `SR/backend/spirv/spirv_emit_context.cpp:1692-1706` | **B8.** Broken in practice. |
| BL8 | Reset values of the blend registers | MISSING | Zero registers decode to src One, dst One, op Add (`E/3d.cpp:94-96`, `:195-198`), which is additive. | `VC/engines/maxwell_3d.cpp:72-86` ("Doom and Bomberman seems to use the uninitialized registers and just enable blend") | **B4.** |
| BL9 | Iterated (advanced) blend | MISSING | Only `ASSERT_DEBUG(!regs.advanced_blend_enabled)` (`E/3d.cpp:1226-1227`), which aborts debug builds. | clb197.h:1049-1055; `VC/renderer_vulkan/fixed_pipeline_state.cpp:291-303` (squashed workaround) | |
| BL10 | BLEND_FLOAT_OPTION (0 × anything = 0) | MISSING | Not decoded. | clb197.h:3516-3519 | Eden ignores it too. |

### a.7 Logic op and sRGB

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| L1 | LOGIC_OP enable and func (16 ops) | MISSING | Not decoded (0x671/0x672). | clb197.h:3521-3542; `VC/renderer_vulkan/fixed_pipeline_state.cpp:345, 366`; `vk_graphics_pipeline.cpp:948` | Metal has no logic op. Emulate with programmable blending (section (d)). |
| S1 | sRGB conversion through sRGB RT formats | IMPLEMENTED_UNVERIFIED | 0xD0/0xD6/0xE7/0xFA map to `*_sRGB` Metal formats (`M/maxwell_to_mtl.cpp:94-95, 164-167`). Textures allow format views (`M/texture.cpp:66-70`). | clb197.h:690, 694, 709, 728 | Metal blends in linear space and encodes on store, like Maxwell. |
| S2 | SET_SRGB_WRITE (FRAMEBUFFER_SRGB) | MISSING | Not decoded (0x56e). | clb197.h:2883-2886; Eden defaults it to 1 (`VC/engines/maxwell_3d.cpp:123-126`); GL honors it, VK ignores it | Only matters for GL-driver titles that toggle it. |

### a.8 Multisampling

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| M1 | ANTI_ALIAS sample mode and MS render targets | MISSING | No sample count in `TextureDescriptor` (`R/const.hpp:223-270`) or `PipelineDescriptor` (`R/const.hpp:557+`). TIC `msaa_mode` is unused (`E/3d.hpp:93`). | clb197.h:2913-2925; `VC/texture_cache/image_info.cpp:167`; `VC/renderer_vulkan/maxwell_to_vk.cpp:865-887` | MSAA targets render at 1x with the guest's pixel size, so there is no AA. MS-texture fetch is unsupported (`T/README-texture-sampling.md:38-40`). |
| M2 | ANTI_ALIAS_ENABLE | MISSING | Not decoded (0x54d). | clb197.h:2743-2746 | |
| M3 | Alpha-to-coverage | MISSING | Not set in `M/pipeline.cpp`. The disk-cache key already includes it (`M/binary_archive_cache.cpp:79-81`). | clb197.h:2751-2754; `VC/renderer_vulkan/fixed_pipeline_state.cpp:169` | Cheap to add. |
| M4 | Alpha-to-one | MISSING | Same as M3. | clb197.h:2755-2757; `fixed_pipeline_state.cpp:170` | |
| M5 | Sample mask (raster, per target, per position) | MISSING | Not decoded (0x3e9, 0x3ea, 0x3ef-0x3f2). | clb197.h:1281-1290, 1315+ | |
| M6 | Sample shading / min samples (HYBRID_ANTI_ALIAS_CONTROL) | MISSING | Not decoded (0x1d5). | clb197.h:581-586 | |
| M7 | Programmable sample locations | MISSING | Not decoded. | clb197.h:1917-1925 | |
| M8 | MS to SS resolve (2D engine or copy) | MISSING | `E/2d.cpp` has no sample handling (grep finds no msaa or sample code). | `VC/texture_cache/texture_cache.h:1787-1795`; `VC/renderer_vulkan/vk_texture_cache.cpp:1403-1406` (`vkCmdResolveImage`) | |

### a.9 Render-target aliasing, barriers, feedback loops

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| T1 | RT sampled as a color texture with a reinterpreted format | IMPLEMENTED_UNVERIFIED | Storages are grouped by bpp, block size and depth flag (`R/const.cpp:720-737`). Views use `MTLTextureUsagePixelFormatView` (`M/texture.cpp:66-70`). | `VC/renderer_vulkan/vk_texture_cache.cpp` `ReinterpretImage` (≈1131+) | Depth aliasing is covered by Z8. There is no color-RT reinterpretation readback. |
| T2 | Feedback loops (sampling a bound attachment in the same pass) | MISSING | No detection (no hazard or feedback grep hits). | `VC/texture_cache/texture_cache.h:241-301` (`CheckFeedbackLoop`), `VC/renderer_vulkan/vk_texture_cache.cpp:1126-1128` | On Apple TBDR the sampler reads stale memory from before the pass. |
| T3 | PIXEL_SHADER_BARRIER (fragment/texture barrier) | MISSING | No handler for 0x378. | clb197.h:1079-1082; `VC/engines/maxwell_3d.cpp:362-363`; `VC/renderer_vulkan/vk_rasterizer.cpp:876-879` (ends the render pass) | |
| T4 | INVALIDATE_TEXTURE_DATA_CACHE | MISSING | No handler (0x4ce). | clb197.h:2244; `VC/engines/maxwell_3d.cpp:364-366` | |

### a.10 Queries, report semaphores, conditional rendering

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| Q1 | Semaphore RELEASE, ONE_WORD | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:544-606`: op 1:0, report 27:23, structure 28; u32 payload at `:595`. | clb197.h:3609-3700 | Written at CPU command-processing time, before earlier GPU work completes. |
| Q2 | Semaphore RELEASE, FOUR_WORDS with timestamp | PARTIAL | `E/3d.cpp:597-605`: `[0]=u64 value, [1]=ns/625*384` | `VC/engines/maxwell_3d.cpp:481-513` (query cache with timeout and fence flags) | The timestamp is host CPU time, not GPU completion time. GPU-time-based dynamic-resolution logic would see near-zero GPU time. |
| Q3 | ACQUIRE | MISSING | Returns immediately (`E/3d.cpp:568-570`). | clb197.h:3622; Eden does not implement it either (`maxwell_3d.cpp:498-502`) | |
| Q4 | TRAP | MISSING | Not implemented (`E/3d.cpp:588-590`). | Eden does not implement it either. | |
| Q5 | REPORT_ONLY ZPASS_PIXEL_CNT / CNT64 (occlusion) | PARTIAL | Fake counter, +1 per draw before any draw-skip decision (`E/3d.cpp:2276-2277`, `:576-582`). | `VC/renderer_vulkan/vk_query_cache.cpp:51` (`VK_QUERY_TYPE_OCCLUSION`), `:1558` | Visibility is conservative, so culling stays safe, but counts are draw counts, not samples. Lens-flare or visibility fades based on pixel counts come out wrong. |
| Q6 | Other counters (primitives generated, invocations, streaming, etc.) | MISSING | Write 0 (`E/3d.cpp:584-586`). | clb197.h:3649-3677; `VC/engines/maxwell_3d.cpp:569-585` | |
| Q7 | CLEAR_REPORT_VALUE | PARTIAL | Only type 1 (ZPASS) resets (`E/3d.cpp:537-542`). | clb197.h:2722-2741 | |
| Q8 | SET_ZPASS_PIXEL_COUNT enable | MISSING | Not decoded (0x545). | clb197.h:2699-2702; `VC/renderer_vulkan/vk_rasterizer.cpp:501` (`CounterEnable(ZPassPixelCount64, zpass_pixel_count_enable)`) | |
| Q9 | Semaphore reduction (RED_ADD and so on) | MISSING | Payload is written instead. | clb197.h:3686-3700 | Eden's 3D path ignores it as well. |
| Q10 | Conditional rendering (RENDER_ENABLE_A-C modes, OVERRIDE, GLOBAL) | MISSING | Not decoded (0x554-0x556, 0x651, 0x4c-0x4e). Every draw runs. | clb197.h:2759-2772, 3427-3431, 80-90 (global); `VC/engines/maxwell_3d.cpp:515-560`; `VC/engines/draw_manager.cpp:69-73` | Mode FALSE / NEVER_RENDER should suppress draws. |
| Q11 | SET_STATISTICS_COUNTER enables | MISSING | Not decoded. | clb197.h:887-932 | |
| Q12 | Cache coherency of report writes | PARTIAL | Direct host write with no `invalidateMemory`. Contrast `loadConstBuffer`, which invalidates (`E/3d.cpp:625-629`). | | B13. Stale data is possible if the GPU reads report memory through the buffer cache. |

### a.11 Fragment-shader outputs and miscellaneous

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| F1 | Color outputs typed by RT format (float/int/uint raw bits) | IMPLEMENTED_UNVERIFIED | `E/3d.cpp:1032-1042`; `R/const.cpp:491+` (`toColorDataType`); `SD/codegen/lang/msl/emitter.cpp` stage outputs ≈`:462-474`; `SD/codegen/lang/emitter.cpp:278-292` (`as_type<T>(reg)`) | | |
| F2 | omap register allocation and component masks | PARTIAL | `SD/codegen/lang/emitter.cpp:291` (`getRegisterStr(i * 4 + c)`); `SD/decompiler.cpp:119-123` (depth reg = popcount + 1) | `SR/frontend/maxwell/translate/impl/exit_program.cpp:12-35` | **B7.** Sparse or partial masks give the wrong registers, and unexported channels receive garbage. |
| F3 | Depth export | **VERIFIED** (full 4-component masks) | `SD/decompiler.cpp:119-123`; MSL `[[depth(any)]]`; test `T/shader_library_tests.cpp:991-1083` (0, 1 and 6 RTs with full masks, depth readback) | `SR/frontend/maxwell/translate/impl/exit_program.cpp:32-34` | Partial masks are covered by F2/B7. |
| F4 | Sample-mask export (omap_sample_mask) | MISSING | Parsed (`SD/decompiler.cpp:82`) but unused. | `SR/frontend/maxwell/translate/impl/exit_program.cpp:29-31`; `SR/backend/spirv/spirv_emit_context.cpp:1714-1718` | |
| F5 | Discard (KIL) | IMPLEMENTED_UNVERIFIED | `SD/decoder/exit.cpp:5-13`; `MslEmitter::emitDiscard` (≈`:1092`) gives `discard_fragment()` | | No test. |
| F6 | Early fragment tests (API_MANDATED_EARLY_Z) | MISSING | Not decoded (0x84). | clb197.h:249-252; `VC/renderer_vulkan/fixed_pipeline_state.cpp:140`; `vk_pipeline_cache.cpp:301`; `SR/backend/spirv/emit_spirv.cpp:323` | Matters for fragment shaders with image or buffer side effects. |
| F7 | PS_SATURATE / COLOR_CLAMP | MISSING | Not decoded (0x4ea, 0x980). | clb197.h:2549-2597, 4100-4103; Eden GL `glClampColor` (`VC/renderer_opengl/gl_rasterizer.cpp:1119-1120`) | Low priority. |
| F8 | Coverage-to-color, post-PS initial coverage, A2C override | MISSING | Not decoded. | clb197.h:1935-1939, 1817-1820, 3161-3167 | Rare. |
| F9 | Z-cull methods (SET_ZCULL*, CLEAR/INVALIDATE/STORE/LOAD_ZCULL, ZCULL_SYNC) | IMPLEMENTED_UNVERIFIED (no-op) | `default: writeReg` (`E/engine_base.hpp:16-20`) | clb197.h:2091, 3449-3453 and others | A no-op is correct; Eden treats them the same way. |

---

## (b) Confirmed bugs (exact code quotes)

### B1: Clears inherit stale encoder state: viewport, scissor, depth-stencil state, depth bias

**Severity: high. Visible corruption is possible.**

Clears are drawn as a fullscreen triangle into the *current* render encoder. `CommandBuffer::getRenderCommandEncoderHot1` reuses the encoder whenever the render pass is the same object, meaning the same targets:

```cpp
// M/command_buffer.cpp:77-78
    if (render_pass_descriptor == encoder_state.render_pass)
        return getRenderCommandEncoderUnchecked();
```

The color clear sets only the pipeline and cull mode:

```cpp
// M/renderer.cpp:402-417 (clearColorHot14)
    auto encoder = getRenderCommandEncoder(command_buffer_impl);
    ...
    command_buffer_impl->setRenderPipelineState(
        clear_color_pipeline_cache.find(pipeline_descriptor));
    command_buffer_impl->setCullMode(MTL::CullModeNone);
    // TODO: set viewport and scissor
    encoder->setVertexBytes(&layer, sizeof(layer), 0);
    encoder->setFragmentBytes(&color, sizeof(color), 0);
```

The clear vertex shader emits `out.position = float4(vertices[vid], 0.0, 1.0);` (`M/clear_color_pipeline_cache.cpp:27`). The clear pipeline also declares the depth attachment (`.depth_stencil_pixel_format = getDepthStencilPixelFormat()`).

When a draw or a depth clear ran earlier in the same encoder, three things go wrong:

1. **Depth corruption.** The color clear runs with whatever depth-stencil state is still bound. After a `clearDepth` that is `depth_stencil_state_always_and_write` (`M/renderer.cpp:450-451`). The color clear then writes depth = viewport near (0.0) over the whole clear area, wiping a depth buffer that was just cleared to 1.0, so standard-Z titles fail every LESS test afterwards. If the leftover state is a reverse-Z draw's GREATER/GEQUAL test with writes on, the color clear (z = 0) is rejected and the target is not cleared.
2. **Stencil interference.** A stencil test or stencil ops left over from the previous draw apply to the clear.
3. **Wrong coverage and depth value.** The clear is confined to the previous draw's viewport and scissor. The depth clear's z is transformed by the previous viewport depth range and offset by the previous depth bias. `clearDepthHot15` also says `// TODO: set viewport and scissor` (`M/renderer.cpp:452`) and never calls `setDepthBias`.

On hardware a clear depends only on the clear registers and CLEAR_SURFACE_CONTROL. Eden uses state-independent `vkCmdClearAttachments` with an explicit rect (`VC/renderer_vulkan/vk_rasterizer.cpp:438-486, 524-532`).

**Fix (`M/renderer.cpp:382-497`):** for each clear:
- set viewport (0, 0, w, h, 0, 1);
- set the scissor to the computed clear rect;
- bind an explicit depth-stencil state: none for color clears, depth-only Always+write for depth clears, stencil Replace with the guest mask for stencil clears;
- set depth bias to 0.

Go through `CommandBuffer::set*` so the raster cache (`T/README-raster-cache.md`) stays coherent.

### B2: GL-encoded depth compare functions become Always

```cpp
// E/3d.hpp:380
    CompareOp depth_compare_op;
// E/3d.cpp:2331
    gpu.getRenderer().setDepthCompareOp(regs.depth_compare_op);
// M/maxwell_to_mtl.cpp:707-710
    default:
        LOG_NOT_IMPLEMENTED(MetalRenderer, "Depth test func {}",
                            depth_compare_op);
        return MTL::CompareFunctionAlways;
```

clb197.h:2176-2193 defines OGL_NEVER..OGL_ALWAYS as 0x200-0x207 next to D3D 1-8. The stencil path in the same function already decodes both encodings:

```cpp
// E/3d.cpp:2366-2372
        const auto decode_func = [](u32 value) {
            if (value >= 0x200 && value <= 0x207) // OGL
                return static_cast<CompareOp>(value - 0x200 + 1);
```

The depth path does not. Eden maps both (`VC/renderer_vulkan/maxwell_to_vk.cpp:611-639`) and even defaults to `Always_GL` (`VC/engines/maxwell_3d.cpp:102`). This affects titles built on the Switch GL or Vulkan drivers. NVN appears to emit D3D values, since depth works in Crash 4 and Jump Force.

### B3: Constant-color/alpha and GL dual-source blend factors are not decoded

```cpp
// E/3d.cpp:128
// TODO: const color and alpha?
// E/3d.cpp:133-135
    if ((blend_factor & GL_BLEND_FACTOR_BIT) != 0u) { // GL
        u32 gl_blend_factor = blend_factor & ~GL_BLEND_FACTOR_BIT;
        switch (gl_blend_factor) {
// ... cases only for 0x0000, 0x0001, 0x0300-0x0308 ...
        default:
            ONCE(LOG_ERROR(Engines, "Unknown GL blend factor 0x{:04x}",
                           gl_blend_factor));
            return renderer::BlendFactor::One;
```

These are defined in clb197.h:2268-2310 and handled by Eden (`VC/renderer_vulkan/maxwell_to_vk.cpp:755-766`), but Hydra turns them all into **One**:

- `OGL_CONSTANT_COLOR` .. `OGL_ONE_MINUS_CONSTANT_ALPHA` (0xC001-0xC004)
- `OGL_SRC1COLOR` .. `OGL_INVSRC1ALPHA` (0xC900-0xC903)
- `D3D_BOTHSRCALPHA` / `D3D_BOTHINVSRCALPHA` (0x0C/0x0D; Eden maps these to constant alpha)
- `D3D_BLENDFACTOR` / `D3D_INVBLENDFACTOR` (0x0E/0x0F)

As a result, `renderer::BlendFactor::ConstColor..InvConstAlpha` (`R/const.hpp:348-351`, mapped in `M/maxwell_to_mtl.cpp:779-786`) are dead code, and the blend constant (`E/3d.cpp:2332`) never affects output.

### B4: Uninitialized blend registers decode as additive blending

Registers are zero-initialized. `getBlendFactor(0)` takes the D3D path and returns `One` for unknown values (`E/3d.cpp:195-198`). `getBlendOperation(0)` returns `Add` (`E/3d.cpp:94-96`). A title that enables blending without programming the factors therefore gets One/One. Eden explicitly resets the blend state to Add/One/Zero because "Doom and Bomberman seems to use the uninitialized registers and just enable blend" (`VC/engines/maxwell_3d.cpp:72-86`).

### B5: Blending is not disabled for integer render targets

```cpp
// E/3d.cpp:1241-1244
        color_target.blend_enabled =
            static_cast<bool>(regs.color_blend_enabled[i]) &&
            !(VisualDiagnostics::instance().selected() &&
              VisualDiagnostics::instance().options().no_blend);
```

`M/pipeline.cpp:161-176` then calls `setBlendingEnabled(true)` for any format, including the UInt and SInt formats. Metal does not allow blending on integer pixel formats: the debug layer asserts, and otherwise the pipeline may fail and the draw is skipped by `canDraw`. Maxwell ignores blend on integer targets, and Eden forces it off:

```cpp
// VC/renderer_vulkan/vk_rasterizer.cpp:1808-1809
            setup_enables[index] =
                (!is_integer && regs.blend.enable[index] != 0) ? VK_TRUE : VK_FALSE;
```

### B6: Format mappings that abort, hit undefined behavior, or swap channels

Full tables are in section (c).

**(a) Zeta 0x15 (X8Z24) aborts.**

```cpp
// R/const.cpp:438-452: no Z24X8Unorm case
        DEPTH_SURFACE_FORMAT_CASE(S8Z24Unorm, Z24Unorm_S8Uint) // TODO
        DEPTH_SURFACE_FORMAT_CASE(Z24S8Unorm, Z24Unorm_S8Uint)
        ...
    default:
        LOG_FATAL(Gpu, "Depth surface format {}", depth_surface_format);
```

`LOG_FATAL` calls `abort()` (`src/common/log.hpp:42-47`). `TextureFormat::Z24Unorm_X8Uint` already exists and is mapped in Metal (`M/maxwell_to_mtl.cpp:87`). Eden maps X8Z24 to X8_D24_UNORM (`VC/surface.cpp:92-93`).

**(b) RT formats 0x40-0x47 abort.** A16, AF16, AF32, A8R8, R16_A16, RF16_AF16, RF32_AF32 and B8G8R8A8 (clb197.h:734-741) are missing from `ColorSurfaceFormat` (`G/const.hpp:601-666`), so they hit `LOG_FATAL(Gpu, "Color surface format {}", ...)` (`R/const.cpp:426-427`).

**(c) RGBA32Sint has no Metal LUT entry.** The LUT jumps from `RGBA32Uint` to the depth entries (`M/maxwell_to_mtl.cpp:83-85`). The lookup then dereferences `end()`:

```cpp
// M/maxwell_to_mtl.cpp:359-363
    auto it = pixel_format_lut.find(format);
    ASSERT_DEBUG(it != pixel_format_lut.end(), MetalRenderer,
                 "Unknown format {}", format);
    const auto& info = it->second;
```

`ASSERT_DEBUG` compiles to `(void)(condition)` in release builds (`src/common/log.hpp:74`). The resulting garbage pixel format then trips the non-debug `ASSERT(texture != nullptr, ...)` in `M/texture.cpp:82`. This is reachable from RT 0xC1 (`R/const.cpp:367`) and from TIC RGBA32 SINT (`R/const.cpp:344`). `RGB5A1Unorm` is also missing but not currently produced.

**(d) Invalid formats reach texture creation.** Unsupported or disabled RT formats (A8 0xF7, X1R5G5B5 0xF8, 0xFB-0xFF, or format 0 with a nonzero address) map to `TextureFormat::Invalid`. `getColorTargetTexture` never checks for that (`E/3d.cpp:664-716`). `getTextureFormatBpp(Invalid)` returns 0, so `width = render_target.width_or_stride / renderer::getTextureFormatBpp(format);` (`E/3d.cpp:695-696`) divides by zero, and `getMinimumWidth` computes `64 / bpp` (`E/3d.cpp:211-212`). The LUT then misses as in (c). Eden returns an empty view for `RenderTargetFormat::NONE` (`VC/texture_cache/texture_cache.h:1914-1916`).

**(e) 0xDF has red and blue swapped.**

```cpp
// R/const.cpp:395
        COLOR_SURFACE_FORMAT_CASE(BGR10A2Unorm, RGB10A2Unorm) // TODO: correct?
```

clb197 `A2R10G10B10` (0xDF, clb197.h:703) has R in the high bits. Metal provides `MTLPixelFormatBGR10A2Unorm`. Eden keeps it distinct as `A2R10G10B10_UNORM` (`VC/surface.cpp:139-140`).

### B7: Fragment output registers ignore the SPH omap masks

```cpp
// SD/codegen/lang/emitter.cpp:279-291
        for (u8 i = 0; i < COLOR_TARGET_COUNT; i++) {
            const auto color_target_data_type =
                state.color_target_data_types[i];
            if (color_target_data_type == ColorDataType::Invalid)
                continue;
            for (u8 c = 0; c < 4; c++) {
                writeStatement(
                    "{} = as_type<{}>({})", ...,
                    toType(color_target_data_type), getRegisterStr(i * 4 + c));
// SD/decompiler.cpp:119-123
        // Maxwell packs enabled color components first, then reserves one
        // register for the sample mask before the optional depth export.
        if (header.ps.omap_depth)
            context.fragment_depth_register =
                static_cast<u32>(std::popcount(header.ps.omap_target)) + 1;
```

The code is internally inconsistent. Colors assume four unpacked registers per RT index. The depth register assumes packed components. Eden's `ExitFragment` (`SR/frontend/maxwell/translate/impl/exit_program.cpp:12-35`) works as follows:
- RTs with no output components consume no registers;
- each RT that has outputs consumes four registers, whether or not each component is enabled;
- the sample mask goes at `src_reg`;
- depth goes at `src_reg + 1`.

Divergent cases:

| omap | Hydra | Eden |
|---|---|---|
| 0xF0 (RT1 only) | reads RT1 from R4-R7 | R0-R3 |
| 0x7 + depth | depth from R4 | depth from R5 |

Hydra also writes register contents into every *bound* RT and component even when the omap bit is clear, which corrupts targets the shader does not export. `testFragmentDepth` covers only full masks (`mask = (1u << (color_count * 4)) - 1`, `T/shader_library_tests.cpp:1001`).

### B8: Dual-source blending cannot work

D3D SRC1 factors are mapped (`E/3d.cpp:187-194` to `M/maxwell_to_mtl.cpp:771-778`). However, the MSL stage outputs are declared only for bound RTs, each with qualifier `color(i)` (`SD/codegen/lang/msl/emitter.cpp` ≈`:470` and ≈`:1434`, `return fmt::format("color({})", sv.index);`). No `[[color(0), index(1)]]` output exists, and with CT count = 1 (the usual dual-source setup) output 1 is not declared at all. Eden moves output 1 to Location 0 / Index 1 when attachment 0 uses an SRC1 factor (`VC/renderer_vulkan/fixed_pipeline_state.cpp:60-90, 133`; `SR/backend/spirv/spirv_emit_context.cpp:1692-1706`).

### B9: Stencil clears ignore the layer and the stencil write mask

```cpp
// M/renderer.cpp:472-473
    // TODO: layer
    (void)layer;
// M/renderer.cpp:489-492
    struct {
        u32 layer_id;
        float value;
    } params = {.layer_id = 0, .value = 0.0f};
// M/renderer.cpp:64 (depth_stencil_state_clear_stencil)
        stencil->setWriteMask(0xff);
```

Eden clears `clear_surface.layer` (`VC/renderer_vulkan/vk_rasterizer.cpp:442`). When SET_STENCIL_MASK is not 0xFF it uses a masked clear shader (`vk_rasterizer.cpp:558-566`). CLEAR_SURFACE_CONTROL.RESPECT_STENCIL_MASK (clb197.h:1561-1563) is not decoded at all.

### B10: Layered color clears abort debug builds

```cpp
// M/renderer.cpp:398-400
    // TODO: layer
    ASSERT_DEBUG(layer == 0, MetalRenderer,
                 "Layered clears (layer: {}) not implemented", layer);
```

The layer is in fact supported through `render_target_array_index` (`M/clear_color_pipeline_cache.cpp:27-28`), and NVN's multi-layer clear macro issues a CLEAR_SURFACE per layer (Eden HLE: `VC/macro.cpp:187-198`). Debug builds abort on cube or array RT clears. Release builds work, apart from stencil (B9).

### B11: The zeta THIRD_DIMENSION control bit is ignored

`struct { u16 layers; bool volume; } depth_target_array_mode;` (`E/3d.hpp:361-364`) is used as the layer count unconditionally (`E/3d.cpp:726-735`). clb197.h:2048-2052 defines bit 16 = `ARRAY_SIZE_IS_ONE`. Eden uses `resources.layers = 1` in that case (`VC/texture_cache/image_info.cpp:217-224`).

### B12: CLEAR_SURFACE and CT_SELECT field width and bounds

**Robustness, low severity.**

- `u32 layer_id : 11;` (`E/3d.hpp:638`) against RT_ARRAY_INDEX 25:10 (16 bits, clb197.h:3570).
- `target_id` (4 bits, up to 15) indexes `color_targets[render_target_id]` (`M/renderer.cpp:386-389`) and `descriptor.pixel_formats[descriptor.render_target_id]` (`M/clear_color_pipeline_cache.cpp:51`). Both arrays have 8 entries.
- `color_target_control.count` (up to 15) indexes `descriptor.color_targets[i]` (`E/3d.cpp:767-772`).

### B13: Report-semaphore writes bypass cache invalidation

`*reinterpret_cast<u32*>(ptr) = static_cast<u32>(value);` and the four-word writes (`E/3d.cpp:593-605`) store directly into guest memory with no `invalidateMemory(...)`. Compare `loadConstBuffer` (`E/3d.cpp:625-629`), the inline engine (`E/inline_base.cpp:44`) and the DMA paths (`E/copy.cpp:136`). A buffer-cache copy of that page, for example a query-result buffer later read by a shader or by conditional rendering, can go stale. The impact is unverified.

### B14: The ZPASS counter ignores its enable and counts draws, not samples

`zpass_pixel_count++;` runs at draw start (`E/3d.cpp:2276-2277`), before draw-skip decisions, regardless of SET_ZPASS_PIXEL_COUNT (0x545). This is a known approximation (TODO at `E/3d.hpp:626-628`). It is listed here because counts feed the conditional-render and visibility logic described in section (e).

### B15: Z24S8 (0x14) and S8Z24 (0x16) are collapsed into one format

```cpp
// R/const.cpp:442-443
        DEPTH_SURFACE_FORMAT_CASE(S8Z24Unorm, Z24Unorm_S8Uint) // TODO
        DEPTH_SURFACE_FORMAT_CASE(Z24S8Unorm, Z24Unorm_S8Uint)
// M/texture.cpp:315-318 (CPU upload decode)
            depth[i] = static_cast<f32>(word & 0xffffffu) /
                       static_cast<f32>(0xffffffu);
            stencil[i] = descriptor.format == TextureFormat::Z24Unorm_S8Uint
                             ? static_cast<u8>(word >> 24)
```

Eden treats 0x14 as S8_UINT_D24_UNORM (stencil in the low byte, which is also the format G24R8 TICs alias) and 0x16 as D24_UNORM_S8_UINT (`VC/surface.cpp:80-83`, `VC/texture_cache/format_lookup_table.cpp:154-161`). Guest-memory uploads of 0x14 surfaces therefore decode depth and stencil swapped. GPU-resident data is unaffected, which is why `T/texture_cache_tests.cpp:662-742` passes. Hydra's enum names are also reversed relative to NVIDIA's (`G/const.hpp:673-675`): Hydra `S8Z24Unorm` is clb197 `Z24S8` (0x14).

### Risks (not confirmed bugs)

- **32-bit hash-only cache lookup.** `CacheBase::find` keys only on a 32-bit hash and never compares descriptors (`src/common/types.hpp:352-364`). A hash collision silently returns the wrong pipeline, render pass, depth-stencil state or clear pipeline. With roughly 10k pipelines the chance of at least one collision is about 1%.
- **RT width heuristic** (`E/3d.cpp:205-220`), see R2.
- **Clear color for integer RTs.** Hydra uses raw bits, Eden uses a float-to-int heuristic (`VC/renderer_vulkan/vk_rasterizer.cpp:511-517`). Hydra's choice is tested (C2) and matches NVN's integer-clear API. Keep it, but document it.

---

## (c) Format mapping gaps

### c.1 Color targets: every clb197 `SET_COLOR_TARGET_FORMAT_V_*` value (clb197.h:673-741)

Chain: Hydra `ColorSurfaceFormat` (`G/const.hpp:601-666`) → `toTextureFormat` (`R/const.cpp:357-430`) → Metal (`M/maxwell_to_mtl.cpp:46-179`). Eden column is `VC/surface.cpp:100-206`.

| Value | clb197 name | Hydra enum | TextureFormat | Metal | Status | Eden |
|---|---|---|---|---|---|---|
| 0x00 | DISABLED | Invalid | Invalid | none | ✗ if address ≠ 0: no guard, UB (B6d) | empty view |
| 0xC0 | RF32_GF32_BF32_AF32 | RGBA32Float | RGBA32Float | RGBA32Float | ✓ | ✓ |
| 0xC1 | RS32_GS32_BS32_AS32 | RGBA32Sint | RGBA32Sint | **not in LUT** | ✗ UB / abort (B6c) | ✓ |
| 0xC2 | RU32_GU32_BU32_AU32 | RGBA32Uint | RGBA32Uint | RGBA32Uint | ✓ | ✓ |
| 0xC3 | RF32_GF32_BF32_X32 | RGBX32Float | RGBX32Float | RGBA32Float (HACK) | ✓ (X treated as A) | same |
| 0xC4 | RS32_GS32_BS32_X32 | RGBX32Sint | RGBX32Sint | RGBA32Sint | ✓ | same |
| 0xC5 | RU32_GU32_BU32_X32 | RGBX32Uint | RGBX32Uint | RGBA32Uint | ✓ | same |
| 0xC6 | R16_G16_B16_A16 | RGBA16Unorm | RGBA16Unorm | RGBA16Unorm | ✓ | ✓ |
| 0xC7 | RN16_GN16_BN16_AN16 | RGBA16Snorm | RGBA16Snorm | RGBA16Snorm | ✓ | ✓ |
| 0xC8 | RS16_GS16_BS16_AS16 | RGBA16Sint | RGBA16Sint | RGBA16Sint | ✓ | ✓ |
| 0xC9 | RU16_GU16_BU16_AU16 | RGBA16Uint | RGBA16Uint | RGBA16Uint | ✓ | ✓ |
| 0xCA | RF16_GF16_BF16_AF16 | RGBA16Float | RGBA16Float | RGBA16Float | ✓ | ✓ |
| 0xCB | RF32_GF32 | RG32Float | RG32Float | RG32Float | ✓ | ✓ |
| 0xCC | RS32_GS32 | RG32Sint | RG32Sint | RG32Sint | ✓ | ✓ |
| 0xCD | RU32_GU32 | RG32Uint | RG32Uint | RG32Uint | ✓ | ✓ |
| 0xCE | RF16_GF16_BF16_X16 | RGBX16Float | RGBX16Float | RGBA16Float | ✓ (X) | same |
| 0xCF | A8R8G8B8 | BGRA8Unorm | BGRA8Unorm | BGRA8Unorm | ✓ | ✓ |
| 0xD0 | A8RL8GL8BL8 | BGRA8Unorm_sRGB | BGRA8Unorm_sRGB | BGRA8Unorm_sRGB | ✓ | ✓ |
| 0xD1 | A2B10G10R10 | RGB10A2Unorm | RGB10A2Unorm | RGB10A2Unorm | ✓ | ✓ |
| 0xD2 | AU2BU10GU10RU10 | RGB10A2Uint | RGB10A2Uint | RGB10A2Uint | ✓ | ✓ |
| 0xD5 | A8B8G8R8 | RGBA8Unorm | RGBA8Unorm | RGBA8Unorm | ✓ | ✓ |
| 0xD6 | A8BL8GL8RL8 | RGBA8Unorm_sRGB | RGBA8Unorm_sRGB | RGBA8Unorm_sRGB | ✓ | ✓ |
| 0xD7 | AN8BN8GN8RN8 | RGBA8Snorm | RGBA8Snorm | RGBA8Snorm | ✓ | ✓ |
| 0xD8 | AS8BS8GS8RS8 | RGBA8Sint | RGBA8Sint | RGBA8Sint | ✓ | ✓ |
| 0xD9 | AU8BU8GU8RU8 | RGBA8Uint | RGBA8Uint | RGBA8Uint | ✓ | ✓ |
| 0xDA | R16_G16 | RG16Unorm | RG16Unorm | RG16Unorm | ✓ | ✓ |
| 0xDB | RN16_GN16 | RG16Snorm | RG16Snorm | RG16Snorm | ✓ | ✓ |
| 0xDC | RS16_GS16 | RG16Sint | RG16Sint | RG16Sint | ✓ | ✓ |
| 0xDD | RU16_GU16 | RG16Uint | RG16Uint | RG16Uint | ✓ | ✓ |
| 0xDE | RF16_GF16 | RG16Float | RG16Float | RG16Float | ✓ | ✓ |
| 0xDF | A2R10G10B10 | BGR10A2Unorm | **RGB10A2Unorm** | RGB10A2Unorm | ✗ R/B swapped; use `MTLPixelFormatBGR10A2Unorm` (B6e) | A2R10G10B10 ✓ |
| 0xE0 | BF10GF11RF11 | RG11B10Float | RG11B10Float | RG11B10Float | ✓ | ✓ |
| 0xE3 | RS32 | R32Sint | R32Sint | R32Sint | ✓ | ✓ |
| 0xE4 | RU32 | R32Uint | R32Uint | R32Uint | ✓ | ✓ |
| 0xE5 | RF32 | R32Float | R32Float | R32Float | ✓ | ✓ |
| 0xE6 | X8R8G8B8 | BGRX8Unorm | BGRX8Unorm | BGRA8Unorm | ✓ (X) | same |
| 0xE7 | X8RL8GL8BL8 | BGRX8Unorm_sRGB | BGRX8Unorm_sRGB | BGRA8Unorm_sRGB | ✓ | same |
| 0xE8 | R5G6B5 | B5G6R5Unorm | B5G6R5Unorm | B5G6R5Unorm | ✓ (Apple GPUs only) | ✓ |
| 0xE9 | A1R5G5B5 | BGR5A1Unorm | BGR5A1Unorm | BGR5A1Unorm | ✓ (Apple GPUs only) | ✓ |
| 0xEA | G8R8 | RG8Unorm | RG8Unorm | RG8Unorm | ✓ | ✓ |
| 0xEB | GN8RN8 | RG8Snorm | RG8Snorm | RG8Snorm | ✓ | ✓ |
| 0xEC | GS8RS8 | RG8Sint | RG8Sint | RG8Sint | ✓ | ✓ |
| 0xED | GU8RU8 | RG8Uint | RG8Uint | RG8Uint | ✓ | ✓ |
| 0xEE | R16 | R16Unorm | R16Unorm | R16Unorm | ✓ | ✓ |
| 0xEF | RN16 | R16Snorm | R16Snorm | R16Snorm | ✓ | ✓ |
| 0xF0 | RS16 | R16Sint | R16Sint | R16Sint | ✓ | ✓ |
| 0xF1 | RU16 | R16Uint | R16Uint | R16Uint | ✓ | ✓ |
| 0xF2 | RF16 | R16Float | R16Float | R16Float | ✓ | ✓ |
| 0xF3 | R8 | R8Unorm | R8Unorm | R8Unorm | ✓ | ✓ |
| 0xF4 | RN8 | R8Snorm | R8Snorm | R8Snorm | ✓ | ✓ |
| 0xF5 | RS8 | R8Sint | R8Sint | R8Sint | ✓ | ✓ |
| 0xF6 | RU8 | R8Uint | R8Uint | R8Uint | ✓ | ✓ |
| 0xF7 | A8 | A8Unorm | **Invalid** | none | ✗ UB when bound (B6d) | unsupported (commented out, `VC/gpu.h:86`) |
| 0xF8 | X1R5G5B5 | BGR5X1Unorm | **Invalid** | none | ✗ UB when bound | maps to A1R5G5B5 (`VC/surface.cpp:173-175`) |
| 0xF9 | X8B8G8R8 | RGBX8Unorm | RGBX8Unorm | RGBA8Unorm | ✓ (X) | same |
| 0xFA | X8BL8GL8RL8 | RGBX8Unorm_sRGB | RGBX8Unorm_sRGB | RGBA8Unorm_sRGB | ✓ | same |
| 0xFB-0xFE | Z1R5G5B5 / O1R5G5B5 / Z8R8G8B8 / O8R8G8B8 | `*Unknown*` | Invalid | none | ✗ UB when bound | unsupported |
| 0xFF | R32 | Y32UintUnknownFF | Invalid | none | ✗ UB when bound | unsupported |
| 0x40-0x47 | A16, AF16, AF32, A8R8, R16_A16, RF16_AF16, RF32_AF32, B8G8R8A8 | **not in enum** | none | none | ✗ `LOG_FATAL` abort (B6b) | unsupported (commented out, `VC/gpu.h:90-104`) |

**X-format note.** RGBX/BGRX targets are stored as RGBA/BGRA. On hardware, DST_ALPHA blend factors on X formats read 1.0, but Hydra reads the stored alpha. Eden behaves the same way. To be exact, map DST_ALPHA to One when the RT format is an X format.

**Metal LUT keys missing for produced TextureFormats:** `RGBA32Sint` (reachable), plus `RGB5A1Unorm` and `Invalid`, which are not reachable today but are also unguarded. The script comparison is enum (130) against LUT (127). `toMtlPixelFormatInfo` should return a sentinel entry instead of dereferencing `end()`.

### c.2 Zeta: every `SET_ZT_FORMAT_V_*` value (clb197.h:1358-1368)

| Value | clb197 name | Hydra enum (`G/const.hpp:668-680`) | TextureFormat | Metal | Status | Eden (`VC/surface.cpp:78-98`) |
|---|---|---|---|---|---|---|
| 0x0A | ZF32 | Z32Float | Z32Float | Depth32Float | ✓ | D32_FLOAT |
| 0x13 | Z16 | Z16Unorm | Z16Unorm | Depth16Unorm | ✓ | D16_UNORM |
| 0x14 | Z24S8 | "S8Z24Unorm" (name reversed) | Z24Unorm_S8Uint | Depth32Float_Stencil8 | ~ emulated; CPU decode packing wrong (B15) | S8_UINT_D24_UNORM |
| 0x15 | X8Z24 | "Z24X8Unorm" | **no case** | none | ✗ `LOG_FATAL` abort (B6a); `Z24Unorm_X8Uint` already exists | X8_D24_UNORM |
| 0x16 | S8Z24 | "Z24S8Unorm" | Z24Unorm_S8Uint | Depth32Float_Stencil8 | ✓ emulated (decode matches) | D24_UNORM_S8_UINT |
| 0x17 | S8 | S8Uint | S8Uint | Stencil8 | ✓ | S8_UINT |
| 0x18 | V8Z24 | Z24C8Unorm | **Invalid** | none | ✗ UB | listed and treated as D24 for bias (`vk_rasterizer.cpp:1290-1293`); format falls to the default |
| 0x19 | ZF32_X24S8 | Z32S8X24Float | Z32Float_X24S8Uint | Depth32Float_Stencil8 | ✓ | D32_FLOAT_S8_UINT |
| 0x1D | X8Z24_X16V8S8 | Z24X8S8C8X16Unorm | Invalid | none | ✗ UB | unsupported |
| 0x1E | ZF32_X16V8X8 | Z32X8C8X16Float | Invalid | none | ✗ UB | unsupported |
| 0x1F | ZF32_X16V8S8 | Z32S8C8X16Float | Invalid | none | ✗ UB | unsupported |

**Host notes.**
- `Depth24Unorm_Stencil8` is not available on Apple silicon, so D32F_S8 is the right host format. Hydra uses it, and `blitTextureHot9` still lists D24S8 (`M/renderer.cpp:224`).
- The depth-bias constant for D24 surfaces needs rescaling when emulated as D32F. Eden: `VC/renderer_vulkan/vk_rasterizer.cpp:1289-1310`. Hydra passes the raw float (`E/3d.cpp:2415-2419`); that belongs to the rasterizer audit.
- The V8 and C8 coverage/compression variants can map to the base depth format: 0x18 → Z24Unorm_S8Uint, 0x1D → Z24Unorm_S8Uint, 0x1E → Z32Float, 0x1F → Z32Float_X24S8Uint. Ignore the coverage plane.

---

## (d) Metal limitations and the standard emulation techniques

| Guest feature | Metal situation | Standard technique and where it goes in Hydra |
|---|---|---|
| Logic ops | Not supported. | **Programmable blending.** Apple-family GPUs (all Apple silicon) can read the attachment in the fragment function with a `[[color(n)]]` input. Generate a fragment variant: read dst, convert UNORM/SNORM to integer bits (`round(x*(2^n-1))`), apply the op to src and dst bits, convert back, and disable fixed-function blend for that attachment. Float RTs ignore logic ops, which is Vulkan behavior; Eden disables them on AMD floats (`vk_rasterizer.cpp:1112-1124`). Key the variant on (op, format) in `GuestShaderState`. |
| Depth bounds test | Not supported. | Ignore it with a one-time warning (Eden's fallback, `vk_rasterizer.cpp:1484-1488`). Exact emulation would need a copy of the depth buffer sampled in the fragment shader with `discard`, because Apple GPUs cannot read the depth attachment in-pass. That is rarely worth it. |
| Alpha test | No fixed function. | Compare RT0 alpha against the reference in the fragment epilogue and `discard_fragment()`, as Eden does (`emit_spirv_special.cpp:67-91`). Put func and ref in `GuestShaderState`. Eden bakes ref into the key (`fixed_pipeline_state.cpp:182`); a uniform avoids permutations. |
| sRGB write enable | Conversion is a property of the pixel format. | When FRAMEBUFFER_SRGB is off on an sRGB RT, render through a non-sRGB *view* of the same texture. `PixelFormatView` usage is already set (`M/texture.cpp:66-70`). |
| MSAA render targets | Native MS textures and `rasterSampleCount`. | Add the sample count to `TextureDescriptor`, storage hash and pipeline. Use `texture2d_ms` for TIC `msaa_mode` fetch. The guest memory footprint of an MS surface is sample-expanded, so the layout math must expand. |
| MS → SS resolve | Store-action resolve (`MTLStoreActionMultisampleResolve` with `resolveTexture`; depth `depthResolveFilter` sample0/min/max); no blit-encoder resolve. | For 2D-engine resolves: run a render pass with a resolve store action, or a custom resolve shader (needed for integer formats, which take sample 0, and for sub-rects). Eden uses `vkCmdResolveImage` (`vk_texture_cache.cpp:1403-1406`). |
| Alpha-to-coverage / alpha-to-one | Native (`alphaToCoverageEnabled`, `alphaToOneEnabled`). | Add both to `PipelineDescriptor` and the hash. They work at 1x as well. |
| Sample mask | `[[sample_mask]]` output. | AND the fixed-function SAMPLE_MASK into the shader output, and export the omap sample mask (F4). |
| Sample shading | Declaring `[[sample_id]]` forces per-sample execution; no min-fraction control. | Enable when HYBRID_ANTI_ALIAS_CONTROL passes > 1. |
| Sample locations | `MTLRenderPassDescriptor setSamplePositions:count:` when `areProgrammableSamplePositionsSupported`. | Low priority. |
| Dual-source blending | `[[color(0), index(1)]]` output (macOS / Apple silicon). | Emit when attachment 0 uses an SRC1 factor (Eden rule). |
| Depth clamp | `setDepthClipMode(MTLDepthClipModeClamp)`. | Derive from VIEWPORT_CLIP_CONTROL using Eden's rule. Cache it in `CommandBuffer`. |
| Early fragment tests | `[[early_fragment_tests]]` on the fragment function. Incompatible with shader depth writes. | Emit only when the shader has no depth export. |
| Occlusion queries | Visibility result buffer (`MTLVisibilityResultModeCounting`, `setVisibilityResultMode:offset:`); results available after command-buffer completion. | Allocate per command buffer and record (offset → guest address) per query. Sum across encoders. Write results in the completion handler, or flush on guest read through the existing memory-tracking path. Respect the ZPASS enable. |
| GPU timestamps | `MTLCounterSampleBuffer` (timestamp set; stage-boundary sampling only on Apple GPUs). | Keep CPU timestamps (current), or sample at encoder boundaries. |
| Conditional rendering | No predication. Indirect command buffers are the only GPU-side option. | CPU-evaluate RENDER_ENABLE from guest memory, as Eden does (`maxwell_3d.cpp:515-560`). Correct here because Hydra writes query results synchronously. |
| Texture barrier / feedback loops | Sampling a bound attachment in the same pass is undefined; on TBDR it reads pre-pass memory. | On PIXEL_SHADER_BARRIER, and when a sampled view's base texture equals a bound attachment, end the render encoder (the next draw reloads). For same-pixel feedback use `[[color(n)]]` programmable blending. Eden ends the render pass (`vk_rasterizer.cpp:876-879`). |
| Blending on integer formats | Not allowed. | Force blend off for integer RT formats (B5). |
| D24S8 | `Depth24Unorm_Stencil8` absent on Apple silicon. | D32F_S8, as now. Keep the CPU pack/unpack per packing (B15) and rescale depth bias. |
| Per-RT write masks and blend state | Pipeline state. | Already permuted through `PipelineCache` (`R/pipeline_cache.cpp`). |

---

## (e) Prioritized implementation backlog

Ordering: crash/UB first, then widespread correctness, then features. The test-harness pattern is the existing Metal GPU tests: `T/shader_library_tests.cpp` (`testClearColor`, `testFragmentDepth`) and `T/raster_state_tests.cpp` create small offscreen targets, drive Hydra's own classes (`ClearColorPipelineCache`, `DepthStencilStateCache`, `Pipeline`, `CommandBuffer`) and read pixels back with `getBytes` or a blit. For engine-level decode, move pure decoders (compare op, blend factor/op, stencil op, format maps) into a header such as `E/maxwell_decode.hpp` and unit-test them. A recording `IRenderer` stub (`R/null/` is a starting point) can then check that `ThreeD` method sequences produce the expected renderer calls.

### P0: crashes, undefined behavior, gross corruption

**1. Clear path rewrite (B1, B9, B10, C9-C11).**
- **What:**
  - Compute the clear rect: attachment size ∩ surface clip (`E/3d.hpp:329-338`). Then ∩ scissor[0] if CLEAR_SURFACE_CONTROL bit 8 (USE_SCISSOR0) and that scissor is enabled; ∩ the clear rect (`regs_raw[0x35b/0x35c]`) if bit 4; ∩ viewport[0] clip (`E/3d.hpp:248-259`) if bit 12.
  - Extend `IRenderer::clearColor/clearDepth/clearStencil` (`R/renderer.hpp:154-160`) with the rect and stencil write mask.
  - In `M/renderer.cpp` clears: set viewport to the full attachment with depth 0..1; scissor = rect; explicit depth-stencil state (a "nothing" state for color, Always+write for depth, Replace with the guest SET_STENCIL_MASK for stencil); `setDepthBias(0,0,0)`; honor the layer for stencil; remove `ASSERT_DEBUG(layer == 0)`.
  - Optional: when the rect covers the attachment and nothing has been drawn yet in the encoder, use load-action clears (`RenderTargetDescriptor::load_action_clear`, `M/render_pass.cpp:34-41`).
- **Why:** every title clears every frame. The stale-state leak corrupts depth whenever clears follow draws or depth clears on the same targets. Scissored clears are common: split-screen, atlases, UI. UE4 titles (Crash 4, Jump Force) clear shadow and GBuffer targets heavily.
- **Test:** Metal readback test in one encoder: draw with depth write on, a small viewport, a scissor and depth bias, then (a) clear color: the full RT changes and depth is unchanged; (b) clear depth to 1.0: the exact value is read back; (c) USE_SCISSOR0 clear changes only the rect; (d) a layered clear of a 2D array changes only layer 1; (e) stencil clear with write mask 0x0F.
- **Risk:** medium (hot path). Mitigate with the raster-cache tests and Crash 4 / Jump Force visual A/B.

**2. Format crash and UB fixes (B6).**
- **What:**
  - Zeta 0x15 → `Z24Unorm_X8Uint`; 0x18 → `Z24Unorm_S8Uint`; 0x1D/0x1E/0x1F → base formats.
  - Add `RGBA32Sint` (and `RGB5A1Unorm`) to `pixel_format_lut`; make `toMtlPixelFormatInfo` return a sentinel instead of dereferencing `end()`.
  - Make `toTextureFormat(ColorSurfaceFormat)` return Invalid instead of `LOG_FATAL`, and add the 0x40-0x47 enum values.
  - Return `nullptr` from `getColorTargetTexture` / `getDepthStencilTargetTexture` for Invalid formats, mirroring Eden `texture_cache.h:1914-1916`.
  - New `TextureFormat::BGR10A2Unorm` → `MTLPixelFormatBGR10A2Unorm` for 0xDF.
  - X1R5G5B5 → BGR5A1Unorm.
  - A8 → R8 with swizzle, or `A8Unorm` if renderable.
- **Why:** one aborting format kills the title, and these are reachable from guest register writes.
- **Test:** a table-driven test over every clb197 RT and ZT value: `toTextureFormat` → LUT → create a Metal texture → clear → read back one pixel. Add a red-in-R check for 0xDF.
- **Risk:** low.

**3. Depth compare GL encodings (B2).**
- **What:** promote the stencil `decode_func` (`E/3d.cpp:2366-2372`) to a shared `decodeCompareOp`, and apply it to `depth_compare_op` (`E/3d.cpp:2331`), alpha-test func (item 6) and clear paths.
- **Why:** any GL- or Vulkan-driver title gets Always, so depth is broken.
- **Test:** pure unit test plus a DS-cache readback with GL LESS against D3D LESS.
- **Risk:** very low.

**4. Blend correctness (B3, B4, B5, D12).**
- **What:**
  - `getBlendFactor`: GL 0xC001-0xC004 → ConstColor / InvConstColor / ConstAlpha / InvConstAlpha; 0xC900-0xC903 → Src1*; D3D 0x0E/0x0F → (Inv)ConstColor; D3D 0x0C/0x0D → (Inv)ConstAlpha (Eden). Strip only the 0x4000 bit for the 0x43xx group.
  - Reset values in `ThreeD::ThreeD` (`E/3d.cpp:235-257`): blend Add/One/Zero (common and per-target); stencil ops Keep, funcs Always, masks 0xFF; depth func Always (Eden `maxwell_3d.cpp:72-102`).
  - Force `blend_enabled=false` for integer RT formats (`toColorDataType != Float`) in `getPipeline` (`E/3d.cpp:1241`).
- **Why:** constant-alpha fades and dual-source paths render wrong; Doom and Super Bomberman R rely on the reset values (Eden comment); integer targets with blend enabled drop draws.
- **Test:**
  - decode unit tests;
  - a Metal readback through Hydra's `Pipeline` with a `ConstColor` factor and `setBlendColor`;
  - pipeline creation succeeds for an RGBA8Uint RT with guest blend enabled.
- **Risk:** low.

### P1: widespread rendering correctness

**5. omap allocation (B7, F4).**
- **What:** pass `omap_target`, `omap_sample_mask` and `omap_depth` into `DecompilerContext` (`SD/decompiler.cpp:116-124`). Allocate registers per Eden `ExitFragment`: skip RTs with no components, four registers per enabled RT, sample mask at `src_reg`, depth at `src_reg+1`. Write only enabled components in `SD/codegen/lang/emitter.cpp:278-292`. For bound RTs or components without omap bits, do not write; also narrow the Metal write mask in `getPipeline` so those channels stay unchanged. Emit `[[sample_mask]]` from the sample-mask register.
- **Why:** shaders that write only some MRTs, or vec3 outputs with depth, read the wrong registers. Unexported targets get garbage.
- **Test:** extend `testFragmentDepth` with omap 0xF0, 0x7+depth, 0xF7 and 0xF00F, reading back color and depth.
- **Risk:** medium (every fragment shader). Output is identical for full masks.

**6. Alpha test (A1).**
- **What:** read `regs_raw[0x4bb/0x4c4/0x4c5]` in `ThreeD::getShader` for the Fragment stage (`E/3d.cpp:972-979`). Add func and ref to `GuestShaderState` and the shader hash. Emit before the outputs: `if (!(as_type<float>(R_rt0_alpha) OP ref)) discard_fragment();`, using the RT0 alpha register from item 5.
- **Why:** NVN exposes alpha test; ported engines use it for foliage, fences and UI cutouts.
- **Test:** quad with an alpha gradient, each of the 8 funcs; read back coverage.
- **Risk:** low (active only when enabled).

**7. Depth clamp (D5).**
- **What:** decode `regs_raw[0x64f]` GEOMETRY_CLIP 13:11 with Eden's rule (`vk_rasterizer.cpp:1659-1664`). Add renderer state, plus `CommandBuffer::setDepthClipMode` with caching, giving `MTLDepthClipModeClamp`.
- **Why:** the NVN default (WZERO_CLIP) means clamp is on in Eden. Without it, shadow casters in front of the near plane and sky or far geometry are clipped.
- **Test:** draw with z>w, read back the clamped depth.
- **Risk:** low to medium. It changes behavior for every title, toward Eden's.

**8. Dual-source blending and common write mask (B8, R10).**
- **What:** compute `dual_source` the way Eden's `ComputeAttachment0DualSourceBlend` does. Add it to `GuestShaderState`. MSL: output 0 → `[[color(0), index(0)]]`, output 1 → `[[color(0), index(1)]]`, declared even when RT1 is unbound. In `getPipeline` use `masks[regs_raw[0x3e4]&1 ? 0 : i]`.
- **Test:** Metal readback with SRC1_COLOR blending; RT1 mask under the common-mask bit.
- **Risk:** low.

**9. Conditional rendering (Q10, C13).**
- **What:** decode RENDER_ENABLE_A/B/C (`regs_raw[0x554-0x556]`), OVERRIDE (`0x651`) and the GLOBAL_RENDER_ENABLE pair. Evaluate as in Eden `ProcessQueryCondition` by reading the 4-word compare struct from guest memory. Gate `drawVertexArray`, `drawVertexElements` and `clearBuffer` (`E/3d.cpp:317, 438, 496`).
- **Why:** NEVER/FALSE modes must suppress work; otherwise predicated-off geometry appears.
- **Test:** engine unit test with a recording renderer.
- **Risk:** medium. The fake ZPASS counts make CONDITIONAL resolve to "render", which matches the current behavior.

### P2: important features

**10. Feedback loops and PIXEL_SHADER_BARRIER (T2, T3, T4).**
- **What:** add a handler for method 0x378 (and 0x4ce) that ends the current render encoder (`CommandBuffer::endEncoding`). In `configureShaderStage` (`E/3d.cpp:1555+`), detect a texture whose base equals a bound attachment's base, then split the encoder or copy to a scratch texture (Eden `CheckFeedbackLoop`).
- **Test:** draw to an RT, barrier, then sample it in the next draw on the same targets; expect new values.
- **Risk:** medium (extra encoders cost performance on TBDR).

**11. Real occlusion queries (Q5, Q8, B14).**
- **What:** a Metal visibility buffer per command buffer, counting mode between CLEAR_REPORT_VALUE and REPORT_ONLY for ZPASS. Respect SET_ZPASS_PIXEL_COUNT. Write deferred results in the completion handler with a memory-tracking flush. Keep the fake counter as a fallback setting.
- **Why:** pixel-count-driven effects (lens flares, sun visibility) and GPU occlusion culling.
- **Test:** draw N known pixels and check the 64-bit count.
- **Risk:** high (synchronization).

**12. Alpha-to-coverage and alpha-to-one (M3, M4).**
- **What:** decode `regs_raw[0x54f]` into `PipelineDescriptor` and the hash, then set it in `M/pipeline.cpp`.
- **Test:** 1x readback with A2C on an alpha gradient.
- **Risk:** low.

**13. Early fragment tests (F6).**
- **What:** `regs_raw[0x84]` → `GuestShaderState` → `[[early_fragment_tests]]`, skipped when the shader exports depth.
- **Risk:** low.

**14. Logic op through programmable blending (L1).**
- See section (d). Shader variants keyed on (op, RT format) for UNORM/SNORM/INT targets.
- **Test:** XOR/INVERT/AND readback on RGBA8Unorm and R32Uint.
- **Risk:** medium.

**15. MSAA (M1-M8).**
- **What:** sample count end to end (`TextureDescriptor`, storage hash, pipeline `rasterSampleCount`, render pass); TIC `msaa_mode` fetch; 2D-engine resolve (store-action or shader); sample mask, shading and positions.
- **Risk:** high.
- **Fallback:** keep 1x, but make sure MS-texture TIC lookups alias the 1x RT storage consistently.

### P3: polish and robustness

16. **SET_SRGB_WRITE (S2):** linear-view fallback, default 1 as in Eden.
17. **Zeta CONTROL bit (B11), ZT and RT layer offsets (R7, Z5), field widths and bounds (B12).**
18. **Report-semaphore invalidation (B13):** `invalidateMemory(range, BufferCache)` after writes. Also defer FOUR_WORDS fence releases until preceding command buffers complete, if titles stall on early fences.
19. **Z24S8 vs S8Z24 packing (B15):** a distinct TextureFormat or a packing flag; fix the CPU upload and writeback pack/unpack.
20. **Iterated blend (BL9):** Eden's squashed workaround. **PS_SATURATE (F7):** `saturate()` on outputs in the shader key. Depth-bounds one-time warning. Replace `ASSERT_DEBUG(!advanced_blend_enabled)` with a warning.
21. **Equality check in `CacheBase` lookups**, or 64-bit hashes, to remove the silent-collision risk (`src/common/types.hpp:352-364`).
22. **Revisit the `getMinimumWidth` heuristic (R2)** against Eden's literal `ct.width`, with a dynamic-resolution title.

### Test infrastructure notes

- No test in the repo exercises `ThreeD`. A small fake `IRenderer` that records calls would allow engine-level regression tests of method decode for clears, blend, stencil, queries and conditional rendering, without a GPU.
- Metal readback tests must keep running with `MTL_DEBUG_LAYER=1`, as the existing READMEs prescribe. That catches illegal integer blending, missing dual-source outputs and attachment hazards immediately.
