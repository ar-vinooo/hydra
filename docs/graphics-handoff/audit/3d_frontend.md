# Hydra audit: MAXWELL_B (0xB197) 3D front-end and geometry pipeline

> Evidence policy (2026-10-18): NVIDIA documentation/headers define guest
> behavior; Apple Metal documentation, SDK headers and validation define host
> behavior. Eden and other emulators are historical comparators only. Any
> conclusion based solely on them is unverified and must be corroborated.

Read-only audit of `/Users/arvino/Documents/Project/hydra`. Nothing in the repository was modified, built or run.

## Scope, method and snapshot

**Scope.** The 3D engine front end: method dispatch, the register file, MME (macro) execution and shadow RAM, draw commands, topologies, index and vertex fetch, programmable-stage enables (VertexA, tessellation, geometry shader, transform feedback), viewport, depth and clip, scissor, rasterizer state, and the fixed-function pieces emulated in MSL. It also covers inline-to-memory (I2M) transfers on the 3D class and the PFIFO pushbuffer parser.

**Method.**
- I read every Hydra code path in scope and compared it to the reference sources.
- I checked register offsets mechanically. The script `audit/regs_layout_check.py` (in the scratchpad) models `Regs3D` field by field and compares it with the method offsets parsed from `clb197.h`.
- I quoted code only after re-reading it at the snapshot below.

**Snapshot: 2026-10-17, about 10:28 WIB.** Someone modified several in-scope files *while this audit was running*:
- `engines/3d.{hpp,cpp}`, `renderer/metal/pipeline.cpp`, `renderer/{pipeline_cache,shader_cache}.{hpp,cpp}` and `renderer/const.hpp`, all at 10:07–10:08.
- `shader_decompiler/{const.hpp,const.cpp}`, `analyzer/memory_analyzer.*` and `codegen/lang/msl/emitter.*`, at 10:03–10:18.
- `tests/shader_coverage_tests.cpp` (new, 10:13).
- `engines/engine_base.hpp` (10:25).

I re-read the changed regions, and the tables describe the **post-change state**. Appendix A lists the defects I observed before those edits that are now fixed. Line numbers in files under active editing (mainly `msl/emitter.cpp`) may drift, so I also give function names.

**Path abbreviations**
- `H/` = `/Users/arvino/Documents/Project/hydra/src/core/hw/tegra_x1/gpu/`
- `T/` = `/Users/arvino/Documents/Project/hydra/tests/`
- `REFS` = `/private/tmp/claude-501/-Users-arvino-Documents-Project-hydra/0e906531-f45a-4e9b-b0bc-dcb303edaa1b/scratchpad/refs`
- `clb197.h` = `REFS/opengpudoc/clb197.h`
- `E/` = `REFS/yuzu/src/video_core/` (Eden)
- `ESR/` = `REFS/yuzu/src/shader_recompiler/`

**Classes**
- **VERIFIED**: implemented, and either covered by a result-checking test or documented as verified in game in `T/README-*.md`.
  - **VERIFIED (pending run)**: the covering test exists but was added during this audit window. I could not run it in a read-only audit, so it has not been observed passing.
- **IMPLEMENTED_UNVERIFIED**: implemented, with no result-checking test.
- **PARTIAL**: modes or variants missing, approximated, or wrong.
- **MISSING**: not implemented, stubbed, or silently ignored.
- **UNKNOWN**: behaviour could not be decided from the code.

### Counts (101 capability rows)

| Class | Count | Notes |
|---|---:|---|
| VERIFIED | 9 | 2 confirmed (vertex-attribute decode test; cull/FLIP_Y verified in game). 7 are pending run: tests added 10:13 today. |
| IMPLEMENTED_UNVERIFIED | 22 | |
| PARTIAL | 26 | |
| MISSING | 43 | |
| UNKNOWN | 1 | |

### Test coverage that exists for this subsystem
- `T/render_hash_tests.cpp:113-128`: `VertexAttribState` bit decode (bgra bit 31, type, size, offset, buffer, source). Also checks that shader and pipeline keys include vertex formats, depth mode and divisor.
- `T/raster_state_tests.cpp`: host-side Metal raster-state caching (viewport, scissor, blend colour, depth bias, stencil ref). It does **not** exercise the guest-register-to-viewport conversion.
- `T/shader_library_tests.cpp`:
  - `testVertexPositionDefaults` (around line 788): VS defaults of (0,0,0,1) and ZeroToOne depth passthrough.
  - `testGeometryLayerForwarding` (around line 1086): the geometry-shader layer-forwarding heuristic and per-instance draws.
- `T/shader_coverage_tests.cpp` (new): front facing, point size and point coord origin, clip distances, viewport-index and layer outputs, `SR_Y_DIRECTION`.
- `T/README-crash4-render.md:22`: "culling keeps the guest front face for FLIP_Y" is verified in game.
- **No tests exist** for the MME interpreter, the PFIFO parser, index decoding and topology conversion, the viewport and scissor math in `ThreeD`, depth bias, instancing or divisors, or the missing draw methods.

---

## (a) Capability table

### A. Method dispatch, register file, MME

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| A1 | Method dispatch and register writes | IMPLEMENTED_UNVERIFIED | `H/engines/engine_base.hpp:10-22` (`DEFINE_METHOD_TABLE`, default `writeReg`), `:66-79`; `H/engines/3d.cpp:224-233`; trampoline `src/core/hot/routes.cpp:755-758` → `methodHot` | `E/engines/maxwell_3d.cpp:388-406` (`CallMethod`: shadow RAM, store, then process) | Methods in the table do **not** store their argument (bug B27). |
| A2 | Offsets of named `Regs3D` fields vs `clb197.h` | IMPLEMENTED_UNVERIFIED | `H/engines/3d.hpp:212-550` | `clb197.h` (method offset / 4) | 0 mismatches across 143 checked fields (section c). Only the vertex-attribute word has a repo test. |
| A3 | Register-file extent; `SET_MME_SHADOW_SCRATCH(0..255)` at 0xD00–0xDFF | PARTIAL | `H/engines/3d.hpp:549` `u32 mme_scratch[0x80];` → `REG_COUNT` = 0xD80; `engine_base.hpp:54-58,66-79` (out-of-range writes dropped, reads return 0, since the 10:25 edit) | `clb197.h:4156-4159` (scratch at 0x3400, `CALL_MME` at 0x3800); `E/engines/maxwell_3d.h:66` (`NUM_REGS = 0xE00`), `:3029` (`std::array<u32,0x100> shadow_scratch`) | Bug B7: scratch registers 0x80–0xFF are lost (writes dropped, MME reads return 0). Before the 10:25 edit this was an out-of-bounds write. |
| A4 | `SET_MME_SHADOW_RAM_CONTROL` (track, track-with-filter, passthrough, replay) | MISSING | No reference to method 0x49; falls through to `writeReg` | `clb197.h:67-72`; `E/engines/maxwell_3d.cpp:274-283` (`ProcessShadowRam`), `:307-312` | Replay restores state from the shadow copy. Hydra always uses the live argument. |
| A5 | MME program upload (`LOAD_MME_INSTRUCTION_RAM[_POINTER]`, `LOAD_MME_START_ADDRESS_RAM[_POINTER]`) | IMPLEMENTED_UNVERIFIED | `H/engines/3d.cpp:277-291`; `H/macro/driver_base.cpp:18-32`; `driver_base.hpp:68-71` | `clb197.h:55-64`; `E/engines/maxwell_3d.cpp:455-461` | No bounds checks on the 0x1000-word and 0x100-word arrays. |
| A6 | `CALL_MME_MACRO`/`CALL_MME_DATA` parameter collection and execution timing | PARTIAL | `H/engines/3d.cpp:261-272`; `H/pfifo.cpp:226-284` (flush when `header.method >= 0xE00`) | `E/engines/maxwell_3d.cpp:203-220` (`ProcessMacro` accumulates every word), `:375-386` | Only correct for IncOnce and single-word forms. Mishandled: NonInc to the even method, Inc spanning macros, macros sending to macro methods (bugs B5, B6). |
| A7 | MME ALU (add, addc, sub, subb, xor, or, and, andnot, nand) | PARTIAL | `H/macro/interpreter/driver.cpp:22-62` | `E/macro.cpp:543-577` (unsigned 64-bit carry; `subb` uses `1-carry`) | Bug B3: signed carry with undefined overflow; borrow inverted. |
| A8 | MME ExtractInsert | IMPLEMENTED_UNVERIFIED | `driver.cpp:70-76`; decode `driver_base.cpp:68-76` | `E/macro.cpp:477-485`; `E/macro.h:69-102` | Matches Eden. |
| A9 | MME ExtractShiftLeftImmediate / ExtractShiftLeftRegister | PARTIAL (wrong) | `driver.cpp:79-94`; decode `driver_base.cpp:77-92` | `E/macro.cpp:487-503`; JIT `:951-971` | Bug B1: src_a and src_b roles swapped in both ops. |
| A10 | MME Read (register read) | PARTIAL | `driver.cpp:96-99`; `driver_base.cpp:46,93-97` (unsigned 18-bit immediate) | `E/macro.cpp:505-508`; `E/macro.h:81` (`BitField<14,18,s32>`) | Signed immediates are not honoured. Reads at ≥ 0xD80 return 0 instead of the stored scratch value (B7). Reads 0 for registers that table methods never store (B27). |
| A11 | MME branch, annul, delay slot, exit | PARTIAL | `driver.cpp:101-131`; `driver_base.cpp:99-120`; `driver.cpp:5-20` | `E/macro.cpp:509-538` | Bug B2: stale `exit_after` across calls. An exit inside a delay slot is only approximately ignored. |
| A12 | MME result operations (8 modes) | IMPLEMENTED_UNVERIFIED | `driver.cpp:133-170`; `driver_base.cpp:125-133` | `E/macro.cpp:580-625` | Matches Eden. |
| A13 | Per-call MME state reset (registers, carry, method address, exit) | MISSING | `driver.cpp:5-8` (only `pc` and r1 set); `driver_base.hpp:79-83` | `E/macro.cpp:421-451` (`Execute` → `Reset`) | Bug B2. |
| A14 | Macro HLE (indirect draws, bind shader, clear CB, transform-feedback setup, multi-layer clear) | MISSING | Only the interpreter exists (`3d.cpp:27-30`) | `E/macro.cpp:393-408` (HLE list), `:66-380` | Mostly a performance gap. Indirect draws via the interpreter read their arguments on the CPU (row B15). |
| A15 | `SET_FALCON04` firmware call | IMPLEMENTED_UNVERIFIED | `3d.cpp:608-613` (`mme_scratch[0] = 1`) | `E/engines/maxwell_3d.cpp:463-468` | Same stub as Eden. |
| A16 | Power-on register defaults | PARTIAL | `3d.cpp:235-257` (swizzles, colour masks, VertexB enable only) | `E/engines/maxwell_3d.cpp:52-129` (depth range 0..1, point size 1, rasterize enable 1, polygon modes fill, line widths 1, attribute constant=1, front face, and more) | Must be extended before raster-enable or polygon-mode support lands (backlog items 8 and 17). |

### B. Draw commands

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| B1 | `BEGIN` topology decode, `END`, `SPLIT_MODE` | PARTIAL | `H/engines/3d.hpp:438-442` (`PrimitiveType primitive_type : 26`); `3d.cpp:293-315`; `END` (0x585) is a plain register write | `clb197.h:3025-3053` (OP 15:0, PRIMITIVE_ID 24, INSTANCE_ID 27:26, SPLIT_MODE 30:29); `E/engines/maxwell_3d.h:2096` (`BitField<0,16>`) | Bug B8: PRIMITIVE_ID leaks into the topology. Hydra draws on the count write, not on `END` (Eden draws at `END`: `E/engines/draw_manager.cpp:187-217`). |
| B2 | `BEGIN.INSTANCE_ID` FIRST/SUBSEQUENT/UNCHANGED | IMPLEMENTED_UNVERIFIED | `3d.cpp:304-314`; each instance becomes a separate host draw with `baseInstance = instance_index` (`3d.cpp:357-368, 489-491`) | `E/engines/draw_manager.cpp:173-185`, `:75-102` | Correct per-instance semantics. Performance cost: one full `drawInternal()` per instance (row B14). |
| B3 | `SET_VERTEX_ARRAY_START` + `DRAW_VERTEX_ARRAY` | IMPLEMENTED_UNVERIFIED | `3d.hpp:276`; `3d.cpp:228,317-372` | `clb197.h:945-949`; `E/engines/draw_manager.cpp:194-203` | |
| B4 | `SET_INDEX_BUFFER_A..F` + `DRAW_INDEX_BUFFER` (address, limit, size, first, count) | PARTIAL | `3d.hpp:449-455`; `3d.cpp:438-494`; `H/renderer/metal/renderer.cpp:1001-1043` | `clb197.h:3183-3205`; `E/engines/maxwell_3d.h:2209-2243` (`IndexStart = start + first*size`) | Bug B10: FIRST is applied as a host offset outside the snapshotted range. The limit is ignored. |
| B5 | `DRAW_VERTEX_ARRAY_BEGIN_END_INSTANCE_FIRST` / `_SUBSEQUENT` (0x485/0x486) | MISSING | Not in the method table (`3d.cpp:224-233`) → silent `writeReg` | `clb197.h:1963-2002`; `E/engines/draw_manager.cpp:53-58,91-102` | Bug B9: the draw is dropped. |
| B6 | `DRAW_INDEX_BUFFER{32,16,8}_BEGIN_END_INSTANCE_FIRST` / `_SUBSEQUENT` (0x5F9–0x5FE) | MISSING | Not in the table; `3d.hpp:457` padding | `clb197.h:3207-3326`; `E/engines/draw_manager.cpp:31-39,219-229`; `maxwell_3d.h:2245-2253` | Bug B9. |
| B7 | `DRAW_INLINE_INDEX`, `DRAW_INLINE_INDEX2X16`, `DRAW_INLINE_INDEX4X8` (+`_ALIGN`) | MISSING | Not in the table | `clb197.h:2160-2169, 2932-2944`; `E/engines/draw_manager.cpp:40-52,127-171,206-215` | Bug B9. |
| B8 | `DRAW_AUTO` (+`SET_DRAW_AUTO_STRIDE`/`START`) | MISSING | Not in the table | `clb197.h:2059, 2217, 2546`; `E/macro.cpp` (`HLE_DrawIndirectByteCount`) | Needs transform feedback (row F5). |
| B9 | `DRAW_ZERO_INDEX` | MISSING | Not in the table | `clb197.h:385-386` | Eden also ignores it. |
| B10 | `SET_GLOBAL_BASE_VERTEX_INDEX` | IMPLEMENTED_UNVERIFIED | `3d.hpp:411`; `3d.cpp:490-491` → Metal `baseVertex` | `clb197.h:2633`; `E/engines/draw_manager.cpp:196`; `E/renderer_vulkan/vk_rasterizer.cpp:153-174` | Metal `[[vertex_id]]` includes the base vertex, matching Eden's `VertexIndex`. |
| B11 | `SET_GLOBAL_BASE_INSTANCE_INDEX` | IMPLEMENTED_UNVERIFIED | `3d.hpp:412`; `3d.cpp:1300-1301` (stream element = `base_instance + instance_index/divisor`) | `clb197.h:2636`; `E/engines/draw_manager.cpp:195` | Changed during the audit (Appendix A). |
| B12 | Shader `InstanceId` / `VertexId` values | VERIFIED (pending run) | `H/renderer/shader_decompiler/codegen/lang/msl/emitter.cpp:570-571` (`[[instance_id]]`, `[[vertex_id]]`); host `baseInstance = instance_index` | `ESR/backend/spirv/emit_spirv_context_get_set.cpp:346-359` (InstanceId = index − base) | `T/shader_coverage_tests.cpp` `testViewportAndLayerOutputs` ("instance ID, which excludes the base instance"). |
| B13 | Instanced streams (`SET_VERTEX_STREAM_INSTANCE_A`, `FREQUENCY` divisor) | IMPLEMENTED_UNVERIFIED | `3d.cpp:1283-1320` (per-draw element offset); `H/renderer/metal/pipeline.cpp:105-112` (constant step for instanced streams) | `clb197.h:3330-3333,3714`; `E/renderer_vulkan/fixed_pipeline_state.cpp:206-212` | IS_INSTANCED with FREQUENCY=0 is treated as per-vertex (Eden uses divisor 0). Offsets `element*stride` may not be 4-byte aligned. Changed during the audit. |
| B14 | Multi-instance batching (one host draw for N instances) | MISSING | `3d.cpp:304` "Instanced draws are submitted as one draw per instance" | `E/engines/draw_manager.cpp:75-81,173-217` | Performance only, but large for foliage and particles. |
| B15 | Indirect draws (NVN macros) | PARTIAL | Run by the generic interpreter; parameters come from guest memory when the pushbuffer is parsed (`pfifo.cpp:78-91,286-292`) | `E/macro.cpp:66-198` (HLE uses the GPU address, `GetMacroAddress`) | Indirect arguments written by GPU compute that is still in flight on Metal would be read stale. |
| B16 | `SET_VERTEX_ID_BASE`, `SET_DA_OUTPUT.VERTEX_ID_USES_ARRAY_START`, `SET_VERTEX_ID_COPY` | MISSING | Unreferenced | `clb197.h:1597, 3055-3059, 3087-3090` | Eden stores but mostly ignores these. |
| B17 | Topology override (`SET_PRIMITIVE_TOPOLOGY_CONTROL` / `SET_PRIMITIVE_TOPOLOGY`) | MISSING | Unreferenced (0x652/0x65C) | `clb197.h:3433-3496`; `E/engines/draw_manager.cpp:251-274` | Override encodings differ from `BEGIN` (Points=1, Lines=2). |
| B18 | `ADD_TO_PRIMITIVE_ID` / `LOAD_PRIMITIVE_ID` | MISSING | Unreferenced | `clb197.h:3061-3065` | Affects `PrimitiveId` continuity in split draws. |

### C. Topologies

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| C1 | Points, Lines, LineStrip, Triangles, TriangleStrip | IMPLEMENTED_UNVERIFIED | `H/renderer/metal/maxwell_to_mtl.cpp:391-403` | `E/renderer_vulkan/maxwell_to_vk.cpp:341-380` | Native in Metal. |
| C2 | Quads | PARTIAL | `H/renderer/index_cache.cpp:17-23,106-111`; `index_decoders.inc:1-11` | `E/renderer_vulkan/vk_buffer_cache.cpp:271-309,579-600` (quad passes, u32 output) | Non-indexed is correct. Indexed is broken by the input/output type confusion (B11) and the stale cache (B13). Flat-shading provoking vertex differs for the first triangle. |
| C3 | TriangleFan | PARTIAL (wrong) | `index_cache.cpp:25-32,112-115`; `index_decoders.inc:13-23` | Vulkan-native fans (`maxwell_to_vk.cpp`); a correct conversion is (0,i,i+1) lists | Bug B12: winding reversed, wrong triangulation, indexed form garbage. |
| C4 | QuadStrip | MISSING | Not converted; `maxwell_to_mtl.cpp:404-406` defaults to `PrimitiveTypeTriangle` | `E/renderer_vulkan/vk_buffer_cache.cpp:311-330` ({0,3,1,0,2,3} per 2 vertices) | Bug B14. |
| C5 | Polygon | MISSING | Same fallback | `E/renderer_vulkan/maxwell_to_vk.cpp:373-376` (→ fan) | Bug B14. |
| C6 | LineLoop | MISSING | Same fallback | Metal has no line loop: use a strip plus a closing index | Bug B14. |
| C7 | Lines/LineStrip/Triangles/TriangleStrip with adjacency | MISSING | Same fallback | `E/renderer_vulkan/maxwell_to_vk.cpp:355-362` | Without a geometry shader, adjacency vertices must be dropped by index rewriting. |
| C8 | Patches | MISSING | Same fallback; tessellation stages ignored (`3d.cpp:1135-1155`) | `E/renderer_vulkan/maxwell_to_vk.cpp:371-372` | Renders control points as triangles (garbage). |

### D. Index buffer and primitive restart

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| D1 | Index sizes u8/u16/u32 | PARTIAL | `H/engines/const.hpp:46-64`; u8→u16 at `index_cache.cpp:12-15,116-120`; `maxwell_to_mtl.cpp:410-420` | `E/renderer_vulkan/vk_buffer_cache.cpp:579-608` (uint8 pass) | Bugs B10 and B11. |
| D2 | Caching and invalidation of converted index buffers | PARTIAL | `index_cache.hpp:17` "TODO: memory invalidation"; `index_cache.cpp:156-159,187-197` | Eden regenerates through the buffer cache (dirty tracking) | Bug B13. |
| D3 | `SET_DA_PRIMITIVE_RESTART` / `_INDEX` | PARTIAL | Unreferenced (0x591/0x592). Metal implicitly restarts strips on 0xFFFF/0xFFFFFFFF. | `clb197.h:3079-3085`; `E/renderer_vulkan/vk_rasterizer.cpp:1512-1528`; GL `E/renderer_opengl/gl_rasterizer.cpp:958-961` | Bug B23: disabled restart, custom index, and u8 0xFF are not honoured. |
| D4 | `SET_DA_PRIMITIVE_RESTART_VERTEX_ARRAY`, restart topology change | MISSING | Unreferenced | `clb197.h:573-576, 1087-1090, 2088` | Rare. |
| D5 | Index buffer limit (`SET_INDEX_BUFFER_C/D`) | MISSING | `3d.cpp:464-469` (commented out; size is `count*size`) | `E/engines/maxwell_3d.cpp` `EstimateIndexBufferSize` | Matters only for indirect draws whose count is unknown on the CPU. |

### E. Vertex fetch

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| E1 | `SET_VERTEX_STREAM_A_FORMAT` stride 11:0, enable bit 12 | IMPLEMENTED_UNVERIFIED | `3d.hpp:489-497`; `3d.cpp:1211-1222, 2429-2439`; `pipeline.cpp:96-128` | `clb197.h:3702-3706` | Bit layout matches. |
| E2 | `SET_VERTEX_STREAM_A_LOCATION` + `SET_VERTEX_STREAM_LIMIT_A` | IMPLEMENTED_UNVERIFIED | `3d.cpp:1283-1314` (size = limit + 1 − address) | `clb197.h:3708-3713, 3923-3927` | |
| E3 | Stride-0 streams | IMPLEMENTED_UNVERIFIED | `pipeline.cpp:108-112` (constant step function) | Hardware fetches the same element for every vertex | |
| E4 | `SET_VERTEX_ATTRIBUTE_A/B` field decode (stream 4:0, source 6, offset 20:7, size 26:21, type 29:27, swap 31) | VERIFIED | `H/engines/const.hpp:101-111`; `T/render_hash_tests.cpp:113-128` | `clb197.h:1841-1915`; `E/engines/maxwell_3d.h:589-597` | |
| E5 | Inactive (constant) attributes read (0,0,0,1) | IMPLEMENTED_UNVERIFIED | `H/renderer/shader_decompiler/codegen/lang/emitter.cpp:117-136`; skipped in the stage-in struct (`msl/emitter.cpp:383`) and pipeline (`pipeline.cpp:67`) | `ESR/backend/spirv/emit_spirv_context_get_set.cpp:309-312` | Matches Eden's disabled-attribute default. |
| E6 | Float32 (raw bits), Float16, UNORM/SNORM 8/16, SINT/UINT 8/16/32 | IMPLEMENTED_UNVERIFIED | `maxwell_to_mtl.cpp:422-681`; `codegen/helper.hpp:30-54`; `lang/emitter.cpp:159-165` | `E/renderer_vulkan/maxwell_to_vk.cpp:382-560` | Raw 32-bit float fetch preserves denormals. |
| E7 | USCALED / SSCALED | PARTIAL | `helper.hpp:17-22` (both → U32); `lang/emitter.cpp:140-153` (`(float)`); `maxwell_to_mtl.cpp:577-644` | `E/renderer_vulkan/vk_pipeline_cache.cpp:118-140` (SignedScaled → SToF) | Bug B21: negative SSCALED values break. |
| E8 | Packed A2B10G10R10 (UNORM/SNORM only) and B10G11R11 float | PARTIAL | `maxwell_to_mtl.cpp:461-462, 501-502, 674-675` | `E/renderer_vulkan/maxwell_to_vk.cpp` (SINT/UINT/SCALED 2-10-10-10) | Metal has no non-normalized 1010102 vertex format, so shader unpacking is needed. |
| E9 | Sizes G8R8 (0x32), X8B8G8R8 (0x33), A8 (0x34) | MISSING | Absent from enum `H/engines/const.hpp:77-99` → `VertexFormatInvalid` | `clb197.h:1855-1865`; `E/engines/maxwell_3d.h:557-575`; `E/renderer_vulkan/maxwell_to_vk.cpp:398-410` | Bug B22. |
| E10 | 32-bit UNORM/SNORM | MISSING | `maxwell_to_mtl.cpp:434-441, 471-478` | Eden also has no Vulkan format | Shader unpack if ever seen. |
| E11 | `SWAP_R_AND_B` (BGRA) | PARTIAL | `maxwell_to_mtl.cpp:424-429, 495-499` (only uchar4 UNORM) | `clb197.h:1874-1876` | Other formats ignore the swap in release builds (debug assert only). Decode is verified. |
| E12 | `VERTEX_STREAM_B`, `VERTEX_STREAM_SUBSTITUTE`, `VERTEX_GLOBAL_BASE_OFFSET` | MISSING | `3d.hpp:499` (padding) | `clb197.h:1255-1259, 2945-2949, 3717-3730` | Eden also ignores these. Low priority. |
| E13 | Active attribute referencing a **disabled** stream | UNKNOWN | `pipeline.cpp:61-93` sets the attribute; `:99-100` skips the layout; `3d.cpp:2432-2434` binds no buffer | Eden binds a null buffer (zeros) | Probably a Metal pipeline-validation failure (layout stride 0) or a stale binding. Needs a test. |

### F. Programmable-stage control, tessellation, geometry shader, transform feedback

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| F1 | `SET_PIPELINE_SHADER`/`PROGRAM`, `SET_PROGRAM_REGION` (VertexB + Fragment) | IMPLEMENTED_UNVERIFIED | `3d.hpp:518-529`; `3d.cpp:912-1100, 1150-1171` | `clb197.h:2996-3001, 3935-3969` | |
| F2 | VertexA + VertexB (dual vertex programs) | MISSING | `3d.cpp:1135-1155` (only logged "unsupported stage") | `E/renderer_vulkan/vk_pipeline_cache.cpp:59` (`MergeDualVertexPrograms`) | VertexB runs alone, so output is wrong when VertexA is used. |
| F3 | Tessellation (TessCtrl/TessEval stages, `SET_TESSELLATION_PARAMETERS`, LOD registers, `SET_PATCH`) | MISSING | Stages ignored (`3d.cpp:1135-1155`); registers unreferenced | `clb197.h:435-466, 1046-1047`; `E/renderer_vulkan/vk_pipeline_cache.cpp:196-226`; `E/renderer_vulkan/fixed_pipeline_state.cpp:122-127` | |
| F4 | Geometry shader | PARTIAL | `3d.cpp:1102-1129` + `H/renderer/shader_decompiler/decompiler.cpp` `findGeometryLayerSource` (VS writes the layer the GS forwards); all other geometry shaders are ignored | Native in Eden; `ESR` passthrough handling | The heuristic is verified by `testGeometryLayerForwarding`. Non-trivial geometry shaders render the raw VS output (wrong). |
| F5 | Transform feedback (`SET_STREAM_OUT_BUFFER_*`, `_CONTROL_*`, `_LAYOUT_SELECT`, `SET_STREAM_OUTPUT`, `SET_RASTER_INPUT`) | MISSING | Unreferenced | `clb197.h:539-571, 4111-4115`; `E/transform_feedback.cpp:19-121`; `E/macro.cpp` `HLE_TransformFeedbackSetup` | |
| F6 | `SET_RASTER_ENABLE` (rasterizer discard) | MISSING | Unreferenced (0xDF) | `clb197.h:534-537`; `E/renderer_vulkan/vk_rasterizer.cpp:1530-1537`; default 1 at `E/engines/maxwell_3d.cpp:124` | Bug B24. |

### G. Viewport, depth and clipping

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| G1 | Viewport scale/offset → Metal viewport, UPPER_LEFT origin | IMPLEMENTED_UNVERIFIED | `3d.cpp:784-862` (origin = offset − scale, size = 2·scale, then Y flip `:849-851`); `renderer.cpp:1394-1425` | `E/renderer_vulkan/vk_rasterizer.cpp:63-109` | Every running title exercises this. A negative Metal height implements the Vulkan-style mapping. |
| G2 | `SET_WINDOW_ORIGIN` LOWER_LEFT | PARTIAL | `3d.cpp:793-795` (negates `scale_y` only) | `E/renderer_vulkan/vk_rasterizer.cpp:79-84` (flip by `surface_clip.height`), `:126-133` (scissor flip) | Bug B18. |
| G3 | `SET_VIEWPORT_COORDINATE_SWIZZLE` | PARTIAL | `3d.cpp:797-820` (±X/±Y via sign; Z/W debug assert only) | `clb197.h:799-835`; `E/renderer_vulkan/vk_rasterizer.cpp:76-91` (y_negate only without NV extension) | NEG_X yields a negative Metal width. |
| G4 | `SET_VIEWPORT_Z_CLIP` (−W..W vs 0..W) | IMPLEMENTED_UNVERIFIED | `3d.hpp:174-177,281`; `3d.cpp:827-834`; VS remap `msl/emitter.cpp:717-720` (`emitExitReturn`) | `ESR/backend/spirv/emit_spirv_special.cpp:13-21,142-148`; `E/renderer_vulkan/vk_rasterizer.cpp:93-101` | The math is correct: near = offset_z − scale_z·(−1..1 mode). Only the ZeroToOne path is touched by a test. |
| G5 | Depth range from scale_z/offset_z | PARTIAL | `3d.cpp:831-838, 853-859` | `E/renderer_vulkan/vk_rasterizer.cpp:93-108` (clamped to [0,1] when unrestricted depth range is unsupported) | Bug B20: the [0,0] → [0,1] hack and the clip-min/max-Z fallback. No [0,1] clamp for Metal. |
| G6 | `SET_VIEWPORT_SCALE_OFFSET` = 0 | MISSING | `3d.cpp:790` `if (/*regs.viewport_transform_enabled*/ true) { // HACK` | `E/renderer_vulkan/vk_rasterizer.cpp:1183-1206, 1232-1252`; `ESR/ir_opt/position_pass.cpp:22-76` | Bug B19. |
| G7 | `SET_VIEWPORT_CLIP_CONTROL` (depth clamp, geometry clip, guardbands) | MISSING | Unreferenced (0x64F) | `clb197.h:3372-3399`; `E/renderer_vulkan/vk_rasterizer.cpp:1652-1667` | Bug B25. Metal `MTLDepthClipModeClamp` exists. |
| G8 | User clip distances (`SET_USER_CLIP_ENABLE` + VS ClipDistance0..7) | VERIFIED (pending run) | `3d.cpp:947,957-958`; `msl/emitter.cpp:426-427,749-756` (`getClipDistanceCount`, `emitExitReturn`) | `clb197.h:2673-2697`; `E/renderer_opengl/gl_rasterizer.cpp:915-929` | `testClipDistances`. Implemented during the audit. |
| G9 | `SET_USER_CLIP_OP` (CULL per plane) | MISSING | Unreferenced (0x650) | `clb197.h:3401-3425` | Metal has no cull distance. |
| G10 | 16 viewports + VS `ViewportIndex` output | VERIFIED (pending run) | All 16 set (`3d.cpp:2424-2427`; `renderer.cpp:1394-1425`); `msl/emitter.cpp:421-422`; pipeline topology class (`pipeline.cpp:35-50`; `3d.cpp:1172-1190`) | `E/renderer_vulkan/vk_rasterizer.cpp:1209-1222` | `testViewportAndLayerOutputs`. |
| G11 | VS/GS `Layer` output | VERIFIED (pending run) | `msl/emitter.cpp:419-420, 727-735` | Eden native | Same test, plus `testGeometryLayerForwarding`. |
| G12 | `SET_RT_LAYER`, `SET_OFFSET_RENDER_TARGET_INDEX`, `SET_VIEWPORT_MULTICAST` (+ ViewportMask attribute) | MISSING | Unreferenced (0x573, 0x47C, 0x401) | `clb197.h:2907-2911, 1927-1930, 1408-1411` | Eden also ignores these. `RT_LAYER.V` (layer when the shader doesn't select one) is ignored. |
| G13 | `INCREASE_SNAP_GRID_PRECISION`, `SET_VIEWPORT_PIXEL` centre, `SET_WINDOW_OFFSET_X/Y` | MISSING | Registers stored (`3d.hpp:239-244`), never used | `clb197.h:837-839, 3362-3365, 1191-1195` | Eden also ignores these. Benign in practice. |

### H. Scissor and window clipping

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| H1 | Per-viewport scissor enable and rect | IMPLEMENTED_UNVERIFIED | `3d.hpp:301-312`; `3d.cpp:864-876`; clamp to render pass `renderer.cpp:1407-1422` | `clb197.h:1197-1209`; `E/renderer_vulkan/vk_rasterizer.cpp:111-151` | Not flipped for LOWER_LEFT (B18). `max < min` wraps to a huge size, then is clamped, so the draw is not culled (Eden behaves the same way). |
| H2 | Window clip rectangles (`SET_WINDOW_CLIP_*`), clip-ID test | MISSING | Stored (`3d.hpp:262-271`), never used | `clb197.h:855-862, 3438-3447` | Eden also ignores these. |
| H3 | `SET_SURFACE_CLIP` as a hardware clip | MISSING | Used only as a render-target width hint (`3d.cpp:697-703`) | `E/renderer_vulkan/vk_rasterizer.cpp:1183-1252` | Needed for the LOWER_LEFT flip and the viewport-disabled path. |

### I. Rasterizer state

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| I1 | `OGL_SET_CULL`, `OGL_SET_CULL_FACE` (FRONT/BACK), `OGL_SET_FRONT_FACE`, `WINDOW_ORIGIN.FLIP_Y` | VERIFIED (game) | `3d.cpp:2402-2408`; `maxwell_to_mtl.cpp:370-389`; `renderer.cpp:886-896,1376-1378` | `clb197.h:3346-3360, 2599-2605`; `E/renderer_vulkan/vk_rasterizer.cpp:1713-1725` (Eden flips the front face for FLIP_Y) | `T/README-crash4-render.md:22`: verified in Crash 4 (FLIP_Y=1) and CTR (FLIP_Y=0). Hydra deliberately does **not** flip, unlike Eden. This only holds if Metal's facing test ignores the viewport sign; add a targeted test (backlog item 5). |
| I2 | Cull FRONT_AND_BACK | PARTIAL (wrong) | `maxwell_to_mtl.cpp:376-378` | Vulkan `VK_CULL_MODE_FRONT_AND_BACK` | Bug B15. |
| I3 | Front-face winding while culling is disabled | PARTIAL (wrong) | `renderer.cpp:886-896` | Eden always updates the front face (`E/renderer_vulkan/vk_rasterizer.cpp:1713`) | Bug B16. Affects two-sided stencil and `[[front_facing]]`. |
| I4 | `D3D_SET_CULL_MODE` | MISSING | Unreferenced (0x4C2) | `clb197.h:2170` | Eden doesn't consume it either. |
| I5 | `SET_FRONT_POLYGON_MODE` / `SET_BACK_POLYGON_MODE` | MISSING | Unreferenced (0x36B/0x36C) | `clb197.h:999-1009`; `E/renderer_vulkan/fixed_pipeline_state.cpp:121`; `E/renderer_opengl/gl_rasterizer.cpp:1048-1058` | Metal: `setTriangleFillMode(Lines)` only. |
| I6 | Polygon offset (`SET_DEPTH_BIAS`, `SLOPE_SCALE`, `CLAMP`, `POLY_OFFSET_POINT/LINE/FILL`) | PARTIAL | `3d.cpp:2410-2422`; `H/renderer/metal/command_buffer.cpp:207-217` | `E/renderer_vulkan/vk_rasterizer.cpp:1285-1328` (`units = depth_bias / 2`), `:1606-1638` (per-class enable); `E/renderer_opengl/gl_rasterizer.cpp:1304` | Bug B17. |
| I7 | Point size (`SET_POINT_SIZE`, `SET_ATTRIBUTE_POINT_SIZE`, VS PointSize) | VERIFIED (pending run) | `3d.cpp:948-950,959-966`; `msl/emitter.cpp:423-425, 737-745` | `clb197.h:2704-2706, 3340-3344`; `ESR/backend/spirv/emit_spirv_special.cpp:23-28` | `testPointSizeAndCoord`. Depends on `point_list`, which inherits bug B8. |
| I8 | Point-sprite coordinates and origin (`SET_POINT_SPRITE_SELECT.ORIGIN`) | VERIFIED (pending run) | `3d.cpp:949,969-971`; `msl/emitter.cpp:579-580, 680-687` | `clb197.h:2957-2964` (ORIGIN bit 2: BOTTOM=0, TOP=1) | Same test. The bit position matches. |
| I9 | `SET_POINT_SPRITE` enable, RMODE/TEXTUREn generation, `SET_POINT_CENTER_MODE`, AA points | MISSING | Unreferenced (0x548, 0x597, 0x596) | `clb197.h:2712-2715, 2957-2995, 3092-3100` | Legacy (fixed-function texture coordinate replacement). |
| I10 | Line width (aliased/smooth), line anti-aliasing | MISSING | Unreferenced (0x4EC/0x4ED/0x55C/0x83) | `clb197.h:244-247, 2607-2611, 2785`; `E/renderer_vulkan/vk_rasterizer.cpp:1461-1468` | Metal rasterizes 1-pixel lines only. |
| I11 | Line stipple, polygon stipple, poly smooth | MISSING | Unreferenced | `clb197.h:3108-3121, 3133-3136, 1011` | No Metal support. |
| I12 | `SET_PROVOKING_VERTEX` | MISSING | Unreferenced (0x5A1) | `clb197.h:3123-3126`; `E/renderer_vulkan/fixed_pipeline_state.cpp:145-166` | Metal has no control. |
| I13 | `SET_CONSERVATIVE_RASTER` | MISSING | Unreferenced (0x452) | `clb197.h:1836-1839`; `E/renderer_vulkan/vk_rasterizer.cpp:1539-1553` | No Metal support. |
| I14 | ZCULL (all `SET_ZCULL_*`), raster bounding box, fill-via-triangle | MISSING (benign) | Unreferenced | `clb197.h:319-372, 3449-3468` | Eden also ignores these; they are hardware optimizations. |
| I15 | Shader-visible front facing (0x3FC) and `SR_Y_DIRECTION` | VERIFIED (pending run) | `msl/emitter.cpp:577-578, 676-679, 1112-1113` | `ESR/backend/spirv/emit_spirv_context_get_set.cpp` (FrontFace; `EmitYDirection`) | `testFrontFacing`, `testSpecialRegisters`. Front facing inherits bug B16 (stale winding). |

### J. Inline-to-memory (I2M) on the 3D class

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| J1 | `LAUNCH_DMA` (layout, completion/semaphore, reduction) | MISSING | `H/engines/inline_base.cpp:11-16` (stub) | `clb197.h:160-191`; `E/engines/engine_upload.cpp:28-33` | |
| J2 | `LOAD_INLINE_DATA` | PARTIAL | `inline_base.cpp:18-47` | `E/engines/engine_upload.cpp:35-80` | Bug B26: partial words, pitch and block-linear unsupported. |

### K. Pushbuffer processing

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| K1 | GPFIFO opcodes (Inc, NonInc, IncOnce, immediate, GRP0/GRP2 tertiary, end segment) | IMPLEMENTED_UNVERIFIED | `H/pfifo.cpp:226-292` | `REFS/opengpudoc/clb06f.h:184-252`; `E/dma_pusher.cpp` | Macro flush keys on `header.method`, which is wrong for old-style tertiary headers (`pfifo.cpp:280-281`). |
| K2 | Host-class methods below 0x40 (semaphores, syncpoint, WFI, mem-op) | MISSING (outside 3D scope) | `H/gpu.cpp:43-93` routes them to the bound engine's register file | `clb06f.h:66-146`; `E/engines/puller.cpp:141-200` | Flagged only; this belongs to the PFIFO/host audit. |

---

## (b) Confirmed bugs (with code quotes)

Severity reflects likely impact on real games multiplied by how directly the code shows the defect. "Confirmed" means the code itself shows the deviation from `clb197.h` or Eden. Runtime impact still needs a test, but none of these depends on speculation about Hydra's code.

### B1. MME ExtractShiftLeft{Immediate,Register} use swapped operands — HIGH
`H/macro/driver_base.cpp:77-91` decodes `rA = GET_REG(11)` (src_a) and `rB = GET_REG(14)` (src_b). Then, in `H/macro/interpreter/driver.cpp:79-94`:
```cpp
u32 Driver::instExtractShiftLeftImmediate(u8 bA, u8 rA, u8 rB, u8 size) {
    const u32 mask = (1u << size) - 1;
    const u32 shift = getRegU32(rB) & 0x1f;
    return ((getRegU32(rA) >> shift) & mask) << bA;
}
u32 Driver::instExtractShiftLeftRegister(u8 rA, u8 bB, u8 rB, u8 size) {
    const u32 mask = (1u << size) - 1;
    const u32 shift = getRegU32(rB) & 0x1f;
    return ((getRegU32(rA) >> bB) & mask) << shift;
}
```
Eden's interpreter (`E/macro.cpp:487-503`) and JIT (`:951-971`) agree with each other:
```cpp
u32 dst = GetRegister(opcode.src_a); u32 src = GetRegister(opcode.src_b);
result = ((src >> dst) & mask) << bf_dst_bit;            // ExtractShiftLeftImmediate
result = ((src >> bf_src_bit) & mask) << dst;            // ExtractShiftLeftRegister
```
Hydra extracts from src_a and shifts by src_b. Eden extracts from src_b and shifts by src_a. Any NVN macro that uses these ops computes wrong method addresses or parameters.

### B2. MME interpreter state persists across macro calls — MEDIUM
`H/macro/driver_base.hpp:79` declares `u32 exit_after = invalid<u32>();`, which is never reset. `driver_base.cpp:115-120`:
```cpp
if ((instruction & EXIT_BIT) != 0u && !branched)
    exit_after = pc + 1;
return pc == exit_after;
```
`driver.cpp:5-8` resets only `pc` and `r1`:
```cpp
void Driver::executeImpl(u32 pc_, u32 param1) {
    pc = pc_;
    setRegU32(1, param1);
```
- Any later macro whose control flow passes through the previous call's exit delay-slot address terminates early there. One example: a multi-exit macro whose early-exit delay slot is also a branch target.
- Registers r2–r7, `carry`, and the method address and increment also leak between calls.
- Eden resets all of these (`E/macro.cpp:421-451`).

### B3. MME carry and borrow semantics wrong, with signed overflow UB — MEDIUM
`driver.cpp:31-50`:
```cpp
case AluOperation::Add: { i32 result = valueA + valueB; carry = static_cast<i8>(result < valueA); ... }
case AluOperation::Subtract: { i32 result = valueA - valueB; carry = static_cast<i8>(result < 0); ... }
case AluOperation::SubtractWithBorrow: { i32 result = valueA - valueB - carry; ... }
```
Eden (`E/macro.cpp:545-563`) computes `carry = (u64)a + b > 0xffffffff`, `sub: carry = (u64)a - b < 2^32` (no borrow), and `subb` subtracts `(carry ? 0 : 1)`.
- Signed `i32` overflow is undefined behaviour, so the compiler may fold `(a+b) < a` into `b < 0`.
- The carry is wrong whenever the low word crosses 0x80000000 or 0xFFFFFFFF. That is exactly the case for 40-bit GPU-address arithmetic with `ADDC`.

### B4. MME Read immediate is unsigned — LOW
`driver_base.cpp:46` defines `#define GET_IMM_U32() GET_DATA_U32(14, 18)` and `:93-97` passes it to `instRead`. Eden decodes it as signed (`E/macro.h:81` `BitField<14, 18, s32> immediate`).

### B5. Macro parameters lost for NonInc or multi-macro writes — LOW-MEDIUM
`3d.cpp:261-272`:
```cpp
if ((method & 0x1) == 0x0) { macro_driver->setIndex(index); macro_driver->loadParam1(arg); }
else { macro_driver->loadParam(arg); }
```
- A NonInc burst to the even method (`pfifo.cpp:259-262`) overwrites `param1` for each word and leaves the queue empty. `fetchParam()` on an empty `std::queue` is UB (`driver_base.hpp:51-58`).
- An incrementing burst crossing into the next macro's method silently switches `index`.
- Eden accumulates every word for the macro that started the burst (`E/engines/maxwell_3d.cpp:203-220`).

### B6. Nested macro calls corrupt the running macro — LOW
`DriverBase::send` (`driver_base.cpp:130-133`) calls `engine_3d.method()`. For methods ≥ 0xE00, that path ends in `ThreeD::macro()`, which pushes into the *executing* macro's parameter queue and changes its `index`. The nested macro never runs because only PFIFO calls `flushMacro`.

### B7. Register file 0x80 words short; shadow scratch 0x80–0xFF lost — MEDIUM if used
`3d.hpp:549` declares `u32 mme_scratch[0x80];` at 0xD00, so `sizeof(Regs3D)/4 == 0xD80`. `clb197.h:4156-4159` puts `SET_MME_SHADOW_SCRATCH(i)` at 0x3400–0x37FC (word 0xD00–0xDFF). Eden has `std::array<u32, 0x100> shadow_scratch` and `NUM_REGS = 0xE00` (`E/engines/maxwell_3d.h:66, 3029`).

The handler as of the 10:25 edit to `engine_base.hpp:66-79`:
```cpp
void writeReg(u32 reg, u32 value) {
    // Methods past the emulated register file are dropped: ...
    if (reg >= REG_COUNT) {
        ONCE(LOG_WARN(Engines, "Ignoring write to unemulated register 0x{:03x} ...", reg, value));
        return;
    }
```
`getReg` returns 0 when out of range (`:54-58`).
- **Before 10:25:** `ASSERT_DEBUG` only, so release builds overwrote the members following `regs` (`InlineBase::inline_data`, `sampler_memo`) on methods 0xD80–0xDFF. That memory corruption is now fixed.
- **Still wrong:** scratch values written to 0xD80–0xDFF are discarded, and MME `Read`s of them return 0 instead of the stored value. Any NVN macro that keeps state in the upper half of the shadow scratch would misbehave.

### B8. `BEGIN` topology field is 26 bits wide — LOW-MEDIUM
`3d.hpp:438-442`:
```cpp
struct {
    PrimitiveType primitive_type : 26;
    bool32 instance_next : 1;
    bool32 instance_ctrl : 1; // TODO: is the name correct?
} begin;
```
`clb197.h:3026-3044` defines OP as 15:0 and PRIMITIVE_ID as bit 24. Eden uses `BitField<0, 16, PrimitiveTopology>` (`E/engines/maxwell_3d.h:2096`). With PRIMITIVE_ID = UNCHANGED, `regs.begin.primitive_type` is not a valid enum value. As a result:
- `toMtlPrimitiveType` falls back to triangles.
- The index cache does not convert quads or fans.
- `point_list` is false, so no `[[point_size]]` is written (`3d.cpp:960-961`).
- The topology class is wrong for layered draws (`3d.cpp:1172-1190`).

### B9. Compact and inline draw methods silently dropped — HIGH where used
The method table at `3d.cpp:224-233` has no entries for any of these, so each one only writes a register:
- `DRAW_VERTEX_ARRAY_BEGIN_END_INSTANCE_FIRST/SUBSEQUENT` (0x485/0x486)
- `DRAW_INDEX_BUFFER{32,16,8}_BEGIN_END_INSTANCE_FIRST/SUBSEQUENT` (0x5F9–0x5FE)
- `DRAW_INLINE_INDEX` (0x57A), `DRAW_INLINE_INDEX2X16` (0x57C), `DRAW_INLINE_INDEX4X8` (0x4C1)
- `DRAW_AUTO` (0x48F)

Eden implements all of these (`E/engines/draw_manager.cpp:31-58, 127-171, 206-229`). The draws vanish without any log.

### B10. Index-buffer FIRST applied outside the snapshotted range — MEDIUM-HIGH for titles using FIRST
`3d.cpp:461-471`:
```cpp
gpu_vaddr_t index_buffer_ptr = tls_crnt_gmmu->unmapAddr(regs.index_buffer_addr);
u32 index_buffer_size = count * getIndexTypeSize(regs.index_type);
const auto range = ztd::Range<uptr>::fromSize(index_buffer_ptr, index_buffer_size);
```
`renderer.cpp:1028-1031`:
```cpp
const auto index_buffer_offset =
    static_cast<u64>(start) * engines::getIndexTypeSize(state.index_type) +
    state.index_buffer.getOffset();
```
The buffer-cache entry covers `[base, base + count·size)` (`buffer_cache.cpp:51-76, 140-181`), but the draw reads `[base + first·size, …)`. The consequences:
1. When FIRST > 0, the GPU reads past the uploaded snapshot (stale data or out-of-bounds MTLBuffer access), and that tail is never invalidation-checked.
2. Decoded buffers (u8, quads, fans) are generated from index 0, and the draw then offsets **into the decoded buffer**, past its end.

Eden uses `IndexStart() = start + first·size` (`E/engines/maxwell_3d.h:2240-2241`). FIRST is non-zero for NVN indirect indexed draws (`E/macro.cpp:172` `DrawIndex(..., parameters[3] /*first*/ ...)`) and for GL- or Vulkan-driver titles.

### B11. IndexCache decodes the guest buffer with the *output* index type — MEDIUM
`index_cache.cpp:143-154` chooses `out_type` from the **output count**:
```cpp
switch (out_count) {
case 0u ... 0xffu:      out_type = engines::IndexType::UInt16; break;
case 0x100u ... 0xffffu: out_type = engines::IndexType::UInt16; break;
default:                out_type = engines::IndexType::UInt32; break;
}
```
`:169` then passes it as the *input* type:
```cpp
#define DECODE(name) decode_##name(in_ptr, out_ptr, out_type, descriptor.count)
```
`GET_INDEX` (`:34-47`) reads `reinterpret_cast<type*>(in)[index]` using that same type. As a result:
- Indexed quads or fans with u8 indices, or u32 indices when there are fewer than 65536 output indices, read the wrong element size. u16 indices are misread once out_count exceeds 0xFFFF.
- On the plain u8 path, if `count > 0xFFFF`, `decode_u8_indices` writes u16 values while the draw declares u32.

### B12. Triangle-fan conversion is wrong — MEDIUM-HIGH where fans are used
`index_decoders.inc:13-23`:
```cpp
for (u32 i = 0; i < count; i++) {
    u32 idx; GET_INDEX(idx, i);
    if (idx % 2 == 0) idx = idx / 2;
    else idx = count - 1 - idx / 2;
    ADD_INDEX(idx);
}
```
- **Non-indexed** fans produce the zig-zag strip 0, n−1, 1, n−2, … This **reverses the winding**: a CCW quad (0,1,2,3) becomes strip triangles (0,3,1) and (1,3,2), both CW. Back-face culling then removes front-facing fans. The triangulation also differs, which is wrong for non-convex fans (pie or radial progress over 180°) and for 3D fans whose hub is not coplanar (cones, caps).
- **Indexed** fans apply the remap to index *values* instead of positions, which is garbage for anything other than identity indices.
- The correct conversion is a list of (v0, vi, vi+1).

### B13. Converted index buffers cached by address only — MEDIUM
`index_cache.cpp:156-159`:
```cpp
const auto hash_value = hash(descriptor);
auto& index_buffer = cache[hash_value];
if (index_buffer != nullptr)
    return BufferView{index_buffer};
```
The key is XxHash32 over `(type, primitive_type, range begin/end, count)` (`:187-197`). Contents are not part of the key, and there is no invalidation (`index_cache.hpp:17` "TODO: memory invalidation"). Index buffers that are rewritten in place (u8, quads, fans) use stale data. A 32-bit hash collision aliases unrelated buffers, and the cache grows without bound.

### B14. QuadStrip, Polygon, LineLoop, adjacency and patches rendered as triangle lists — MEDIUM
`maxwell_to_mtl.cpp:391-408`:
```cpp
default:
    LOG_NOT_IMPLEMENTED(MetalRenderer, "Primitive type {}", primitive_type);
    return MTL::PrimitiveTypeTriangle;
```
`index_cache.cpp:106-128` converts only Quads and TriangleFan. Eden generates quad-strip indices (`E/renderer_vulkan/vk_buffer_cache.cpp:311-330`) and maps Polygon to a fan (`E/renderer_vulkan/maxwell_to_vk.cpp:373-376`).

### B15. Cull FRONT_AND_BACK culls only front faces — LOW-MEDIUM
`maxwell_to_mtl.cpp:376-378`:
```cpp
case engines::CullFaceMode::FrontAndBack:
    LOG_NOT_IMPLEMENTED(MetalRenderer, "Cull face mode front and back");
    return MTL::CullModeFront;
```
Back faces are drawn when nothing should be.

### B16. Front-face winding goes stale when culling is disabled — MEDIUM
`renderer.cpp:886-896`:
```cpp
if (!enabled) {
    state.cull_mode = MTL::CullModeNone;
    return;                       // front_face_winding not updated
}
state.cull_mode = toMtlCullMode(mode);
state.front_face_winding = toMtlWinding(front_face);
```
`renderer.cpp:1377-1378` always applies `setFrontFaceWinding(state.front_face_winding)`. Metal uses that winding for front/back two-sided stencil selection and for `[[front_facing]]`. Hydra now reads `[[front_facing]]` in the fragment shader (I15), so stencil shadow volumes and two-sided lighting with culling off inherit the previous draw's winding.

### B17. Constant depth bias not halved, and only the FILL enable is honoured — MEDIUM
`3d.cpp:2410-2421`:
```cpp
if (regs_raw[REG_POLY_OFFSET_FILL] != 0) {
    gpu.getRenderer().setDepthBias(
        std::bit_cast<f32>(regs_raw[REG_DEPTH_BIAS]),
        std::bit_cast<f32>(regs_raw[REG_SLOPE_SCALE_DEPTH_BIAS]),
        std::bit_cast<f32>(regs_raw[REG_DEPTH_BIAS_CLAMP]));
```
- Eden: `float units = regs.depth_bias / 2.0f;` (`E/renderer_vulkan/vk_rasterizer.cpp:1289`). GL: "Hardware divides polygon offset units by two" (`E/renderer_opengl/gl_rasterizer.cpp:1304-1305`). Metal's `depthBias` uses the same units as Vulkan, so Hydra applies **twice** the constant bias.
- The enable should come from POINT, LINE or FILL according to the topology class (`E/renderer_vulkan/vk_rasterizer.cpp:1606-1638`). Hydra uses FILL for everything.

### B18. LOWER_LEFT origin ignores the surface height; scissors not flipped — MEDIUM for lower-left titles
`3d.cpp:793-795,822-825,849-851`:
```cpp
if (any(regs.window_origin_flags & engines::WindowOriginFlags::LowerLeft))
    scale_y = -scale_y;
...
res.rect.origin.y() = transform.offset_y - scale_y;   res.rect.size.y() = scale_y * 2.0f;
...
res.rect.origin.y() += res.rect.size.y();             res.rect.size.y() = -res.rect.size.y();
```
For LOWER_LEFT this yields a Metal viewport with `originY = ty − sy` and `height = 2·sy`, so `y_w = ty − sy·y_ndc`. Hardware maps window y to memory row H − y (H = `SET_SURFACE_CLIP` height, `clb197.h:1386-1393`), so the correct value is `originY = H − ty − sy`.
- Hydra is exact only when `ty = H/2`, that is, a vertically centred viewport. Viewports at (0,0,w,h) with h < H are placed at the top instead of the bottom.
- Eden flips by `surface_clip.height` (`E/renderer_vulkan/vk_rasterizer.cpp:79-84`) and flips scissors (`:126-133`). `getScissor` (`3d.cpp:864-876`) does not flip.

### B19. Disabling the viewport transform is ignored — MEDIUM-LOW
`3d.cpp:790`: `if (/*regs.viewport_transform_enabled*/ true) { // HACK`. With `SET_VIEWPORT_SCALE_OFFSET = 0`, the guest outputs window coordinates. Eden uses the surface clip as viewport and scissor and rewrites the VS position to `x·2/W − 1` (`E/renderer_vulkan/vk_rasterizer.cpp:1183-1206, 1232-1252`; `ESR/ir_opt/position_pass.cpp:53-74`). Hydra applies stale or zero scale/offset instead.

### B20. Depth-range hacks — LOW
`3d.cpp:831-838,853-859`:
```cpp
if (transform.scale_z != 0.0f || transform.offset_z != 0.0f) { ... } else {
    res.depth_near = extent.near;   // VIEWPORT_CLIP_MIN_Z: a clamp range, not the depth range
    res.depth_far = extent.far;
}
...
if (res.depth_near == 0.0f && res.depth_far == 0.0f) { res.depth_near = 0.0f; res.depth_far = 1.0f; }
```
A legitimate [0,0] range (scale_z = offset_z = 0, used to force geometry to the near plane) becomes [0,1] or the clip range. Values outside [0,1] are not clamped for Metal (Eden clamps: `E/renderer_vulkan/vk_rasterizer.cpp:104-107`). The hack exists because Hydra never initializes the depth range to 0..1 at power-on, unlike Eden (`E/engines/maxwell_3d.cpp:61-64`).

### B21. SSCALED attributes treated as unsigned — LOW-MEDIUM
`codegen/helper.hpp:17-19`:
```cpp
case engines::VertexAttribType::Sscaled:
    return ir::ScalarType::U32; // TODO: use float if the Renderer backend supports scaled attributes
```
`lang/emitter.cpp:151-153` then emits `writeStatement("{} = as_type<uint>((float){})", attr, qualified_name);`. The Metal format is signed (`maxwell_to_mtl.cpp:577-604`, `VertexFormatShort` and so on), but the shader input is declared `vec<u32,4>`, so −1 converts to 4294967295.0f. Eden uses SignedScaled → `OpConvertSToF` (`E/renderer_vulkan/vk_pipeline_cache.cpp:135-137`).

### B22. Missing vertex sizes, and `getVertexFormatSize` falls off the end — LOW
`H/engines/const.hpp:77-99` lacks G8R8 (0x32), X8B8G8R8 (0x33) and A8 (0x34). These are defined at `clb197.h:1855-1865` and supported by Eden (`E/renderer_vulkan/maxwell_to_vk.cpp:398-410`). They fall into `default: return MTL::VertexFormatInvalid`, so the pipeline fails and the draw is skipped. `H/renderer/const.cpp:892-925` (`getVertexFormatSize`) has no default return, which is UB.

### B23. Primitive-restart registers ignored — LOW-MEDIUM
Registers 0x591/0x592/0x37A are never read. Metal always restarts strips on the maximum index value and never otherwise. As a result:
- Disabled restart with a real 0xFFFF vertex breaks.
- A custom restart index is not remapped.
- u8 restart 0xFF is lost by the u8→u16 conversion (`index_cache.cpp:12-15`).

Eden's GL backend honours the index (`E/renderer_opengl/gl_rasterizer.cpp:958-961`). Its Vulkan backend supports only the maximum index, like Metal.

### B24. Rasterizer discard ignored — MEDIUM where used
`SET_RASTER_ENABLE` (word 0xDF, `clb197.h:534-537`) is never read. Draws issued with rasterization off (stream-out passes) rasterize into the bound render targets. Eden discards (`E/renderer_vulkan/vk_rasterizer.cpp:1530-1537`) and defaults the register to 1 (`E/engines/maxwell_3d.cpp:124`).

### B25. Depth clamp (VIEWPORT_CLIP_CONTROL) ignored — MEDIUM
Word 0x64F is never read, and Metal clips depth by default. Eden enables depth clamp unless `geometry_clip` is Passthrough, FrustumXYZ or FrustumZ (`E/renderer_vulkan/vk_rasterizer.cpp:1652-1667`). Shadow-map "pancaking" casters behind the near plane are lost.

### B26. I2M on the 3D class — MEDIUM where used
`inline_base.cpp:11-16`: `LAUNCH_DMA` is a stub. `:22-24`:
```cpp
if (inline_data.size() * sizeof(u32) ==
    static_cast<usize>(regs.line_length_in) * static_cast<usize>(regs.line_count)) {
```
- A transfer whose byte count is not a multiple of 4 never flushes and pollutes the next transfer.
- `pitch_out` and block-linear destinations are ignored (`:29-40` does a contiguous `memcpy`).
- Eden handles partial words, pitch and swizzling (`E/engines/engine_upload.cpp:35-80`).

### B27. Table-dispatched methods don't store their argument — LOW
`DEFINE_METHOD_TABLE` (`engine_base.hpp:10-22`) calls the handler *instead of* `writeReg`. So `DRAW_VERTEX_ARRAY`, `DRAW_INDEX_BUFFER`, `CLEAR_SURFACE`, `SET_REPORT_SEMAPHORE_D`, `LOAD_CONSTANT_BUFFER*` and `BIND_GROUP*` are never stored, and MME `Read` returns 0 or a stale value for them. Eden stores every method before processing it (`E/engines/maxwell_3d.cpp:401-405`).

### B28. Vertex-attribute stream index can overflow host arrays — LOW (latent)
`3d.cpp:1194-1208` does `bool vertex_arrays_used[VERTEX_ARRAY_COUNT]; ... vertex_arrays_used[state.buffer_id] = true;`, and `pipeline.cpp:88-91` does `min_vertex_array_strides[vertex_attrib_state.buffer_id]`. `buffer_id` is a 5-bit field (0–31) but both arrays hold 16 entries. A garbage or "B-stream" attribute writes out of bounds on the stack.

### Divergences that are not counted as bugs
- **Draw timing.** Hydra draws when the count register is written; Eden draws at `END` (`E/engines/draw_manager.cpp:187-217`). Equivalent for NVN's BEGIN/first/count/END sequence.
- **Instance splitting.** Correct after today's changes, but performance scales with the instance count (backlog item 10).
- **Scissor `max < min` wrap.** Same as Eden (`E/renderer_vulkan/vk_rasterizer.cpp:140-143`).
- **FLIP_Y front face.** Opposite to Eden, but verified in game (row I1). Only valid while Metal's facing test is independent of the viewport sign.

---

## (c) Register-offset check (Hydra `Regs3D` vs `clb197.h`)

The script `audit/regs_layout_check.py` (run against `REFS/opengpudoc/clb197.h`) models every field and padding block of `H/engines/3d.hpp:212-550` and `H/engines/inline_base.hpp:21-38`. Result: **0 offset mismatches across 143 named fields.**

Fields checked: I2M block 0x60–0x6D, render targets 0x200, viewport transforms 0x280, viewports 0x300, window rects 0x340, `VERTEX_ARRAY_START`/`DRAW_VERTEX_ARRAY`/`VIEWPORT_Z_CLIP` 0x35D–0x35F, clear values, `ITERATED_BLEND` 0x374, scissors 0x380, ZT 0x3F8–0x3FC, surface clip 0x3FD, vertex attributes 0x458/0x468, `CT_SELECT` 0x487, ZT size 0x48A–0x48C, depth/blend state, `WINDOW_ORIGIN` 0x4EB, global base vertex/instance 0x50D/0x50E, `ZT_SELECT` 0x54E, TSC/TIC pools, program region / attribute default / `END` / `BEGIN` 0x582–0x586, index buffer 0x5F2–0x5F8, stream instance 0x620, OGL cull 0x646–0x648, `VIEWPORT_SCALE_OFFSET` 0x64B, `CT_WRITE` 0x680, report semaphore 0x6C0–0x6C3, vertex streams 0x700, per-target blend 0x780, stream limits 0x7C0, pipelines 0x800, CB selector 0x8E0–0x8F3, bindless 0x982, MME scratch 0xD00. The raw-indexed registers in `3d.cpp` also match: stencil 0x4E0–0x4E7, 0x565–0x569, 0x3D5–0x3D7; depth bias 0x372/0x55B/0x56F/0x61F; and the new constants 0x544/0x546/0x581/0x644.

Remaining layout and bitfield discrepancies:

| Item | Hydra | clb197.h / Eden | Effect |
|---|---|---|---|
| Register-file size | `mme_scratch[0x80]` → 0xD80 words (`3d.hpp:549`) | Shadow scratch 0x100 words (0xD00–0xDFF), `NUM_REGS` 0xE00 | Scratch 0x80–0xFF dropped or read as 0 (B7). Out-of-bounds access was fixed at 10:25. |
| `BEGIN.OP` width | 26 bits (`3d.hpp:439`) | OP 15:0; PRIMITIVE_ID 24:24; SPLIT_MODE 30:29 (`clb197.h:3026-3053`) | B8. |
| `BEGIN` bits 26–27 names | `instance_next`, `instance_ctrl` | INSTANCE_ID 27:26 (FIRST=0, SUBSEQUENT=1, UNCHANGED=2) | Cosmetic. `beginDraw` uses `extractBits(arg, 26, 2)` correctly. |
| `SET_VIEWPORT_CLIP_HORIZONTAL/VERTICAL` field types | `i16 x, width` / `i16 y, height` (`3d.hpp:249-256`) | Unsigned 15:0 / 31:16 (`clb197.h:841-847`) | Signedness only; unused. |
| `SET_COLOR_TARGET_THIRD_DIMENSION` | `struct { u16 layers; bool volume : 1; }` (`3d.hpp:163-166`) | V 27:0 (`clb197.h:767-768`); the depth/array flag is `MEMORY.THIRD_DIMENSION_CONTROL` bit 16 | Misnamed field, limited to 16 bits; outside front-end scope. |
| `SET_INDEX_BUFFER_A`, `SET_*_A` upper-address words | Full 32 bits used | `ADDRESS_UPPER` 7:0 | Harmless unless the guest sets the high bits. |
| `0x374` name | `advanced_blend_enabled` | `SET_ITERATED_BLEND` | Name only. |
| `0x35F` | Whole word compared with the enum | RANGE is bit 0 | Harmless. |

---

## Metal constraints and standard emulation, per missing or partial feature

| Feature | Metal support | Standard technique (Eden / MoltenVK / other Metal ports) |
|---|---|---|
| Quads, quad strips, polygons, fans, line loops | None (point, line, line-strip, triangle, triangle-strip only) | Generate index lists: quad {0,1,2,0,2,3}; quad strip {0,3,1,0,2,3} per 2 vertices; fan and polygon (0,i,i+1); line loop = strip + first index. Indexed variants are expanded on the CPU or in a compute pass (Eden QuadIndexedPass). |
| Primitive restart toggle / custom index | Always on for strips with 0xFFFF/0xFFFFFFFF; no toggle | Rewrite indices: custom→max when enabled; when disabled and the maximum value occurs, promote u16→u32 or split the draw. Convert u8 0xFF→0xFFFF. |
| u8 indices | No `MTLIndexTypeUInt8` | Convert to u16 (Hydra does). |
| Geometry shaders | None | Passthrough/layer-only geometry shaders fold into the VS (Hydra's heuristic). General geometry shaders: Metal 3 object/mesh shaders, or a compute pre-pass writing a vertex buffer plus indirect draw. |
| Tessellation | Fixed-function tessellator; factors from a compute kernel; TES = post-tessellation vertex function `[[patch(...)]]` | MoltenVK-style multistage: VS as compute (vertex pulling) → TCS as compute (writes `MTLTriangle/QuadTessellationFactorsHalf` + patch data) → `drawPatches` with the TES as the post-tessellation vertex function. |
| Transform feedback / `DRAW_AUTO` | None | The vertex function writes varyings to device buffers using the `STREAM_OUT_LAYOUT_SELECT` mapping (`E/transform_feedback.cpp`). Unroll indexed and strip draws to lists so outputs are in primitive order. `DRAW_AUTO` = draw `byte_count / stride` vertices. |
| Rasterizer discard | `rasterizationEnabled = NO` (needs a void-returning vertex function) | Skip the draw, or use a void-VS pipeline variant when transform-feedback emulation is active. |
| Polygon mode | `setTriangleFillMode(.lines)` only, no point fill, same for front and back | LINE: fill mode Lines when front == back. POINT, or front ≠ back: generate a point/line list from the triangle indices. |
| Provoking vertex | No control | Rotate indices per primitive for lists. Strips/fans: convert to lists first. Or pass flat attributes through a per-primitive buffer. |
| Line width, smooth lines, stipple | 1-pixel lines only; no stipple | Expand each line into a screen-space quad (instanced quad + vertex pulling). Stipple via fragment `discard` with the pattern. |
| Conservative raster | None | Usually ignore. Approximate via geometry expansion if a title depends on it. |
| Depth clamp | `setDepthClipMode(MTLDepthClipModeClamp)` | Direct mapping (Eden's geometry_clip rule). |
| Depth range | znear/zfar expected in [0,1]; znear > zfar allowed | Clamp, or remap in the VS for unrestricted ranges. |
| Multi-viewport / layer from VS | `setViewports` (16) + `[[viewport_array_index]]` / `[[render_target_array_index]]`; needs `inputPrimitiveTopology` | Implemented today; topology class wired in `pipeline.cpp:35-50`. |
| Clip / cull distances | `[[clip_distance]]` (8); no cull distance | Clip implemented. Cull: rarely needed; can be approximated in a GS/compute path. |
| Point size / sprite coordinate | `[[point_size]]`, `[[point_coord]]` (origin top-left) | Implemented today; flip for ORIGIN=BOTTOM. |
| Base vertex / base instance | Supported (`drawIndexedPrimitives` baseVertex/baseInstance) | Used. |
| Instance divisors | `MTLVertexStepFunctionPerInstance` + `stepRate` | Hydra emulates per-draw. When batching instances, use stepRate = divisor and keep the shader `InstanceId = instance_id − base_instance`. |
| GPU-sourced indirect draws | `drawPrimitives(indirectBuffer:)`, ICBs | HLE the NVN indirect macros (Eden list) to bind the guest indirect buffer instead of reading the arguments on the CPU. |
| Depth bounds test (noted for completeness; ROP scope) | None | Fragment-shader discard against a sampled depth copy. |

---

## (d) Prioritized implementation backlog

Ordering: correctness impact × likelihood in NVN titles ÷ effort. "Test" refers to Hydra-style Metal GPU tests that render and read back (`T/shader_library_tests.cpp`, `T/shader_coverage_tests.cpp`, `T/raster_state_tests.cpp`).

**Missing test harness.** Recommended first: a small **ThreeD replay harness** in `T/` built on the existing pieces:
- `GMmu` mapping host buffers (as in `T/render_hash_tests.cpp:132-135`).
- The Metal renderer.
- Guest shader binaries assembled the way `testGeometryLayerForwarding` builds raw instructions.

It would push 3D methods through `ThreeD::method()` and read back the colour/depth target. Most items below are testable only through such a harness.

### P0: correctness, low effort

**1. MME correctness pack (B1–B4, B6, A13).**
- **Why:** every NVN draw, constant-buffer bind and indirect draw passes through macros. Wrong bitfield extraction or a carry error corrupts method addresses, parameters, or GPU VAs.
- **How:**
  - `H/macro/interpreter/driver.cpp`: swap the operands in `instExtractShiftLeftImmediate` and `instExtractShiftLeftRegister` to match `E/macro.cpp:487-503`; implement carry/borrow with `u64` math (`:543-563`).
  - `driver_base.cpp`: decode the Read immediate as signed.
  - Add `DriverBase::reset()` called from `execute()` that clears registers, carry, method/increment, `exit_after` and `branch_after`.
  - Queue nested macro calls (send to ≥ 0xE00) and run them after the current macro, or forbid them with a log.
- **Test:** a unit test with a small engine interface (refactor `DriverBase` to depend on an `IMacroTarget { method(); getReg(); }` instead of `ThreeD&`). Assemble MME words with Eden's `Opcode` layout (`E/macro.h:69-102`) and compare the emitted (method, value) sequence against a port of Eden's `Step()` used as the oracle. Cover multi-exit and delay-slot exit cases.
- **Risk:** low. Behaviour converges to two independent Eden implementations. The macro semantics change globally, so validate CTR, Crash 4 and Jump Force after the change.

**2. Register file to 0xE00 words (B7).**
- **How:** `H/engines/3d.hpp`: `u32 mme_scratch[0x100];` plus `static_assert(sizeof(Regs3D) == 0xE00 * 4)`. Release-safe `writeReg`/`getReg` already landed at 10:25 (`engine_base.hpp:54-79`).
- **Test:** write method 0xDFF through `ThreeD::method` and read it back with `getReg`; MME Read at 0xD9B.
- **Risk:** low. The object layout changes, so a full restart is needed (not a hot reload, see `T/README-hot-debug.md`).

**3. Implement the dropped draw methods (B9, rows B5–B8).**
- **How:** add table entries in `3d.cpp:224-233`.
  - 0x485/0x486 and 0x5F9–0x5FE decode `first 15:0, count 27:16, topology 31:28` (`clb197.h:1963-1966, 3207-3210`). Set the instance index (FIRST = 0, SUBSEQUENT = +1) and call shared helpers `drawArrays(topology, first, count)` / `drawIndexed(topology, first, count, type)` refactored out of `drawVertexArray`/`drawVertexElements`.
  - Inline index (0x57A/0x57C/0x4C1, honouring `_ALIGN` start/count) accumulates into a host vector. Flush it at `END` (0x585, new table entry) as a u32 index draw (`E/engines/draw_manager.cpp:127-171, 206-215`).
  - `DRAW_AUTO` falls back to `byte_count / stride` vertices (`E/macro.cpp` `HLE_DrawIndirectByteCount::Fallback`).
- **Test:** replay harness drawing a 2-triangle mesh through each method, checking coverage per method and index size.
- **Risk:** medium. `END` becomes stateful; make sure a normal BEGIN/DRAW/END does not double-draw.

**4. Index FIRST and limit handling (B10, D5).**
- **How:** `3d.cpp` `drawVertexElements`: start the range at `addr + first*size` with length `count*size`, decode from there, and call `drawIndexed(..., start = 0, ...)`. Alternatively keep the offset but extend the range to `(first + count)*size`. Clamp to the limit address when it is valid.
- **Test:** replay harness with one index buffer holding two triangles and FIRST = 3; only the second triangle may render. Repeat for u8 and quads.
- **Risk:** low.

**5. Culling and winding fixes (B15, B16) plus a FLIP_Y matrix test.**
- **How:** `renderer.cpp` `setCullStateHot26`: always store `front_face_winding`. In `3d.cpp` `drawInternal`: when the mode is FRONT_AND_BACK and the topology class is triangle, skip the draw but still count it for queries.
- **Test:**
  - Two-sided stencil with culling disabled (front writes ref 1, back writes ref 2), checking both triangles' stencil after a draw that previously enabled CW culling.
  - FRONT_AND_BACK produces no fragments.
  - Add a {UPPER_LEFT, LOWER_LEFT} × {sy > 0, sy < 0} × {FLIP_Y 0/1} winding matrix to pin down the game-verified convention in row I1.
- **Risk:** low.

**6. `BEGIN` topology bitfield (B8).**
- **How:** `3d.hpp:439` → `PrimitiveType primitive_type : 16; u32 : 8; bool32 primitive_id_unchanged : 1; u32 : 1; u32 instance_id : 2; u32 : 1; u32 split_mode : 2;`
- **Test:** static decode test next to `T/render_hash_tests.cpp:113`.
- **Risk:** very low.

**7. Depth bias (B17).**
- **How:** `3d.cpp:2410-2422`: constant / 2; choose the POINT/LINE/FILL enable from the topology class using Eden's LUT (`E/renderer_vulkan/vk_rasterizer.cpp:1606-1638`).
- **Test:** D32F readback of a sloped triangle with known units, slope and clamp, compared against the Metal formula.
- **Risk:** low to medium. Shadows and decals visibly change; compare CTR and Crash 4 before and after.

### P1: correctness, moderate effort

**8. Rasterizer discard and safe handling of unsupported stages (B24, F2/F3).**
- **How:**
  - In the `ThreeD` constructor, set `regs_raw[0xDF] = 1` (power-on default, see A16).
  - In `drawInternal`, return early when it is 0 (unless transform-feedback emulation exists).
  - Also skip draws with TessCtrl/TessEval enabled, or a geometry shader that is not a recognised layer forward, instead of rendering garbage. Keep a counter or log of skipped draws.
- **Test:** a draw with raster disabled leaves the render target untouched.
- **Risk:** low with the default set. Skipping unsupported-stage draws could hide content that currently renders "close enough"; gate it behind a config option at first.

**9. IndexCache rewrite (B11–B14, B23; rows C2–C7, D1–D3).**
- **How:** in `H/renderer/index_cache.cpp` and `index_decoders.inc`:
  - Separate input and output types (template the decoders on `<InT, OutT>`).
  - Fan and polygon → list (0,i,i+1).
  - Quad strip per Eden (`E/renderer_vulkan/vk_buffer_cache.cpp:311-330`).
  - Line loop → strip + closing index.
  - Adjacency → drop the adjacent vertices (list) when no geometry shader is present.
  - Restart: remap a custom index to the type maximum; promote u8 0xFF → 0xFFFF when restart is enabled.
  - Caching: either regenerate into a transient upload buffer every draw (simplest; index data is small), or key the cache on a content hash combined with the buffer cache's invalidation range. Remove the unbounded `std::map`.
  - Map converted topologies in `maxwell_to_mtl.cpp`.
- **Test:** for every topology × {non-indexed, u8, u16, u32} × {FIRST 0/3}, render with back-face culling enabled and check coverage. Include a non-convex fan (a 270° pie).
- **Risk:** medium. Touches every converted draw; watch CPU cost.

**10. Instance batching (performance; row B14).**
- **How:**
  - Defer draws while `BEGIN.INSTANCE_ID = SUBSEQUENT`, like Eden's `DrawMode::Instance` (`E/engines/draw_manager.cpp:75-102, 173-217`), and flush on the next FIRST, at macro end, or on any state write.
  - Emit one Metal draw with `instanceCount = N` and `baseInstance = GLOBAL_BASE_INSTANCE_INDEX`.
  - Revert instanced streams to `MTLVertexStepFunctionPerInstance` with `stepRate = divisor` (`pipeline.cpp:105-118`).
  - In the shader: `iid = instance_id - base_instance` (MSL `[[base_instance]]`).
- **Test:** replay an instanced draw (N = 64, divisor 2, base instance 5) and compare its render with the current per-instance path; also check the draw count via `PerfStats`.
- **Risk:** medium-high. Deferred-draw semantics and state changes between instances. Keep per-instance splitting as a fallback when registers change between BEGINs.

**11. Depth clamp and depth-range clamp (B25, B20).**
- **How:** add renderer state `depth_clip_mode` and `CommandBuffer::setDepthClipMode` (`MTL::RenderCommandEncoder::setDepthClipMode`). Derive it from `VIEWPORT_CLIP_CONTROL.GEOMETRY_CLIP` per Eden (`E/renderer_vulkan/vk_rasterizer.cpp:1652-1667`). Clamp the viewport's znear/zfar to [0,1]. Initialize the power-on depth range to 0..1 so the [0,0] hack can be removed.
- **Test:** a triangle crossing z < 0 with clamp on or off, checking the stored depth values (0 versus clipped).
- **Risk:** medium. Eden's rule enables clamp when the register is 0, so verify that NVN writes this register before trusting the default.

**12. LOWER_LEFT origin and viewport-transform-disabled path (B18, B19; rows G2, G6, H3).**
- **How:** in `3d.cpp` `getViewport`/`getScissor`, use `H = screen_scissor.vertical.height`:
  - LOWER_LEFT: `originY = H − ty − sy`, `height = +2·sy`.
  - Flip the scissor to `[H − max_y, H − min_y]`.
  - When `viewport_transform_enabled == 0`: use the surface clip as viewport and scissor, and add an emitter flag that rewrites the VS output position `x' = x·2/W − 1`, `y' = y·2/H − 1`. W and H come from a small uniform; this mirrors `ESR/ir_opt/position_pass.cpp`.
- **Test:** LOWER_LEFT with a sub-rect viewport (0,0,w,h) on a taller target, checking the rows covered; a pre-transformed quad with the transform disabled.
- **Risk:** medium. It changes every lower-left title, so cross-check with Eden's formula on a GL-convention title.

**13. Vertex-format gaps (B21, B22; rows E7–E9, E11).**
- **How:**
  - `helper.hpp`: SSCALED → I32, so the conversion becomes signed.
  - Add sizes 0x32/0x33/0x34 to the enum (`H/engines/const.hpp`), `maxwell_to_mtl.cpp` (G8R8 → UChar2 with swizzle, X8B8G8R8 → UChar4, A8 → UChar) and `getVertexFormatSize` (with a default).
  - Fetch integer/scaled 1010102 as `uint` and unpack in the shader.
  - Apply BGRA by shader swizzle for formats other than uchar4.
- **Test:** a vertex-fetch matrix that renders each format's decoded value into an RGBA32F target.
- **Risk:** low.

**14. Shadow RAM and macro parameter accumulation (A4, A6; B5).**
- **How:** keep a shadow copy of the register array in `ThreeD`. In `methodHot`, implement Track/TrackWithFilter (store) and Replay (substitute) per `E/engines/maxwell_3d.cpp:274-283`. Make `ThreeD::macro` accumulate every word of a burst for the macro that started it (Eden `ProcessMacro`).
- **Test:** replay harness (MethodTrack, change register, MethodReplay, read back); NonInc macro call.
- **Risk:** low to medium.

**15. Topology override (row B17).**
- **How:** in `drawVertexArray`/`drawVertexElements`, apply `SET_PRIMITIVE_TOPOLOGY` when `SET_PRIMITIVE_TOPOLOGY_CONTROL = 1`, using Eden's mapping (`E/engines/draw_manager.cpp:251-274`).
- **Test:** replay harness.
- **Risk:** low.

**16. I2M fixes (B26).**
- **How:** in `inline_base.cpp`, size the transfer at `LAUNCH_DMA` (`line_length_in·line_count`, handling partial last words). Write each line at `offset_out + line·pitch_out` for pitch layout. For block-linear destinations, use the existing swizzle helpers with `dst_block_size/width/height/depth/origin`. Implement completion semaphore release.
- **Test:** I2M upload into a block-linear texture, then sample it; a 6-byte transfer followed by an 8-byte transfer.
- **Risk:** low.

### P2: larger features

**17. Polygon mode (row I5):** map LINE (front == back) to `setTriangleFillMode(Lines)`; use list generation for POINT or front ≠ back. Low risk.

**18. Provoking vertex LAST (row I12):** rotate per-primitive indices in the IndexCache when `SET_PROVOKING_VERTEX = LAST` and the shader has flat inputs. Low-medium risk.

**19. Transform feedback + `DRAW_AUTO` (rows F5, B8):**
- Emit buffer writes in the VS from the `STREAM_OUT_LAYOUT_SELECT` table (`E/transform_feedback.cpp:19-121`).
- Unroll indexed and strip draws to lists.
- Track the write pointer per buffer (`SET_STREAM_OUT_BUFFER_LOAD_WRITE_POINTER`).
- Combine with rasterizer discard.
- Medium-high risk.

**20. Wide lines and stipple (rows I10, I11):** instanced screen-space quads with vertex pulling. Medium risk.

**21. General geometry shaders (row F4):** Metal 3 mesh shaders, or a compute pre-pass. High effort.

**22. Tessellation (rows F3, C8):** MoltenVK-style compute pipeline. High effort.

**23. VertexA (row F2):** merge the A and B programs in the decompiler (`E/renderer_vulkan/vk_pipeline_cache.cpp` `MergeDualVertexPrograms`). Medium effort.

**24. GPU-sourced indirect draws (row B15):** HLE the NVN indirect macros by hash (Eden list `E/macro.cpp:393-408`), binding the guest indirect buffer for `drawPrimitives(indirectBuffer:)`. Medium effort.

**25. Low-value or benign registers (rows G12, G13, H2, I9, I13, I14, E12, B16, B18):** keep ignoring them; log once if they are ever set to non-default values.

---

## Appendix A: defects fixed by edits made during this audit (10:03–10:25 WIB)

Observed in the earlier file versions, verified fixed in the snapshot:
1. `getSvAccessFromAddr` (old `H/renderer/shader_decompiler/const.cpp`) mapped every address ≥ 0x80 below 0x2F8 to `UserInOut`. As a result:
   - VS clip-distance writes (0x2C0) became `user36`/`user37`. The MSL emitter then shifted `1u << output` (UB for ≥ 32) and referenced undeclared `__out.user36`, which would fail compilation.
   - Point-coord, legacy-varying and front-face (0x3FC → "VertexID") reads were garbage.

   It now maps PrimitiveID, Layer, ViewportIndex, PointSize, ClipDistance, PointCoord, TessCoord and FrontFacing explicitly (`const.cpp:17-46`).
2. Host draws passed `baseInstance = GLOBAL_BASE_INSTANCE_INDEX + instance_index`, so the shader `InstanceId` included the base, unlike Eden. They now pass `instance_index` (`3d.cpp:357-368, 489-491`), and instanced streams are offset to `base + instance/divisor` (`3d.cpp:1283-1320`).
3. Vertex-stream divisors were ignored (`step_rate = 1; // TODO: divisor`). They are now emulated per draw (`pipeline.cpp:105-112`).
4. Newly implemented, with tests in `T/shader_coverage_tests.cpp`:
   - Point size (fixed and program) and point-coord origin.
   - Masked clip distances.
   - VS layer and viewport-index outputs, with the pipeline input topology class.
   - Fragment front-facing, primitive ID, layer and viewport index.
   - `SR_Y_DIRECTION`.
5. `EngineWithRegsBase::writeReg`/`getReg` (`H/engines/engine_base.hpp:54-79`, edited at 10:25) now drop out-of-range writes and return 0 for out-of-range reads. Previously only `ASSERT_DEBUG` guarded them, so release builds corrupted memory on methods 0xD80–0xDFF. The functional gap that remains is B7.
