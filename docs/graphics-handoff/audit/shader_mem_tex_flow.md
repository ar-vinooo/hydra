# Hydra Maxwell (SM 5.3) shader translation audit — memory, texture/surface, control flow, warp/system, stage I/O

Read-only audit. Nothing in the repository was modified, built or run.

## 0. Scope, sources, conventions

**Paths used in citations**

| Short name | Location |
|---|---|
| `SD/` | `src/core/hw/tegra_x1/gpu/renderer/shader_decompiler/` (Hydra) |
| `GPU/` | `src/core/hw/tegra_x1/gpu/` (Hydra) |
| `T/` | `tests/` (Hydra) |
| `EDEN/` | `<scratchpad>/refs/yuzu/src/shader_recompiler/` (Eden, treated as ground truth) |
| `EDEN-I/` | `EDEN/frontend/maxwell/translate/impl/` |
| `NAK` | `build/mesa-sm50.rs` (Mesa NAK SM50 encoder) |
| `ENVY` | `build/gm107.c` (envytools GM107 disassembler tables) |

**The repository was being edited while this audit ran.** Between 10:03 and 10:22 (17 Oct 2026) another writer changed `SD/decoder/decoder.cpp`, `memory.cpp`, `move.cpp`, `const.hpp/.cpp`, `analyzer/memory_analyzer.*`, `codegen/lang/msl/emitter.cpp`, `GPU/engines/3d.cpp`, `GPU/renderer/shader_cache.hpp`, and added `T/shader_coverage_tests.cpp` (linked into `hydra-shader-library-tests`, `CMakeLists.txt:360-361`). All findings below were re-checked against a frozen copy taken at **10:22:47** (`<scratchpad>/snapshot2_102247/`). Line numbers refer to that copy. Items fixed during the audit are marked "fixed during audit". If the tree has moved again, re-check the line numbers. The defects in §(c) were re-verified in that copy.

**Classification**
- **VERIFIED**: decoded and emitted correctly, and a Metal test in `T/shader_library_tests.cpp` or `T/shader_coverage_tests.cpp` checks the result on the GPU. A test that builds IR directly checks only the emitter, and the row says so.
- **IMPLEMENTED_UNVERIFIED**: the code matches the reference semantics, but no test executes it.
- **PARTIAL**: implemented, but some variant or semantic is wrong or ignored. `(BUG)` marks a confirmed wrong result, as opposed to a missing feature.
- **MISSING**: not decoded, a stub, or treated as a no-op where semantics are required.
- **UNKNOWN**: depends on Metal compiler or runtime behaviour that cannot be confirmed without running.

**Architecture summary (relevant to every row)**
- Decoder (`SD/decoder/decoder.cpp:143-903`) is a priority list of mask/value patterns. Unknown or unimplemented instructions log and emit **no IR**, so destination registers keep stale values. There is no assertion.
- IR → MSL has no structurizer. Each guest basic block becomes an MSL function. `main` runs a `while(next != None) switch(next)` dispatcher with a 65536-transition guard (`SD/codegen/lang/emitter.cpp:195-249`). `SD/analyzer/cfg.cpp` exists but is unused (its call site is commented out at `emitter.cpp:218-223`).
- Guest state is a per-thread `State` struct holding `Reg r[256]`, `a_in[0x200]`, `a_out[0x200]`, `p[8]`, `cc[4]` and `lmem`, created uninitialised (`SD/codegen/lang/emitter.cpp:33-44`).
- SPH is parsed only for fragment shaders (interpolation imaps, omap depth). Vertex-stage SPH is ignored and inputs/outputs are inferred from IR (`SD/decompiler.cpp:96-133`, `SD/analyzer/memory_analyzer.cpp`).
- Metal render pipelines contain only VS (VertexB) and FS. Compute is a separate kernel. GS is replaced by a layer-forwarding heuristic. TCS/TES/VertexA are only logged (`GPU/engines/3d.cpp:1102-1152`).

---

## (a) Opcode × variant inventory

### a.1 Memory

| Opcode/variant | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| LD (generic, all sizes, .E, pred-out) | MISSING | `SD/decoder/decoder.cpp:522-524` logs "ld". Mask `0x8000/0xe000` also catches IMAD32I (`0x8000/0xfc00`) | `EDEN/frontend/maxwell/maxwell.inc:282`, `:272`; `EDEN-I/not_implemented.cpp:162` | Eden also lacks LD. Any implementation must test IMAD32I first. |
| ST (generic) | MISSING | `decoder.cpp:519-521` | `maxwell.inc:283`; `not_implemented.cpp:246` | Eden also lacks ST. |
| LDG.B32 (32-bit address) | MISSING | `decoder.cpp:256` → `SD/decoder/memory.cpp:167-213`. Constant 0 written at `memory.cpp:200-204` | `EDEN-I/load_store_memory.cpp:48-78,81-133`; NAK 2595-2617 | A stub that **writes 0**: wrong data rather than a log-only gap. |
| LDG.U8/S8/U16/S16 | MISSING | `memory.cpp:191-194` (log) + zeros | `load_store_memory.cpp:94-105` | |
| LDG.B64/B128/U.128 | MISSING | `memory.cpp:180-186` | `load_store_memory.cpp:109-129` | |
| LDG.E (64-bit register pair) | MISSING | `SD/decoder/memory.hpp:94` decodes `e` but nothing uses it (`memory.cpp:166` "TODO: cache_op, extended") | `load_store_memory.cpp:57-67` | |
| LDG cache ops CA/CG/CI/CV | MISSING | `memory.hpp:81-86,95` decoded, unused | `load_store_memory.cpp:33-38` | No host meaning once LDG exists. |
| LDG with predicate output (0xEEC8) | MISSING | `decoder.cpp:257-259` | ENVY 1900 | Not in Eden's table. |
| STG (all sizes, .E, cache ops) | MISSING | `decoder.cpp:253-255` | `load_store_memory.cpp:135-180`; NAK 2652-2672 | Writes are dropped. |
| Global pointer → SSBO resolution (NVN descriptors in c[0][0x100..0x700]) | MISSING | No IR op (`SD/ir/instruction.hpp:7-103`). `STORAGE_BUFFER_BINDING_COUNT` unused (`GPU/const.hpp:757`). "TODO: storage buffers" at `SD/codegen/lang/msl/emitter.cpp:198,503-504,599-600` | `EDEN/ir_opt/global_memory_to_storage_buffer_pass.cpp:23-43,336-412,415-433` | Blocks every SSBO: compute particles, Unity/UE4 structured buffers, NVN first-party compute. |
| SSBO size/bounds | MISSING | — | same pass (descriptor size) | |
| LDS.U8/S8/U16/S16 | IMPLEMENTED_UNVERIFIED | `decoder.cpp:247`; `SD/decoder/compute.cpp:76-105`; `msl/emitter.cpp:1178-1181` | `EDEN-I/load_store_local_shared.cpp:130-151` | Shares the tested LDL path but uses different storage. No LDS test. |
| LDS.B32/B64/B128 (.U flag) | IMPLEMENTED_UNVERIFIED | `compute.cpp:106-121` | `load_store_local_shared.cpp:130-151` | `.U` (bit 44) is ignored, which is fine. |
| STS.U8/S8/U16/S16 | PARTIAL | `compute.cpp:84-92`: non-atomic load, mask, store | `load_store_local_shared.cpp:193-202` (backend uses atomic CAS or 8/16-bit shared views) | **Race**: concurrent stores to different bytes of one word lose updates. |
| STS.B32/B64/B128 | IMPLEMENTED_UNVERIFIED | `compute.cpp:108-111`; `msl/emitter.cpp:1183-1188` | `load_store_local_shared.cpp:193-218` | |
| Shared-memory size and bounds | PARTIAL | `msl/emitter.cpp:248-250` clamps to 0x2000 words (32 KiB). `:1179,1185` use `min(addr>>2, N-1)` | GM20B allows 48 KiB per CTA | Out-of-bounds accesses alias the last word instead of being dropped. Guest allocations over 32 KiB are truncated (a Metal limit). |
| LDL.U8/S8/U16/S16 | VERIFIED | `decoder.cpp:248`; `compute.cpp:93-105`; `msl/emitter.cpp:1190-1193`; `T/shader_library_tests.cpp:231-234,258-261` | `load_store_local_shared.cpp:98-113` | |
| LDL.B32/B64 | VERIFIED | `compute.cpp:106-121`; test `:229-230,256-257` | `load_store_local_shared.cpp:114-127` | |
| LDL.B128 | IMPLEMENTED_UNVERIFIED | `compute.cpp:107` | same | |
| STL.B32/B64 | VERIFIED | `decoder.cpp:246`; test `:227-228` | `load_store_local_shared.cpp:153-191` | |
| STL.U8..S16, STL.B128 | IMPLEMENTED_UNVERIFIED | `compute.cpp:84-92` | `load_store_local_shared.cpp:166-178` | Read-modify-write is safe on thread-private memory. |
| LDL/STL cache ops | IMPLEMENTED_UNVERIFIED | ignored | ENVY tabef40_0/tabef50_0 (`:597-611`) | No host meaning. |
| Local memory size (SPH lo/hi/crs, QMD local_pos_alloc) and out-of-bounds behaviour | PARTIAL | `SD/decompiler.cpp:107-115`: graphics uses `max(lo+hi+crs, 0x100)`, compute is fixed at 0x400. `GPU/engines/compute.hpp:66` `localPosAlloc()` is unused. `msl/emitter.cpp:243-247,1190-1200` clamp | `EDEN/program_header.h:216-219` (lo OR hi<<24); `load_store_local_shared.cpp:91-95` (out-of-bounds load → 0), `:153-162` (out-of-bounds store dropped) | Compute shaders needing more than 1 KiB alias the last word. Out-of-bounds stores clobber it. |
| LDC.B32 default mode, immediate or register index | VERIFIED | `decoder.cpp:238`; `memory.cpp:81-164,286-291`. Executed in `T/shader_library_tests.cpp:635-637` (BRX table `LDC R1, c[2][R1]`) and `T/shader_coverage_tests.cpp:1121-1163` | `EDEN-I/load_constant.cpp:34-55`; NAK 2618-2650 | |
| LDC signed register offset | VERIFIED | `memory.cpp:113-116` sign-extends when `src != RZ` (fixed during audit); test `shader_coverage_tests.cpp:1131-1135` | `EDEN-I/load_constant.h:32` (`s64`); ENVY 309-311 (`C36_08_S16_20`) | The `memory.cpp:287` TODO comment is now stale. |
| LDC.U8/S8/U16/S16 | VERIFIED | `memory.cpp:118-147` (fixed during audit); test `shader_coverage_tests.cpp:1121-1163` | `load_constant.cpp:41-52` | |
| LDC.B64 | IMPLEMENTED_UNVERIFIED | `memory.cpp:95-97,149-160` | `load_constant.cpp:56-65` | |
| LDC.B128 | IMPLEMENTED_UNVERIFIED | `memory.cpp:98-100` | Eden throws (`load_constant.cpp:66-67`) | Superset of Eden. |
| LDC.IL | MISSING | `memory.cpp:105-111` logs, then treats as default | `load_constant.cpp:27-30` (throws) | |
| LDC.IS (segmented) | MISSING | `memory.cpp:105-111` | `load_constant.cpp:17-26` (implemented) | Eden implements IS. Hydra silently reads the wrong slot. |
| LDC.ISL | MISSING | same | throws | |
| ATOM (global) ADD/MIN/MAX/INC/DEC/AND/OR/XOR/EXCH/SAFEADD × U32/S32/U64/F32.FTZ.RN/F16x2/S64 | MISSING | `decoder.cpp:266-268` | `EDEN-I/atomic_operations_global_memory.cpp:13-33,192-202`; NAK 2763-2778; ENVY 673-695 | |
| ATOM.CAS (global, 0xEEF0) | MISSING | `decoder.cpp:250-252` | `maxwell.inc:114`; `not_implemented.cpp:18`; NAK 2734-2761 | Eden also lacks it. |
| ATOM .E (64-bit address) | MISSING | — | `atomic_operations_global_memory.cpp:86-109` | |
| RED (all ops and types) | MISSING | `decoder.cpp:272-274` | `atomic_operations_global_memory.cpp:204-213`; NAK 2718-2732 | |
| ATOMS ADD/MIN/MAX/INC/DEC/AND/OR/XOR/EXCH .U32 | VERIFIED | `decoder.cpp:269-271`; `compute.cpp:330-351`; `msl/emitter.cpp:256-268,1202-1220`; `T/shader_library_tests.cpp:266-335` | `EDEN-I/atomic_operations_shared_memory.cpp:28-52,83-107`; NAK 2805-2830 | Tested with 64-thread contention and wrap semantics. |
| ATOMS MIN/MAX .S32 | VERIFIED | `compute.cpp:347`; `msl/emitter.cpp:1204-1207`; test `:272-273,327` | `atomic_operations_shared_memory.cpp:97` | |
| ATOMS predicated / signed relative offset / RZ destination | VERIFIED | `compute.cpp:338-349`; test `:274-282` | `atomic_operations_shared_memory.cpp:54-68` | |
| ATOMS .U64 (EXCH) / .S64 | MISSING | `compute.cpp:333-336` rejects size > 1 | `atomic_operations_shared_memory.cpp:93-96` (EXCH.64 supported) | Metal has no 64-bit threadgroup atomics, so this needs CAS-pair emulation. |
| **ATOMS.CAS / CAST / CAST.SPIN (0xEE00/0xFF80)** | PARTIAL (BUG) | `decoder.cpp:263-265` routes to `emitAtoms`, which reads op from bits 52..55 (`compute.cpp:332`). CAS.32 has subop 4 and runs as **DEC**; CAS.64 has subop 5 and runs as **AND** | ENVY 654-665,1902; NAK 2789-2803 (`subop U32=>4, U64=>5`); `maxwell.inc:234`; `not_implemented.cpp:22-23` | Wrong atomics with no error. Eden at least fails loudly. |
| CCTL / CCTLL / CCTLT | IMPLEMENTED_UNVERIFIED | `decoder.cpp:239-244,275-277` (logged, no IR) | `not_implemented.cpp:46-55` (Eden throws) | Cache control has no host effect, so a no-op is acceptable. |
| MEMBAR.CTA | PARTIAL | `decoder.cpp:234-237` no-op ("ordered by barriers") | `EDEN-I/barrier_operations.cpp:22-33` (WorkgroupMemoryBarrier) | Fine when paired with BAR, but it is not a fence on its own. |
| MEMBAR.GL/SYS/VC | PARTIAL | same | `barrier_operations.cpp:30-32` (DeviceMemoryBarrier) | Matters once device memory exists. |

### a.2 Texture

| Opcode/variant | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| TEX 1D / 1D ARRAY, implicit LOD | IMPLEMENTED_UNVERIFIED | `SD/decoder/texture.cpp:491-499`; `msl/emitter.cpp:984-989` | `EDEN-I/texture_fetch.cpp:57-63` | Emits a `vec<f32,1>` coordinate. Never compile-tested. |
| TEX 1D / 1D ARRAY .LZ/.LL/.LB | PARTIAL | `msl/emitter.cpp:1007-1017` appends `level()`/`bias()` | MSL `texture1d::sample` has no lod_options (1D textures have no mips) | Probably an MSL compile error. |
| TEX 2D implicit LOD | IMPLEMENTED_UNVERIFIED | `texture.cpp:500-504,539-559`; `msl/emitter.cpp:984-989` | `texture_fetch.cpp:177-191` | |
| TEX 2D .LZ | VERIFIED | `texture.cpp:542-546`; `msl/emitter.cpp:1008-1009`; `T/shader_library_tests.cpp:1803-1809` (scenario 7) | `texture_fetch.cpp:84-85` | |
| TEX 2D .LB / .LBA | VERIFIED | `texture.cpp:552-556`; `msl/emitter.cpp:1014-1016`; test `:1740-1901` | `texture_fetch.cpp:86-90,173` | Fragment only. Bias is dropped in VS and replaced by `level(0)` in compute (`msl/emitter.cpp:1017-1019`). |
| TEX 2D .LL / .LLA | IMPLEMENTED_UNVERIFIED | `texture.cpp:547-551` | `texture_fetch.cpp:86-90` | |
| TEX 2D ARRAY (all LOD modes) | IMPLEMENTED_UNVERIFIED | `texture.cpp:505-510,259-264` | `texture_fetch.cpp:66-67` | Layer comes from the low 16 bits of the first coordinate register, as in Eden. |
| TEX 3D | IMPLEMENTED_UNVERIFIED | `texture.cpp:511-516` | `texture_fetch.cpp:68-69` | |
| TEX CUBE / CUBE ARRAY | IMPLEMENTED_UNVERIFIED | `texture.cpp:524-536` | `texture_fetch.cpp:72-75` | |
| TEX ARRAY_3D (dim 5, invalid) | PARTIAL | `texture.cpp:517-523` → `msl/emitter.cpp:66` emits `texture3d_array`, which is not an MSL type | Eden throws (`texture_fetch.cpp:47-48`) | Should be rejected, not emitted. |
| TEX .AOFFI | IMPLEMENTED_UNVERIFIED | `texture.cpp:561-572` (4-bit signed per axis); `msl/emitter.cpp:1022-1027`. IR-level offset test at `T/shader_library_tests.cpp:1998-2003` | `texture_fetch.cpp:98-120` | The decode path itself is untested. |
| TEX .DC | PARTIAL | `texture.cpp:574-577`; `msl/emitter.cpp:985-986,1031-1033` | `texture_fetch.cpp:167-172,199-201` (RGB = result, A = 1.0) | Hydra returns (r,0,0,0). If the same binding is also sampled without DC, the depth2d/texture2d declarations conflict (`memory_analyzer.cpp:141-149`). |
| TEX .LC (LOD clamp) | MISSING | `SD/decoder/texture.hpp:126` decoded, ignored | Eden throws (`texture_fetch.cpp:148-150`) | |
| TEX .NDV / .NODEP | IMPLEMENTED_UNVERIFIED | ignored (`texture.cpp:474`) | informational in Eden | No host equivalent needed. |
| TEX sparse predicate (bits 51..53 ≠ PT) | MISSING | `decoder.cpp:518` mask `0xfc38` requires PT, so the instruction becomes "Unknown instruction" (`decoder.cpp:890-902`) | `texture_fetch.cpp:140,207-209`; ENVY 1970 also fixes these bits | |
| TEX write mask → consecutive registers | VERIFIED | `texture.cpp:309-326` (mask 0xF tested) | `texture_fetch.cpp:193-206` | Partial masks use the same loop as Eden. |
| TEX implicit LOD in vertex stage | UNKNOWN | `msl/emitter.cpp:1017-1019` adds `level(0)` only for compute | Eden lowers implicit LOD outside FS | Whether MSL accepts implicit `sample()` in a vertex function is untested. |
| TEX.B (handle in Rb) | IMPLEMENTED_UNVERIFIED | `decoder.cpp:510`; `texture.cpp:817-825` | `texture_fetch.cpp:225-234`; NAK 2177-2185; ENVY 1962 | Emission untested. Handle tracking itself is VERIFIED (see below). |
| TEXS 1D.LZ (enc 0) | PARTIAL | `texture.cpp:351-357` plus `level()` on texture1d | `EDEN-I/texture_fetch_swizzled.cpp:79-81` | MSL texture1d has no LOD option. |
| TEXS 2D (1) / 2D.LZ (2) / 2D.LL (3) | IMPLEMENTED_UNVERIFIED | `texture.cpp:358-377` | `texture_fetch_swizzled.cpp:82-92` | |
| TEXS 2D.DC (4) / 2D.LZ.DC (6) | PARTIAL | `texture.cpp:378-384,393-401` | `texture_fetch_swizzled.cpp:93-98,106-111,174-182` | DC result vector differs, as for TEX.DC. |
| **TEXS 2D.LL.DC (5)** | PARTIAL (BUG) | `texture.cpp:385-392` (`cmp_value = RB(); lod = RB();`) | `texture_fetch_swizzled.cpp:99-105` (dref = reg_b+1, lod = reg_b) | Depth reference and LOD are swapped. |
| TEXS ARRAY_2D (7) / .LZ (8) / .LZ.DC (9) | IMPLEMENTED_UNVERIFIED | `texture.cpp:402-426` | `texture_fetch_swizzled.cpp:112-131` | |
| TEXS 3D (10) / 3D.LZ (11) | IMPLEMENTED_UNVERIFIED | `texture.cpp:427-441` | `texture_fetch_swizzled.cpp:132-141` | |
| TEXS CUBE (12) / CUBE.LL (13) | IMPLEMENTED_UNVERIFIED | `texture.cpp:442-455` | `texture_fetch_swizzled.cpp:142-152` | |
| TEXS invalid encodings 14/15 | PARTIAL | `texture.cpp:343,350-456`: switch without a default leaves `type` uninitialised | Eden throws | Undefined behaviour on garbage input. |
| TEXS swizzle LUT and 2-register packing (dst_b = RZ selects the RG LUT) | IMPLEMENTED_UNVERIFIED | `texture.cpp:328-331,463-468` | `texture_fetch_swizzled.cpp:34-51,158-212` | Tables are identical to Eden's. |
| TEXS .F16 result packing (bit 59 = 0) | MISSING | `texture.hpp:73-84` has no bit-59 field | `texture_fetch_swizzled.cpp:11-18,214-261`; ENVY 1968 (`ZNV(59,f16…)`) | Writes F32 values where packed half2 is expected. |
| TLDS 1D.LZ (enc 0) | IMPLEMENTED_UNVERIFIED | `texture.cpp:706-710` via TEXS table entry 0 | `EDEN-I/texture_load_swizzled.cpp:81-84` | Correct only because the two tables happen to agree here. |
| **TLDS 1D.LL (enc 1)** | PARTIAL (BUG) | decoded as TEXS enc 1 (2D, coords Ra,Rb) | `texture_load_swizzled.cpp:85-89`; ENVY 932-956 | Wrong dimensionality and LOD. |
| TLDS 2D.LZ (enc 2) | IMPLEMENTED_UNVERIFIED | TEXS enc 2 | `texture_load_swizzled.cpp:90-93` | Same layout in both tables, so it works. |
| **TLDS 2D.LZ.AOFFI (enc 4)** | PARTIAL (BUG) | decoded as 2D.DC: the offset register becomes the depth reference and the binding is declared depth2d | `texture_load_swizzled.cpp:94-99` | |
| **TLDS 2D.LL (enc 5)** | PARTIAL (BUG) | decoded as 2D.LL.DC: LOD read from Rb+1, DC flag set | `texture_load_swizzled.cpp:100-105` | |
| **TLDS 2D.LZ.MS (enc 6)** | PARTIAL (BUG) | decoded as 2D.LZ.DC with no sample index | `texture_load_swizzled.cpp:106-111` | |
| **TLDS 3D.LZ (enc 7)** | PARTIAL (BUG) | decoded as ARRAY_2D (Ra = layer) | `texture_load_swizzled.cpp:112-116` | |
| **TLDS ARRAY_2D.LZ (enc 8)** | PARTIAL (BUG) | coordinates taken from (Ra+1, Rb) instead of (Rb, Rb+1) | `texture_load_swizzled.cpp:117-123` | |
| **TLDS 2D.LL.AOFFI (enc 12)** | PARTIAL (BUG) | decoded as CUBE (texturecube declared) | `texture_load_swizzled.cpp:124-131` | |
| TLDS .F16 (bit 59) | MISSING | `texture.hpp:73-84` | `texture_load_swizzled.cpp:44,193-240` | |
| TLD 1D/1D ARRAY/2D/2D ARRAY/3D, .LZ/.LL | IMPLEMENTED_UNVERIFIED | `decoder.cpp:514`; `texture.cpp:827-833`; `msl/emitter.cpp:954-982,1010-1011` | `EDEN-I/texture_load.cpp:45-67,93-151` | |
| TLD .AOFFI | IMPLEMENTED_UNVERIFIED | `texture.cpp:561-572`; `msl/emitter.cpp:941-951`. IR-level integer offset tested (`T/shader_library_tests.cpp:1987-1995`) | `texture_load.cpp:69-91,127-129` | |
| TLD .MS (sample index) | MISSING | `texture.cpp:827` "TODO: multisample index"; no `texture2d_ms` declaration | `texture_load.cpp:130-132` | MSAA resolves read sample 0 of a non-MS view. |
| TLD .CL | MISSING | ignored | Eden throws (`texture_load.cpp:133-135`) | |
| TLD sparse | MISSING | mask `0xff38` (`decoder.cpp:514`) | `texture_load.cpp:148-150` | |
| TLD on buffer textures (TIC 1D buffer) | IMPLEMENTED_UNVERIFIED | `msl/emitter.cpp:77-114,970-976`; TIC specialisation in `GPU/engines/3d.cpp` getShader | Eden `TextureType::Buffer` | |
| TLD.B | IMPLEMENTED_UNVERIFIED | `decoder.cpp:513`; `texture.cpp:835-843` | `texture_load.cpp:158-160` | |
| TLD4 2D, component R | VERIFIED | `decoder.cpp:517`; `texture.cpp:592-647`; `msl/emitter.cpp:889-939`; `T/shader_library_tests.cpp:2028-2033` | `EDEN-I/texture_gather.cpp:119-184` | |
| TLD4 components G/B/A | IMPLEMENTED_UNVERIFIED | `texture.cpp:296-307`; `component::y/z/w` | `texture_gather.cpp:30-35,165` | |
| TLD4 2D ARRAY | VERIFIED | test `:2040-2052` (scenario 6) | `texture_gather.cpp:68-69` | |
| TLD4 CUBE / CUBE ARRAY | IMPLEMENTED_UNVERIFIED | `texture.cpp:601-614` | `texture_gather.cpp:74-77` | |
| TLD4 .AOFFI | IMPLEMENTED_UNVERIFIED | `texture.cpp:618-627` (6-bit fields at bits 0 and 8) | `texture_gather.cpp:82-104` | MSL's offset range is [-8,7]; the hardware allows [-32,31]. |
| TLD4 .PTP | VERIFIED | `texture.cpp:628-636`; `msl/emitter.cpp:915-930`; test `:2004-2010,2028-2033` | `texture_gather.cpp:106-117` | |
| TLD4 .DC | VERIFIED | `texture.cpp:637-639`; test `:2013-2033` (scenario 7) | `texture_gather.cpp:159-171` | |
| TLD4 1D / 3D dimensions | PARTIAL | `texture.cpp:601-614` treats them as 2D | `texture_gather.cpp:59-80` | Not legal in GLSL, so low impact. |
| TLD4 sparse | MISSING | mask `0xfc38` | `texture_gather.cpp:181-183` | |
| TLD4.B | IMPLEMENTED_UNVERIFIED | `decoder.cpp:509`; `texture.cpp:845-853` | `texture_gather.cpp:196-203` | |
| TLD4S plain / AOFFI / DC / AOFFI+DC | IMPLEMENTED_UNVERIFIED | `decoder.cpp:508`; `texture.cpp:650-691,855-859` | `EDEN-I/texture_gather_swizzled.cpp:49-81` | Register layout matches Eden. |
| TLD4S .F16 (bit 55) | MISSING | `texture.hpp:229-241` | `texture_gather_swizzled.cpp:11-14,111-129` | ENVY (`:1960`) marks F16 as SM60; Eden implements it for SM53. |
| TLD4S with dst_b = RZ | PARTIAL | `texture.cpp:686-687`: `dst1 + 1` wraps to R0 because `reg_t` is u8 | Eden's alignment check rejects this | Edge case: clobbers R0. |
| TMML / TMML.B | MISSING | `decoder.cpp:497-502` | `EDEN-I/texture_mipmap_level.cpp:69-121`; NAK 2282-2307 | MSL has `calculate_clamped_lod`/`calculate_unclamped_lod` (FS only). |
| **TXD 2D** | PARTIAL (BUG) | `decoder.cpp:512`; `texture.cpp:719-791` (derivatives at `753-761`). The test at `T/shader_library_tests.cpp:1791-1802` uses symmetric gradients | `EDEN-I/texture_gradient.cpp:103-108,119-133`; `EDEN/backend/spirv/emit_spirv_image.cpp:131-141` (interleaved) | The current test cannot detect the layout bug. |
| **TXD 2D ARRAY** | PARTIAL (BUG) | `texture.cpp:734,748-752` | `texture_gradient.cpp:109-114` | Same layout bug. The layer correctly follows the coordinates. |
| TXD 1D / 1D ARRAY | MISSING | `texture.cpp:735-741` logs and writes nothing | `texture_gradient.cpp:91-102` | |
| TXD 3D / CUBE | MISSING | same | Eden throws (`texture_gradient.cpp:115-116`) | |
| TXD .AOFFI | IMPLEMENTED_UNVERIFIED | `texture.cpp:764-773` | `texture_gradient.cpp:45-51,135-138` | |
| TXD .LC | IMPLEMENTED_UNVERIFIED | Implemented (`texture.cpp:774-784`) but unreachable: mask `0xfffc` requires bit 50 = 0 (`decoder.cpp:511-512`), matching ENVY 1963-1964 | Eden throws (`texture_gradient.cpp:68-71`) | Dead code. |
| **TXD.B** (handle in Ra) | PARTIAL (BUG) | `texture.cpp:725-733,797-799` | `texture_gradient.cpp:78-80` | Handle position is correct; inherits the derivative bug. |
| **TXQ (bound, 0xDF48)** | PARTIAL (BUG) | `decoder.cpp:504` `EMIT(TxqB)`. `texture.cpp:808-815` then tracks Ra, which holds the LOD, as a bindless handle | `maxwell.inc:109`; `EDEN-I/texture_query.cpp:63-70`; NAK 2351-2354; ENVY 1958 | The common `textureSize()` form has Ra = RZ, so tracking fails and the destination is never written. |
| **TXQ.B (bindless, 0xDF50)** | PARTIAL (BUG) | `decoder.cpp:503` `EMIT(Txq)` uses bits 36..48 as a bound index | `maxwell.inc:110`; `texture_query.cpp:72-74`; NAK 2357-2360; ENVY 1957 | Queries texture word 0 or junk. |
| TXQ DIMENSION width/height/depth/layers | PARTIAL | `texture.cpp:267-294` (`(void)src;` ignores LOD); `msl/emitter.cpp:1062-1089` | `texture_query.cpp:20-25` (LOD from register) | Mip-level sizes are wrong. |
| **TXQ DIMENSION mip count (mask bit 3)** | PARTIAL (BUG) | `msl/emitter.cpp:173-184` `dimensionToStr` defaults to `INVALID`, emitted at `:1087-1088` as `get_INVALID()` | `texture_query.cpp:21-24` | MSL compile failure. |
| TXQ TEXTURE_TYPE / SAMPLE_POS / FILTER / LOD / WRAP / BORDER | MISSING | `texture.cpp:287-289` | Eden throws (`texture_query.cpp:26-29`) | |
| TXQ query field width | PARTIAL | `texture.hpp:25` `BitField64<TextureQuery, 22, 7>` | NAK 2365 (`22..28`, 6 bits); ENVY 882-891 (mask 0x0fc00000) | The 7-bit read includes bit 28. Minor. |
| TXA | MISSING | `decoder.cpp:505-507` | `not_implemented.cpp:262-263` | Eden also lacks it. |
| Bindless handle tracking (LDC B32..B128 words, MOV c[], MOV RZ, MOV32I, MOV Rn, skipping predicate-only writers, branch over EXIT) | VERIFIED | `texture.cpp:111-257`; `T/shader_library_tests.cpp:33-120` | `EDEN/ir_opt/texture_pass.cpp:271-291,315-360` | |
| Separate texture/sampler handles (LOP.OR c / LOP.OR R,R) | IMPLEMENTED_UNVERIFIED | `texture.cpp:198-224` | `texture_pass.cpp:271-287` (secondary handle with shifts) | Shifts are not supported. |
| Dynamic descriptor arrays (indexed LDC, SHL+IADD handle math) | MISSING | `texture.cpp:167-170` (`src != RZ` fails) | `texture_pass.cpp:38-59,191-199` (count > 1) | |
| Texture type and shadow flag from TIC | PARTIAL | `memory_analyzer.cpp:55-68,115-117`: type comes from the instruction; query-only textures become 2D. TIC is used only for buffer/integer formats (`GPU/engines/3d.cpp` getShader) | `texture_pass.cpp:289-299` (ReadTextureType/PixelFormat) | |

### a.3 Surface / image

| Opcode/variant | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| SULD.P | MISSING | `decoder.cpp:285-287`; no ImageLoad IR op (`SD/ir/instruction.hpp`) | `EDEN-I/surface_load_store.cpp:171-229`; NAK 2463-2496 | |
| SULD.D (U8/S8/U16/S16/B32/B64/B128) | MISSING | same | `surface_load_store.cpp:78-112,208-214` | |
| SUST.P with full RGBA mask | IMPLEMENTED_UNVERIFIED | `decoder.cpp:284`; `compute.cpp:378-421`; `msl/emitter.cpp:1272-1291` | `surface_load_store.cpp:231-278` | Compute only (see last row). |
| SUST.P partial mask | PARTIAL | `compute.cpp:400-407` writes 0 to unselected components | Eden throws (`surface_load_store.cpp:271-274`) | |
| SUST.D sizes | PARTIAL | `compute.cpp:408-415` ("TODO: convert raw data to the image format") | `surface_load_store.cpp:258-269` (typed R8..R32G32B32A32_UINT view) | Correct only for R32-class integer images. |
| SUST/SULD clamp IGN/Default/TRAP | IMPLEMENTED_UNVERIFIED | `msl/emitter.cpp:1227-1256`: bounds check drops out-of-bounds writes | Eden supports only IGN (`surface_load_store.cpp:248-250`) | |
| SUST .BA (byte addressing) | MISSING | ignored | Eden throws for typed + BA | |
| SUST cache ops | IMPLEMENTED_UNVERIFIED | ignored | Eden accepts WB/CG only | |
| SUST bindless (handle in Rc) | IMPLEMENTED_UNVERIFIED | `compute.cpp:381-388` | `surface_load_store.cpp:261-262` | |
| Surface dims 1D, 1D buffer, 1D array, 2D, 2D array, 3D | IMPLEMENTED_UNVERIFIED | `compute.cpp:130-172`; `msl/emitter.cpp:1258-1270` | `surface_load_store.cpp:114-150` | Array index is not masked to 16 bits. |
| SUATOM.D ADD/MIN/MAX/AND/OR/XOR/EXCH (32-bit) | IMPLEMENTED_UNVERIFIED | `decoder.cpp:294-295`; `compute.cpp:423-448`; `msl/emitter.cpp:1293-1345` | `EDEN-I/surface_atomic_operations.cpp:97-122,176-194`; NAK 2553-2593 | |
| SUATOM INC/DEC | PARTIAL | `msl/emitter.cpp:1309-1316` ("TODO: wrap", ±1) | `surface_atomic_operations.cpp:107-110` | The wrap bound is ignored. |
| SUATOM S32 vs U32 | PARTIAL | `compute.cpp:438` ("TODO: size"). Signedness comes from the image format (`msl/emitter.cpp:117-148`) | `surface_atomic_operations.cpp:156,182` | |
| SUATOM U64/S64/F32/F16x2/SD32/SD64 | MISSING | — | Eden supports only 32-bit integers (`surface_atomic_operations.cpp:153-155`) | |
| SUATOM.CAS | MISSING | `decoder.cpp:288-293` | `not_implemented.cpp:254-255` | Eden also lacks it. |
| SURED | MISSING | `decoder.cpp:281-283` | `surface_atomic_operations.cpp:196-213` | |
| Images in vertex/fragment stages | MISSING | Only `dispatch.images` is bound (`GPU/renderer/metal/renderer.cpp:1131,1172`); the 3D engine binds no images | Eden binds images per stage | Graphics SUST/SUATOM write to unbound textures. |

### a.4 Control flow

| Opcode/variant | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| BRA immediate, PT | IMPLEMENTED_UNVERIFIED | `decoder.cpp:449-464` | `EDEN/frontend/maxwell/control_flow.cpp:275-294,406-421`; NAK 3020-3030 | No executed test. A BRA appears only in the handle-tracking scan (`T/shader_library_tests.cpp:99-110`). |
| BRA predicated | IMPLEMENTED_UNVERIFIED | `decoder.cpp:465-473` | `control_flow.cpp:414-419` | |
| Flow-test (CC) field on BRA/BRX/EXIT/KIL/SYNC/BRK (bits 0..4) | MISSING | `decoder.cpp:176,315,360,450` ("TODO: ccc"); `SD/decoder/exit.hpp:8` | `control_flow.cpp:109-127,415-418` | CC-conditional flow becomes unconditional. |
| BRA .U / .LMT / .CA | IMPLEMENTED_UNVERIFIED | ignored | hints only | |
| BRA with constant-buffer target (bit 5) | MISSING | `decoder.cpp:446-448` | Eden throws (`control_flow.cpp:408-410`) | |
| BRX bounded switch (IMNMX clamp, optional SHL, optional LDC table) | VERIFIED | `decoder.cpp:39-141,439-445`; `T/shader_library_tests.cpp:625-740` | `EDEN-I/branch_indirect.cpp:27+`; `control_flow.cpp:432-475` | Predicated BRX is supported (Eden throws). |
| BRX other patterns / constant-buffer form | MISSING | `decoder.cpp:436-438,441-444` | Eden tracks jump tables more generally | |
| JMP / JMX | MISSING | `decoder.cpp:484-495` | `control_flow.cpp:99-107,276-294,323-326` | |
| SSY / SYNC (immediate) | IMPLEMENTED_UNVERIFIED | `decoder.cpp:175-210,406-417` | `control_flow.cpp:72-97,296-308` | |
| SSY/PBK/PCNT with constant-buffer target | MISSING | `decoder.cpp:388-390,394-396,406-408` | Eden throws | |
| PBK / BRK | IMPLEMENTED_UNVERIFIED | `decoder.cpp:314-347,397-405` | `control_flow.cpp:296-308,316-322` | |
| PCNT / CONT | MISSING | `decoder.cpp:311-313,388-393` | `control_flow.cpp:76-79,296-308,316-322` | `continue` statements fall through. |
| PEXIT | MISSING | `decoder.cpp:475-477` | `control_flow.cpp:80-82` (PEXIT token in AnalyzeEXIT) | |
| EXIT unconditional | VERIFIED | `decoder.cpp:359-368`. Every decoded test ends with EXIT (e.g. `T/shader_library_tests.cpp:283,370`) | `EDEN-I/exit_program.cpp:38-46`; `control_flow.cpp:477-535` | |
| EXIT predicated | IMPLEMENTED_UNVERIFIED | `decoder.cpp:369-374` | `control_flow.cpp:489-503` | |
| CAL / JCAL / RET | MISSING | `decoder.cpp:430-435,478-483` ignored; `:349-355` RET becomes exit | `control_flow.cpp:331-351` (function calls) | Subroutines are silently broken. |
| PRET | MISSING | `decoder.cpp:424-429` | Eden throws (`control_flow.cpp:329-330`) | |
| LONGJMP / PLONGJMP | MISSING | `decoder.cpp:356-358,418-423` | `control_flow.cpp:83-85,296-308` | |
| KIL (predicated) | IMPLEMENTED_UNVERIFIED | `decoder.cpp:348`; `SD/decoder/exit.cpp:5-13`; `msl/emitter.cpp:1092` | `control_flow.cpp:310-315` (demote semantics) | |
| DEPBAR | IMPLEMENTED_UNVERIFIED | `decoder.cpp:211-214` | `barrier_operations.cpp:35-37` | A no-op is correct. |
| BPT | IMPLEMENTED_UNVERIFIED | `decoder.cpp:296-298` (logged, no IR) | `not_implemented.cpp:30-31` | An acceptable no-op. |
| NOP | IMPLEMENTED_UNVERIFIED | `decoder.cpp:701` | — | |
| Unknown/garbage instruction handling | IMPLEMENTED_UNVERIFIED | `decoder.cpp:890-902` | Eden throws | Unimplemented ops leave stale registers. |
| Structurization / reconvergence | MISSING | `SD/analyzer/cfg.cpp:7-41` unused; dispatcher loop in `SD/codegen/lang/emitter.cpp:218-249` | `EDEN/frontend/maxwell/structured_control_flow.cpp` | Warp ops, barriers and implicit derivatives can run on partial SIMD-groups or quads (§d D1). |
| Loop / block-transition bound | PARTIAL | `SD/codegen/lang/emitter.cpp:225-238` (65536 transitions) | Eden has no bound | Long loops are silently truncated. |
| Sync/break stack merge at joins | PARTIAL | `SD/decoder/decoder.hpp:80-85` ("TODO: … make sure they match"; last writer wins) | `control_flow.cpp` keeps a Stack per label | |

### a.5 Warp and synchronisation (system registers are in §b.2)

| Opcode/variant | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| SHFL IDX/UP/DOWN/BFLY (compute) | IMPLEMENTED_UNVERIFIED | `decoder.cpp:249`; `SD/decoder/warp.cpp:20-60`; `msl/emitter.cpp:1370-1375` | `EDEN-I/warp_shuffle.cpp:20-88`; `EDEN/backend/spirv/emit_spirv_warp.cpp:236-312` | Clamp/segmask/in-bounds arithmetic mirrors Eden. |
| SHFL in-bounds predicate (compute) | IMPLEMENTED_UNVERIFIED | `warp.cpp:59-60,68` | `warp_shuffle.cpp:66-68` | |
| SHFL.BFLY in fragment (quad) | PARTIAL | `warp.cpp:16-19` ignores the index range and mask, and forces the predicate true. The emitter is verified at IR level (`T/shader_library_tests.cpp:901-989`) | `warp_shuffle.cpp:51-64` (quad path only for mask 0x1c03 and index 1..3) | Cross-quad butterflies return the lane's own value. |
| SHFL IDX/UP/DOWN in fragment | MISSING | `warp.cpp:61-66` (identity) | `warp_shuffle.cpp:54-58,66` | The QuadBroadcast pattern is unsupported. |
| SHFL in vertex and other stages | MISSING | `warp.cpp:61-66` | `warp_shuffle.cpp:66` | |
| VOTE.ALL/ANY/EQ → predicate | IMPLEMENTED_UNVERIFIED | `decoder.cpp:696`; `compute.cpp:489-510`; `msl/emitter.cpp:1348-1362` | `EDEN-I/vote.cpp:16-47`; NAK 3227-3248 | |
| VOTE ballot → register | IMPLEMENTED_UNVERIFIED | `compute.cpp:503-506`; `msl/emitter.cpp:1364-1368` | `vote.cpp:41` | |
| VOTE.VTG | MISSING | `decoder.cpp:693-695` | `vote.cpp:49-51` (stub) | Matches Eden's stub. |
| BAR.SYNC 0 (immediate) | IMPLEMENTED_UNVERIFIED | `decoder.cpp:222`; `compute.cpp:369-376`; `msl/emitter.cpp:1222-1225` | `EDEN-I/barrier_operations.cpp:39-94`; NAK 3114-3137 | |
| BAR.SYNC with barrier id, thread count or input predicate | PARTIAL | `compute.cpp:370` ("TODO: modes other than BAR.SYNC 0") | Eden throws for non-zero operands | Becomes a full threadgroup barrier. |
| BAR.ARV | PARTIAL | treated as SYNC | `barrier_operations.cpp:71-72` | Producer/consumer patterns can deadlock. |
| BAR.RED.POPC/AND/OR, BAR.SCAN | MISSING | no result is written | `barrier_operations.cpp:59-68`; ENVY 1816-1820 | |

### a.6 Stage I/O and pipeline stages

| Opcode/variant | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| ALD (input, immediate address) | IMPLEMENTED_UNVERIFIED | `decoder.cpp:227`; `memory.cpp:8-45,276-279` | `EDEN-I/load_store_attribute.cpp:61-98`; NAK 2855-2878 | |
| ALD indexed (Ra ≠ RZ) | PARTIAL | `memory.cpp:34-35`; `memory_analyzer.cpp:120-125` (`ASSERT_DEBUG(amem.reg == RZ…)`) | `load_store_attribute.cpp:92-97` | Attributes reached only through the index are never declared. |
| ALD.O (read outputs) | IMPLEMENTED_UNVERIFIED | `memory.cpp:277` (`!inst.o`) | `load_store_attribute.cpp:69` | |
| ALD.P (patch) / vertex index (Rc) | MISSING | `memory.hpp:20,22` (`todo`) | `load_store_attribute.cpp:68,78-89` | Needs GS/tessellation. |
| ALD/AST offset field width | PARTIAL | `memory.hpp:19,33` read 11 bits | NAK 2873,2893 (`20..30`, 10 bits); Eden absolute 10 bits | Minor. |
| AST (immediate address) | IMPLEMENTED_UNVERIFIED | `decoder.cpp:223`; `memory.cpp:48-79` | `load_store_attribute.cpp:100-139` | |
| AST indexed | PARTIAL | `memory_analyzer.cpp:131-139` | Eden throws | |
| AST.P | MISSING | ignored | `load_store_attribute.cpp:123-125` | |
| IPA.PASS | IMPLEMENTED_UNVERIFIED | `decoder.cpp:496`; `memory.cpp:217-273` | `load_store_attribute.cpp:141-207`; NAK 2900-2936 | |
| IPA.MULTIPLY | IMPLEMENTED_UNVERIFIED | `memory.cpp:259-264` | `load_store_attribute.cpp:192-194` | |
| IPA.CONSTANT / IPA.SC | PARTIAL | `memory.cpp:258-259` ("TODO: what about other?") | `load_store_attribute.cpp:171-172` (SC disables the w multiply for legacy attributes) | |
| IPA perspective decision from SPH imap | PARTIAL | `memory.cpp:249-256`: only Perspective multiplies; Unused does not | `load_store_attribute.cpp:173-186` (Perspective **or Unused**) | |
| IPA legacy attributes (0x280–0x2BC, 0x2E8, 0x300–0x39C) | MISSING | `SD/const.cpp:44-50` maps them to Invalid, so they are never populated. The out-of-bounds `pixel_imaps` read was fixed during the audit (`memory.cpp:227-252`, `is_generic`) | `load_store_attribute.cpp:169-172`; `EDEN/frontend/ir/attribute.h:160-231` | Reads uninitialised `a_in`. |
| IPA .IDX | PARTIAL | `memory.cpp:244-256` always multiplies by w; an index register of RZ reads `a_in[0]` | `load_store_attribute.cpp:165-168` | |
| IPA .SAT | IMPLEMENTED_UNVERIFIED | `memory.cpp:266` | `load_store_attribute.cpp:198-204` | |
| IPA sample mode CENTROID/OFFSET (bits 52..53) and offset register | MISSING | `memory.cpp:215` ("TODO: msi") | Eden ignores it too | Low impact. |
| IPA FrontFace (0x3FC) | VERIFIED | Fixed during audit: `memory.cpp:229-240`, `msl/emitter.cpp:577-578,677-679`; `T/shader_coverage_tests.cpp:290-353` | `EDEN/backend/spirv/emit_spirv_context_get_set.cpp:366-369` (~0u / 0) | Matches Eden's all-ones mask. |
| IPA PointSprite S/T (0x2E0/0x2E4) | VERIFIED | `msl/emitter.cpp:579-580,680-688`; `T/shader_coverage_tests.cpp:356-443` (both origins) | `emit_spirv_context_get_set.cpp:370-375` | |
| IPA PrimitiveId / Layer / ViewportIndex in FS (0x60–0x68) | IMPLEMENTED_UNVERIFIED | `msl/emitter.cpp:581-586,689-696`; `memory.cpp:229-240` | `attribute.h:18-21` | |
| IPA Position X/Y/Z/W | VERIFIED | `SD/codegen/lang/emitter.cpp:173`; IR-level tests `T/shader_library_tests.cpp:911-912,1779-1786` | `attribute.h:22-25` | MSL `position.w` is 1/w, matching a[0x7C]. |
| IPA TessCoord (0x2F0/0x2F4) | MISSING | `const.cpp:26-27` recognised, never populated | `attribute.h:188-189` | Tessellation only. |
| OUT EMIT/CUT/stream | MISSING | `decoder.cpp:169-174,278-280` | `EDEN-I/output_geometry.cpp:10-42`; NAK 3250-3277 | |
| AL2P | MISSING | `decoder.cpp:231-233` | `EDEN-I/attribute_memory_to_physical.cpp:17-31` | |
| ISBERD | MISSING | `decoder.cpp:228-230` | `EDEN-I/internal_stage_buffer_entry_read.cpp:55-126` | |
| PIXLD (MY_INDEX, COVMASK, …) | MISSING | `decoder.cpp:224-226` | `EDEN-I/pixel_load.cpp:20-43` (MY_INDEX) | |
| **Fragment color outputs (omap)** | PARTIAL (BUG) | `SD/codegen/lang/emitter.cpp:279-292` reads `getRegisterStr(i * 4 + c)` for every **bound** RT | `EDEN-I/exit_program.cpp:12-28` | Unwritten RTs (gaps) shift the registers of later RTs. |
| **Fragment depth output** | PARTIAL (BUG) | `SD/decompiler.cpp:121-123` (`popcount(omap_target) + 1`). Verified only for full RT masks (`T/shader_library_tests.cpp:994-1084`) | `exit_program.cpp:29-34` (4 × enabled RTs, then sample mask, then depth) | Wrong register when an RT has a partial component mask. |
| Fragment sample-mask output | MISSING | `decompiler.cpp:82` decoded, unused | `exit_program.cpp:29-31` | |
| VS Position output | VERIFIED | `SD/codegen/lang/emitter.cpp:103-107,272`; `T/shader_library_tests.cpp:791-897` | `attribute.h:22-25` | |
| VS generic outputs (0x80–0x27C) | VERIFIED | `msl/emitter.cpp:446-460`; test `:827-830` | `attribute.h:26-159` | |
| VS PointSize, fixed size | VERIFIED | `msl/emitter.cpp:423-425,737-745`; `T/shader_coverage_tests.cpp:356-443` | Eden writes gl_PointSize | Fixed during audit. |
| VS PointSize written by the shader (program point size) | IMPLEMENTED_UNVERIFIED | `msl/emitter.cpp:738-741` | — | |
| VS ClipDistance 0–7 | VERIFIED | Fixed during audit: `SD/const.cpp:21-23`, `msl/emitter.cpp:426-427,746-756`, `memory_analyzer.cpp:136-138`; IR-level `T/shader_coverage_tests.cpp:446-494` | `attribute.h:176-183` | Before the fix this produced an MSL compile error (`__out.user36`). |
| VS Layer written directly (0x64) | VERIFIED | `msl/emitter.cpp:419-420,731-733`; `T/shader_coverage_tests.cpp:551-614` | `attribute.h:19` | |
| VS Layer via forwarded GS | VERIFIED | `SD/decompiler.cpp:198-257`; `msl/emitter.cpp:727-730`; `T/shader_library_tests.cpp:1089-1231` | — | Heuristic. |
| VS ViewportIndex | VERIFIED | `msl/emitter.cpp:421-422,734-736`; `T/shader_coverage_tests.cpp:497-549` | `attribute.h:20` | |
| VS ViewportMask (0x3A0) | MISSING | `const.cpp:44-50` (Invalid) | `attribute.h:232` | |
| VS legacy colours / fog / FixedFncTexture outputs | MISSING | `const.cpp:44-50` (Invalid, dropped) | `attribute.h:160-231` | No longer causes a compile failure. |
| VS VertexId / InstanceId inputs | VERIFIED | `SD/codegen/lang/emitter.cpp:168-169`; `msl/emitter.cpp:570-571`; tests `:803-808,1153-1155` | `emit_spirv_context_get_set.cpp:346-357` | Instance ID excludes the base instance and vertex ID includes the base vertex (`GPU/engines/3d.cpp:367,491`), as in Eden. |
| Geometry shaders | MISSING | `GPU/engines/3d.cpp:1102-1130` (layer heuristic only) | Eden supports GS | Metal has no GS stage. |
| Tessellation control/evaluation | MISSING | `GPU/engines/3d.cpp:1134-1152` (log only) | Eden supports tessellation | |
| VertexA + VertexB (dual vertex program) | MISSING | `GPU/engines/3d.cpp:1135` | Eden merges the two programs | |

---

## (b) SPH, system registers, attribute map

### b.1 Shader Program Header (`SD/decompiler.cpp:20-87`; reference `EDEN/program_header.h:30-220`)

All bit positions match Eden (CommonWord0–4, VTG imap/omap, PS imap/omap; `sizeof == 0x50`).

| Field | Bits | Decoded | Used by Hydra | Eden use | Class |
|---|---|---|---|---|---|
| sph_type / version / shader_type | CW0 0-4 / 5-9 / 10-13 | yes | `version` is debug-asserted to 3 (`decompiler.cpp:100-101`) | informational | IMPLEMENTED_UNVERIFIED |
| mrt_enable, kills_pixels, does_global_store, sass_version | CW0 14,15,16,17-20 | yes | no | mostly informational | IMPLEMENTED_UNVERIFIED |
| geometry_passthrough (`is_fast_gs`) | CW0 24 | yes | no (the GS heuristic scans code instead) | passthrough GS | MISSING |
| does_load_or_store, does_fp64 | CW0 26,27 | yes | no | — | IMPLEMENTED_UNVERIFIED |
| stream_out_mask (transform feedback) | CW0 28-31 | yes | no | TFB | MISSING |
| shader_local_memory_low/high | CW1 0-23 / CW2 0-23 | yes | summed with crs as `lo+hi+crs`, minimum 0x100 (`decompiler.cpp:109-115`) | lo OR (hi<<24) | PARTIAL |
| shader_local_memory_crs | CW3 0-23 | yes | wrongly added to the local size | call/return stack (no local memory) | PARTIAL |
| per_patch_attribute_count | CW1 24-31 | yes | no | tessellation | MISSING |
| threads_per_input_primitive | CW2 24-31 | yes | no | TCS | MISSING |
| output_topology / max_output_vertices | CW3 24-27 / CW4 0-11 | yes | no | GS | MISSING |
| store_req_start/end | CW4 12-19 / 24-31 | yes | no | — | IMPLEMENTED_UNVERIFIED |
| VTG imap system values A/B/C (clip, point sprite, tess UV, instance/vertex id) | 0x14.. | yes | no: inputs come from IR | declares inputs | PARTIAL |
| VTG imap/omap generic vectors | 16 B each | yes | no: inputs/outputs come from IR plus the FS input mask | declares I/O | PARTIAL |
| VTG imap/omap colour and fixed-function texture | — | yes | no | legacy varyings | MISSING |
| PS imap generic vector (2-bit interpolation per component) | 32 B | yes | yes (`decompiler.cpp:124-132` → flat/no_perspective, IPA w multiply) | yes | IMPLEMENTED_UNVERIFIED |
| PS imap colour / system C / fixed-function texture | — | yes | no | legacy | MISSING |
| PS omap target (8 × 4 bits) | 0x48 | yes | depth register only, via popcount. Colour mapping ignores it | per-RT register allocation | PARTIAL (BUG) |
| PS omap sample_mask | 0x4C bit 0 | yes | no | SetSampleMask | MISSING |
| PS omap depth | 0x4C bit 1 | yes | yes (verified for full masks) | SetFragDepth | PARTIAL |
| Compute "header" (QMD): shared_alloc / local_pos_alloc / block dims / cbufs | QMD | `sharedAlloc` used (`GPU/engines/compute.cpp:140`); `localPosAlloc` unused (`compute.hpp:66`) | Eden uses both | | PARTIAL |

### b.2 System registers (S2R; CS2R is now routed through S2R at `decoder.cpp:700`)

Emission: `msl/emitter.cpp:1106-1176`; stage handling: `SD/decoder/move.cpp:59-72` (TID/CTAID read 0 outside compute). Reference: `EDEN-I/move_special_register.cpp:113-167`.

| SR (index) | Compute | VS | FS | Eden | Class |
|---|---|---|---|---|---|
| SR_LANEID (0x00) | `state.lane` ([[thread_index_in_simdgroup]]) | 0 | `__lane` (fixed during audit; tested `T/shader_coverage_tests.cpp:683-725`) | LaneId | VERIFIED (FS); IMPLEMENTED_UNVERIFIED (compute); PARTIAL (VS = 0) |
| SR_CLOCK / CLOCKLO / CLOCKHI / GLOBALTIMER (0x01, 0x50–0x53) | 0 | 0 | 0 | 0 (stub) | VERIFIED (CLOCKLO = 0, `shader_coverage_tests.cpp:655,665`) |
| SR_VIRTCFG / VIRTID / ORDERING_TICKET / PRIM_TYPE / SM_SHADER_TYPE / AFFINITY (0x02, 0x03, 0x0F, 0x10, 0x14, 0x1C) | 0 | 0 | 0 | 0 (stub) | IMPLEMENTED_UNVERIFIED |
| SR_INVOCATION_ID (0x11) | 0 | 0 | 0 | InvocationId (GS/TCS) | MISSING (only matters for GS/tess) |
| SR_Y_DIRECTION (0x12) | ±1.0 from `state.y_negate` (fixed during audit) | same | same | YDirection | VERIFIED (`shader_coverage_tests.cpp:619-664`) |
| SR_THREAD_KILL (0x13) | 0 | 0 | `simd_is_helper_thread() ? ~0 : 0` | helper ? -1 : 0 | VERIFIED (compute 0); IMPLEMENTED_UNVERIFIED (FS) |
| SR_INVOCATION_INFO / WSCALEFACTOR_XY/Z (0x1D–0x1F) | 0x00ff0000 | same | same | stage-dependent; default 0x00ff0000 | VERIFIED (default, `shader_coverage_tests.cpp:664`) |
| SR_TID packed (0x20) | `x OR y<<16 OR z<<26` | 0 | 0 | same packing | IMPLEMENTED_UNVERIFIED |
| SR_TID_X/Y/Z (0x21–0x23) | `state.tid.*` | 0 | 0 | LocalInvocationId | IMPLEMENTED_UNVERIFIED |
| SR_CTA_PARAM (0x24) / SR_NTID (0x28) | 0 (logged) | 0 | 0 | 0 (stub) | IMPLEMENTED_UNVERIFIED |
| SR_CTAID_X/Y/Z (0x25–0x27) | `state.ctaid.*` | 0 | 0 | WorkgroupId | IMPLEMENTED_UNVERIFIED |
| SR_EQMASK (0x38) | `1u << lane` | lane 0 | `1u << __lane` | SubgroupEqMask | VERIFIED (FS); IMPLEMENTED_UNVERIFIED (compute) |
| SR_LTMASK / LEMASK / GTMASK / GEMASK (0x39–0x3C) | correct formulas (wrap-safe at lane 31) | lane 0 | `__lane` | subgroup masks | IMPLEMENTED_UNVERIFIED |
| SR_SWINHI/LO/SZ, SMEMSZ, REGALLOC, … | 0 (logged) | 0 | 0 | 0 (stub) | IMPLEMENTED_UNVERIFIED |

### b.3 Attribute address map (`SD/const.hpp:63-96`, `SD/const.cpp:5-51`; reference `EDEN/frontend/ir/attribute.h:16-233`)

| Address (bytes) | Name | VS in | VS out | FS in | Class |
|---|---|---|---|---|---|
| 0x060 | PrimitiveId | — | — | `[[primitive_id]]` | IMPLEMENTED_UNVERIFIED |
| 0x064 | Layer | — | `[[render_target_array_index]]` (verified) | `[[render_target_array_index]]` | VERIFIED (out) / IMPLEMENTED_UNVERIFIED (in) |
| 0x068 | ViewportIndex | — | `[[viewport_array_index]]` (verified) | `[[viewport_array_index]]` | VERIFIED (out) / IMPLEMENTED_UNVERIFIED (in) |
| 0x06C | PointSize | — | `[[point_size]]` when drawing points | — | VERIFIED (fixed size) |
| 0x070–0x07C | Position | — | `[[position, invariant]]` | `[[position]]` (w = 1/w) | VERIFIED |
| 0x080–0x27C | Generic 0–31 | `[[attribute(i)]]` by vertex format | `[[user(locn i)]]` | `[[user(locn i)]]` + flat/center_no_perspective | VERIFIED (out/in), IMPLEMENTED_UNVERIFIED (formats are out of scope) |
| 0x280–0x2BC | Front/back diffuse/specular colour | — | dropped | uninitialised | MISSING |
| 0x2C0–0x2DC | ClipDistance 0–7 | — | `[[clip_distance]]` gated by the enable mask | uninitialised | VERIFIED (out) |
| 0x2E0/0x2E4 | PointSprite S/T | — | — | `[[point_coord]]` with origin handling | VERIFIED |
| 0x2E8 | FogCoordinate | — | dropped | uninitialised | MISSING |
| 0x2F0/0x2F4 | TessEvalPoint U/V | — | — | not populated | MISSING |
| 0x2F8 / 0x2FC | InstanceId / VertexId | `[[instance_id]]` / `[[vertex_id]]` | — | — | VERIFIED |
| 0x300–0x39C | FixedFncTexture 0–9 | — | dropped | uninitialised | MISSING |
| 0x3A0 | ViewportMask | — | dropped | — | MISSING |
| 0x3FC | FrontFace | — | — | `[[front_facing]]` → ~0/0 | VERIFIED |
| Indexed access (register + immediate) | any | read works | written into `a_out` | read works | PARTIAL: undeclared by the analyzer |

---

## (c) Bit-position mismatches and semantic bugs (exact quotes)

Each quote comes from the 10:22:47 snapshot.

**C1. TXQ and TXQ.B opcodes are swapped.** `SD/decoder/decoder.cpp:503-504`
```cpp
INST(0xdf50000000000000, 0xfff8000000000000) { EMIT(Txq); }
INST(0xdf48000000000000, 0xfff8000000000000) { EMIT(TxqB); }
```
Reference: `maxwell.inc:109-110` `INST(TXQ, "TXQ", "1101 1111 0100 1---")` / `INST(TXQ_b, "TXQ (b)", "1101 1111 0101 0---")`. NAK 2351-2360: `TexRef::Bound(idx) => { e.set_opcode(0xdf48); e.set_field(36..49, idx); }`, `TexRef::Bindless => { e.set_opcode(0xdf50); }`. ENVY 1957-1958 agrees.

Effect: a bound `TXQ Rd, RZ, …` (`textureSize(tex, 0)`) goes to `emitTxqB`. That calls `trackBindlessHandle(context, inst.src)` with `src == RZ`, which returns nullopt (`texture.cpp:156-157,808-811`). The destination registers are never written. A real TXQ.B queries `cbuf_index` (bits 36..48, normally 0). The upstream mapping was already inverted (commit 487aa32d), and commit 0ba775bc added `TxqB` to the other half of the pair.

**C2. TLDS is decoded with the TEXS encoding table.** `SD/decoder/texture.hpp:86-87` `using InstTexs = InstTexsTlds; using InstTlds = InstTexsTlds;` and `texture.cpp:706-710`:
```cpp
void emitTlds(DecoderContext& context, InstTlds inst) {
    emitTextureSample(context, inst.pred, inst.pred_inv, true, inst.target, ...
```
`TextureSampleTarget` (`texture.hpp:56-71`) is the TEXS table: 0 1D.LZ, 1 2D, 2 2D.LZ, 3 2D.LL, 4 2D.DC, 5 2D.LL.DC, 6 2D.LZ.DC, 7 A2D, 8 A2D.LZ, 9 A2D.LZ.DC, 10 3D, 11 3D.LZ, 12 CUBE, 13 CUBE.LL.

The real TLDS table (`EDEN-I/texture_load_swizzled.cpp:80-131`; ENVY tabd200_0/1 at 932-956) is: 0 1D.LZ, 1 1D.LL, 2 2D.LZ, 4 2D.LZ.AOFFI, 5 2D.LL, 6 2D.LZ.MS, 7 3D.LZ, 8 A2D.LZ (coords Rb, Rb+1), 12 2D.LL.AOFFI.

Only encodings 0 and 2 are correct. Encodings 4, 5 and 6 also set `DepthCompare`, so the binding is declared `depth2d` (`memory_analyzer.cpp:62-66`) even though a colour texture is bound.

**C3. TEXS 2D.LL.DC swaps the depth reference and LOD.** `texture.cpp:385-392`
```cpp
case TextureSampleTarget::_2DLodLevelDepthCompare:
    ...
    cmp_value = RB();
    lod = RB();
```
Reference: `EDEN-I/texture_fetch_swizzled.cpp:99-105`, `ImageSampleDrefExplicitLod(handle, Composite(v, reg_a, reg_a + 1), v.F(reg_b + 1), v.F(reg_b), {}, info)`. The signature is `(handle, coords, dref, lod, …)` (`EDEN/frontend/ir/ir_emitter.h:326-328`), so dref = Rb+1 and lod = Rb.

**C4. TXD derivative register layout.** `texture.cpp:753-761`
```cpp
reg_t derivative = inst.src_b;
const auto dx = b.opVectorConstruct(..., {R(derivative++), R(derivative++)});
const auto dy = b.opVectorConstruct(..., {R(derivative++), R(derivative++)});
```
This reads the layout as [dPdx.x, dPdx.y, dPdy.x, dPdy.y]. Eden's layout is interleaved: `EDEN/backend/spirv/emit_spirv_image.cpp:133-136` uses `deriv_x_accum.push_back(OpCompositeExtract(derivatives, i * 2)); deriv_y_accum.push_back(OpCompositeExtract(derivatives, i * 2 + 1));`, so the registers are [dPdx.x, dPdy.x, dPdx.y, dPdy.y].

The existing test (`T/shader_library_tests.cpp:1791-1802`) loads `{0.5, 0, 0, 0.5}`, which gives the same gradients under both layouts, so it cannot catch the error. Axis-aligned isotropic gradients happen to agree under both layouts. Rotated or anisotropic gradients come out transposed.

**C5. ATOMS.CAS is decoded as an ordinary ATOMS.** `decoder.cpp:263-265`
```cpp
INST(0xee00000000000000, 0xff80000000000000) {
    emitAtoms(context, inst);
}
```
`compute.cpp:332`: `const u32 op = static_cast<u32>(extractBits(inst, 52, 4));`. In this encoding bits 52..55 are the CAS subop: NAK 2797-2803 `e.set_opcode(0xee00); … let subop = match self.atom_type { AtomType::U32 => 4_u8, AtomType::U64 => 5_u8, … }; e.set_field(52..56, subop);`. ENVY 654-665: `cast` / `cast.spin` / `cas`, size at bit 52.

Result: `ATOMS.CAS.32` runs as **ATOMS.DEC** with the compare register as the wrap bound. `ATOMS.CAS.64` (subop 5) fails the size check only when bits 28..29 are non-zero; with them zero it runs as AND.

**C6. Fragment output registers.** `SD/decompiler.cpp:121-123`
```cpp
if (header.ps.omap_depth)
    context.fragment_depth_register =
        static_cast<u32>(std::popcount(header.ps.omap_target)) + 1;
```
and `SD/codegen/lang/emitter.cpp:287-291` `... toType(color_target_data_type), getRegisterStr(i * 4 + c));`.

Eden (`EDEN-I/exit_program.cpp:15-34`) does the following:
- It skips render targets with no omap bits.
- Each enabled RT consumes 4 registers, including disabled components.
- The sample mask comes next at `src_reg`, then depth at `src_reg + 1`.

This matches nouveau's `nvc0_fp_assign_output_slots`. Hydra is wrong whenever an RT has a partial mask (for example 0x7, where depth should be R5 but Hydra uses R4) and whenever earlier RTs are unwritten (for example only RT1 written, which Eden reads from R0..R3 and Hydra from R4..R7).

**C7. LDG returns zero.** `memory.cpp:200-204`
```cpp
for (u32 i = 0; i < count; i++) {
    // TODO: global memory
    values.push_back(ir::Value::createConstantU(0));
}
```
STG, ATOM and RED are not implemented. There is no storage-buffer tracking (compare `EDEN/ir_opt/global_memory_to_storage_buffer_pass.cpp:373-412`, `nvn_bias {index 0, 0x100..0x700, align 16}`).

**C8. IPA perspective rule differs from Eden.** `memory.cpp:248-256`
```cpp
// TODO: why force perspective multiply when indexing?
if (indexed ||
    (is_generic &&
     context.decomp_context.frag.pixel_imaps[(src_a_imm - 0x80) >> 0x4]
             .getFirstUsedType() == PixelImapType::Perspective)) {
```
Eden (`load_store_attribute.cpp:165-190`) treats `Perspective || Unused` as perspective, applies the rule to legacy attributes unless the mode is SC, and handles indexed access by the attribute field. An index register of RZ is non-indexed in Eden, but Hydra reads `a_in[0]`. The earlier out-of-bounds `pixel_imaps[…]` read for addresses ≥ 0x280 was fixed during the audit by the `is_generic` guard.

**C9. LDC .IL/.IS/.ISL fall back to the default mode.** `memory.cpp:105-111`
```cpp
// TODO: slot addressing (mode 0 is the default and already supported)
if (static_cast<u32>(address_mode) != 0)
    LOG_NOT_IMPLEMENTED(ShaderDecompiler, "LDC address mode {} ...
```
Eden `load_constant.cpp:17-26` implements IS: `index = (Ra+imm)>>16 + imm_index`, `offset = (Ra+imm) & 0xffff`. (The sign and small-size LDC issues were fixed during the audit.)

**C10. TXQ ignores the LOD and emits invalid MSL for the level count.** `texture.cpp:270-271` `// TODO: src` / `(void)src;`. `msl/emitter.cpp:173-184` `dimensionToStr` has a `default: return INVALID_VALUE;`, which `:1087-1088` emits as `state.tex{}.get_{}()` → `get_INVALID()`. Eden `texture_query.cpp:20-25` reads the LOD from the register and returns mips in `.w`.

**C11. TEXS/TLDS F16 and TLD4S F16 bits are not decoded.** `texture.hpp:73-84` (`InstTexsTlds` stops at bit 56) and `:229-241` (`InstTld4s` has no bit 55). Eden: `texture_fetch_swizzled.cpp:18` `BitField<59, 1, Precision> precision;` with `Store16` packing at `:218-251`; `texture_load_swizzled.cpp:44`; `texture_gather_swizzled.cpp:25` (bit 55).

**C12. Depth-compare result vector.** `msl/emitter.cpp:1031-1033`
```cpp
// HACK: construct float4 if sample_compare
if (depth_compare)
    res = fmt::format("float4({}, 0.0, 0.0, 0.0)", res);
```
Eden `texture_fetch.cpp:199-201`: `value = element < 3 ? IR::F32{sample} : v.ir.Imm32(1.0f);` (also `texture_fetch_swizzled.cpp:174-182`).

**C13. Sub-word STS is a non-atomic read-modify-write.** `compute.cpp:84-92`
```cpp
const auto word = loadMem(addr);
... const auto kept = builder.opBitwiseAnd(word, builder.opBitwiseNot(mask));
storeMem(addr, builder.opBitwiseOr(kept, value));
```
Two threads storing different bytes of the same word lose one update. Eden routes `WriteShared 8/16` to an atomic helper or explicit 8/16-bit layout.

**C14. SUATOM INC/DEC ignore the wrap operand, and size/signedness is ignored.** `msl/emitter.cpp:1309-1316`
```cpp
case 3: // TODO: wrap
    func = "atomic_fetch_add"; operand = "1u";
case 4: // TODO: wrap
    func = "atomic_fetch_sub"; operand = "1u";
```
Also `compute.cpp:438` `// TODO: size (assumed to be 32 bit)`. Reference: Eden `surface_atomic_operations.cpp:156` `is_signed{size == Size::S32}`; NAK 2577 `e.set_field(36..39, atom_type)` for the bindless form.

**C15. SUST.D writes raw words without format conversion.** `compute.cpp:409-415` `// TODO: convert raw data to the image format` followed by `values[i] = getRegOffset(data, i);`. Reference: Eden `surface_load_store.cpp:258-269` (typed R8..R128_UINT image format).

**C16. Condition-code flow tests are dropped.** `decoder.cpp:176` (SYNC), `:315` (BRK), `:360` (EXIT), `:450` (BRA) carry `// TODO: ccc`, and `exit.hpp:8` `BitField64<u32, 0, 5> ccc; // TODO`. Reference: Eden `control_flow.cpp:109-127,415-418`: `const IR::FlowTest flow_test{has_flow_test ? inst.branch.flow_test.Value() : IR::FlowTest::T}; if (pred != Predicate{true} || flow_test != IR::FlowTest::T) …`.

**C17. CONT/PCNT, CAL/JCAL/RET are unsupported.** `decoder.cpp:311-313` `COMMENT_NOT_IMPLEMENTED("cont")`, `:388-393` pcnt, `:430-435` cal, `:349-355` `COMMENT_NOT_IMPLEMENTED("ret"); … context.builder.opExit();`. Reference: `control_flow.cpp:76-79` (PCNT/CONT token), `:331-351` (CAL creates a function call; RET returns).

**C18. Local memory sizing.** `decompiler.cpp:109-115`
```cpp
context.local_memory_size = (type == ShaderType::Compute ? 0x400u
    : std::max<u32>(header.sh_local_mem_lo_sz + header.sh_local_mem_hi_sz +
                    header.sh_local_mem_crs_sz, 0x100u));
```
Eden `program_header.h:216-219` uses `low | (high << 24)` and excludes crs. Compute should use QMD `local_pos_alloc` (`GPU/engines/compute.hpp:66`, never called). Out-of-bounds handling: Hydra clamps the index (`msl/emitter.cpp:1191,1197`), whereas Eden returns 0 on load and drops the store (`load_store_local_shared.cpp:91-95,155-161`).

**C19. TLD.MS is ignored.** `texture.cpp:827-833` `// TODO: multisample index`, with no `texture2d_ms` path (`msl/emitter.cpp:52-75`). Eden `texture_load.cpp:130-132` reads the sample index from the next meta register.

**C20. BAR modes are ignored.** `compute.cpp:369-376` `// TODO: modes other than BAR.SYNC 0` followed by an unconditional `opBarrier()`. NAK 3127-3134: mode in bits 32..34 (SYNC/ARV/RED/SCAN), reduction op in 35..36 (POPC/AND/OR).

**C21. Minor encoding-width mismatches.**
- `texture.hpp:25` TXQ query reads 7 bits (22..28); NAK/ENVY use 6 bits (22..27).
- `memory.hpp:19,33` ALD/AST offset reads 11 bits (20..30); NAK 2873 uses `set_field(20..30, …)` (10 bits).
- `memory.hpp:62` LDC offset is unsigned, but the RZ path is fine and the register path is now sign-extended in code.

**C22. reg_t wrap-around.** `texture.cpp:686-687` `copyTextureResult(context.builder, {dst0 + 0, dst0 + 1, dst1 + 0, dst1 + 1}, res, 0b1111);`. With `dst1 == RZ` (0xFF), `dst1 + 1` converts to `reg_t` (u8) value 0, which clobbers R0 (`common/types.hpp:299-301`). The same pattern exists in multi-register loads whose destination is RZ.

**C23. Shuffle outside compute.** `warp.cpp:16-19` `if (mode == ShuffleMode::Bfly && context.decomp_context.type == ShaderType::Fragment) res = context.builder.opQuadShuffleXor(src_a_v, src_b);` handles only BFLY in fragment shaders, ignores the mask and index range, and forces the predicate true. Every other non-compute case reaches `// TODO: implement` (`warp.cpp:61-66`), returning the source value. Eden `warp_shuffle.cpp:51-68` takes the quad fast path only for `mask == 0x1c03` with an immediate index in range, and uses general subgroup shuffles otherwise.

**C24. Texture declaration conflicts.** `memory_analyzer.cpp:141-149` `// TODO: handle this` / `LOG_WARN(ShaderDecompiler, "Texture type mismatch");`. The first access fixes the declaration type: depth2d vs texture2d, or array vs non-array. Mixed compare and non-compare use of one binding then fails to compile (`depth2d::sample` returns a scalar; `texture2d` has no `sample_compare`). Query-only textures are declared `texture2d` (`:115-117`).

---

## (d) Metal emission issues

| # | Issue | Evidence | Consequence / recommendation |
|---|---|---|---|
| D1 | **No reconvergence.** Every guest block is called from a per-thread `switch` inside a `while` loop. Lanes that take paths of different lengths to a merge block run it in different iterations. | `SD/codegen/lang/emitter.cpp:225-249` | `threadgroup_barrier` executes in non-uniform control flow, which is undefined in MSL: a partial SIMD-group can pass the barrier before its siblings' STS writes. `simd_shuffle`/`simd_ballot`/`quad_shuffle_xor` see inactive lanes. Implicit-LOD `sample()` and quad derivatives take values from inactive quad lanes, giving wrong mips or seams after divergent branches. Eden structurizes. Fix: a structurizer (Eden `structured_control_flow.cpp` style), or at minimum an explicit reconvergence join on SSY/SYNC and PBK/BRK regions. |
| D2 | Block-transition guard | `emitter.cpp:229-237` (65536) | Long compute loops truncate silently. Make it configurable per stage, or remove it once structured. |
| D3 | Implicit LOD outside fragment | `msl/emitter.cpp:1017-1019` covers compute only | Vertex `sample()` without `level()` is untested (UNKNOWN). Emit `level(0)` for every non-fragment stage, as Eden lowers implicit LOD. |
| D4 | 1D textures | `msl/emitter.cpp:1000` (`gradient1d` does not exist in MSL); `:1007-1017` add `level()`/`bias()` to `texture1d::sample`, which takes `(sampler, float)` only; 1D coordinates are emitted as `vec<f32,1>` | Probable compile failures for TEXS 1D.LZ and TEX 1D with any LOD mode. Drop LOD options for 1D (Metal 1D has no mips) and emit scalar coordinates. |
| D5 | Depth textures | `memory_analyzer.cpp:62-66,141-149`; `msl/emitter.cpp:1031-1033` | A depth/colour declaration conflict on one binding fails to compile. `float4(r,0,0,0)` differs from hardware/Eden `(r,r,r,1)`. Non-compare sampling of a `depth2d` returns a scalar that is then swizzled. Fix: declare both a `depth2d` and a `texture2d` alias for the binding and bind the same MTLTexture to both slots, or track usage per binding. |
| D6 | Texel fetch with DC flag | TLDS mis-decode (C2) reaches `msl/emitter.cpp:978-982` ("Texture read depth compare") and passes the compare value as an argument to `read()` | Invalid or nonsensical MSL plus a `depth2d` declaration for a colour texture. Fixing C2 removes the path; also assert that IntCoords and DepthCompare are never combined. |
| D7 | TXQ emission | `msl/emitter.cpp:1062-1089` | `get_width()` etc. take no `lod` argument (use `get_width(lod)`). `get_INVALID()` for mips (use `get_num_mip_levels()`). Cube depth returns 1. Query-only textures are typed `texture2d` regardless of the bound type, which risks a Metal validation error. |
| D8 | MSAA | no `texture2d_ms` anywhere | Needs `texture2d_ms<T>::read(uint2, uint sample)` and a renderer texture-type mapping. |
| D9 | Texel offsets | `msl/emitter.cpp:1022-1027,946-951` | MSL documents a [-8,7] range; TLD4 AOFFI/PTP allow [-32,31]. `T/README-texture-sampling.md` notes that dynamic offsets were validated only on the macOS 27 host. Clamp, or fall back to coordinate arithmetic outside the range. |
| D10 | Images | `msl/emitter.cpp:150-155` (only `access::write` or `read_write`); `compute.cpp:408-415`; `GPU/renderer/metal/renderer.cpp:1131,1172` | No `access::read` (SULD missing). Raw (.D) stores need an integer texture view (`MTLTextureUsagePixelFormatView` is already set in `GPU/renderer/metal/texture.cpp:70,78`). sRGB storage writes need a linear view. Images are never bound in graphics stages. |
| D11 | Atomics | `msl/emitter.cpp:1202-1220,1293-1345` | `memory_order_relaxed` matches Metal's model. There are no 64-bit threadgroup atomics (ATOMS.64 needs CAS-pair emulation). Texture atomics need R32Uint/R32Sint, MSL 3.1 and `MTLTextureUsageShaderAtomic` (set in `texture.cpp:60`). INC/DEC wrap is missing for images but correct for shared memory. |
| D12 | Threadgroup memory | `msl/emitter.cpp:248-250` | Metal's 32 KiB limit vs Maxwell's 48 KiB. Allocations above the limit are clamped and accesses alias. Log loudly. |
| D13 | Local memory | `msl/emitter.cpp:243-247,488` | A per-thread `Reg lmem[N] = {}` of up to 16 KiB inside `State` adds heavy register/stack pressure and zero-init cost. Out-of-bounds stores clobber the last word. |
| D14 | Constant buffers | `SD/codegen/lang/emitter.hpp:287-293` | Indexed `state.cN[(Rn + imm) >> 2]` has no bounds check and Metal has no robust buffer access. A garbage index can fault the GPU. Clamp the index to the bound size (pass sizes in a small buffer). |
| D15 | SIMD/quad width and lane | `warp.cpp:20-60`; `msl/emitter.cpp:498-501,662-665` | Apple SIMD-groups are 32 wide, matching Maxwell warps, so compute semantics hold. VS lane is 0 by design. Fragment lanes come from `[[thread_index_in_simdgroup]]`, but quad layout is derived from positions (good, `msl/emitter.cpp:297-321`). |
| D16 | Helper lanes and `discard_fragment()` | `msl/emitter.cpp:1092` | Execution continues after discard. Correct derivatives afterwards depend on Metal demote semantics (UNKNOWN), whereas Eden emits OpDemoteToHelperInvocation. |
| D17 | Uninitialised guest attributes | `SD/codegen/lang/emitter.cpp:65-193` (`State state;`) | Legacy, fog and fixed-function inputs read garbage. Zero-initialise `a_in` for unknown addresses, or populate them from SPH. |
| D18 | Compile options | `GPU/renderer/metal/shader_library_cache.cpp:58-61` | Good: `preserveInvariance=true` (needed by `[[position, invariant]]`) and fast math off. |
| D19 | Filtered sampling of integer textures | `msl/emitter.cpp:880-882` (log only) | `sample()` on `texture2d<uint>` is emitted without nearest-filter enforcement. |

---

## (e) Prioritized implementation backlog

Priorities combine how often real Switch shaders hit the case (UE4/Unity HLSL cross-compiled through NVN, plus NVN first-party GLSL) with severity and cost. "Test" means a new case in `T/shader_library_tests.cpp` or `T/shader_coverage_tests.cpp` using the existing harnesses: decode guest words, run `MemoryAnalyzer`, emit with `MslEmitter`, append a wrapper kernel or vertex/fragment pair, compile with `device->newLibrary`, run, and read back. Compute cases can reuse `runAlu`/`runKernel` (`T/shader_coverage_tests.cpp:146-272`). Texture cases can reuse the texture fixtures in `testTextureOffsetAndGather`/`testTextureLodSelection` (`T/shader_library_tests.cpp:1720-2149`).

### P0: confirmed decode bugs in common shaders (cheap fixes)

1. **Swap TXQ/TXQ.B and complete TXQ** (C1, C10)
   - *Why:* `textureSize`/`GetDimensions` are common in NVN GLSL and in UE4/Unity HLSL (post-processing, HZB, TAA, SSR). Today bound TXQ results are never written, so later code reads garbage registers.
   - *How:* in `decoder.cpp:503-504`, map `0xdf48 → EMIT(Txq)` and `0xdf50 → EMIT(TxqB)`. Pass the LOD register (bound: Ra; bindless: Ra+1) through `opTextureQueryDimension`. Emit `get_width(lod)` etc. and `get_num_mip_levels()` for component 3. Narrow `query` to 6 bits.
   - *Test:* bound 8×4 texture with 3 mips plus a 5-layer 2D array and a 3D texture. Decode `TXQ R4, RZ, DIMENSION, idx 7, mask 0xF` (`0xdf48 | 4 | RZ<<8 | 7<<16 | 1<<22 | 0xF<<31 | 7<<36`) and a second TXQ with Ra = R1 holding 1. Expect (8,4,1,3) and (4,2,1,3). Add TXQ.B with the handle traced from `MOV32I`. Assert that the generated source contains no `INVALID`.
   - *Risk:* low. The current behaviour is broken; check that Crash 4 shaders using 0xdf48 still find their texture binding.

2. **Give TLDS its own encoding table** (C2)
   - *Why:* `texelFetch`/`Texture.Load` with an explicit mip (HZB, SSR, SSAO), with offset, on 3D textures (volumetric fog, LUTs), on 2D arrays (shadow cascades) or with MSAA. Five of nine encodings are wrong and three declare colour textures as depth.
   - *How:* replace `emitTlds` (`texture.cpp:706-710`) with a switch mirroring `EDEN-I/texture_load_swizzled.cpp:80-134`, reusing `emitTextureSample2`'s IntCoords path with explicit LOD, offset and MS sample.
   - *Test:* for each encoding (0, 1, 2, 4, 5, 6, 7, 8, 12), build a matching texture with grid texels (`1 + y*10 + x`, plus a per-mip/layer/slice/sample bias), decode the TLDS word (`0xd200… | enc<<53 | swizzle RGBA (4)<<50 | dst0/dst1 | Ra/Rb | idx 7<<36 | 1<<59`), and compare with a CPU texelFetch.
   - *Risk:* low-medium. Encodings 0 and 2 must stay identical.

3. **Fix TXD derivative interleave and the TEXS 2D.LL.DC swap** (C3, C4)
   - *Why:* `textureGrad` (UE4 SampleGrad in decals, POM and virtual texturing) and shadow `SampleCmpLevel` with LOD.
   - *How:* `texture.cpp:753-761`: dx = (d0, d2), dy = (d1, d3). `texture.cpp:390-391`: lod = Rb, cmp = Rb+1.
   - *Test:* extend `testTextureLodSelection` with asymmetric gradients, e.g. registers `[0.5, 0.25, 0, 0]` (correct: dPdx = (0.5,0), dPdy = (0.25,0), LOD 1.0; transposed: LOD ≈ 1.16, detectable with a linear mip filter). For TEXS LL.DC, use a depth texture with mip0 = 0.25 and mip1 = 0.75, LOD = 1 and ref = 0.5 with a LESS sampler; check the compare result.
   - *Risk:* low.

4. **Fix fragment output register mapping per the SPH** (C6)
   - *Why:* MRT passes that leave earlier targets unwritten (UE4 DBuffer/deferred decals, partial G-buffer passes) and depth export with vec3 colour outputs.
   - *How:* compute per-RT base registers from `omap_target` in `decompiler.cpp` (4 per RT with any bit set; skip empty RTs). Store them in `DecompilerContext`, set depth = 4·nRT + 1, and use them in `SD/codegen/lang/emitter.cpp:279-292`. Do not write RTs absent from the omap.
   - *Test:* clone `testFragmentDepth` with `omap_target = 0x70` (RT1 RGB only) and 0x7 (RT0 RGB) plus depth. Check that RT1 receives R0..R2 and depth comes from R5.
   - *Risk:* low for full masks, which map identically.

5. **Decode ATOMS.CAS** (C5)
   - *Why:* shared-memory CAS loops (float atomics emulation, lock-free lists in compute). It is currently silently DEC or AND.
   - *How:* route `0xee00/0xff80` to `emitAtomsCas`: subop at bits 52..55 (4 = 32-bit, 5 = 64-bit), compare in Rb, new value in Rb+1, result in Rd. Emit an `atomic_compare_exchange_weak_explicit` loop on `threadgroup atomic_uint`; emulate 64-bit with a word-pair lock or CAS on the low word plus validation. CAST/SPIN variants can use the same code.
   - *Test:* 64 threads each run a CAS-increment loop on a word. Expect the final value 64 and unique returned values. Add single-thread mismatch/no-swap cases.
   - *Risk:* low.

### P1: missing features with high game impact

6. **Global memory (LDG/STG/ATOM/RED) through SSBO tracking** (C7)
   - *Why:* Unity and UE4 structured/RW buffers, GPU particles, NVN first-party compute (TotK and similar). Today LDG returns 0 and STG is dropped.
   - *How:* in the decoder, trace the address register (and pair for `.E`) back with the backward scanner used for bindless handles (`texture.cpp:111-228`) through IADD/IADD3/IADD.X/ISCADD to `LDC/MOV c[0][0x100..0x6F0]` (16-byte aligned descriptor: addr_lo, addr_hi, size), as in Eden's `nvn_bias`. Add IR `LoadStorage/StoreStorage/AtomicStorage(binding, byte_offset, size)`. In MSL, bind `device uint* ssboN [[buffer(k)]]` with bounds against the descriptor size. In `GPU/engines/3d.cpp` and `GPU/engines/compute.cpp`, read the descriptor from the bound cbuf and bind the buffer-cache range. Later, add a global fallback (address-range table or Metal GPU addresses).
   - *Test:* compute program `LDC.64 R2, c[0][0x110]; S2R R0, SR_TID.X; ISCADD …; LDG.E R4,[R2]; IADD R4,R4,1; STG.E [R2],R4; ATOM.ADD …; EXIT` over a 256-word buffer whose address and size sit in cbuf0. Check the increments and the atomic total.
   - *Risk:* medium (buffer cache coherency, aliasing with textures).

7. **SULD and image completeness** (C15, D10)
   - *Why:* `imageLoad`/RWTexture reads in compute (UE4 HZB, histograms; NVN post-processing). SUST.D on non-R32 formats. Fragment-stage image stores.
   - *How:* add IR `ImageLoad`. P mode: per-component mask → registers. D mode: typed integer view. Declare `access::read_write` when an image is both loaded and stored. Add a format-view cache in the texture cache. Bind images for VS/FS in `GPU/engines/3d.cpp`. Implement SURED via the ImageAtomic path with no result.
   - *Test:* R32Uint and RGBA8Unorm images. SULD.P mask R/RG/RGBA, SULD.D.32 on RGBA8 (raw packed), SUST.D.32 round trip, and a fragment SUST writing pixel coordinates.
   - *Risk:* medium.

8. **Control-flow completeness: PCNT/CONT, CAL/JCAL/RET, CC flow tests, PEXIT** (C16, C17)
   - *Why:* `continue` in loops (UE4 light loops), compiler subroutines, and CC-conditioned branches. Today these become silent fallthrough or exit.
   - *How:* add a continue stack alongside `break_point_stack` (`decoder.hpp:14-19`). Handle CAL by pushing the return label and branching (inline per call site, or a return-label stack variable plus dispatch). RET pops. Evaluate `ccc` (bits 0..4) with the CSETP condition helper (`compute.cpp:281-306` already handles tests 0/13/15; extend to all 32 Maxwell flow tests) and AND it with the predicate.
   - *Test:* an 8-iteration loop that skips odd iterations via CONT and checks the sum. CAL to a block that adds a constant, then RET. `IADD.CC` then `BRA CC.NE`, checking both branches.
   - *Risk:* medium (the decoder block model).

9. **Reconvergence and structurization** (D1)
   - *Why:* barrier correctness in compute after divergent prologues, SHFL/VOTE correctness, and implicit derivatives after branches (a plausible source of the edge noise and stippling seen in Crash 4/UE4 games).
   - *How:* port Eden's structured control flow over Hydra IR (using `SD/analyzer/cfg.*` as the starting point), or emit SSY/SYNC regions as structured if/else in MSL.
   - *Test:* (a) compute: `@P0 BRA skip; STS [tid*4], tid; skip: BAR.SYNC; LDS R1, [(tid^1)*4]` across many threadgroups; every lane must see its neighbour's value. (b) fragment: a divergent if/else with blocks of different lengths, followed by an implicit-LOD TEX on coloured mips; compare with the non-divergent version.
   - *Risk:* high (large change), but it removes a class of bugs.

10. **F16 texture results** (C11)
    - *Why:* mediump-heavy shaders (mobile-style Unity/UE4 content, some first-party titles). Eden implements this, which suggests real titles use it.
    - *How:* decode bit 59 (TEXS/TLDS, F16 when 0) and bit 55 (TLD4S). Pack with `as_type<uint>(half2(a,b))` into Rd/Rd' following Eden's Store16 (`texture_fetch_swizzled.cpp:218-251`).
    - *Test:* TEXS with bit 59 = 0 and RG/RGB/RGBA swizzles. The wrapper reads the registers as uint and unpacks the halves.
    - *Risk:* low.

### P2: correctness gaps with medium frequency

11. LDC.IS (and logged IL/ISL) (C9). Test: two cbufs where segmented addressing selects slot 3 from `Ra >> 16`.
12. DC result vector (r,r,r,1) plus depth/colour alias declarations (C12, C24, D5). Test: TEX.DC with mask 0xF; a shader mixing TEXS and TEXS.DC on one binding must compile and both results must be correct.
13. 1D sampling emission (D4) and `TEX ARRAY_3D` rejection. Test: compile and run TEXS 1D.LZ / TEX 1D.LB on a 1D texture.
14. TXD 1D/1D-array. TLD/TLDS MS (`texture2d_ms`) (C19). TMML via `calculate_*_lod` (FS) with Eden's fixed-point packing (`texture_mipmap_level.cpp:98-111`). Tests: MS resolve-style fetch of sample 2; TMML on a quad with known derivatives.
15. SHFL outside compute (C23) using `simd_shuffle*` in FS (with the Eden quad fast path for mask 0x1c03) and VS. VOTE/BAR tests. BAR.RED.POPC/AND/OR via threadgroup scratch reduction; BAR.ARV via split barrier emulation (C20). Tests: SHFL modes against a CPU reference over 32 lanes including clamp/segmask and the predicate; BAR.RED.POPC count.
16. Local memory size from the QMD and the SPH formula (`lo | hi<<24`, without crs). Out-of-bounds load → 0 and out-of-bounds store dropped (C18). Test: a compute local array of 0x600 bytes with a stride pattern.
17. STS sub-word atomicity (C13): use `threadgroup uchar/ushort` views or `atomic_fetch_and/or`. Test: 64 threads each storing a distinct byte of 16 words; every byte must survive.
18. SUATOM size/signedness and INC/DEC wrap (C14); 64-bit and float SUATOM where Metal allows. Test: R32Sint MIN/MAX with negative operands on an R32Uint-declared binding; INC wrap at 3.
19. IPA rule alignment with Eden: Unused imap counts as perspective, indexed IPA uses the attribute field, legacy attributes are populated from SPH (C8). Test: IPA.MULTIPLY on an attribute whose imap is Unused; indexed IPA over two flat varyings.
20. Memory analyzer: declare attribute ranges reachable by indexed ALD/AST/IPA (using SPH imap/omap) instead of asserting. Test: a VS writing `a[R0 + 0x80]` over 4 outputs while the FS reads all of them.

### P3: large or rare

21. Dynamic bindless descriptor arrays (indexed LDC, SHL/IADD handle math, shifted secondary handles), and texture type from TIC for query-only or shadow-only bindings (`EDEN/ir_opt/texture_pass.cpp`).
22. Geometry shader emulation (compute pre-pass or Metal mesh/object shaders), tessellation (Metal tessellator with a compute TCS and a post-tessellation vertex function), and VertexA merge. Implement OUT, ISBERD, AL2P, ALD/AST vertex index and patch, and SR_INVOCATION_ID/INFO with them.
23. Sparse residency (.SP predicates): emit `sparse_sample` where Metal supports it, otherwise set the predicate to resident. PIXLD (MY_INDEX → `[[sample_id]]`, COVMASK → `[[sample_mask]]`). Fragment sample-mask output. IPA centroid/sample qualifiers.
24. MEMBAR → `atomic_thread_fence` once device memory exists. LD/ST generic (Eden lacks these too), keeping IMAD32I ahead of LD in the decoder. Constant-buffer index clamping (D14).
25. Block-guard removal after structurization (D2). Log loudly when shared memory exceeds 32 KiB (D12).

**Regression notes.**
- Items 1–5 change only clearly broken paths.
- For item 1, confirm with a capture of a known `textureSize` user, because the inverted mapping existed upstream.
- Item 4 is identity for full masks.
- Items 6 and 9 are the only high-risk changes and need the full `hydra-shader-library-tests` run, plus Crash 4 and Jump Force gameplay comparisons with GPU captures.

---

## Summary counts (section (a) inventory, 10:22:47 snapshot)

Computed mechanically from the table rows above (217 opcode/variant rows):

| Class | Rows | Memory | Texture | Surface | Control flow | Warp/sync | Stage I/O |
|---|---|---|---|---|---|---|---|
| VERIFIED | 30 | 9 | 8 | 0 | 2 | 0 | 11 |
| IMPLEMENTED_UNVERIFIED | 67 | 9 | 28 | 6 | 11 | 5 | 8 |
| PARTIAL | 48 (17 marked BUG) | 6 | 25 | 4 | 2 | 3 | 8 |
| MISSING | 71 | 19 | 15 | 7 | 11 | 4 | 15 |
| UNKNOWN | 1 | 0 | 1 | 0 | 0 | 0 | 0 |

Section (b), counted separately:
- SPH fields: 20 rows. IMPLEMENTED_UNVERIFIED 5, PARTIAL 7 (1 BUG: the omap-target register mapping), MISSING 8.
- System registers: 14 rows (primary class). VERIFIED 6, IMPLEMENTED_UNVERIFIED 7, MISSING 1.
- Attribute map: 16 rows. VERIFIED 9, IMPLEMENTED_UNVERIFIED 1, PARTIAL 1, MISSING 5.

**Fixed while this audit ran** (verified in the 10:22:47 snapshot; earlier state seen at ~09:55):
- FS FrontFacing, PointCoord, PrimitiveId, Layer and ViewportIndex inputs.
- VS PointSize, ClipDistance, Layer and ViewportIndex outputs. Before the fix, a ClipDistance write produced an undeclared `__out.user36` and an MSL compile failure.
- Attribute address map, and the out-of-bounds `pixel_imaps` read in IPA.
- S2R in graphics stages: Y_DIRECTION, THREAD_KILL, LANEID/masks in FS, INVOCATION_INFO. CS2R is now routed to S2R.
- LDC signed register offsets and U8/S8/U16/S16 sizes.
- New GPU tests in `T/shader_coverage_tests.cpp`.
