# Hydra GPU audit: 2D engine, DMA copy engine, inline-to-memory, surface layouts

Scope: FERMI_TWOD_A (0x902D), MAXWELL_DMA_COPY_A (0xB0B5), KEPLER_INLINE_TO_MEMORY_B (0xA140) plus the I2M
method block embedded in MAXWELL_B 3D (0xB197) and compute (0xB1C0), the block-linear / pitch surface code they use,
and their coherency with Hydra's texture and buffer caches. Read-only audit of the working tree on branch `arvino`
(2026-10-17, uncommitted changes included). Nothing was built or run.

Paths: Hydra paths are relative to `/Users/arvino/Documents/Project/hydra`.
`REFS=/private/tmp/claude-501/-Users-arvino-Documents-Project-hydra/0e906531-f45a-4e9b-b0bc-dcb303edaa1b/scratchpad/refs`
(`$REFS/opengpudoc/*.h` = NVIDIA class headers, `$REFS/yuzu/src/video_core/...` = Eden).

Classes used below:
- **VERIFIED**: implemented and covered by a test that checks results, or documented as verified in game in `tests/README-*.md`.
- **IMPLEMENTED_UNVERIFIED**: matches the reference semantics (checked by reading), no test.
- **PARTIAL**: some modes missing, approximated, or wrong.
- **MISSING**: not implemented, stubbed, or silently ignored.
- **UNKNOWN**: the references do not settle the expected behaviour.

Where Hydra code lives (relevant to the hot-reload note in the task): the engine bodies are ordinary members in their
.cpp files (`TwoD::copy` in `engines/2d.cpp`, `Copy::launchDma` in `engines/copy.cpp`, `InlineBase::*Impl` in
`engines/inline_base.cpp`). Only the per-engine `methodHot` dispatchers (`DEFINE_METHOD_TABLE`, `engine_base.hpp:10-22`)
and some renderer and texture-cache functions are routed through `src/core/hot/routes.cpp` (`TwoD::method` 762-765,
`Copy::method` 776-779, `Inline::method` 783-786, `Renderer::blitTexture` → `blitTextureHot9` 63-71). The texture cache
policies are in `src/core/hot/texture_cache_hot.cpp`.

---

## (a) Capability table

### A. FERMI_TWOD_A (0x902D), `engines/2d.{hpp,cpp}`

Register model: `Regs2D` (`2d.hpp:65-80`) has `dst` at 0x80, `src` at 0x8C, and `pixels_from_memory` at 0x22C. Everything
else falls in `padding_0x96[0x196]` or past the end of the struct, so it is written but never read. Only method 0x237
(`PIXELS_FROM_MEMORY_SRC_Y0_INT`) runs code (`2d.cpp:10`).

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| T1 | Class binding 0x902D → TwoD | VERIFIED (in game) | `gpu.cpp:61-63` | `cl902d.h:25` | 2D copies verified in game per `tests/README-crash4-render.md:25`. |
| T2 | SET_DST/SRC_FORMAT (V 7:0), common color formats (A8R8G8B8/A8B8G8R8 and sRGB variants, X8 variants, RF16/RF32 vectors, R16G16B16A16, RN16, BF10GF11RF11, A2B10G10R10, R5G6B5, A1R5G5B5, G8R8, RN8, Y8→R8, Y16→R16) | IMPLEMENTED_UNVERIFIED | `2d.hpp:17`, `toTextureFormat(ColorSurfaceFormat)` `renderer/const.cpp:357-428` | `cl902d.h:303-345, 394-437`; yuzu `surface.cpp:100-200` | Only the RGBA8→RGBA16Unorm conversion is exercised (renderer level, `tests/texture_cache_tests.cpp:862-920`). The format field is stored as a full u32; unknown values hit `LOG_FATAL` (`const.cpp:427`). |
| T3 | A2R10G10B10 (0xDF) | PARTIAL | `const.cpp:395` maps `BGR10A2Unorm` to `RGB10A2Unorm` with a "TODO: correct?" comment | `cl902d.h:307`; yuzu keeps a separate `A2R10G10B10_UNORM` (`surface.cpp:139-140`) | R and B are swapped whenever 0xDF is converted to or from another format. A 0xDF→0xDF copy is raw, so it is unaffected. Bug B11. |
| T4 | A8 (0xF7), X1R5G5B5 (0xF8), Z1R5G5B5/O1R5G5B5 (0xFB/0xFC), Z8R8G8B8/O8R8G8B8 (0xFD/0xFE), Y32 (0xFF), Y1_8X8 (0x1C), AY8 (0x1D) | MISSING | `const.cpp:364-365, 417-425` all → `TextureFormat::Invalid` | `cl902d.h:317-325, 344, 410`; yuzu maps X1R5G5B5→A1R5G5B5 (`surface.cpp:174-175`) | An Invalid format produces a zero-size descriptor and then fails the texture creation ASSERT (`metal/texture.cpp:82-84`), which is fatal. Bug B11. |
| T5 | Y8/Y16 luminance semantics (replicate Y into RGB when converting) | UNKNOWN | Y8→R8Unorm, Y16→R16Unorm (`const.cpp`) | `cl902d.h:318-319` (names only) | Hydra converts as R-only: (Y,0,0,1). The header does not define the conversion. |
| T6 | SET_*_MEMORY_LAYOUT (0 = BLOCKLINEAR, 1 = PITCH) | VERIFIED | `2d.hpp:18`; `getTextureDescriptor` `2d.cpp:154-163` | `cl902d.h:346-349, 438-441` | Pitch and tiled decode of the same address are tested (`texture_cache_tests.cpp:76-123`). Pitch 2D destinations verified in game (README:25). |
| T7 | SET_*_BLOCK_SIZE HEIGHT (6:4), DEPTH (10:8) | IMPLEMENTED_UNVERIFIED | bitfield `2d.hpp:19-25` (bits 4-6 and 8-10, checked) | `cl902d.h:351-366, 443-458` | Bit positions match. The texture cache then clamps the programmed block size for small surfaces (S6). |
| T8 | SET_*_DEPTH, SET_DST_LAYER | PARTIAL | `2d.cpp:161` passes `std::max(info.depth, 1u)` as **layer_count** of a `_2D` descriptor; `dst.layer` is only used as the render-target array index (`2d.cpp:66-69`, `renderer.cpp:343`) | `cl902d.h:367-372, 459`; yuzu also only supports depth == 1 and layer == 0 (`fermi_2d.cpp:65-67`) | DEPTH > 1 requests a multi-layer `_2D` texture. Metal only has an ASSERT_DEBUG check (`metal/texture.cpp:43-46`) and the view creation is invalid (`metal/texture_view.cpp:42-46`), so this can crash. Bug B10. |
| T9 | 0x240 TWOD_INVALIDATE_TEXTURE_DATA_CACHE (V 1:0: L1_ONLY/L2_ONLY/L1_AND_L2) | PARTIAL (misdecoded) | the `src` member is a `Texture2DInfo`, so 0x240 is read as `src.layer` (`2d.hpp:26-27, 74`) and passed as `src_layer` (`2d.cpp:66, 97`) | `cl902d.h:462-466`; the class has no SET_SRC_LAYER. yuzu has the same naming (`fermi_2d.h:94`) and asserts the value is 0 (`fermi_2d.cpp:65`) | Harmless while the value is 0. Values 1 or 2 break the copy: the depth path uses it as the Metal slice, the native path is rejected, and the compressed-alias path is skipped. Bug B9. |
| T10 | SET_*_PITCH / WIDTH / HEIGHT | IMPLEMENTED_UNVERIFIED | `2d.cpp:157-161` | `cl902d.h:373-381, 468-476` | Pitch surfaces whose width×bpp exceeds the pitch over-read guest memory in `decodeTexture` (`texture_cache.cpp:736-746`). yuzu has a workaround for this case (`fermi_2d.cpp:99-110`). |
| T11 | SET_*_OFFSET_UPPER (7:0) / LOWER | IMPLEMENTED_UNVERIFIED | `Iova` (`engines/const.hpp:7-15`) | `cl902d.h:382-387, 477-482` | The upper word is not masked to 8 bits. yuzu masks it for DMA (`maxwell_dma.h:108`). Minor. |
| T12 | NO_OPERATION, WAIT_FOR_IDLE, PM_TRIGGER, FLUSH_AND_INVALIDATE_ROP_MINI_CACHE | IMPLEMENTED_UNVERIFIED (no-op) | `writeReg` default (`engine_base.hpp:18-20`) | `cl902d.h:234, 248, 287, 388` | Ignoring these is correct for an in-order emulator. |
| T13 | PIXELS_FROM_MEMORY_SRC_Y0_INT starts the blit | VERIFIED (in game) | `DEFINE_METHOD_TABLE(TwoD, 0x237, …)` `2d.cpp:10, 15-17` | `cl902d.h:980`; yuzu `fermi_2d.cpp:43-45` | |
| T14 | PIXELS_FROM_MEMORY DST_X0/Y0/WIDTH/HEIGHT | VERIFIED (renderer level) | `2d.hpp:54-58`, `2d.cpp:67-69` | `cl902d.h:947-957`; yuzu uses s32 (`fermi_2d.h:280-283`) | The renderer subrectangle test (`texture_cache_tests.cpp:825-860`) checks the origin and size. Hydra stores u32, so negative values are not supported. |
| T15 | DU_DX / DV_DY (32.32, FRAC + INT) | PARTIAL | `FixedPoint`→f64 (`2d.hpp:34-41`); the source extent is truncated: `src_width = static_cast<u32>(pixels.dst_width * dudx)` (`2d.cpp:60-61`) | `cl902d.h:959-969`; yuzu `fermi_2d.cpp:95-96` truncates the same way, its sw blitter rounds (`blitter.cpp:33`) | The effective scale is src_width/dst_width rather than du/dx: one source column is dropped whenever dst×du/dx is not integral. The integer part is u32, so negative (mirrored) steps are not supported. The renderer test uses a 1×1 source, which cannot detect scale errors. Bugs B7, B8. |
| T16 | SRC_X0 / SRC_Y0 (32.32) | PARTIAL | `2d.cpp:40-41, 65`; fractional offsets only take effect in the shader path (`renderer.cpp:346-352`) | `cl902d.h:971-981`; yuzu s64 (`fermi_2d.h:286-287`) | Integral offsets are tested (`texture_cache_tests.cpp:839-840`). Negative values are misdecoded (u32 integer part). Bug B8. |
| T17 | SAMPLE_MODE ORIGIN (CENTER/CORNER, bit 0) | MISSING | 0x88C (index 0x223) falls in `padding_0x96` (`2d.hpp:76`); never read | `cl902d.h:939-942`; yuzu converts CORNER to CENTER (`fermi_2d.cpp:79-82`) | Hydra always samples at CENTER: src_x0 + (i+0.5)·du. A CORNER source is shifted by du/2. Bug B6. |
| T18 | SAMPLE_MODE FILTER (POINT/BILINEAR, bit 4) | MISSING | `renderer.cpp:356-358` always binds `linear_sampler` ("TODO: use the correct sampler"); `nearest_sampler` already exists (`renderer.cpp:74-76`) | `cl902d.h:943-945`; yuzu picks the sampler from the filter (`blit_image.cpp:665-673`) | Point-filtered scaled copies come out bilinear (blurred). 1:1 copies use the native copy, which is unaffected. Bug B6. |
| T19 | Overlapping copies (SAFE_OVERLAP, DIRECTION) | MISSING | if src and dst resolve to the same cached view, the native path is skipped (`src_mtl != dst_mtl`, `renderer.cpp:277`) and the shader path samples the attachment it renders to | `cl902d.h:629-638, 934-937` | Undefined in Metal. Only affects in-place copies within one surface. |
| T20 | BLOCK_SHAPE, CORRAL_SIZE, SECTOR_PROMOTION, NUM_PROCESSING_CLUSTERS, SET_COMPRESSION | IMPLEMENTED_UNVERIFIED (ignored) | padding | `cl902d.h:483-497, 615-618, 925-933` | Performance and storage hints with no visible effect. |
| T21 | SET_CLIP_X0/Y0/WIDTH/HEIGHT/ENABLE | MISSING | padding | `cl902d.h:524-540`; yuzu: UNIMPLEMENTED (`fermi_2d.cpp:68`) | |
| T22 | SET_OPERATION = SRCCOPY | VERIFIED (in game) | implicit (SET_OPERATION is never read) | `cl902d.h:571-580` | Hydra copies regardless of the programmed operation. |
| T23 | SET_OPERATION SRCCOPY_AND, ROP_AND, BLEND_AND, ROP, SRCCOPY_PREMULT, BLEND_PREMULT | MISSING | not modeled | `cl902d.h:574-580`; yuzu asserts SrcCopy (`fermi_2d.cpp:64`; `vk_texture_cache.cpp:1313`) | |
| T24 | SET_ROP, SET_BETA1, SET_BETA4 | MISSING | padding | `cl902d.h:559-570` | Only meaningful with ROP or BLEND operations. |
| T25 | Color key (SET_COLOR_KEY_FORMAT / COLOR_KEY / ENABLE) | MISSING | padding | `cl902d.h:541-558` | |
| T26 | Patterns (SET_PATTERN_OFFSET/SELECT, MONOCHROME_PATTERN_*, COLOR_PATTERN_*) | MISSING | padding | `cl902d.h:581-590, 642-698` | |
| T27 | Solid primitives (RENDER_SOLID_PRIM_MODE points/lines/polyline/triangles/rects, COLOR_FORMAT, COLOR, COLOR0-3, LINE_TIE_BREAK_BITS, PRIM_POINT_X_Y, PRIM_POINT_SET_X/Y) | MISSING | padding | `cl902d.h:699-808` | yuzu does not implement these either. |
| T28 | Pixels from CPU (SET_PIXELS_FROM_CPU_*, PIXELS_FROM_CPU_DATA) and SET_BIG_ENDIAN_CONTROL | MISSING | padding | `cl902d.h:809-923` | yuzu does not implement these either. |
| T29 | SET_DST_COLOR_RENDER_TO_ZETA_SURFACE and depth surfaces copied as color | PARTIAL | heuristic instead of the register: `findDepthStencilAlias` (`texture_cache.cpp:95-120`) forces the depth format on **both** src and dst (`2d.cpp:24-36`); depth blit without scaling (`renderer.cpp:232-268`) | `cl902d.h:592-595`; yuzu depth blits (`texture_cache.h:1760-1803`, `vk_texture_cache.cpp:1288-1308`) | depth→color, color→depth, and scaled depth copies are wrong. |
| T30 | Render enable (SET_RENDER_ENABLE_A/B/C, OVERRIDE, SET_GLOBAL_RENDER_ENABLE_*) | MISSING | padding | `cl902d.h:270-283, 498-511, 623-628` | Copies always execute. |
| T31 | Notify (SET_NOTIFY_A/B, NOTIFY) | MISSING | padding | `cl902d.h:237-246` | |
| T32 | MME on the 2D subchannel (LOAD_MME_*, CallMme at ≥ 0xE00) | MISSING (fatal) | TwoD has no `macro()`, so `EngineBase::macro` calls LOG_FATAL (`engine_base.hpp:41-46`) | `cl902d.h:222-227` (CallMme[0xE0]) | Fatal. Unlikely to be used by games. |
| T33 | Same-format 1:1 copy | VERIFIED | native `copyFromTexture` (`renderer.cpp:277-317`) | yuzu `vk_texture_cache.cpp:1264-1400` | `texture_cache_tests.cpp:825-860` checks raw RGBA and untouched neighbours. |
| T34 | Scaled or format-converting copy, normalized/float formats | VERIFIED (limited) | shader path (`renderer.cpp:319-362`, `blit_pipeline_cache.cpp:52-118`) | yuzu sw blitter IR conversion (`blitter.cpp:176-199`) | The test (`texture_cache_tests.cpp:862-920`) only covers a 1×1→2×2 RGBA8→RGBA8/RGBA16Unorm copy. Positions and filtering are not covered. |
| T35 | Scaled or converting copy, **integer** formats | PARTIAL (broken) | the fragment shader is `texture2d<float>` and returns `float4` (`blit_pipeline_cache.cpp:69-74`) | Metal requires uint/sint texture and output types for integer formats | Invalid pipeline or sampling whenever the native path is rejected. Bug B5. |
| T36 | sRGB conversions (A8RL8GL8BL8 etc.) | IMPLEMENTED_UNVERIFIED | Metal sRGB views (`metal/texture_view.cpp:42-46`), created with PixelFormatView usage (`metal/texture.cpp:75-78`) | `cl902d.h:307-314` | |
| T37 | MSAA resolve through 2D (source addressed in sample space) | MISSING | there is no MSAA anywhere in the GPU path | yuzu shifts by the sample count (`texture_cache.h:756-781`) and has a resolve path (`vk_texture_cache.cpp:1279-1330`) | Depends on 3D MSAA support. |
| T38 | Compressed-block aliases (BCn/ASTC copied as RG32/RGBA32) mirrored into guest memory | VERIFIED (in game) | `2d.cpp:85-100`, `copyGuestMemory` `2d.cpp:108-150` | n/a (Hydra-specific workaround) | README:25. Also corrupts genuine GPU-rendered float32 copies (bug B3). |
| T39 | Readback of pitch and tiny float32 destinations (flush right after the copy) | VERIFIED (in game) | `2d.cpp:71-84, 102-105` | n/a | README:25. Each such copy waits synchronously for the GPU (`texture_cache_hot.cpp:442`). |
| T40 | Register file bounds | PARTIAL (bug) | `REG_COUNT` = 0x238; `writeReg` is unchecked outside Debug builds (`engine_base.hpp:66-71`, `log.hpp:74`) | `cl902d.h:194-227` defines methods up to 0x3FFC | Methods ≥ 0x238 (for example SET_FALCON00 at 0x8E0) overwrite `TwoD::gpu`. Bug B1. |

### B. MAXWELL_DMA_COPY_A (0xB0B5), `engines/copy.{hpp,cpp}`

Register model `RegsCopy` (`copy.hpp:75-120`): LAUNCH_DMA at 0xC0, OFFSET_IN/OUT through LINE_COUNT at 0x100-0x107,
REMAP at 0x1C0-0x1C2, DST params at 0x1C3, SRC params at 0x1CA. The bit layouts of `LaunchDMAData` (`copy.hpp:133-149`),
`remap_components` (`copy.hpp:95-111`) and `BlockSize` (`copy.hpp:55-61`) were checked against
`clb0b5.h:66-121, 142-189, 190-242` and match. Semaphore (0x240-0x248), render enable (0x254-0x25C) and phys mode
(0x260-0x264) fall in `padding_0x0`.

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| D1 | Class binding 0xB0B5; LAUNCH_DMA (0x300) triggers the copy | VERIFIED | `gpu.cpp:64-66`, `copy.cpp:10` | `clb0b5.h:33, 66`; yuzu `maxwell_dma.cpp:49-51` | `tests/xci_tests.cpp:203-242`. |
| D2 | DATA_TRANSFER_TYPE (NONE / PIPELINED / NON_PIPELINED, 1:0) | PARTIAL | `data.type` is never read (`copy.cpp:142-344`) | `clb0b5.h:67-70` | NONE (semaphore-only launch) still performs a copy using stale registers. yuzu also ignores it. Bug B12. |
| D3 | FLUSH_ENABLE, BYPASS_L2, FORCE_RMWDISABLE | IMPLEMENTED_UNVERIFIED (no-op) | fields declared (`copy.hpp:135, 142, 148`) | `clb0b5.h:71-73, 94-96, 119-121` | Writes are immediate on the CPU, so ignoring these is correct. |
| D4 | SEMAPHORE_TYPE (NONE / ONE_WORD / FOUR_WORD) + SET_SEMAPHORE_A/B/PAYLOAD | MISSING | field declared (`copy.hpp:136`) and never used; registers are not modeled | `clb0b5.h:39-44, 74-77`; yuzu `ReleaseSemaphore` (`maxwell_dma.cpp:347-368`) | A waiter on that semaphore never sees the payload. Bug B12. |
| D5 | SEMAPHORE_REDUCTION / SIGN / ENABLE | MISSING | fields declared (`copy.hpp:145-147`) | `clb0b5.h:103-118` | yuzu does not implement reductions either. |
| D6 | INTERRUPT_TYPE | MISSING | field declared | `clb0b5.h:78-81`; yuzu asserts NONE (`maxwell_dma.cpp:65`) | Low impact. |
| D7 | SRC_TYPE/DST_TYPE PHYSICAL + SET_SRC/DST_PHYS_MODE | MISSING (n/a) | fields declared | `clb0b5.h:56-65, 97-102` | Not reachable from Switch user mode. |
| D8 | OFFSET_IN/OUT (UPPER 7:0) | VERIFIED | `copy.hpp:83-84` | `clb0b5.h:122-129`; yuzu masks the upper word (`maxwell_dma.h:107-109`) | Hydra does not mask the upper word. |
| D9 | PITCH_IN/OUT, LINE_LENGTH_IN, LINE_COUNT (pitch) | VERIFIED | `copy.hpp:85-88`, `copy.cpp:275-302` | `clb0b5.h:130-137` | Padded-row case tested (`xci_tests.cpp:229-238`). |
| D10 | MULTI_LINE_ENABLE, pitch→pitch, both values | VERIFIED | `copy.cpp:275` | `clb0b5.h:88-90`; yuzu `maxwell_dma.cpp:78-85, 142-149` | `xci_tests.cpp:212-238`. |
| D11 | Pitch→pitch across separate GMMU or host mappings | VERIFIED | chunking with `findAddrImpl` (`copy.cpp:282-302`) | yuzu `memory_manager.CopyBlock` | `xci_tests.cpp:170-242`, scattered backing. |
| D12 | Overlapping pitch→pitch (src range intersects dst range) | PARTIAL | `memcpy` (`copy.cpp:293-294`) | – | memcpy on overlapping ranges is undefined. The remap path copies through a temporary and is safe (`copy.cpp:107-111`). Bug B16. |
| D13 | Pitch→block-linear | IMPLEMENTED_UNVERIFIED | `copySubrect(..., true)` (`copy.cpp:16-41, 305-311`) | yuzu `CopyPitchToBlockLinear` (`maxwell_dma.cpp:225-288`), `SwizzleSubrect` (`decoders.cpp:95-153`) | The offset function is tested (S1-S3). The engine path has no test. |
| D14 | Block-linear→pitch | IMPLEMENTED_UNVERIFIED | `copySubrect(..., false)` (`copy.cpp:313-318`) | yuzu `CopyBlockLinearToPitch` (`maxwell_dma.cpp:156-223`) | The source flush is tested (D26). |
| D15 | Block-linear→block-linear | MISSING | `LOG_NOT_IMPLEMENTED(Engines, "BlockLinear to BlockLinear")` (`copy.cpp:319-321`) | yuzu `CopyBlockLinearToBlockLinear` (`maxwell_dma.cpp:290-345`) | Bug B13. |
| D16 | MULTI_LINE_ENABLE = 0 with a block-linear layout | UNKNOWN | Hydra copies one line at (origin.x, origin.y) (`copy.cpp:310, 318`) | yuzu ignores the layout bits, checks the PTE kind, and applies a GOB-local address swizzle (`maxwell_dma.cpp:107-141`) | The two implementations disagree and the header does not decide it. Needs a hardware trace. |
| D17 | SET_*_BLOCK_SIZE HEIGHT (7:4) / DEPTH (11:8) | IMPLEMENTED_UNVERIFIED | `copy.cpp:19-22` (sign-extended log2, clamped ≥ 0) | `clb0b5.h:194-207, 226-239` | The raw block height is used here, while the texture cache clamps it (S6). The two can disagree. |
| D18 | BLOCK_SIZE WIDTH (QUARTER_GOB/ONE_GOB) and GOB_HEIGHT (TESLA_4/FERMI_8) | MISSING (n/a on GM20B) | ignored | `clb0b5.h:191-193, 208-210` | Maxwell uses ONE_GOB and FERMI_8. |
| D19 | SET_*_WIDTH / HEIGHT / DEPTH | IMPLEMENTED_UNVERIFIED | `copy.hpp:63-73` (`stride` holds SET_*_WIDTH) | `clb0b5.h:211-216, 243-248` | Width is in bytes, or in elements when remap is enabled (`copy.cpp:161-173`), which matches yuzu's base_bpp (`maxwell_dma.cpp:187-205`). |
| D20 | SET_*_ORIGIN X (15:0) / Y (31:16) | IMPLEMENTED_UNVERIFIED | `copy.cpp:23-25` | `clb0b5.h:219-221, 251-253` | Byte offsets are tested (`texture_cache_tests.cpp:1057-1083`). `copySubrect` itself is not. |
| D21 | SET_*_LAYER | PARTIAL | used as the Z slice of a 3D block-linear surface (`copy.cpp:32`) | `clb0b5.h:217-218, 249-250`; yuzu asserts layer == 0 for pitch→BL (`maxwell_dma.cpp:227`) | Unclear whether NVN uses LAYER for array layers. For mip-mapped arrays the layer stride is not the 2D slice size. |
| D22 | Copies spanning several slices (LINE_COUNT > height − origin.y) and clamping to the surface height | MISSING | y keeps growing past the height while z stays fixed (`copy.cpp:24-26`); TODO comments at `copy.cpp:306, 314` | yuzu clamps each slice to the height and continues in the next Z slice (`decoders.cpp:116-153`) | Writes land outside the surface. Relevant to 3D textures such as UE4 volumetric lightmaps. Bug B15. |
| D23 | REMAP pitch→pitch: DST_X/Y/Z/W = SRC_X..W / CONST_A / CONST_B / NO_WRITE, COMPONENT_SIZE 1-4, NUM_SRC/DST_COMPONENTS | IMPLEMENTED_UNVERIFIED | `remapPitchCopy` (`copy.cpp:48-140`) | `clb0b5.h:138-189`; yuzu only handles a 1-D CONST_A fill (`maxwell_dma.cpp:96-105`) | More general than yuzu: overlap-safe, multi-line, discards GPU writes on a full overwrite. No tests. |
| D24 | REMAP with a block-linear layout | PARTIAL | only the identity swizzle is supported; others only trigger an ASSERT_DEBUG, which compiles out in Release (`copy.cpp:154-159`) | `clb0b5.h:142-189` | A constant fill or swizzle into a block-linear surface copies source bytes instead. Bug B14. |
| D25 | SET_RENDER_ENABLE_A/B/C (conditional copy) | MISSING | not modeled | `clb0b5.h:45-55` | |
| D26 | Source coherency: GPU-rendered textures written back before the CPU reads | VERIFIED | `flushMemory(src_range)` (`copy.cpp:266-271`) | yuzu `ImageToBuffer` acceleration (`texture_cache.h:1044-1086`) | `texture_cache_tests.cpp:974-1007` (UE4 1×1 RGBA32F readback); README-crash4-render.md:19. |
| D27 | Destination coherency | PARTIAL | pitch flush/invalidate size is `line_count * stride_out` regardless of MULTI_LINE_ENABLE (`copy.cpp:236-237, 325-330`); block-linear destinations only invalidate the TextureCache scope (`copy.cpp:339-342`); partial overlaps with GPU-dirty storages (S9) | yuzu `DmaBufferImageCopy` / `PrepareDmaImage` (`texture_cache.h:1044-1086, 1880-1889`) | 1-D copies never flush GPU-written destination aliases, and single-line BL→pitch copies do not invalidate the buffer cache. Bug B4. |
| D28 | Readback of depth/stencil and 3D textures | MISSING | `encodeTextureImpl` returns early for these (`texture_cache_hot.cpp:389-394`) | yuzu downloads every image type | DMA reads stale guest bytes. Bug B17. |
| D29 | Block-linear paths: mapping splits and bounds | PARTIAL | one `unmapAddr` pointer per surface (`copy.cpp:211-212`) with no size check | yuzu `GpuGuestMemory` handles discontiguous mappings | Assumes the whole surface is one contiguous host mapping. |

### C. Inline-to-memory: KEPLER_INLINE_TO_MEMORY_B (0xA140), 3D 0xB197 and compute 0xB1C0 I2M blocks, `engines/inline*.{hpp,cpp}`

`RegsInline` (`inline_base.hpp:21-38`) models 0x180-0x1B4 (indices 0x60-0x6D). 3D and compute embed it at the same
offsets (`3d.hpp:212-213`, `compute.hpp:9-10`), and all three dispatch 0x6C (LAUNCH_DMA) and 0x6D (LOAD_INLINE_DATA)
(`inline_base.hpp:5-13`; `3d.cpp:224`, `compute.cpp:22`, `inline.cpp:5`).

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| I1 | Class binding 0xA140 and the I2M methods of 3D/compute | IMPLEMENTED_UNVERIFIED | `gpu.cpp:58-60`; method tables above | `clb197.h:109-205`, `clb1c0.h:83-160`; yuzu register positions `kepler_memory.h:86-88` | No tests (`tests/README-hot-debug.md:40` only lists inline uploads as hot-reloadable). |
| I2 | LINE_LENGTH_IN / LINE_COUNT | PARTIAL | only their product is used, as the exact-match flush condition (`inline_base.cpp:22-24`) | `clb197.h:109-113`; yuzu `copy_size = line_length_in * line_count` (`engine_upload.cpp:28-33`) | Bug B2. |
| I3 | OFFSET_OUT_UPPER / OFFSET_OUT | IMPLEMENTED_UNVERIFIED | `inline_base.cpp:29` | `clb197.h:115-119` | |
| I4 | PITCH_OUT (multi-line pitch destination) | MISSING | the whole payload is written contiguously (`inline_base.cpp:28-40`) | `clb197.h:121-122`; yuzu writes each line at `address + line * pitch` (`engine_upload.cpp:52-58`) | Bug B2. |
| I5 | Block-linear destination: SET_DST_BLOCK_SIZE / WIDTH / HEIGHT / DEPTH / LAYER / ORIGIN_BYTES_X / ORIGIN_SAMPLES_Y | MISSING | registers are stored but never read | `clb197.h:124-158`; yuzu `SwizzleSubrect` path (`engine_upload.cpp:59-82`) | A block-linear upload is written linearly over the texture start. Bug B2. |
| I6 | LAUNCH_DMA DST_MEMORY_LAYOUT (bit 0) | MISSING | `launchDmaImpl` is a stub (`inline_base.cpp:11-16`) | `clb197.h:160-163`; yuzu `maxwell_3d.cpp:358`, `kepler_memory.cpp:44-46` | |
| I7 | LAUNCH_DMA starts a transfer (resets the buffer, latches the size) | MISSING | `inline_data` is never reset at LAUNCH_DMA | yuzu `ProcessExec` (`engine_upload.cpp:28-33`) | Bug B2. |
| I8 | LOAD_INLINE_DATA accumulation | PARTIAL | appends words until `size*4 == line_length*line_count` (`inline_base.cpp:20-24`) | yuzu copies `min(4, copy_size - write_offset)` bytes per word and flushes on the last call (`engine_upload.cpp:35-43`) | Sizes that are not a multiple of 4 never flush and corrupt every later transfer. Bug B2. |
| I9 | COMPLETION_TYPE (FLUSH_DISABLE / FLUSH_ONLY / RELEASE_SEMAPHORE), SET_I2M_SEMAPHORE_A/B/C, SEMAPHORE_STRUCT_SIZE, REDUCTION_ENABLE/OP/FORMAT | MISSING | not modeled | `clb197.h:164-203` | |
| I10 | INTERRUPT_TYPE, SYSMEMBAR_DISABLE | MISSING (low impact) | not modeled | `clb197.h:168-170, 189-191` | |
| I11 | Cache coherency: flush GPU textures before the write, invalidate buffer and texture caches after | IMPLEMENTED_UNVERIFIED | `inline_base.cpp:33-45` | yuzu `AccelerateInlineToMemory` | Correct for pitch destinations; block-linear destinations are not supported (I5). |
| I12 | Destination spanning several mappings | PARTIAL | a single `unmapAddr` followed by one `memcpy` (`inline_base.cpp:29, 40`) | – | |
| I13 | A140 register file bounds | PARTIAL (bug) | `RegsInline_` is only 0x6E words (`inline.hpp:7-9`); `writeReg` is unchecked in Release | I2M semaphores at 0x1DC-0x1E4 (`clb197.h:196-203`) | An overflow lands in `InlineBase::inline_data`, `Inline::gpu`, then the vtable of `two_d_engine` (`gpu.hpp:193-194`). Bug B1. |

### D. Surface layouts and texture-cache coherency (shared by all three engines)

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| S1 | GOB swizzle (64 B × 8 rows, 16 B sectors: x%16, bit5←x bit4, bit8←x bit5, bit4←y bit0, bits6-7←y bits1-2) | VERIFIED | `getBlockLinearOffset` (`memory_util.cpp:194-211`), `unswizzleGobCoords` (`memory_util.cpp:40-44`) | yuzu `SWIZZLE_X_BITS`/`SWIZZLE_Y_BITS` (`decoders.h:26-27`), `SwizzleImpl` (`decoders.cpp:52-79`) | Checked analytically against yuzu. Tests (`texture_cache_tests.cpp:1057-1126`) compare two Hydra implementations against each other, not against an independent reference. |
| S2 | Block height 1-32 GOBs; block, row and slice sizes | VERIFIED | `memory_util.cpp:196-208` | yuzu `decoders.cpp:52-71` | Identical block_size and slice_size formulas. |
| S3 | Block depth (3D), with a partial last Z block | VERIFIED | `getMemoryLayout` (`memory_util.cpp:21-38`), converters (`memory_util.cpp:48-167`) | yuzu `CalculateSize` (`decoders.cpp:252-262`) | README-crash4-render.md:22 (UE4 volumetric lightmap). |
| S4 | Pitch-linear decode/encode, partial readback that preserves neighbouring bytes and padding | VERIFIED | `texture_cache.cpp:736-746`; `texture_cache_hot.cpp:447-534` | – | `texture_cache_tests.cpp:921-971`. |
| S5 | Base-level tile width spacing (block width) when decoding | IMPLEMENTED_UNVERIFIED | `texture_cache.cpp:772-777` | yuzu `GobSize` / tile_width_spacing (`util.cpp:188-193`) | Only reachable from TIC descriptors; 2D and DMA always pass 0. |
| S6 | Block-size clamping for single-level level-0 surfaces | PARTIAL | `getLevelBlockSizeLog2` always clamps (`const.cpp:773-792`), even for 2D/RT surfaces with `level_count == 1` | yuzu `TileShift` keeps the raw block size when `level == 0 && num_levels == 1` (`util.cpp:172-179`) | If a surface is programmed with an oversized block height, Hydra's decode disagrees with the hardware and with the raw values used by DMA (`copy.cpp:19-22`) and `copyGuestMemory` (`2d.cpp:123-125`). Impact unknown; usually NVN programs the minimal size. |
| S7 | Layer stride alignment | VERIFIED | `alignLayerSize` (`const.cpp:821-840`) | yuzu layer stride | "nonzero mip/layer alias synchronization" test (`texture_cache_tests.cpp:~125-267`). |
| S8 | Writeback of depth/stencil and 3D storages | MISSING | `texture_cache_hot.cpp:389-394` | – | Affects DMA readbacks (D28) and the CPU fault path. |
| S9 | CPU/DMA/I2M writes that partially overlap a GPU-dirty storage | PARTIAL | a partial flush does not mark the storage flushed (`texture_cache_hot.cpp:395-398`); invalidation skips dirty storages, so they are never marked CPU-modified (`texture_cache_hot.cpp:103-106`); with stage-2 write protection `updateImpl` also refuses to reload them (`texture_cache_hot.cpp:147-150, 170-173`) | yuzu downloads the image or uses the `DmaBufferImageCopy` GPU path | The write is lost on the host, and a later full flush overwrites the new guest bytes with the old GPU data. Bug B18. |
| S10 | Mixed buffer/2D byte sync kernel (pitch and tiled) | VERIFIED | `metal/texture.cpp:100-239` | – | `texture_cache_tests.cpp:405-479`. README-crash4-render.md "Scaled color copy alpha" records that the complete executable passed on 2026-10-17. |

**Counts: 92 capabilities.** VERIFIED 21, IMPLEMENTED_UNVERIFIED 18, PARTIAL 20, MISSING 31, UNKNOWN 2.

| Section | VERIFIED | IMPL_UNVERIFIED | PARTIAL | MISSING | UNKNOWN |
|---|---|---|---|---|---|
| 2D (40) | 9 | 7 | 8 | 15 | 1 |
| DMA (29) | 6 | 7 | 6 | 9 | 1 |
| I2M (13) | 0 | 3 | 4 | 6 | 0 |
| Layouts/coherency (10) | 6 | 1 | 2 | 1 | 0 |

---

## (b) Confirmed bugs (from reading the code, with exact quotes)

Severity: **S1** can crash or corrupt memory; **S2** produces wrong results on a realistic path; **S3** edge case or
performance.

### B1 (S1). `writeReg` has no bounds check outside Debug builds; the Inline engine register file ends at 0x6D

`engine_base.hpp:66-71`:
```cpp
    void writeReg(u32 reg, u32 value) {
        ASSERT_DEBUG(reg < REG_COUNT, Engines, "Invalid reg 0x{:08x}", reg);
        ...
        regs_raw[reg] = value;
```
`common/log.hpp:68-75`: `#ifdef HYDRA_DEBUG` … `#else` … `#define ASSERT_DEBUG(condition, c, ...) (void)(condition)`.
`CMakeLists.txt:253-255` only defines `HYDRA_DEBUG` for `CMAKE_BUILD_TYPE == "Debug"`, so the documented
`build/swiftui-release` build has no check.

Register file sizes:
- Inline (A140): `RegsInline_ { RegsInline regs_inline; }` is 0x6E words (`inline.hpp:7-9`, `inline_base.hpp:21-38`).
- TwoD: 0x238 words (`2d.hpp:65-80`).
- Copy: 0x446 words, which covers the whole class (last method is PM_TRIGGER_END at 0x1114).

Every method from REG_COUNT up to 0xDFF (the macro region starts at 0xE00, `gpu/const.hpp:749`) writes past `regs_raw`.

Overflow targets:
- Inline: `regs_raw[0x6E..0x73]` overlaps the `InlineBase::inline_data` vector, `[0x74..0x75]` overlaps `Inline::gpu`, and
  `[0x76..]` reaches the next `Gpu` member, `two_d_engine` (`gpu.hpp:193-194`), starting with its vtable pointer.
  The I2M completion-semaphore methods SET_I2M_SEMAPHORE_A/B/C sit at 0x1DC-0x1E4, indices 0x77-0x79, in every class
  that embeds the I2M block (`clb197.h:196-203`, `clb1c0.h:170`, `cla0c0.h:170`, `cla097.h:196`). The standalone A140
  header is not in the reference set, but it shares that block. One of these methods on the A140 subchannel would
  overwrite the high half of the 2D engine's vtable pointer.
- TwoD: `regs_raw[0x238]` aliases `TwoD::gpu`. SET_FALCON00 is at 0x8E0 (`cl902d.h:182-218`).

**Fix:** keep the check in every build type: `if (reg >= REG_COUNT) { ONCE(LOG_WARN(...)); return; }`. Better, size each
register struct to the full class method space (0xE00 words) so the 2D sample mode, clip, operation and semaphore
registers can be modeled later.

### B2 (S1/S2). I2M flushes only on an exact byte count and never resets at LAUNCH_DMA; it ignores pitch and block-linear layouts

`inline_base.cpp:11-16` and `18-40`:
```cpp
void InlineBase::launchDmaImpl(Gpu& gpu, RegsInline& regs, const u32 index,
                               const u32 data) {
    (void)this;
    ONCE(LOG_FUNC_WITH_ARGS_STUBBED(Engines, "index: {}, data: {:#x}", index,
                                    data));
}
...
    inline_data.push_back(data);
    // TODO: correct?
    if (inline_data.size() * sizeof(u32) ==
        static_cast<usize>(regs.line_length_in) *
            static_cast<usize>(regs.line_count)) {
        ...
        // Buffer to buffer
        uptr dst_ptr = tls_crnt_gmmu->unmapAddr(regs.offset_out);
        ...
        memcpy(reinterpret_cast<void*>(dst_ptr), inline_data.data(), size);
```
1. **Sizes that are not a multiple of 4.** If `line_length_in * line_count` is not a multiple of 4 (for example a
   6-byte upload), the condition is never true. The leftover words stay queued, so every later transfer on that engine
   is misaligned: its first words come from the previous transfer and its last words spill into the next one. A later
   transfer smaller than the backlog never flushes at all. I2M on that engine (3D, compute or A140) does not recover.
   yuzu copies `min(4, copy_size - write_offset)` bytes per word (`engine_upload.cpp:35-43`).
2. **No reset at LAUNCH_DMA.** yuzu resets in `ProcessExec` (`engine_upload.cpp:28-33`).
3. **Multi-line uploads ignore `pitch_out`.** yuzu writes per line (`engine_upload.cpp:52-58`).
4. **Block-linear destinations are written linearly at the surface base.** yuzu swizzles them (`engine_upload.cpp:59-82`).
5. **The destination is assumed to be one host mapping.** A single `unmapAddr` is followed by one `memcpy`.

### B3 (S2). 2D compressed-alias workaround discards GPU-rendered RGBA32F/RG32F/RGBA32UI/RG32UI copies

`2d.cpp:91-100`:
```cpp
    const bool block_alias = format == renderer::TextureFormat::RG32Float ||
                             format == renderer::TextureFormat::RG32Uint ||
                             format == renderer::TextureFormat::RGBA32Float ||
                             format == renderer::TextureFormat::RGBA32Uint;
    if (!depth_stencil_format && regs.src.format == regs.dst.format &&
        block_alias && regs.src.layer == 0 && regs.dst.layer == 0 &&
        dudx == 1.0 && dvdy == 1.0 && (bpp == 8 || bpp == 16))
        copyGuestMemory(bpp, static_cast<u32>(src_x0),
                        static_cast<u32>(src_y0), src_width, src_height);
```
`copyGuestMemory` copies the source **guest** bytes. Its own comment says "Guest memory is the source of truth here …
flushing them would overwrite valid mips" (`2d.cpp:115-117`). It then marks the destination as flushed and invalidates
it (`2d.cpp:146-149`):
```cpp
    texture_cache.markGpuWritesInGuest(dst_range);
    texture_cache.invalidateMemory(dst_range);
```
What happens next:
- `markGpuWritesInGuestImpl` sets `flushed_timestamp = gpu_written_timestamp` (`texture_cache_hot.cpp:129-131`).
- `invalidateMemoryImpl` then sets `cpu_modified_timestamp = now` (`texture_cache_hot.cpp:104-111`).
- The next lookup decodes the destination from guest memory (`texture_cache_hot.cpp:170-189`).

The workaround runs for **any** same-format 1:1 copy in these four formats. If the source is a GPU-rendered render
target that has not been flushed, its guest bytes are stale. The correct GPU blit result is then replaced by stale data,
and the readback flush right after it (`2d.cpp:102-105`) finds nothing to flush because the destination is already
marked flushed. UE4 renders into float32 targets (for example the 1×1 RGBA32F sky-light target cited at
`texture_cache_tests.cpp:974-976`), so same-format float32 2D copies of GPU-rendered data are realistic. The in-game
readback that README-crash4-render.md:25 reports as working may simply use different source and destination formats,
which skips this path.

**Fix:** only take this path when a compressed-format storage overlaps the source or destination range (add a
`TextureCache` query modeled on `findDepthStencilAlias`) and the source has no unflushed GPU writes.

### B4 (S2). DMA destination flush and invalidate ranges ignore MULTI_LINE_ENABLE and LINE_LENGTH_IN

`copy.cpp:236-237` and `325-330`:
```cpp
        if (data.dst_memory_layout == MemoryLayout::Pitch) {
            dst_size = static_cast<u64>(regs.line_count) * regs.stride_out;
...
    if (data.dst_memory_layout == MemoryLayout::Pitch) {
        gpu.getRenderer().invalidateMemory(
            ztd::Range<uptr>::fromSize(
                dst_ptr, static_cast<uptr>(regs.line_count) * regs.stride_out),
```
For 1-D copies (MULTI_LINE_ENABLE = 0), LINE_COUNT and PITCH_OUT are unused and usually 0 or stale. The test
`xci_tests.cpp:212` programs exactly that ("zero pitches/count"). The destination flush range is then empty.

- **Pitch→pitch:** GPU-written storages in the destination (for example GPU-written texel buffers) are not written back.
  The per-chunk invalidation (`copy.cpp:295-298`) skips dirty storages, so the host keeps the old GPU contents. A later
  CPU fault or readback then writes them over the DMA bytes in guest memory.
- **Single-line BL→pitch:** the invalidation range is empty, so the buffer cache is never told about the new bytes.

The source size is computed correctly (`copy.cpp:218-220`).

**Fix:** `dst_size = stride_out * (lines - 1) + line_length_in * bpp`, with
`lines = multi_line_enable ? line_count : 1`.

### B5 (S2). The 2D shader path is float-only, so integer-format scaled or converting copies are invalid

`blit_pipeline_cache.cpp:69-74`:
```
fragment float4 fragment_blit(VertexBlitOut in [[stage_in]], constant BlitParams& params [[buffer(0)]], texture2d<float> tex [[texture(0)]], sampler samplr [[sampler(0)]]) {
```
Metal requires `texture2d<uint>`/`texture2d<int>` and uint/int outputs for integer pixel formats, and it cannot filter
integer textures linearly. Every copy that `blitTextureHot9` does not send through the native path
(`renderer.cpp:277-317`) uses this pipeline. That includes scaled copies, copies where src and dst pixel formats differ,
non-integral origins, rectangles out of bounds, src and dst being the same view, and a non-zero 0x240 word (B9).

If pipeline creation fails, `createHot0` logs the error and returns `nullptr` (`blit_pipeline_cache.cpp:109-115`), which
is then bound with `setRenderPipelineState` (`renderer.cpp:335-338`).

**Fix:** add uint and sint pipeline variants that read texels with integer coordinates (point sampling only) and pick
the variant from the format class of src and dst, or fall back to a compute converter.

### B6 (S2). 2D SAMPLE_MODE (filter and origin) is not modeled; the shader path always filters bilinearly

`renderer.cpp:356-358`:
```cpp
    encoder->setFragmentSamplerState(
        linear_sampler,
        static_cast<NS::UInteger>(0)); // TODO: use the correct sampler
```
Register 0x88C (index 0x223) is inside `padding_0x96` (`2d.hpp:76`). Point-filtered scaled copies (FILTER = POINT,
`cl902d.h:943-945`) are blurred. CORNER origin (`cl902d.h:940-942`) is treated as CENTER, which shifts sampling by
du/2. The exact conversion is `src0_center = src0_corner − du/2`. yuzu only subtracts the integer part
(`fermi_2d.cpp:79-82`).

### B7 (S3). 2D source extent is truncated

`2d.cpp:60-61`:
```cpp
    const auto src_width = static_cast<u32>(pixels.dst_width * dudx);
    const auto src_height = static_cast<u32>(pixels.dst_height * dvdy);
```
The blit scale becomes `src_width/dst_width` instead of du/dx (`renderer.cpp:349-350`). Example: a driver computing
`du_dx = (1280<<32)/720` gets 0x1C71C71C7; 720 × du/dx = 1279.99999998, which truncates to 1279. The last source column
is dropped and the image stretches by 1/1280. yuzu truncates the same way (`fermi_2d.cpp:95-96`).

**Fix:** pass du/dx and dv/dy (or float extents) to `blitTexture`.

### B8 (S3). Fixed-point coordinates are decoded as unsigned

`2d.hpp:34-41`:
```cpp
struct FixedPoint {
    u32 fractional;
    u32 integer;
    explicit operator f64() const {
        return static_cast<f64>(integer) + (static_cast<f64>(fractional) / static_cast<f64>(1llu << 32llu));
```
Negative SRC_X0/SRC_Y0, or negative DU_DX/DV_DY, become values around 4.29e9. yuzu uses s64 (`fermi_2d.h:284-287`).

### B9 (S2, latent). Register 0x240 is used as a source layer

`2d.hpp:73-74` declares `src` as a `Texture2DInfo`, whose fifth word is `layer` (`2d.hpp:26-27`):
```cpp
    u32 depth;
    u32 layer;
...
    // 0x8c
    Texture2DInfo src;
```
The fifth word of `src` is index 0x90, byte offset 0x240. The NVIDIA header defines that word differently
(`cl902d.h:462-466`; whitespace shortened):
```
#define NV902D_TWOD_INVALIDATE_TEXTURE_DATA_CACHE                    0x0240
#define NV902D_TWOD_INVALIDATE_TEXTURE_DATA_CACHE_V                  1:0
#define NV902D_TWOD_INVALIDATE_TEXTURE_DATA_CACHE_V_L1_ONLY          0x00000000
#define NV902D_TWOD_INVALIDATE_TEXTURE_DATA_CACHE_V_L2_ONLY          0x00000001
#define NV902D_TWOD_INVALIDATE_TEXTURE_DATA_CACHE_V_L1_AND_L2        0x00000002
```
The class has no SET_SRC_LAYER. With a value of 1 or 2:
- the depth path passes it as the Metal slice (`renderer.cpp:261-264`), out of range;
- the native path is rejected (`src_layer < src_mtl->arrayLength()`, `renderer.cpp:285`), so integer copies fall into B5;
- the compressed-alias guest copy is skipped (`2d.cpp:97`).

yuzu and nouveau call this word "layer". It is harmless while the value is 0.

### B10 (S1/S2). 2D DEPTH is treated as the layer count of a `_2D` texture

`2d.cpp:156-162`:
```cpp
    // TODO: is depth always layer count? How are levels handled?
    return renderer::TextureDescriptor(
        tls_crnt_gmmu->unmapAddr(info.addr), renderer::TextureType::_2D,
        ...
        info.height, 1, 1, std::max(info.depth, 1u), 0x0,
```
With depth > 1 this asks for a `_2D` texture with several layers. Metal guards that only with ASSERT_DEBUG
(`metal/texture.cpp:43-46`), and the multi-slice `_2D` view (`metal/texture_view.cpp:42-46`) is invalid.

### B11 (S1/S2). Color surface format gaps

`renderer/const.cpp`:
```cpp
395:        COLOR_SURFACE_FORMAT_CASE(BGR10A2Unorm, RGB10A2Unorm) // TODO: correct?
417:        COLOR_SURFACE_FORMAT_CASE(A8Unorm, Invalid)     // TODO
418:        COLOR_SURFACE_FORMAT_CASE(BGR5X1Unorm, Invalid) // TODO
421-425:    ...UnknownFB/FC/FD/FE, Y32UintUnknownFF → Invalid
```
- 0xDF is A2R10G10B10 (`cl902d.h:307`; yuzu `gpu.h:63`, `surface.cpp:139-140`). Mapping it to RGB10A2 swaps R and B in
  every conversion. The format is also used by 3D render targets.
- Formats that map to Invalid end in a fatal ASSERT at texture creation (`metal/texture.cpp:82-84`). Metal has
  `BGR10A2Unorm`, `A8Unorm` and `A1BGR5Unorm`, and Hydra already has `BGR5Unorm` / `A1BGR5Unorm` texture formats
  (`renderer/const.hpp:145-147`).

### B12 (S2). DATA_TRANSFER_TYPE NONE and semaphore releases are ignored

`copy.hpp:133-149` declares `type`, `semaphore_type` and the reduction fields. `Copy::launchDma` (`copy.cpp:142-344`)
never reads `data.type` or `data.semaphore_type`, and SET_SEMAPHORE_A/B/PAYLOAD (0x240-0x248) are not modeled.

Effects:
- A launch with NONE that only releases a semaphore re-runs a copy with stale registers.
- No semaphore payload is ever written. yuzu writes one-word and four-word releases (`maxwell_dma.cpp:347-368`).

### B13 (S2). DMA block-linear→block-linear is not implemented

`copy.cpp:319-321`:
```cpp
        } else {
            LOG_NOT_IMPLEMENTED(Engines, "BlockLinear to BlockLinear");
        }
```
The destination is still invalidated (`copy.cpp:331-343`), so the texture reloads unchanged guest bytes. yuzu implements
this by unswizzling into a temporary and swizzling out (`maxwell_dma.cpp:290-345`).

### B14 (S2). Block-linear remap silently degrades to an identity copy in Release builds

`copy.cpp:154-159`:
```cpp
        ASSERT_DEBUG(
            c.dst_x == 0 && c.dst_y == 1 && c.dst_z == 2 && c.dst_w == 3,
            Engines, "Component remapping not implemented ({}, {}, {}, {})",
```
A CONST_A/CONST_B fill into a block-linear surface, or a swizzle, copies source bytes instead (or reads an unprogrammed
OFFSET_IN).

### B15 (S2). DMA block-linear copies never move to the next slice and are not clamped to the surface height

`copy.cpp:24-33`:
```cpp
    for (u32 line = 0; line < line_count; line++) {
        const u32 y = info.origin.y + line;
        ...
            u8* bl = block_linear +
                     getBlockLinearOffset(x, y, info.layer, width, info.height,
```
`copy.cpp:306` and `314` carry the TODO "can this copy to multiple slices at once?". yuzu clamps each slice to
`height - origin_y` and continues in slice z+1 (`decoders.cpp:116-153`). In Hydra, rows beyond the height land in
another block row or Z block, outside the slice and possibly outside the allocation.

### B16 (S3). Overlapping pitch→pitch copies use `memcpy`

`copy.cpp:293-294`:
```cpp
                    memcpy(reinterpret_cast<void*>(dst),
                           reinterpret_cast<const void*>(src), bytes);
```
This is undefined when the ranges overlap (for example buffer compaction). Use `memmove`, as the remap path already
avoids the problem (`copy.cpp:107-111`).

### B17 (S2). Depth/stencil and 3D storages are never written back, so DMA readbacks of them are stale

`texture_cache_hot.cpp:389-394`:
```cpp
    if (getTextureFormatInfo(descriptor.format).is_depth_stencil ||
        descriptor.type == TextureType::_3D) {
        ONCE(LOG_NOT_IMPLEMENTED(Gpu, "Flushing {} texture with format {}",
                                 descriptor.type, descriptor.format));
        return;
    }
```

### B18 (S2). Writes that partially overlap a GPU-dirty storage are lost

`texture_cache_hot.cpp:395-398`:
```cpp
    // A partial readback does not make the rest of the GPU storage coherent.
    if (range.getBegin() <= descriptor.ptr &&
        range.getEnd() >= descriptor.ptr + descriptor.size)
        storage.flushed_timestamp = storage.gpu_written_timestamp;
```
Together with the dirty-storage skip in `invalidateMemoryImpl` (`texture_cache_hot.cpp:104-106`), a DMA, I2M or 2D
guest write into part of a larger GPU-written storage (one mip of a rendered mip chain, or one layer of a rendered
array) never reaches the host texture. A later full flush then overwrites the new guest bytes with the old GPU data.

### B19 (S3). Unconditional per-call debug instrumentation in hot paths

- `copy.cpp:249-265` runs `seen[fmt::format(...)]++` on every DMA launch.
- `texture_cache_hot.cpp:427-441` does the same on every readback.
- `2d.cpp:43-58` and `copy.cpp:176-209` log the first 40 2D copies and the first 100 (+300 subrectangle) DMA copies.

The user's memory notes already flag tracer and dump code to strip before committing.

### B20 (S3). Other robustness gaps

- **Block-linear DMA and I2M assume one contiguous host mapping and are not bounds-checked:** `copy.cpp:211-212`,
  `inline_base.cpp:29`.
- **Macros on the 2D subchannel are fatal:** `engine_base.hpp:41-46`.
- **Pitch 2D surfaces with width×bpp > pitch over-read** in `decodeTexture` (`texture_cache.cpp:741-745`; yuzu
  workaround `fermi_2d.cpp:99-110`).
- **Remap source flush size uses element count, not bytes:** the BL path's pitch-source `src_size` uses
  `line_length_in` without bpp when remap is enabled (`copy.cpp:219-220`).

---

## (c) Prioritized backlog

Each item gives what to do, why it matters for games, how to do it in Hydra, how to test it, and the regression risk.
Changing a struct or a virtual signature requires a full rebuild and restart (the hot-reload ABI check rejects layout
changes, `tests/README-hot-debug.md`).

### P0: crash or corruption, cheap to fix

**1. Bounds-safe register files (B1).**
- **Why:** in Release builds any A140 method ≥ 0x1B8 or any 2D method ≥ 0x8E0 corrupts adjacent engine objects. The
  `two_d_engine` vtable sits right after `inline_engine`.
- **How:** in `EngineWithRegsBase::writeReg` (`engine_base.hpp:66-71`), drop writes with `reg >= REG_COUNT` in every
  build type and log once. Grow `RegsInline_`, `Regs2D` and `RegsCopy` to 0xE00 words so later items can model the
  registers they need (2D sample mode 0x223, clip, operation; I2M semaphores 0x77-0x79; DMA semaphores 0x90-0x92).
- **Test:** in the `xci_tests --check-gpu-memory` harness (`tests/xci_tests.cpp:170-243`), bind A140 and 902D on two
  subchannels, send 0x77/0x78 to A140 and 0x238 to 2D, then verify a following 2D copy and DMA still work and
  `two_d.getReg()` values are unchanged.
- **Risk:** none.

**2. Inline-to-memory rework (B2).**
- **Why:** guest command streams use I2M on the 3D, compute and A140 subchannels; yuzu implements all three
  (`maxwell_3d.cpp:358`, `kepler_compute.cpp:49-56`, `kepler_memory.cpp:43-52`). Compute QMD uploads and indirect
  dispatch go through the compute I2M (yuzu `kepler_compute.cpp:49-79`). One upload whose size is not a multiple of 4
  currently breaks all later uploads on that engine. Block-linear I2M is a supported path in yuzu
  (`engine_upload.cpp:59-82`).
- **How:** in `InlineBase::launchDmaImpl`, latch layout bit 0, completion type 5:4, struct size 12 and reduction bits;
  set `copy_size = line_length_in * line_count`, `write_offset = 0`, and clear the buffer. In `loadInlineDataImpl`,
  append `min(4, copy_size - write_offset)` bytes and flush when `write_offset == copy_size`.
  - Pitch flush: one line of `line_length_in` bytes at `offset_out + line * pitch_out`, reusing the mapping-split loop
    from `copy.cpp:282-302`.
  - Block-linear flush: move `copySubrect` from `copy.cpp:16-41` into `memory_util` (for example
    `copyLinearToBlockLinearSubrect`) and call it with origin (ORIGIN_BYTES_X, ORIGIN_SAMPLES_Y), width, height, depth,
    layer and block size.
  - Then flush and invalidate the texture cache over the whole surface range, as `copy.cpp:239-247` does.
  - RELEASE_SEMAPHORE: write the payload (one word) or {payload, 0, u64 timestamp} (four words) using the timestamp code
    from `ThreeD::setReportSemaphore` (`3d.cpp:595-606`).
- **Test:** engine-level harness as in item 1:
  - a 6-byte transfer followed by an 8-byte transfer (both correct);
  - 3 lines of 12 bytes with pitch 32 (padding preserved);
  - a block-linear 64×16 destination, block height 1, origin (16, 3), checked byte by byte against an independent
    swizzle;
  - the same sequence through `ThreeD` and `Compute`.
- **Risk:** medium, because compute and constant uploads depend on this path. Run Crash 4 and Jump Force afterwards.

**3. Restrict the 2D compressed-alias guest copy (B3).**
- **Why:** it currently replaces correct GPU blits of real RGBA32F/RG32F/RGBA32UI/RG32UI render targets with stale guest
  bytes. UE4 uses float32 render targets, and its readback copies go through the same path (`2d.cpp:71-84`).
- **How:**
  - Add `TextureCache::hasCompressedStorage(range)`, iterating like `findDepthStencilAlias` (`texture_cache.cpp:95-120`).
  - Add `TextureCache::hasUnflushedGpuWrites(range)`.
  - In `TwoD::copy` (`2d.cpp:96-100`), call `copyGuestMemory` only if a compressed storage overlaps the dst or src range
    and the src range has no unflushed GPU writes.
- **Test:** in `texture_cache_tests.cpp`, using an engine-level TwoD from the xci harness:
  - Render (clear) a 1×1 RGBA32F src through `find(..., Write)`, run a 2D copy to a 1×1 RGBA32F pitch dst, and check
    that dst guest memory equals the cleared value.
  - Keep a positive case: a CPU-uploaded BC1 texture plus an RG32 2D mip copy, where the BC1 storage reloads the copied
    blocks.
- **Risk:** medium. Crash 4 texture streaming relies on the workaround; the positive test protects it.

**4. DMA destination ranges (B4).**
- **Why:** 1-D DMA copies (MULTI_LINE_ENABLE = 0, the usual buffer-to-buffer form) never flush or invalidate
  GPU-written aliases, which causes lost updates and later write-back over new data.
- **How:** in `copy.cpp:236-237` and `325-330`, compute
  `dst_size = stride_out * (lines - 1) + line_length_in * bytes_per_pixel`. Use the same size for the post-copy
  BufferCache invalidation of block-linear→pitch.
- **Test:** xci harness. Create a pitch `_1DBuffer` or `_2D` storage at the destination with `TextureUsage::Write`,
  issue a 1-D DMA (0x180), then `find(Read)` and read back, and check the DMA bytes. Run `flushMemory` and check guest
  memory still holds the DMA bytes.
- **Risk:** low.

### P1: correctness of common NVN copy paths

**5. 2D sampling semantics (B6, B7, B8).**
- **Why:** the guest chooses POINT or BILINEAR per copy (`cl902d.h:943-945`) and yuzu honours it
  (`blit_image.cpp:665-673`). Hydra blurs point-sampled scaled copies (UI or pixel-art upscales, mip and thumbnail
  downsamples), shifts CORNER-origin copies by du/2, and drops a source column on non-integral scales.
- **How:**
  - Model 0x223 (`sample_mode`) in `Regs2D`.
  - Decode FixedPoint as signed 32.32.
  - In `TwoD::copy`, compute the center-origin source offset (`src0 -= du/2` for CORNER).
  - Extend `IRenderer::blitTexture` (`renderer.hpp:135-140`, the `routes.cpp:63-71` trampoline, and the null renderer)
    with a filter argument and float source extents or scale.
  - In `blitTextureHot9`, choose `nearest_sampler` (`renderer.cpp:74-76`) for POINT and only use the native copy when
    the copy is 1:1 and integral.
  - `blitTexture` is only called by the 2D engine (presentation uses `BlitPipelineCache` directly from
    `metal/surface_compositor.cpp:39`), so other users are unaffected.
- **Test:** `texture_cache_tests.cpp` renderer level:
  - 2×2 checkerboard → 4×4 POINT gives exact 2×2 blocks; BILINEAR gives interpolated texels.
  - 3→2 downscale with du = 1.5 at CENTER and CORNER, checking texel values against a CPU reference.
  - 1280→720 extent with du = (1280<<32)/720 samples the last source column.
- **Risk:** low to medium (UE4 copies that look correct today change only if they use POINT or CORNER).

**6. Integer formats in the 2D shader path (B5).**
- **Why:** prevents invalid pipelines and garbage results for integer copies once the native path is rejected (scaled
  copies, reinterpreting copies, out-of-range rectangles, B9).
- **How:**
  - Add `fragment uint4/int4` variants that use `tex.read(uint2(...))` with the integer texel computed from the center
    mapping (point only).
  - Key `BlitPipelineDescriptor` (`blit_pipeline_cache.hpp:14-18`) by data class.
  - In `blitTextureHot9`, select the variant from `toColorDataType` of the dst format; refuse BILINEAR for integer
    formats and log once.
- **Test:** R32Uint 2×2 → 4×4 POINT and R32Uint → RG16Uint same-size conversion, both read back and checked with
  `MTL_DEBUG_LAYER=1`.
- **Risk:** low.

**7. DMA block-linear→block-linear (B13).**
- **Why:** used for texture-to-texture copies between differently tiled surfaces, for example compressed or streamed
  mips between memory pools (yuzu `maxwell_dma.cpp:290-345`). Today the result is the unchanged destination.
- **How:** in `Copy::launchDma`, call `copySubrect(temp, line_bytes, src, regs.src, src_stride, bpp, line_bytes, lines, false)`
  and then `copySubrect(temp, …, dst, regs.dst, dst_stride, …, true)`. The flushes at `copy.cpp:266-271` and the
  invalidation at `331-343` already cover it.
- **Test:** xci harness. Copy a 256×64 source with block height 16 at origin (32, 8) to a destination with block height 4
  at origin (0, 16), with multi-line on, and compare against an independent reference swizzle (port yuzu's pdep formula
  into the test).
- **Risk:** low (new path).

**8. DMA semaphores and transfer type NONE (B12).**
- **Why:** completion semaphores are how a driver signals that a copy finished. A waiter, whether a GPU semaphore
  acquire or a CPU poll, would hang or read stale values. yuzu implements this.
- **How:**
  - Model 0x90-0x92 (SET_SEMAPHORE_A upper / B lower / PAYLOAD) in `RegsCopy`.
  - Add `Copy::releaseSemaphore(const LaunchDMAData&)`: one word writes a u32 payload; four words write
    {payload, 0, timestamp (u64)}, where the timestamp is computed like `3d.cpp:595-606`. Optionally support the
    reductions IMIN..FADD with the sign bit.
  - Invalidate the BufferCache for the written range.
  - At the top of `launchDma`: `if (data.type == TransferType::None) { releaseSemaphore(data); return; }`. Call it at
    the end of every copy path too.
- **Test:** xci harness for one-word and four-word releases (payload and a non-zero timestamp), the IADD reduction, and
  a NONE launch that leaves the destination untouched.
- **Risk:** low.

**9. Color surface format table (B11).**
- **Why:** 0xDF (A2R10G10B10) is a 10-bit color format used for render targets and copies, and the current mapping
  swaps R and B on conversion. The other formats crash.
- **How:** in `toTextureFormat(ColorSurfaceFormat)` (`const.cpp:357-428`):
  - 0xDF → a BGR10A2 TextureFormat backed by `MTLPixelFormatBGR10A2Unorm`, or RGB10A2 with an R/B swizzle view.
  - 0xF8 → `BGR5Unorm` (alpha forced to 1).
  - 0xF7 → a new `A8Unorm`.
  - 0xFB/0xFC → BGR5 with alpha 0 or 1; 0xFD/0xFE → BGRX8 with alpha 0 or 1.
  - 0xFF → R32Uint.
  - Make `Invalid` a logged no-op in `TwoD::copy` instead of a fatal error.
- **Test:** renderer-level conversion of 0xDF→RGBA8 with known channel values; creating each format.
- **Risk:** low to medium (0xDF also affects 3D render targets; check UE4 HDR scenes).

**10. DMA copies spanning slices, plus bounds (B15, B20).**
- **Why:** 3D texture uploads and readbacks (UE4 volumetric lightmaps and volume LUTs) with LINE_COUNT = height × depth,
  and protection against rows written outside the surface.
- **How:** in `copySubrect`, clamp each slice to `height - origin.y`, then continue with z + 1 and `y = origin.y`,
  advancing the pitch side by `pitch * lines_in_slice` (mirroring yuzu `decoders.cpp:116-153`). Bounds-check each write
  against the computed block-linear size, and resolve the block-linear base through `findAddrImpl` per chunk or verify
  the whole span is mapped.
- **Test:** pitch→BL upload of a 64×25×5 surface (block depth 2) with LINE_COUNT 125 compared with
  `convertLinearToBlockLinear`; BL→pitch round trip.
- **Risk:** medium. Confirm first in a trace whether NVN uses LAYER with LINE_COUNT = height, or multi-slice copies.

### P2: coherency and less common paths

**11. Partial-overlap writes into GPU-dirty storages (B18) and depth/3D writeback (B17).**
- **How:**
  - When a CPU or engine write range partially covers a dirty storage, flush the whole storage first (`encodeTexture`
    with the storage's full range), then mark it flushed so `invalidateMemoryImpl` reloads it.
  - Longer term, add GPU-side BufferToImage and ImageToBuffer for dirty storages, like yuzu's `DmaBufferImageCopy`
    (`texture_cache.h:1044-1086`). This also removes the synchronous GPU waits.
  - Implement depth encode (Metal depth/stencil → guest Z24S8/Z32F layouts, the inverse of
    `copyDepthStencilFromBufferHot3`) and 3D encode (per-slice `convertLinearToBlockLinear` with depth).
- **Test:** texture_cache_tests. Render to layer 1 of a 2-layer array, DMA-upload layer 0, and sample both layers. Clear
  a Z32F depth target, then run a DMA BL→pitch readback.
- **Risk:** medium (adds GPU waits on paths that do not wait today).

**12. 2D register map and depth semantics (B9, B10, T29).**
- **How:**
  - Split `Regs2D::src` so 0x240 is `invalidate_texture_data_cache` and is ignored, and pass `src_layer = 0`.
  - Model `dst_render_to_zeta` (0x2B8) and use it, rather than the src-only heuristic, to choose a depth destination.
  - Allow color↔depth copies through a shader (depth write from color, or depth→R32F).
  - For DEPTH > 1, treat LAYER as a slice: create a `_3D` or `_2DArray` descriptor and a single-slice view instead of a
    `_2D` texture with `layer_count = depth`.
- **Test:** a 2D copy with 0x240 = 2 behaves like 0x240 = 0; a copy into slice 2 of a depth-4 surface.
- **Risk:** low to medium.

**13. DMA remap for block-linear layouts (B14).**
- **How:** generalize `remapPitchCopy`'s element loop into a lambda that produces destination bytes from source bytes
  and constants, and use it inside the block-linear loop (or remap into a temporary pitch buffer and then swizzle).
- **Test:** CONST_A fill of a 64×8 block-linear surface with component size 4; NO_WRITE preserves bytes.
- **Risk:** low.

**14. Block-size clamping consistency (S6).**
- **How:** in `TextureDescriptor::getLevelBlockSizeLog2`, skip clamping when `level == 0 && level_count == 1`, as yuzu
  `TileShift` does (`util.cpp:172-179`), or apply the same clamp in DMA and `copyGuestMemory`.
- **Test:** a 64-row 2D surface programmed with block height 16, decoded and DMA-copied, both checked against the
  hardware formula.
- **Risk:** medium (affects every render target; land it behind tests).

**15. MSAA resolve through 2D (T37).** Depends on 3D MSAA support. Add sample counts to `TextureDescriptor`, shift
source regions by the sample count (yuzu `texture_cache.h:756-781`), and use `MTLRenderPassDescriptor` resolve or a
box-filter shader. Risk: high (large feature).

**16. Clean-ups.**
- Strip the debug instrumentation (B19).
- Use `memmove` in the pitch path (B16).
- Mask OFFSET_UPPER to 8 bits.
- Skip `flushMemory` of the destination in the block-linear path when the copy covers the whole storage (use
  `discardGpuWrites`, as `copy.cpp:96-97` already does).
- Avoid the synchronous `debugSyncCommandBuffer` wait for every pitch 2D destination (`2d.cpp:102-105` →
  `texture_cache_hot.cpp:442`) by deferring until the CPU actually faults.
- Risk: low.

### P3: rarely used 2D features (yuzu lacks them too; implement only if a trace shows use)

**17.** Clip rectangle (T21), ROP / BETA / non-SRCCOPY operations with premultiply and blend (T23-T24), color key (T25),
patterns (T26), solid points/lines/rectangles (T27), pixels from CPU and big-endian control (T28), render enable (T30),
notify (T31), and MME on 2D (T32: at least make it non-fatal).
- **How:** model the registers, then render with a dedicated Metal pipeline (for solid fills and clipping, scissor plus
  constant color; blend state for BLEND/PREMULT).
- **Test:** one Metal readback per feature.
- **Risk:** low (new paths).

**18. Shader-code uploads through DMA or I2M (cross-subsystem).** `IRenderer::invalidateMemory`'s ShaderCache scope is a
TODO (`renderer.hpp:94-96`), and the shader cache is keyed by `code_ptr`, not content (`shader_cache.cpp:232-238`).
Shader binaries that DMA or I2M upload to a reused address therefore keep the old compiled shader. Track shader address
ranges and evict on invalidation.

---

## Test coverage summary for this subsystem

| Area | Covered by |
|---|---|
| Block-linear offset math (2D and 3D) | `texture_cache_tests.cpp:1057-1126` (self-consistency between two Hydra implementations) |
| Pitch and tiled decode at the same address | `texture_cache_tests.cpp:76-123` |
| Renderer-level native blit and 1×1→2×2 converting blit | `texture_cache_tests.cpp:825-920` |
| Readback flush (partial and full) | `texture_cache_tests.cpp:921-1054` |
| Mixed buffer/2D byte sync | `texture_cache_tests.cpp:405-479` |
| DMA pitch→pitch, single and multi-line, scattered backing | `xci_tests.cpp:170-242` |

No engine-level test exists for `TwoD::copy`, DMA pitch↔BL, BL→BL, remap, semaphores, or any inline-to-memory path.
The xci `--check-gpu-memory` harness (`tests/xci_tests.cpp:247-256`) already constructs a real `Gpu`, `GMmu` and Copy
engine, and is the natural home for the engine-level tests proposed above.
