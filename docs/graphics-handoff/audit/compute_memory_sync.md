# Hydra GPU audit: compute, GPU memory, command submission and synchronization

> Evidence policy (2026-10-18): NVIDIA documentation/headers define guest
> behavior; Apple Metal documentation, SDK headers and validation define host
> behavior. Eden and other emulators are historical comparators only. Any
> conclusion based solely on them is unverified and must be corroborated.

Read-only audit of `/Users/arvino/Documents/Project/hydra`. No build or run was done.
Scope: MAXWELL_COMPUTE_B (B1C0) and QMD, inline-to-memory (I2M), PFIFO/GPFIFO host methods (B06F),
engine semaphores and reports, fences and syncpoints, GMMU, buffer and index caches, CPU/GPU coherency
(stage-1 and stage-2 tracking), Metal encoder ordering and barriers.

**Snapshot note.** The working tree changed while I was auditing it. `engines/3d.cpp`,
`shader_decompiler/codegen/lang/msl/emitter.cpp` and `tests/shader_library_tests.cpp` were all modified during
the audit. Line numbers are as of 2026-10-17 ~10:10–10:20 WIB. I re-checked every quoted snippet against
the tree at that time. `3d.cpp` was still being edited while I wrote this (`getVertexBuffer` moved by
40 lines). When a 3d.cpp line looks off, search for the function name given next to it.

`REFS = /private/tmp/claude-501/-Users-arvino-Documents-Project-hydra/0e906531-f45a-4e9b-b0bc-dcb303edaa1b/scratchpad/refs`.
Hydra paths are relative to the repo root. Abbreviations:
- `gpu/` = `src/core/hw/tegra_x1/gpu/`
- `mtl/` = `gpu/renderer/metal/`
- `hv/` = `src/core/hw/tegra_x1/cpu/hypervisor/`
- `nvdrv/` = `src/core/horizon/services/nvdrv/ioctl/`
- `eden/` = `$REFS/yuzu/src/video_core/`
- `ogd/` = `$REFS/opengpudoc/`
- `sr/` = `$REFS/yuzu/src/shader_recompiler/` (`sr/maxwell.inc` = `sr/frontend/maxwell/maxwell.inc`)

## Executive summary

- **No guest-visible GPU synchronization exists.** `SubmitGpfifo` returns a zero fence and ignores the fence flags.
  GPU syncpoint 0 waits succeed immediately. Host methods `0x00–0x3F` (semaphores, syncpoints, WFI,
  reference) are never decoded: they are stored as plain engine registers. PFIFO runs asynchronously, so the
  guest is never throttled by GPU progress. It can read GPU results, or reuse buffers, before the GPU
  thread has even encoded the work.
- **Compute is partial.** The compute engine decodes the QMD correctly: grid, block and shared size, and the
  8 constant buffers all match Eden's `LaunchParams` bit for bit. Global memory is not implemented:
  - `LDG` returns the constant 0.
  - `STG`, `ATOM`, `RED`, generic `LD`/`ST`, `SULD` and `SURED` are not implemented.
  - No storage buffers (SSBOs) are tracked anywhere.

  Compute shaders that use SSBOs (NVN exposes SSBOs as global memory) therefore read zeros and drop their
  writes. `linked_tsc` and the QMD local-memory sizes are also ignored.
- **The buffer cache is sound for CPU→GPU data.** Immutable snapshots are taken at encode time
  (≤64 KiB), and larger ranges are ordered with blits, so each draw/dispatch sees CB contents as of its
  own encode. This includes `CB_DATA` updates. It is covered by Metal pixel tests.
- **GPU→guest write-back is only partial.**
  - It works for color textures before copy/2D/I2M engine reads (tested).
  - It does not happen on CPU access in the default stage-1 mode.
  - It never reaches the buffer cache.
  - Depth and 3D textures are never written back.
  - Query and semaphore values are written at encode time.
- **Coherency holes:**
  - HLE service writes into guest memory (fs reads, IPC out-buffers) never invalidate GPU caches.
  - Decoded index buffers (u8 / quads / fans) are never invalidated.
  - Index ranges ignore `INDEX_BUFFER_F.FIRST`.
  - Stage-1 write tracking re-protects page-table entries with no TLB invalidation. This needs a test; it could
    explain *nondeterministic* stale data.
- **Thread-safety bugs:**
  - `Compute::launch` uses the `BufferCache` without its mutex.
  - The `GMmu` `std::map` is changed by nvdrv ioctl threads while the PFIFO thread reads it.
  - The stage-2 CPU-fault readback runs on a separate Metal command buffer that can execute *before* the GPU
    thread's in-flight command buffer.

Class counts in table (a), 118 rows: **VERIFIED 16, IMPLEMENTED_UNVERIFIED 26, PARTIAL 23, MISSING 52, UNKNOWN 1.**

Legend:
- **VERIFIED**: implemented, and either covered by a test that checks results, or documented as working in game in `tests/README-*.md`. "in-game implicit" marks the core command path, which README-stability-followups.md:66-77 documents running CTR at 58.5–60 FPS with 430–814 draws/frame.
- **IMPLEMENTED_UNVERIFIED**: code exists, but no result-checking test.
- **PARTIAL**: implemented with gaps or bugs.
- **MISSING**: not implemented.
- **UNKNOWN**: cannot be determined from code and references.

---

## (a) Capability table

### A. Compute engine (MAXWELL_COMPUTE_B, class 0xB1C0)

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| A1 | SET_OBJECT bind of 0xB1C0 to subchannel | IMPLEMENTED_UNVERIFIED | gpu/gpu.cpp:55-57 | eden/engines/puller.cpp:48-50 | |
| A2 | SEND_PCAS_A = QMD address >> 8 (method 0xAD) | IMPLEMENTED_UNVERIFIED | gpu/engines/compute.hpp:14-15; compute.cpp:81-86 | ogd/clb1c0.h:349-350; eden/engines/kepler_compute.h:75-80 | QMD is read at launch time, with a plain `memcpy` of 0x100 bytes from one host mapping. |
| A3 | SEND_PCAS_B (FROM/DELTA, 0xAE) | MISSING | compute.hpp:16 (padding) | ogd/clb1c0.h:352-354 | Harmless: Eden ignores it too. |
| A4 | SEND_SIGNALING_PCAS_B launch (0xAF): INVALIDATE bit 0, SCHEDULE bit 1 | PARTIAL | compute.cpp:22 (`0xaf, 1, launch`) | ogd/clb1c0.h:356-362; eden/kepler_compute.cpp:65-81 | Any write launches, even SCHEDULE=0. This matches Eden. |
| A5 | Program address = SET_PROGRAM_REGION (0x582) + QMD PROGRAM_OFFSET (w0x08) | IMPLEMENTED_UNVERIFIED | compute.cpp:88-90; compute.hpp:50 | eden/shader_cache.cpp:86-89; ogd/clb1c0.h:625 | Matches. |
| A6 | QMD grid dims (w0x0C[30:0], w0x0D[15:0]/[31:16]) | IMPLEMENTED_UNVERIFIED | compute.hpp:51-53 | eden/engines/kepler_compute.h:139-143 | Bit-exact. See the QMD table below. |
| A7 | QMD block dims (w0x12[31:16], w0x13[15:0]/[31:16]) | IMPLEMENTED_UNVERIFIED | compute.hpp:55-57 | kepler_compute.h:149-153 | Bit-exact. |
| A8 | QMD SHARED_ALLOC (w0x11[17:0]) → threadgroup memory | PARTIAL | compute.hpp:54; compute.cpp:140; msl/emitter.cpp:247-249 (`std::clamp(..., 1u, 0x2000u)`) | kepler_compute.h:147; eden/shader_environment.cpp:418 | Clamped to 32 KiB, the Metal limit; Maxwell allows 48 KiB. Accesses past the clamp silently alias the last word through `min()`. |
| A9 | Shared memory ops LDS/STS/ATOMS | VERIFIED | decoder/compute.cpp:48-63, 338-367 | sr/maxwell.inc:233 (ATOMS) | tests/shader_library_tests.cpp `testSharedAtomics` (PASS line 334): 64-thread contention on Metal. |
| A10 | QMD CONSTANT_BUFFER_VALID mask (w0x14[7:0]) + 8×(ADDR_LO w0x1D+2i, ADDR_HI[7:0]/SIZE[31:15] w0x1E+2i) | IMPLEMENTED_UNVERIFIED | compute.hpp:58-65; compute.cpp:24-35 | kepler_compute.h:155-172, 262 | Bit-exact. The range is not checked against the mapping's end. |
| A11 | Each dispatch sees CB contents as of dispatch encode | IMPLEMENTED_UNVERIFIED | compute.cpp:203-216 → BufferCache::get | eden/buffer_cache/buffer_cache.h:400 (SetUniformBuffersState) | Same mechanism as F10 (verified for draws). The compute path has no test, and see bug B4 (no lock). |
| A12 | QMD LOCAL_POS/NEG/CRS alloc (w0x2D–0x2F) | PARTIAL | compute.hpp:66 (`localPosAlloc()` is defined but never used); decompiler.cpp:109-114 (`? 0x400u`) | eden/shader_environment.cpp:415 (`local_pos_alloc + local_crs_alloc`) | Compute local memory is fixed at 1 KiB per thread. Larger accesses clamp to the last word: silent corruption. |
| A13 | Local memory LDL/STL | VERIFIED | decoder/compute.cpp:361-367; msl/emitter.cpp:243-246 | – | `testLocalMemory` (PASS shader_library_tests.cpp:263). |
| A14 | SET_SHADER_LOCAL_MEMORY_A/B/WINDOW, THROTTLED/NON_THROTTLED | MISSING | not in RegsCompute | ogd/clb1c0.h:364-380, 482-489 | Harmless: Hydra keeps local memory per thread in MSL. Only generic LD/ST (A28) would need the window. |
| A15 | QMD BARRIER_ALLOC / GPR_ALLOC / SASS_VERSION / CACHE_LAYOUT | MISSING | not decoded | kepler_compute.h:157, 176, 181, 186 | Harmless on Metal. |
| A16 | QMD LINKED_TSC (w0x0B bit 30): sampler index = header index | MISSING | compute.cpp:239-240 always uses `getSamplerHandle(*handle)` (bits 20..31) | kepler_compute.h:137; eden/renderer_vulkan/vk_compute_pipeline.cpp:173-191; eden/textures/texture.h:159-166 | Wrong sampler when NVN uses linked TSC. Bug B16. |
| A17 | QMD RELEASE0/RELEASE1 semaphores | MISSING | – | UNKNOWN: QMD release fields are not in the provided headers (no clb1c0qmd.h). Eden pads w0x15-0x1C (kepler_compute.h:160). | Eden does not implement it either. |
| A18 | Dependent QMD chaining | MISSING | – | Eden: not implemented | Low priority. |
| A19 | "Indirect" dispatch where the CPU/pushbuffer writes the QMD through I2M before launch | IMPLEMENTED_UNVERIFIED | inline_base.cpp:18-46 writes guest memory immediately; compute.cpp:81-86 reads it at launch | eden/kepler_compute.cpp:49-64 | Works implicitly, because I2M runs on the CPU in stream order. |
| A20 | Indirect dispatch from GPU-written args | MISSING | – | eden/kepler_compute.cpp:68-79; eden/renderer_vulkan/vk_rasterizer.cpp:600-615 (DispatchIndirect) | Needs SSBO / GPU-write tracking first. |
| A21 | SET_TEX_HEADER_POOL / SET_TEX_SAMPLER_POOL (+ MAXIMUM_INDEX) for compute | IMPLEMENTED_UNVERIFIED | compute.hpp:21-31; compute.cpp:109-132 | ogd/clb1c0.h:607-621; kepler_compute.h:88-106 | `tic_limit==0` is treated as "no limit"; the TSC index is not bounds-checked (B30). |
| A22 | SET_BINDLESS_TEXTURE (0x982) / tracked bindless handles in CBs | IMPLEMENTED_UNVERIFIED | compute.hpp:41-42; compute.cpp:37-67 | kepler_compute.h:120; shader_environment.cpp:416 | |
| A23 | Compute sampled textures + samplers | IMPLEMENTED_UNVERIFIED | compute.cpp:218-247; mtl/renderer.cpp:1112-1130 | vk_compute_pipeline.cpp:209-217 | |
| A24 | Compute storage images: SUST, SUATOM (0xEA00-0xEA3F, 0xEA60-0xEA7F) | IMPLEMENTED_UNVERIFIED | decoder.cpp:283, 293-294; compute.cpp:249-265 (`TextureUsage::Write`); mtl/renderer.cpp:1131-1137 | sr/maxwell.inc:209, 226 | No SUST test. |
| A25 | SULD, SURED, SUATOM.CAS (and SUATOM 0xEA40-0xEA5F) | MISSING | decoder.cpp:281, 285, 288, 291 (`COMMENT_NOT_IMPLEMENTED`) | sr/maxwell.inc:207-208, 227 | Compute image loads yield nothing. |
| A26 | Global loads LDG | MISSING | memory.cpp:165-172 returns `createConstantU(0)`; decoder.cpp:255 | sr/ir_opt/global_memory_to_storage_buffer_pass.cpp:373-412 | Bug B3. |
| A27 | STG / ATOM / ATOM.CAS / RED | MISSING | decoder.cpp:250, 253, 266, 272 | sr/maxwell.inc:61, 88, 101, 114, 232 | Bug B3. |
| A28 | Generic LD/ST | MISSING | decoder.cpp:519, 522 | sr/maxwell.inc:282-283 | |
| A29 | SSBO binding/tracking (CB-relative address + size), compute and 3D | MISSING | renderer.hpp:42-61 (no SSBOs in ComputeDispatch); renderer.hpp:196; const.hpp:469; 3d.cpp:1586 (`// TODO: storage buffers`) | eden/buffer_cache/buffer_cache.h:497-536, 2006-2050; vk_compute_pipeline.cpp:156-164 | Bug B3. |
| A30 | MEMBAR | PARTIAL | decoder.cpp:232-236 (comment only) | – | No `atomic_thread_fence`. Only matters once global atomics exist. |
| A31 | BAR.SYNC → `threadgroup_barrier(threadgroup\|device\|texture)` | IMPLEMENTED_UNVERIFIED | decoder/compute.cpp:369-376; msl/emitter.cpp:1218-1219 | – | Predicated BAR becomes a divergent barrier (UB in MSL). Other BAR modes are missing (B21). |
| A32 | INVALIDATE_SHADER_CACHES (0x87) / _NO_WFI (0x5A6) | MISSING | shader cache key = host pointer + 8 sampled bytes (shader_cache.cpp:232-270); renderer.hpp:94-96 (`// TODO`) | ogd/clb1c0.h:214-229, 631 | Code memory reused with the same sampled bytes gives a stale shader (B27). |
| A33 | INVALIDATE_TEXTURE_HEADER/SAMPLER/DATA caches, INVALIDATE_CONSTANT_BUFFER_CACHE_A-C | IMPLEMENTED_UNVERIFIED | Implicit: TIC/TSC are re-read per launch (compute.cpp:109-132); CBs are snapshotted per launch | ogd/clb1c0.h:260-312, 547-576 | A no-op is correct given Hydra's model. |
| A34 | Compute SET_REPORT_SEMAPHORE_A-D (0x6C0-0x6C3) | MISSING | compute.cpp:22 handles only 0x6C, 0x6D, 0xAF | ogd/clb1c0.h:663-699 | Eden is missing it too. |
| A35 | Compute SET_RENDER_ENABLE (conditional dispatch) | MISSING | – | ogd/clb1c0.h:593-605 | |
| A36 | Compute WAIT_FOR_IDLE (0x44) | IMPLEMENTED_UNVERIFIED | writeReg only | ogd/clb1c0.h:51 | A no-op is acceptable: execution is in order and Metal tracks hazards. |
| A37 | Compute register file vs class method range | PARTIAL | compute.hpp:44 (`static_assert(sizeof(RegsCompute) == 0x983 * sizeof(u32))`); engine_base.hpp:66-71 (only `ASSERT_DEBUG`) | ogd/clb1c0.h:704-747 (methods to 0x33E4 and 0x3400+); Eden NUM_REGS 0xCF8 (kepler_compute.h:57) | Methods ≥0x984 write past `regs_raw` in release builds (B18). |
| A38 | Metal dispatch: pipeline per MTLFunction, threadgroup sizing | PARTIAL | mtl/renderer.cpp:1049-1145 | eden/renderer_vulkan/vk_rasterizer.cpp:585-636 | The dispatch is silently skipped when block > `maxTotalThreadsPerThreadgroup` (logged once) (B19). |
| A39 | Compute→compute visibility in one encoder | IMPLEMENTED_UNVERIFIED | mtl/command_buffer.cpp:117-131 (default serial compute encoder) | Eden uses an explicit barrier: vk_rasterizer.cpp:623-630 | Serial dispatch plus tracked resources are enough. |
| A40 | Compute↔render/blit ordering | IMPLEMENTED_UNVERIFIED | separate encoders (command_buffer.cpp:84-131); no untracked resources or heaps (grep finds no `HazardTracking`, `newHeap` or `MTL::Fence` in mtl/) | vk_rasterizer.cpp:623-630 | Relies on Metal automatic hazard tracking. |
| A41 | Locking around the caches in compute launch | PARTIAL | compute.cpp:76 (texture lock only); compute.cpp:214-215 BufferCache::get with no lock | 3D takes the lock: 3d.cpp:448-450 | Bug B4. |

### B. Inline-to-memory (I2M) in 3D / compute / A140

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| B1 | LINE_LENGTH_IN / LINE_COUNT / OFFSET_OUT / LOAD_INLINE_DATA (linear, contiguous) | IMPLEMENTED_UNVERIFIED | inline_base.hpp:21-38; inline_base.cpp:18-46 | ogd/clb1c0.h:83-133, 166-167; eden/engines/engine_upload.cpp:35-58 | Register offsets 0x60-0x6D match. |
| B2 | Multi-line with PITCH_OUT | MISSING | inline_base.cpp:40 (single `memcpy`) | engine_upload.cpp:52-58 | Bug B8. |
| B3 | Block-linear destination (DST_BLOCK_SIZE/WIDTH/HEIGHT/DEPTH/LAYER/ORIGIN) | MISSING | fields are defined but unused (inline_base.hpp:29-35) | engine_upload.cpp:59-82 | Bug B8. |
| B4 | LAUNCH_DMA (resets the transfer, DST_MEMORY_LAYOUT, COMPLETION_TYPE) | MISSING | inline_base.cpp:11-16 (stub) | ogd/clb1c0.h:134-165; engine_upload.cpp:28-33 | Bug B8. |
| B5 | Sizes that are not a multiple of 4 | MISSING | inline_base.cpp:22-24 (exact size equality) | engine_upload.cpp:35-43 (byte-accurate) | Never flushes, and corrupts later uploads (B8). |
| B6 | Destination spanning GPU mappings | MISSING | inline_base.cpp:29-40 (one `unmapAddr` + `memcpy`) | – | Copy does split by mapping (copy.cpp:276-303). |
| B7 | I2M semaphore (SET_I2M_SEMAPHORE_A-C + COMPLETION_TYPE=RELEASE_SEMAPHORE) | MISSING | – | ogd/clb1c0.h:138-141, 170-176 | Eden is missing it too. |
| B8 | Flush GPU-written textures before overwriting, invalidate caches after | IMPLEMENTED_UNVERIFIED | inline_base.cpp:31-45 | eden: `AccelerateInlineToMemory` | |

### C. PFIFO / GPFIFO / host methods (B06F)

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| C1 | GPFIFO entry decode (GET/GET_HI, LENGTH, SYNC) and per-submission processing | VERIFIED (in-game implicit) | gpu/const.hpp:740-747; pfifo.cpp:200-224 | ogd/clb06f.h:154-176; eden/dma_pusher.h:77-86 | Bits 40/41 are PRIV/LEVEL, not "allow_flush"/"is_push_buffer"; both are ignored, which is harmless. |
| C2 | Pushbuffer fetch through the GPU VA (host window + GMMU fallback) | VERIFIED (in-game implicit) | pfifo.cpp:68-91, 215-223 | eden/dma_pusher.cpp:86-93 | |
| C3 | INC / NON_INC / IMMD / ONE_INC / END_PB_SEGMENT | VERIFIED (in-game implicit) | pfifo.cpp:255-271 | ogd/clb06f.h:192-243; eden/dma_pusher.cpp:137-163 | 12-bit address, 13-bit count, 13-bit immediate: correct. |
| C4 | Old-format GRP0/GRP2 (tertiary op 0) and subdevice-mask ops | IMPLEMENTED_UNVERIFIED | pfifo.cpp:237-254 | ogd/clb06f.h:179-189, 244-253 | Mask ops are ignored, which is correct for a single GPU. |
| C5 | Method data that continues into the next GPFIFO entry | PARTIAL | pfifo.cpp:219-222, 286-292 (args are read sequentially past `end`) | eden/dma_pusher.cpp:114-167 (dma_state persists across entries) | B28. |
| C6 | Entry flags SYNC/LEVEL/PRIV/FETCH; control entries with length 0 | PARTIAL | pfifo.cpp:203-212 (only logged) | eden/dma_pusher.cpp:96-109 | Acceptable for in-order execution. |
| C7 | SET_OBJECT for B197/B1C0/A140/902D/B0B5 | VERIFIED (in-game implicit) | gpu.cpp:44-84 | eden/engines/puller.cpp:32-64 | |
| C8 | SET_OBJECT for NV01_TIMER (0x0004) / B06F | MISSING | gpu.cpp:67-78 | eden/engines/puller.h:26; puller.cpp:57-58 | Logged; the previous binding is kept. |
| C9 | Routing of host methods 0x00-0x3F to the puller | MISSING | gpu.cpp:43-94 handles only `method == 0x0`, then `(*engine)->method(method, arg)` | eden/dma_pusher.cpp:175-193; eden/dma_pusher.h:52-75 | Bug B2. |
| C10 | SEMAPHOREA-D release (4 B / 16 B with timestamp) | MISSING | (see C9) | ogd/clb06f.h:74-107; eden/engines/puller.cpp:82-124 | |
| C11 | SEMAPHORED acquire (EQ / GEQ / AND, acquire switch) | MISSING | – | ogd/clb06f.h:81-89; puller.cpp:89-138 | |
| C12 | SEMAPHORED reduction | MISSING | – | ogd/clb06f.h:86, 96-107 | |
| C13 | NON_STALL_INTERRUPT | MISSING | – | ogd/clb06f.h:108-109; puller.cpp:170-172 | Harmless. |
| C14 | FB_FLUSH / MEM_OP_C/D (membar, L2 flush/invalidate, TLB invalidate) | MISSING | – | ogd/clb06f.h:110-137; puller.cpp:174-187 | Harmless: no GPU-side caches are emulated. |
| C15 | SET_REFERENCE | MISSING | – | ogd/clb06f.h:138-139; puller.cpp:157-159 | |
| C16 | SYNCPOINTA/B (payload, then increment/acquire) | MISSING | – | eden/dma_pusher.h:69-70; puller.h:62-71; puller.cpp:66-80 | Part of B1/B2. |
| C17 | WFI / CRC_CHECK / YIELD | MISSING | – | ogd/clb06f.h:140-151 | Harmless. |
| C18 | SubmitGpfifo FenceWait/FenceGet flags and returned fence | MISSING | nvdrv/nvhost_gpu.cpp:41-58 (`// HACK ... inout_fence = hw::tegra_x1::gpu::Fence{};`) | eden/fence_manager.h:104-108; eden/host1x/syncpoint_manager.cpp:60-75 | Bug B1. |
| C19 | Per-channel GPU context (engine state per nvhost-gpu channel) | MISSING | gpu/gpu.hpp:187-196 (one set of engines and subchannels); nvhost_gpu.cpp:49-52 passes only the GMmu | eden/control/channel_state.h:35-62; eden/gpu_thread.cpp:44, 65 | Bug B15. |
| C20 | Asynchronous GPU thread; submitted/completed counts used by present | PARTIAL | pfifo.cpp:110-139, 141-198 | eden/gpu.cpp:123-160 | "Completed" means *encoded*. Present waits up to 250 ms (pfifo.cpp:136) (B31). |

### D. Engine-level semaphores, reports, barriers

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| D1 | 3D SET_REPORT_SEMAPHORE release, one word | IMPLEMENTED_UNVERIFIED | 3d.cpp:544-606 (method 0x6C3: 3d.cpp:231); 3d.hpp:481-484 | ogd/clb197.h:3609-3700; eden/engines/maxwell_3d.cpp:481-497 | Written immediately, at encode time. |
| D2 | Release, four words (u64 value + u64 timestamp) | PARTIAL | 3d.cpp:596-605 | eden/maxwell_3d.cpp:471-479 (StampQueryResult); eden/gpu.cpp:167-170 | Layout matches Eden. Timestamp is `ns / 625 * 384`, which drops the remainder (625 ns granularity), and uses `steady_clock` instead of `WallClock::getGpuTick` (src/core/hw/wall_clock.cpp:30-32). |
| D3 | Report ACQUIRE (op 1) | MISSING | 3d.cpp:568-570 (`return;`) | maxwell_3d.cpp:498-502 (Eden: unimplemented) | |
| D4 | Report-only Payload counter (report 0) | IMPLEMENTED_UNVERIFIED | 3d.cpp:571-575 | query_cache.h:297-306 | |
| D5 | ZPASS_PIXEL_CNT / CNT64 (0x02 / 0x15) | PARTIAL | 3d.cpp:576-582; 3d.cpp:2276-2277 (`zpass_pixel_count++` per draw) | eden/query_cache (streamers); ogd/clb197.h:3693-3694 | The count is the number of draws, so occlusion always reads "visible" (B29). |
| D6 | Other counters (PRIMITIVES_GENERATED, STREAMING_*, VTG_PRIMITIVES_OUT, etc.) | MISSING | 3d.cpp:583-585 (`value = 0`) | ogd/clb197.h:3651-3694; query_cache.h:238-242 (Eden falls back to **payload = 1**) | Returns 0 where Eden returns 1 (B9). |
| D7 | TRAP / AWAKEN_ENABLE / FLUSH_DISABLE / PIPELINE_LOCATION / SUB_REPORT | MISSING | 3d.cpp:588-590 (TRAP is logged) | ogd/clb197.h:3619-3648, 3696-3698 | Mostly harmless; TRAP is rare. |
| D8 | Report reduction (bit 3, OP[11:9], FORMAT[18:17]) | MISSING | not decoded | ogd/clb197.h:3699-3711 | B9. |
| D9 | Report/semaphore write *timing* (after preceding GPU work) | PARTIAL | written in `setReportSemaphore`, at encode time | query_cache.h:263-310 (fence-type releases are deferred through SignalFence) | B9. |
| D10 | Report writes invalidate GPU caches of that memory | MISSING | 3d.cpp:593-605 (no `invalidateMemory`) | – | Contrast `loadConstBuffer`, which does invalidate (3d.cpp:626-628). |
| D11 | CLEAR_REPORT_VALUE (0x54C) | PARTIAL | 3d.cpp:537-542 (`if (data == 0x1)`) | ogd/clb197.h:2722-2743; maxwell_3d.cpp:569-586 | Only ZPASS is handled, and it compares the whole word instead of TYPE[4:0]. |
| D12 | 3D INCREMENT_SYNC_POINT (0xB2) | MISSING | not in the 3D method table (3d.cpp:224-233) | ogd/clb197.h:284-290; maxwell_3d.cpp:588-592 | B1. |
| D13 | 3D WAIT_FOR_IDLE (0x44) | IMPLEMENTED_UNVERIFIED | writeReg | maxwell_3d.cpp:309-310; vk_rasterizer.cpp:853-875 | A no-op is acceptable without SSBO / fragment storage writes. |
| D14 | FRAGMENT_BARRIER (0x378), INVALIDATE_TEXTURE_DATA_CACHE (0x3DD), TILED_CACHE_BARRIER (0x3DF) | MISSING | not in the method table; no `memoryBarrier`/`textureBarrier` anywhere in mtl/ | eden/maxwell_3d.cpp:363-369; maxwell_3d.h:3374, 3389-3390; vk_rasterizer.cpp:876-883 | Render-target feedback inside a Metal render encoder is undefined (B22). |
| D15 | Copy engine semaphore release (one/four words, reduction) | MISSING | copy.hpp:75-120 (no SET_SEMAPHORE regs); copy.cpp:142-347 (never released) | ogd/clb0b5.h:39-44, 74-77; eden/engines/maxwell_dma.cpp:347-368 | Bug B10. |
| D16 | SET_RENDER_ENABLE_A-C / OVERRIDE (conditional rendering) | MISSING | – | ogd/clb197.h:2759-2765, 3427; maxwell_3d.cpp:515-567 | |
| D17 | Transform feedback (stream out) with its counters and write-back | MISSING | grep finds no stream-out handling in gpu/ | ogd/clb197.h:568; eden buffer_cache.h:1457 (UpdateTransformFeedbackBuffers) | |

### E. Fences, ordering, readback

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| E2 | nvhost_ctrl syncpoint wait / event wait for the GPU syncpoint | PARTIAL | nvdrv/nvhost_ctrl.cpp:84, 96 (`if (id == 0) { ... return NvResult::Success; } // Existing GPU path.`); video IDs are real (src/core/video/syncpoints.hpp) | eden/host1x/syncpoint_manager.cpp:68-75 | README-video.md:52-53: "Existing GPU syncpoint ID zero retains its old stub behavior." |
| E3 | SYNCPT_READ of id 0 | PARTIAL | nvhost_ctrl.cpp:121-126 → syncpoints.hpp:21-25 (`if (!id ...) return std::nullopt`) | – | Returns BadValue (B25). |
| E4 | Metal: one command buffer per GPFIFO list, single queue, commit at end | IMPLEMENTED_UNVERIFIED | pfifo.cpp:166-187; mtl/command_buffer.cpp:18-65 | eden/renderer_vulkan/vk_scheduler | In-order. Completion handlers only log and recycle buffers. |
| E5 | GPU-thread synchronous readback (commit + wait) before CPU-side engine reads | VERIFIED | hot/texture_cache_hot.cpp:383-442 (`renderer.debugSyncCommandBuffer(command_buffer)`) | eden/gpu.cpp:210-227 | tests/texture_cache_tests.cpp "rendered texture flushes to guest memory" (PASS line 1007); README-crash4-render.md:19-20. |
| E6 | CPU-thread readback ordering (stage-2 faults) | PARTIAL | renderer.hpp:101-105 (new command buffer); hot/texture_cache_hot.cpp:332-341 (write marked at encode time) | eden/gpu.cpp:214-227 (OnCPURead runs on the GPU thread) | Bug B11. |
| E7 | Explicit Metal barriers/fences (memoryBarrier, MTLFence, MTLEvent) | MISSING | grep over mtl/ finds none | vk_rasterizer.cpp:623-630, 876-883 | Not needed while automatic tracking covers everything, except D14. |
| E8 | Hazard tracking between a texture-from-buffer and its parent buffer (buffer textures plus blit-based alias sync) | UNKNOWN | mtl/texture.cpp:69-73 (`buffer_backing->newTexture(desc, 0, bytes)`) | – | Verify with a Metal test: SUST to the buffer-texture, then a blit from the backing buffer in the next encoder. |

### F. GPU memory (GMMU, buffer/index cache, coherency)

| # | Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|---|
| F1 | GMMU map (fixed/auto), with host backing split by physical run | VERIFIED | gpu/gmmu.cpp:56-111; nvdrv/nvhost_as_gpu.cpp:89-124 | eden/memory_manager.cpp:178-182 | tests/xci_tests.cpp `checkGpuMappings` (PASS lines 241-242). |
| F2 | GMMU unmap | MISSING | nvhost_as_gpu.cpp:76-86 (`// TODO: remove the GPU mapping`) | eden/memory_manager.cpp:190-203 | B23. |
| F3 | MapBufferEx MODIFY | MISSING | nvhost_as_gpu.cpp:98-103 (logs, then returns Success) | – | B23. |
| F4 | Remap: sparse bind/unbind (unbind reads zero) | IMPLEMENTED_UNVERIFIED | nvhost_as_gpu.cpp:166-196; gmmu.cpp:181-185 (calloc) | eden/memory_manager.cpp:184-187 | Kind and page size are ignored; the big page size is assumed. |
| F5 | AllocSpace/FreeSpace; reads of reserved, unbacked VA | PARTIAL | nvhost_as_gpu.cpp:24-42; gmmu.cpp:113-127 (`malloc`, uninitialized) | memory_manager.cpp:320-338 (Reserved vs unmapped) | Reads garbage instead of 0 or a fault. FreeSpace is a stub. |
| F6 | Big/small page sizes, VA regions | PARTIAL | gpu/const.hpp:5 (`GPU_PAGE_SIZE = 0x20000; // ... (TODO: correct?)`); nvhost_as_gpu.cpp:126-140 (HACK) | memory_manager.cpp:29-51 | |
| F7 | PTE kinds | MISSING | nvhost_as_gpu.cpp:96 (`(void)kind;`) | memory_manager.cpp:91 (GetPageKind) | Mostly harmless on Tegra. |
| F8 | GMMU thread safety | PARTIAL | generic_mmu.hpp:16-35 (std::map, no lock); gmmu.cpp:28-54; nvhost_as_gpu.cpp:119 (ioctl thread) | – | Bug B5. |
| F9 | Unmapped GPU VA accesses | PARTIAL | generic_mmu.hpp:77-84 (`DEBUGGER_ASSERT_DEBUG(impl...); return *impl;`) | memory_manager.cpp:338-349 (unmapped callbacks) | Null dereference in release builds (B24). |
| F10 | Buffer cache: immutable snapshots ≤64 KiB, replaced on change, so each draw keeps its own version | VERIFIED | renderer/buffer_cache.cpp:51-76, 101-138; mtl/renderer.cpp:149-162 | eden/buffer_cache/buffer_cache.h:915-960 | tests/buffer_snapshot_tests.cpp (PASS line 138); texture_cache_tests.cpp:345-403; README-cpu-gpu-memory.md:18-37. |
| F11 | Buffer cache >64 KiB: private buffer + staged blit, ordered | VERIFIED | buffer_cache.cpp:127-137; mtl/buffer.cpp:51-60 | – | buffer_snapshot_tests with `HYDRA_DISABLE_BUFFER_SNAPSHOTS=1` (README-cpu-gpu-memory.md:34-37). Each update splits the render encoder (perf). |
| F12 | Buffer cache merge ownership/lifetime | VERIFIED | buffer_cache.cpp:140-181 | – | buffer_snapshot_tests.cpp:121-133. |
| F13 | LOAD_CONSTANT_BUFFER (CB_DATA, 0x8E4-0x8F3) seen by later draws only | IMPLEMENTED_UNVERIFIED | 3d.cpp:615-629 (writes guest memory, then `invalidateMemory(..., BufferCache)`) | eden/maxwell_3d.cpp:613-631 | The versioning is verified (F10), but the CB_DATA path itself has no test. No bound check against `const_buffer_selector_size`. |
| F14 | BIND_GROUP CB binding (0x904 + 8·stage) | VERIFIED (in-game implicit) | 3d.cpp:631-660 | maxwell_3d.cpp:594-611 | The host range is resolved at bind time, so it goes stale if the GMMU remaps before the draw. |
| F15 | Vertex buffers (limit-inclusive size) | VERIFIED (in-game implicit) | 3d.cpp:1283-1299 (`ThreeD::getVertexBuffer`) | buffer_cache.h:1365+ | |
| F16 | Index buffers (INDEX_BUFFER_A-F) | PARTIAL | 3d.cpp:462-471, 489-491; mtl/renderer.cpp:1029-1031 | eden/buffer_cache/buffer_cache.h:1337-1341; ogd/clb197.h:3183-3205 | The range ignores FIRST (B6). |
| F17 | Decoded index buffers (u8 → u16, quads, fans) | MISSING (invalidation) | index_cache.hpp:17 (`// TODO: memory invalidation`); index_cache.cpp:157-159 | eden/renderer_vulkan/vk_buffer_cache.cpp:579-595 (converted per draw) | Stale indices after CPU writes (B7). |
| F18 | GPU-written *buffer* tracking/write-back (SSBO, TFB, buffer-backed queries) | MISSING | buffer cache assumes CPU memory is authoritative (buffer_cache.cpp:51-138) | buffer_cache.h:1523-1533 (MarkWrittenBuffer); 184-200 (DownloadMemory) | B3. |
| F19 | CPU write tracking, stage-1 (default): fault, queue, apply at GPFIFO entry, re-arm at draw/launch | PARTIAL | hv/thread.cpp:288-292; cpu/mmu.cpp:68-112; pfifo.cpp:202; compute.cpp:79; 3d.cpp (drawInternal flushTrackedPages) | – | No TLB invalidation on re-arm (B14). Alias VAs are untracked. |
| F20 | HLE/service host writes into GPU-mapped guest memory | MISSING | fssrv/file.cpp:31-32 (`writeReturningSpan`); kernel/hipc/const.cpp:19 (host pointer); grep: no `invalidateMemory` outside gpu/, cpu/mmu, nvhost_as_gpu | eden/rasterizer_interface.h:100-114 (InvalidateRegion / OnCacheInvalidation / OnCPUWrite) | Bug B13. |
| F21 | Stage-2 tracking (opt-in `HYDRA_STAGE2_TRACKING`): faults, flush, invalidate, re-arm | VERIFIED | hv/stage2_tracker.cpp:31-105; hv/mmu.cpp:85-94, 189-260 | – | tests/memory_tracking_tests.cpp (PASS line 126). Integration ordering is buggy (B11). |
| F22 | GPU texture writes reach guest memory before copy/2D/I2M engines read or overwrite | VERIFIED | copy.cpp:86-100, 266-271; inline_base.cpp:31-39; 2d.cpp:100-106 | eden/maxwell_dma.cpp (accelerate paths) | texture_cache_tests.cpp:1007, 1054; README-crash4-render.md:19-20. Depth and 3D surfaces are never flushed (texture_cache_hot.cpp:389-393). |
| F23 | GPU writes visible to CPU loads (default mode) | MISSING | hv/mmu.cpp:197 (`protectsGpuWrites() { return !useStage1Tracking(); }`); 256-260 (no-op under stage-1) | eden/gpu.cpp:214-227 | B12. |
| F24 | GPU writes visible to the buffer cache (vertex/index/uniform reading memory the GPU wrote) | MISSING | buffer_cache.cpp:51-76 never consults the texture cache | eden: unified buffer cache, texture buffers live in the buffer cache (vk_compute_pipeline.cpp:223-247) | B32. |
| F25 | Snapshot recycling only after GPU completion | VERIFIED | buffer_cache.cpp:17-46; mtl/upload_buffer_pool.hpp | – | texture_cache_tests.cpp:380-403 (PASS line 402). |

### QMD bit-position check (Hydra `ComputeQmd` vs Eden `LaunchParams`)

| Field | Eden (eden/engines/kepler_compute.h) | Hydra (gpu/engines/compute.hpp) | Result |
|---|---|---|---|
| PROGRAM_START | w0x08 [31:0] (l.133, assert l.257) | `words[0x8]` (l.50) | match |
| LINKED_TSC | w0x0B bit 30 (l.137) | not decoded | **missing** |
| GRID_DIM_X | w0x0C [30:0] (l.139, assert l.258) | `words[0xc] & 0x7fffffff` (l.51) | match |
| GRID_DIM_Y / Z | w0x0D [15:0] / [31:16] (l.141-142) | `words[0xd] & 0xffff` / `>> 16` (l.52-53) | match |
| SHARED_ALLOC | w0x11 [17:0] (l.147, assert l.259) | `words[0x11] & 0x3ffff` (l.54) | match (clamped later to 32 KiB) |
| BLOCK_DIM_X | w0x12 [31:16] (l.149, assert l.260) | `words[0x12] >> 16` (l.55) | match |
| BLOCK_DIM_Y / Z | w0x13 [15:0] / [31:16] (l.151-152) | (l.56-57) | match |
| CONST_BUFFER_ENABLE_MASK | w0x14 [7:0] (l.156, assert l.261) | `words[0x14] & 0xff` (l.58) | match |
| CACHE_LAYOUT | w0x14 [30:29] (l.157) | not decoded | missing (harmless) |
| CB[i] ADDR_LO / ADDR_HI / SIZE | w0x1D+2i / w0x1E+2i [7:0] / [31:15] (l.162-172, assert l.262) | (l.59-65) | match |
| LOCAL_POS_ALLOC / BARRIER_ALLOC | w0x2D [19:0] / [31:27] (l.175-176) | `words[0x2d] & 0xfffff` (l.66), **unused**; barrier not decoded | partial |
| LOCAL_NEG_ALLOC / GPR_ALLOC | w0x2E [19:0] / [28:24] (l.180-181) | not decoded | missing |
| LOCAL_CRS_ALLOC / SASS_VERSION | w0x2F [19:0] / [28:24] (l.185-186) | not decoded | missing (Eden adds CRS to the local size) |

Register offsets in `RegsCompute` (compute.hpp:9-44) all match Eden's asserts (kepler_compute.h:249-256):
upload 0x60, exec_upload 0x6C, data_upload 0x6D, launch_desc 0xAD, launch 0xAF, tsc 0x557, tic 0x55D,
code 0x582, tex_cb_index 0x982.

### Report-semaphore structure and timestamp check

- **Registers.** `report_semaphore_addr` is 0x6C0/0x6C1 (hi/lo), payload is 0x6C2, and control is 0x6C3
  (3d.hpp:481-484, 3d.cpp:231). They match ogd/clb197.h:3609-3618 and eden/maxwell_3d.h:3610.
- **Control-word decode.**

  | Field | Bits | Hydra |
  |---|---|---|
  | OPERATION | [1:0] | decoded |
  | REPORT | [27:23] | decoded |
  | STRUCTURE_SIZE | [28] | decoded |
  | FLUSH_DISABLE | [2] | ignored |
  | REDUCTION_ENABLE | [3] | ignored |
  | RELEASE | [4] | ignored |
  | SUB_REPORT | [7:5] | ignored |
  | ACQUIRE | [8] | ignored |
  | REDUCTION_OP | [11:9] | ignored |
  | PIPELINE_LOCATION | [15:12] | ignored |
  | COMPARISON | [16] | ignored |
  | REDUCTION_FORMAT | [18:17] | ignored |
  | AWAKEN | [20] | ignored |
  | REPORT_DWORD_NUMBER | [21] | ignored |

  Sources: 3d.cpp:546-548; ogd/clb197.h:3619-3711.
- **Layout.** One word is a u32 at the address. Four words are u64 value at +0 and u64 timestamp at +8.
  This matches Eden's `LongQueryResult`/StampQueryResult (maxwell_3d.cpp:471-479).
- **Units.** GPU ticks at 614.4 MHz (ns·384/625), as in Eden `GetTicks` (gpu.cpp:167-170).
  - Hydra computes `ns / 625 * 384` (3d.cpp:602), which loses the remainder.
  - Its clock source (`std::chrono::steady_clock`) differs from the `GetGpuTime` ioctl, which uses
    `WallClock::getGpuTick` (nvhost_ctrl_gpu.cpp:162-164, wall_clock.cpp:30-32). On macOS arm64 both derive from the
    boot-relative system counter, so they agree in practice. It is still a duplicated clock path.
  - Eden also has a GPU-clock divisor option to report faster GPU times (gpu.cpp:44-53).
- **Timing.** Hydra writes at *encode* time on the PFIFO thread, before any of the preceding Metal work has
  executed. Eden defers fence-type releases until GPU completion (query_cache.h:294-296) and writes
  non-fence payload long queries immediately (query_cache.h:297-306). As a result, GPU-timing pairs in Hydra measure
  CPU encode time, including synchronous readback waits.

### GPU-write → reader propagation matrix

| GPU writer | Stored in | Written back to guest memory when | Never propagated to |
|---|---|---|---|
| Color render targets, compute storage images | texture cache (Metal texture) | CPU-side engine reads/writes (copy/2D/I2M call `flushMemory`); stage-2 CPU fault (opt-in, B11) | CPU in default mode (F23); buffer cache (F24); HLE readers |
| Depth/stencil and 3D surfaces | texture cache | never (texture_cache_hot.cpp:389-393) | everyone outside the texture cache |
| Image buffers (1D buffer images) | texture cache (texture over MTLBuffer) | as color targets | buffer cache (vertex/index/uniform) |
| SSBO / STG / ATOM / RED | not implemented | – | – |
| Transform feedback | not implemented | – | – |
| Report/semaphore writes | guest memory, directly, at encode | immediately (too early) | buffer cache not invalidated (D10) |
| Copy / I2M / CB_DATA (CPU-executed) | guest memory | immediately, with cache invalidation | ✓ |

---

## (b) Confirmed bugs and hazards (exact code quotes)

The severity labels describe the impact on real games.

### B1. GPU fences and syncpoints are stubbed (High)

`src/core/horizon/services/nvdrv/ioctl/nvhost_gpu.cpp:41-58`:
```cpp
// TODO: gpfifo
NvResult NvHostGpu::submitGpfifo(
    ...
    system->getGpu().getPfifo().submitEntries(
        process->getGMmu(),
        std::span<const hw::tegra_x1::gpu::GpfifoEntry>(entries, num_entries),
        inout_flags_and_detailed_error);

    // HACK
    inout_flags_and_detailed_error = 0;
    inout_fence = hw::tegra_x1::gpu::Fence{};
    return NvResult::Success;
```
`nvhost_ctrl.cpp:84` and `:96`:
```cpp
if (id == 0) { *out_value = 0; return NvResult::Success; } // Existing GPU path.
```
`src/core/horizon/display/binder.cpp:48-50` documents the same thing:
```cpp
// Fences are not implemented, so remember how much GPU work has been
// submitted to wait for it before presenting
```

`Pfifo::submitEntries` (pfifo.cpp:110-125) only enqueues; the GPU thread processes the work later. Consequences:

1. The guest never waits for the GPU. `nvnSyncWait`, `QueueFinish` and buffer fences all succeed at once.
2. CPU readbacks of GPU results race the GPU thread. The thread may not yet have *encoded* the work, let alone executed it.
3. Ring buffers that the guest protects with fences can be overwritten before the PFIFO thread snapshots them. Hydra
   captures data at encode time, so this is only harmless while the GPU thread keeps up.
4. GPU-side fence waits across channels or engines (FenceWait) are ignored.

The same applies to the host `SYNCPOINTB` methods (C16) and 3D `INCREMENT_SYNC_POINT` (D12).
Eden increments the guest value immediately and the host value after the GPU fence (fence_manager.h:104-108,
syncpoint_manager.cpp:60-75).

### B2. Host methods 0x00–0x3F are forwarded to the bound engine (High)

`gpu/gpu.cpp:43-93`:
```cpp
void Gpu::subchannelMethod(u32 subchannel, u32 method, u32 arg) {
    if (method == 0x0) { // SetEngine
        ...
        return;
    }
    const auto engine = getEngineAtSubchannel(subchannel);
    ...
    (*engine)->method(method, arg);
}
```
SEMAPHOREA-D (0x4-0x7), SET_REFERENCE (0x14), SYNCPOINTA/B (0x1C/0x1D), WFI (0x1E), MEM_OP and
NON_STALL_INTERRUPT therefore all become `writeReg` into the engine's padding.

- Semaphore releases are never written.
- Acquires never wait.
- Syncpoints never increment.

A guest that polls a host-semaphore release value spins forever. Eden routes methods below 0x40 to the puller
(eden/dma_pusher.cpp:175-193; puller.cpp:141-205).

### B3. Global memory and SSBOs are not implemented (High for compute-heavy titles, e.g. UE4)

`gpu/renderer/shader_decompiler/decoder/memory.cpp:165-172`:
```cpp
    std::vector<ir::Value> values;
    values.reserve(count);
    for (u32 i = 0; i < count; i++) {
        // TODO: global memory
        values.push_back(ir::Value::createConstantU(0));
    }
```
`decoder.cpp:252-257`:
```cpp
    INST(0xeed8000000000000, 0xfff8000000000000) {
        COMMENT_NOT_IMPLEMENTED("stg");
    }
    INST(0xeed0000000000000, 0xfff8000000000000) { EMIT(Ldg); }
```
`ATOM` (l.250, 266), `RED` (l.272), `ST`/`LD` (l.519, 522), `SULD` (l.285) and `SURED` (l.281) are also
not implemented. `ResourceMapping` has `// TODO: storage buffers` (renderer/const.hpp:469), as does
`ThreeD::configureShaderStage` (3d.cpp:1586). `ComputeDispatch` (renderer.hpp:42-61) carries only uniform buffers,
textures and images.

Effect: every NVN SSBO access reads zero and every write is dropped. This covers compute particles,
histograms, culling and light lists, and skinning caches. Indirect args generated on the GPU are impossible.
Reference design:
- sr/ir_opt/global_memory_to_storage_buffer_pass.cpp:373-412 (tracking with the NVN bias: cbuf0 0x100–0x700, align 16);
- eden/buffer_cache/buffer_cache.h:497-536, 2006-2050 (binding, size from the next qword, capped at 8 MiB);
- eden/buffer_cache/buffer_cache.h:1523-1533 (GPU-modified tracking).

### B4. `Compute::launch` uses `BufferCache` without its mutex (Medium; crash risk)

`gpu/engines/compute.cpp:76` takes only the texture lock:
```cpp
    std::scoped_lock texture_cache_lock(renderer.getTextureCache().getMutex());
```
Then it mutates the buffer cache at compute.cpp:214-215:
```cpp
        dispatch.uniform_buffers[host_index] =
            renderer.getBufferCache().get(tls_crnt_command_buffer, range);
```
The 3D path does lock (3d.cpp:448-450):
```cpp
        std::scoped_lock buffer_cache_lock(
            gpu.getRenderer().getBufferCache().getMutex());
```
Other threads concurrently call `IRenderer::invalidateMemory`, which takes the buffer mutex (renderer.hpp:84-87):
nvdrv map/unmap (`nvhost_as_gpu.cpp:65`), stage-2 fault callbacks (`hv/mmu.cpp:90-93`) and perf stats
(`system.cpp:612-614`). `BufferCache::findHot3` inserts into and erases from `std::map entries`, so this is
undefined behavior.

### B5. GMMU `std::map` read and written concurrently without a lock (Medium; crash risk)

`GenericMmu::map` mutates `mapped_ranges` (generic_mmu.hpp:16-35). `GMmu::carveOut` and `mapIntoGaps`
erase from and insert into it (gmmu.cpp:131-179). These run on nvdrv ioctl threads:
- `nvhost_as_gpu.cpp:119-120` (`mapBufferToAddressSpace`);
- `nvhost_as_gpu.cpp:179-192` (`remap` → `unbindToSparse` / `mapBufferToAddressSpace`).

At the same time the PFIFO thread resolves addresses through `findAddrImplRef` (generic_mmu.hpp:52-75)
in `GMmu::unmapAddr` (gmmu.cpp:28-54) and in `push_buffer_window` (pfifo.cpp:215-218). Nothing in `GenericMmu` or
`GMmu` holds a mutex. Map/remap during gameplay, such as streaming or sparse textures, can crash the GPU thread.

### B6. The index buffer range ignores `INDEX_BUFFER_F.FIRST` (Medium)

`gpu/engines/3d.cpp:462-471`:
```cpp
        gpu_vaddr_t index_buffer_ptr =
            tls_crnt_gmmu->unmapAddr(regs.index_buffer_addr);
        // TODO: uncomment?
        u32 index_buffer_size =
            count *
            getIndexTypeSize(
                regs.index_type); // u64(regs.index_buffer_limit_addr) + 1
                                  // - u64(regs.index_buffer_addr);
        const auto range =
            ztd::Range<uptr>::fromSize(index_buffer_ptr, index_buffer_size);
```
The draw then offsets by FIRST (3d.cpp:489-491 → `drawIndexed(..., regs.vertex_elements_start, count, ...)`;
mtl/renderer.cpp:1029-1031):
```cpp
    const auto index_buffer_offset =
        static_cast<u64>(start) * engines::getIndexTypeSize(state.index_type) +
        state.index_buffer.getOffset();
```
With FIRST > 0, Metal reads `[first, first+count)` indices from a snapshot that holds only `count` indices. The
remaining bytes are stale pool contents, since snapshots are ≥4 KiB pool buffers (upload_buffer_pool.hpp:27-48), or
past the end of the range. The decoded paths (u8/quads/fans) also decode from index 0.

Eden binds `min(limit - start, (count + first) * size)` (eden/buffer_cache/buffer_cache.h:1337-1341).
FIRST is method 0x5F7 (ogd/clb197.h:3201-3202).

### B7. Decoded index buffers are never invalidated (Medium)

`gpu/renderer/index_cache.hpp:17`: `// TODO: memory invalidation`. In `index_cache.cpp:157-159`:
```cpp
    auto& index_buffer = cache[hash_value];
    if (index_buffer != nullptr)
        return BufferView{index_buffer};
```
The key is a 32-bit hash of (type, primitive, range begin/end, count), with no data. It has three problems:
- Rewriting u8 indices, or quad/fan index data, at the same address reuses stale indices forever.
- A hash collision returns the wrong buffer.
- The map grows without bound.

Eden converts on every draw from the tracked buffer (eden/renderer_vulkan/vk_buffer_cache.cpp:579-595).

### B8. Inline-to-memory is incomplete (Medium)

`gpu/engines/inline_base.cpp:11-16`: `launchDmaImpl` is a stub, so the transfer is never reset.
`inline_base.cpp:18-24`:
```cpp
    inline_data.push_back(data);
    // TODO: correct?
    if (inline_data.size() * sizeof(u32) ==
        static_cast<usize>(regs.line_length_in) *
            static_cast<usize>(regs.line_count)) {
```
and `inline_base.cpp:29-40`, one linear `memcpy`:
```cpp
        uptr dst_ptr = tls_crnt_gmmu->unmapAddr(regs.offset_out);
        const usize size = inline_data.size() * sizeof(u32);
        ...
        memcpy(reinterpret_cast<void*>(dst_ptr), inline_data.data(), size);
```
- A `line_length_in*line_count` that is not a multiple of 4 never flushes, and its leftover words corrupt the next upload.
- `PITCH_OUT` is ignored for multi-line transfers.
- Block-linear destinations (LAUNCH_DMA bit 0 = 0) are written linearly.
- Writes can run past a host mapping.

Eden: engine_upload.cpp:28-83.

### B9. Report semaphores are timed and decoded incompletely (Medium)

The relevant code in `3d.cpp:563-605`:
```cpp
    case 1: // Acquire
        // The GPU executes in order, nothing to wait for
        return;
    ...
        default:
            value = 0;
            break;
    ...
        const u64 timestamp = ns / 625 * 384;
        reinterpret_cast<u64*>(ptr)[0] = value;
        reinterpret_cast<u64*>(ptr)[1] = timestamp;
```
- All writes happen at encode time.
- Reductions (bit 3) are ignored.
- Unknown counters return 0, where Eden falls back to payload 1 (query_cache.h:238-242).
- Timestamps have 625 ns granularity.
- No buffer-cache invalidation follows the write (contrast 3d.cpp:626-628).

### B10. Copy engine semaphore release is missing (Medium-low)

`LaunchDMAData` decodes `semaphore_type`, `semaphore_reduction*` (copy.hpp:136-147). `RegsCopy` has no
SET_SEMAPHORE_A/B/PAYLOAD (0x90-0x92, ogd/clb0b5.h:39-44): they fall into `padding_0x0[0xc0]` (copy.hpp:76).
`Copy::launchDma` never writes a semaphore (copy.cpp:142-347). Eden: maxwell_dma.cpp:347-368.

### B11. Stage-2 CPU-fault readback can run ahead of the GPU work that produced the data (Medium when stage-2 is used)

`gpu/renderer/renderer.hpp:101-105`:
```cpp
    void flushMemoryForCpu(ztd::Range<uptr> range) {
        std::scoped_lock lock(texture_cache.getMutex());
        std::unique_ptr<ICommandBuffer> command_buffer(createCommandBuffer());
        texture_cache.flushMemory(command_buffer.get(), range);
    }
```
Two facts combine here:
- The texture is marked GPU-written when the draw is *encoded* (hot/texture_cache_hot.cpp:332-341), on the PFIFO
  thread's command buffer. That buffer is only committed when the whole GPFIFO list ends (pfifo.cpp:185).
- The readback command buffer above is committed (and waited on) first (texture_cache_hot.cpp:442).

Metal executes in commit order, so the blit reads the pre-draw contents. If the readback covers the whole texture,
`flushed_timestamp` is then set (texture_cache_hot.cpp:394-397), and the stale data stays in guest memory.
Eden instead runs CPU-requested flushes on the GPU thread (gpu.cpp:214-227).

### B12. Default mode never writes GPU results back on CPU access (Medium)

`hv/mmu.cpp:189-197`:
```cpp
// Stage 2 (hypervisor) tracking is opt-in with HYDRA_STAGE2_TRACKING: it
// also makes GPU writes visible to the CPU, but UE4 games still render wrong
// lighting with it. Guest page table (stage 1) tracking is the default.
...
bool Mmu::protectsGpuWrites() const { return !useStage1Tracking(); }
```
`hv/mmu.cpp:256-260` (`protectGpuWritten`) returns early under stage-1. CPU readbacks of rendered or computed data
see guest memory that was never updated, unless a copy engine read happened to flush it first.

### B13. HLE service writes into guest memory are invisible to GPU caches (Medium-high, can cause stale data)

`src/core/horizon/services/fssrv/file.cpp:31-32`:
```cpp
    stream->seekTo(offset);
    stream->readToSpan(out_buffer.stream->writeReturningSpan<u8>(size));
```
The buffer is a raw host pointer (`kernel/hipc/const.cpp:19`: `return reinterpret_cast<u8*>(mmu->unmapAddr(addr));`).
Host writes neither go through the guest page tables nor trigger stage-2 faults. No caller outside gpu/,
cpu/mmu and nvhost_as_gpu invokes `invalidateMemory` (grep).

A file read, or any IPC out-buffer, landing in a GPU-mapped memory pool therefore leaves the
buffer and texture caches stale. Eden routes such writes through `InvalidateRegion`/`OnCacheInvalidation`
(rasterizer_interface.h:100-114).

### B14. Stage-1 tracking re-protects PTEs without TLB invalidation (hazard; needs a test)

`hv/page_table.cpp:173-183`:
```cpp
void PageTable::resumeWriteTracking(ztd::Range<vaddr_t> range) {
    modifyRange(range,
                []([[maybe_unused]] ztd::Range<vaddr_t> range, u64& entry,
                   [[maybe_unused]] horizon::kernel::MemoryState& state,
                   PageFlags& flags) {
                    if (any(flags & PageFlags::WriteTrackingEnabled)) {
                        entry &= ~AP_FLAGS_MASK;
                        entry |= static_cast<u64>(ApFlags::UserReadKernelRead);
                    }
                });
}
```
Nothing in `hv/` issues `tlbi` (grep for `tlbi`/`TLBI` returns nothing). The tracker comment
(hv/stage2_tracker.hpp:13-14) says: "The VM invalidates its TLBs when those change, unlike for the guest's own
page tables."

On ARMv8, reducing a permission needs TLB maintenance to be guaranteed. A writable TLB entry left over from the
previous write can let later guest writes skip the fault, so the GPU caches would miss them. That would cause
*nondeterministic* stale buffers and textures, depending on TLB residency. The open "nondeterministic garbage textures"
symptom is a candidate. Confirm with a hypervisor test before acting (backlog P0-5).

### B15. No per-channel GPU state (Medium for multi-queue games)

`gpu/gpu.hpp:187-196` holds a single `ThreeD`/`Compute`/`Inline`/`TwoD`/`Copy` and a single
`subchannels[SUBCHANNEL_COUNT]`. `NvHostGpu::submitGpfifo` passes only the GMmu (nvhost_gpu.cpp:49-52).
Two NVN queues (two nvhost-gpu channels) interleave their register state. Eden keeps per-channel engines:
control/channel_state.h:35-62.

### B16. QMD linked TSC is ignored (Low-medium)

`compute.cpp:239-240`:
```cpp
        const auto sampler =
            three_d.getSampler(tsc_pool[getSamplerHandle(*handle)]);
```
When QMD w0x0B bit 30 is set, the sampler index must equal the raw handle (eden/textures/texture.h:159-166,
vk_compute_pipeline.cpp:173-191).

### B17. Compute local memory is fixed at 1 KiB (Low-medium)

`renderer/shader_decompiler/decompiler.cpp:109-114`:
```cpp
    context.local_memory_size =
        (type == ShaderType::Compute
             ? 0x400u
             : std::max<u32>(header.sh_local_mem_lo_sz + ...
```
The QMD `localPosAlloc()` (compute.hpp:66) is never used. Eden uses `local_pos_alloc + local_crs_alloc`
(shader_environment.cpp:415). Accesses beyond the size are clamped to the last word (msl/emitter.cpp:243-246),
which silently corrupts data.

### B18. The compute register array can overflow in release builds (Low; memory safety)

`compute.hpp:44`: `static_assert(sizeof(RegsCompute) == 0x983 * sizeof(u32));`. `engine_base.hpp:66-71`:
```cpp
    void writeReg(u32 reg, u32 value) {
        ASSERT_DEBUG(reg < REG_COUNT, Engines, "Invalid reg 0x{:08x}", reg);
        ...
        regs_raw[reg] = value;
```
The B1C0 class defines methods up to 0x33E4 (method 0xCF9) and `SET_MME_SHADOW_SCRATCH(i)` at 0x3400+
(ogd/clb1c0.h:704-747). Method 0x983 (SET_TRAP_HANDLER) lands in tail padding. Methods ≥0x984 overwrite
the `InlineBase::inline_data` vector that follows.

### B19. Oversized compute blocks are silently skipped (Low-medium)

`mtl/renderer.cpp:1079-1087`:
```cpp
    if (block.x() * block.y() * block.z() >
        pipeline->maxTotalThreadsPerThreadgroup()) {
        ONCE(LOG_ERROR(MetalRenderer, ...));
        return;
    }
```
Heavy MSL state (registers + 1 KiB local array) can push the limit below Maxwell's 1024, and then the whole dispatch
is dropped.

### B20. Shared memory is clamped to 32 KiB (Low)

`msl/emitter.cpp:247-249`: `std::clamp((state.shared_memory_size + 3) / 4, 1u, 0x2000u)`. This is a Metal
limit, but it fails silently.

### B21. Divergent BAR and no-op MEMBAR (Low)

`decoder/compute.cpp:369-376`:
```cpp
void emitBar(DecoderContext& context, instruction_t inst) {
    // TODO: modes other than BAR.SYNC 0
    const auto conditional =
        handlePredCond(context.builder, getPred(inst), getPredInv(inst));
    context.builder.opBarrier();
```
A predicated `threadgroup_barrier` is undefined behavior in MSL and can hang. MEMBAR is only a comment
(decoder.cpp:232-236).

### B22. Render-target barrier methods are ignored; Metal render encoders are never split or barriered (Medium on TBDR)

FRAGMENT_BARRIER (0x378), INVALIDATE_TEXTURE_DATA_CACHE (0x3DD) and TILED_CACHE_BARRIER (0x3DF) are not in
the 3D method table (3d.cpp:224-233). There are no `memoryBarrier`/`textureBarrier` calls in mtl/ (grep).
A draw that samples a texture written by earlier draws in the *same* render encoder (feedback after a guest
barrier) gives undefined results on Apple GPUs. Eden: maxwell_3d.cpp:363-369, vk_rasterizer.cpp:876-883.

### B23. GMMU unmap, MODIFY and reservations (Low-medium)

`nvhost_as_gpu.cpp:78`: `// TODO: remove the GPU mapping (sparse reservations read as zero)`.
`nvhost_as_gpu.cpp:98-103` handles MODIFY by logging and returning `NvResult::Success` without changing anything.
Reservations are backed by `malloc` (gmmu.cpp:113-114), so reads are uninitialized rather than 0.
`getVaRegions` is a HACK (nvhost_as_gpu.cpp:134-139).

### B24. Unmapped GPU VA dereferences null in release builds (Low; robustness)

`generic_mmu.hpp:77-84`:
```cpp
    const Impl& findAddrImpl(uptr addr, uptr& out_base) const {
        auto impl = const_cast<GenericMmu*>(this)->findAddrImplRef(addr, out_base);
        DEBUGGER_ASSERT_DEBUG(impl, Mmu, "Failed to find impl for addr 0x{:08x}", addr);
        return *impl;
    }
```

### B25. `SYNCPT_READ` of id 0 fails (Low)

`nvhost_ctrl.cpp:121-126` reads the video syncpoints, and `syncpoints.hpp:23` returns `nullopt` for id 0, so the
ioctl returns `NvResult::BadValue`.

### B26. Debug code changes behavior (Low; strip before commit)

The `HYDRA_PROFILE_COMPUTE` block (mtl/renderer.cpp:1148-1195) re-encodes every dispatch into a *second*
command buffer. That buffer is committed and waited on *before* the in-flight one: non-idempotent kernels
(atomics, accumulation) run twice and out of order.

The per-launch debug blocks run on every dispatch: `HYDRA_DUMP_COMPUTE` (compute.cpp:92-106), the
"describe/skip" block with `static std::set` and `getenv` (compute.cpp:267-297), and the semaphore logger (3d.cpp:550-561).

### B27. Shader cache relies on 8 sampled code bytes and is never invalidated (Low-medium)

`shader_cache.cpp:258-270` takes 8 bytes at offsets 80..206. The key also includes `code_ptr` (l.236).
`renderer.hpp:94-96`: `if (any(scope & MemoryInvalidationScope::ShaderCache)) { // TODO }`. If code memory is reused
for a different shader whose sampled bytes match, the stale shader runs.

### B28. A method's data cannot continue into the next GPFIFO entry (Low)

`pfifo.cpp:286-292` reads arguments sequentially through `read<u32>(gpu_addr)`, with no per-entry state. Eden keeps
`dma_state` across entries (dma_pusher.cpp:114-167).

### B29. The occlusion counter is a draw counter (Low-medium)

`3d.cpp:2276-2277`:
```cpp
    // Every draw counts as visible for occlusion queries
    zpass_pixel_count++;
```
It also counts draws rejected later in `drawInternal`. `resetCounter` compares the full word (`if (data == 0x1)`,
3d.cpp:540) instead of TYPE[4:0].

### B30. Compute descriptor bounds (Low)

`compute.cpp:129` treats `tic_limit == 0` as unlimited, and `getSamplerHandle(*handle)` is not checked against
`tsc_limit` (compute.cpp:240). Both allow out-of-bounds reads of the guest pool.

### B31. Present wait times out (Low)

`pfifo.cpp:136`: `completed_cond_var.wait_for(lock, std::chrono::milliseconds(250), ...)`. On timeout, the present
runs while the frame is still being encoded.

### B32. GPU-written memory never reaches the buffer cache (Medium once SSBOs or image buffers are used)

`BufferCache::getHot0`/`updateRangeHot2` (buffer_cache.cpp:51-138) always copy from guest memory. Suppose a compute
image buffer or a linear render target writes memory that is later bound as a vertex, index or uniform buffer.
The buffer cache then reads stale guest bytes, because the texture cache has not flushed them.

### Checked and found OK

- **CB_DATA vs pending draws.** Each draw binds a `BufferView` that owns the snapshot taken at its encode
  (buffer_cache.cpp:116-120; renderer.hpp state holds shared ownership). The >64 KiB path blits into the private buffer
  inside the ordered command buffer (buffer_cache.cpp:127-137). Earlier draws therefore keep the old contents. Verified by
  buffer_snapshot_tests.cpp:85-120 (red/green per-draw pixels) and texture_cache_tests.cpp:345-403.
- **Compute write → later render read.** These are separate encoders on tracked resources (no heaps, no untracked
  hazard mode in mtl/), so Metal orders them automatically. Compute → compute within one serial compute encoder is ordered
  too (command_buffer.cpp:124-125).
- **CPU-side engines (copy/I2M) vs pending GPU work.** They flush GPU-written textures with a synchronous commit+wait on
  the *same* command stream before touching memory (copy.cpp:266-271, texture_cache_hot.cpp:442). Tested.

---

## (c) Prioritized implementation backlog

Test infrastructure available:
- `hydra-buffer-snapshot-tests` / `hydra-texture-cache-tests` (metal::Renderer + caches, pixel readback);
- `hydra-shader-library-tests` (decode → MSL → Metal kernels);
- `hydra-xci-tests --check-gpu-memory` (System + GMmu + Copy engine);
- `hydra-memory-tracking-tests` (HVF stage-2);
- `hydra-render-hash-tests` (GMmu with `nullptr` CPU MMU + `tls_crnt_gmmu`).

All Metal targets need `-DHYDRA_METAL_CACHE_TESTS=ON` (CMakeLists.txt:344-389).

### P0: crash/corruption fixes and cheap, high-value correctness

**P0-1. Lock the buffer cache in compute launch (B4).**
- How: in `Compute::launch` (compute.cpp:69), add `std::scoped_lock buffer_lock(renderer.getBufferCache().getMutex());`
  *before* the texture lock. This is the same order as 3d.cpp:448 then drawInternal.
- Test: a TSAN build of a small harness that loops `IRenderer::invalidateMemory` on one thread while another calls
  `BufferCache::get`. Also a code-review check that the lock order is buffer → texture everywhere.
- Risk: very low.

**P0-2. Make GMmu thread-safe (B5, B24).**
- How: add `std::shared_mutex` to `GMmu`:
  - unique lock in `createAddressSpace`, `unbindToSparse`, `carveOut`/`mapIntoGaps`, and the `map`/`unmap` wrappers;
  - shared lock in `unmapAddr`, `findAddrImplRef` users, `readMappedBytes` and `load`.
  - Keep `push_buffer_window` valid by holding a shared lock for the duration of a GPFIFO entry, or by re-validating
    per entry.
  - A lock-free alternative: copy-on-write `std::shared_ptr<const Map>` swapped atomically.
  - Also return 0 / a dummy for unmapped VA instead of dereferencing null (generic_mmu.hpp:77-84), and drop
    such accesses in engines with a warning.
- Test: in `hydra-xci-tests --check-gpu-memory`, map/remap/unbind in a loop on one thread while another resolves
  `unmapAddr`/`readMappedBytes` (run under TSAN). Add a unit check that an unmapped VA returns 0 without crashing.
- Risk: low to medium (lock order: GMmu → IMmu, since `enableWriteTracking` is called under it).

**P0-3. Index buffer FIRST (B6).**
- How: in `ThreeD::drawVertexElements` (3d.cpp:462-479), set
  `size = min(limit + 1 - start, (first + count) * fmt_size)`, as in Eden buffer_cache.h:1337-1341.
  - Pass `first` to `IndexDescriptor`, so the u8/quad/fan decoders read from `start + first*size` and the draw uses
    `start = 0` for decoded buffers.
  - Keep `drawIndexed(start = first)` for the passthrough path.
- Test: Metal pixel test (raster_state_tests style). Put dummy indices at [0..3) and a full-screen triangle at
  [3..6), draw with FIRST=3, and expect coverage. Repeat with u8 indices and with quads.
- Risk: low (index cache key changes).

**P0-4. Invalidate decoded index buffers (B7).**
- How, option 1: add a generation counter to `BufferEntry` that is bumped in `invalidateMemoryHot1`. Include the
  source entry's generation in `IndexCache::hash`. Evict cache entries LRU (or per frame).
- How, option 2: hash the source bytes (u8/quad index data is small).
- Long term: GPU conversion kernels, like Eden's Uint8Pass/QuadIndexedPass.
- Test: decode u8 indices from a host vector, change one index, `invalidateMemory`, decode again, and expect the new
  value (no GPU needed: check `getPtr()` of the Shared buffer).
- Risk: low.

**P0-5. Stage-1 TLB investigation and fix (B14).**
- Test first: extend memory_tracking_tests with an EL0 guest that has the MMU on and runs Hydra's `PageTable`. It
  writes a tracked page (expect a fault), lets the host call `resumeWriteTracking`, writes again, and expects a
  second fault. Without TLBI, record whether the fault is missed.
- Fix if confirmed: keep a TLB generation counter. On re-arm, bump it and `hv_vcpus_exit` the running vCPUs. Before
  `hv_vcpu_run`, a vCPU whose generation is stale executes `tlbi vmalle1is; dsb ish; isb` through the exception
  trampoline (hv/cpu.cpp:24-33). Alternatively, make stage-2 the default for GPU-mapped memory, after P1-4 (B11).
- Risk: medium (performance of TLB flushes and vCPU kicks).

**P0-6. Invalidate GPU caches on HLE host writes (B13).**
- How: add `IMmu::notifyHostWrite(ztd::Range<vaddr_t>)`. It reuses the `pending_invalidations` queue (cpu/mmu.cpp:75-86)
  without changing protection, and filters to write-tracked/GPU-mapped ranges.
  - Call it centrally wherever HIPC out-buffers (B/C/MapAlias/static) are finalized, plus in direct writers: fssrv
    `IFile::read`/`IStorage::read`, audio renderer outputs, and any `IMmu::write` used by services.
- Test: unit test. Create a BufferCache snapshot of a guest range, run `IFile::read` into it through a fake process,
  apply the pending invalidations, and assert that `get()` returns a new snapshot with the new bytes.
- Risk: low to medium (IPC overhead; filtering keeps it cheap).

**P0-7. Bound compute register writes (B18) and strip debug side effects (B26).**
- How: grow `RegsCompute` to 0xD80 words, or make `writeReg` drop `reg >= REG_COUNT` in release builds with a
  one-time warning. Remove or gate the `HYDRA_PROFILE_COMPUTE` duplicate dispatch and the per-launch `std::set`/`getenv`
  debug code.
- Test: a unit test sending method 0xCF9 to a `Compute` instance and checking that `inline_data` is intact.
- Risk: very low.

### P1: synchronization and memory model, needed for correctness in many games

**P1-1. Real GPU syncpoints and fences (B1, C16, C18, D12, E2, E3).**
- How:
  - Generalize `video::Syncpoints` (src/core/video/syncpoints.hpp) into a GPU-wide manager with separate guest
    (max) and host (completed) values, like eden/host1x/syncpoint_manager.cpp:60-75.
  - `ChannelBase::getSyncpoint` for GPU channels allocates a real ID.
  - `NvHostGpu::submitGpfifo`: on `FenceGet` (increment), reserve `threshold = ++guest_max[id]` and return
    `{id, threshold}`; on `FenceWait`, have the PFIFO thread wait for `host[id] >= value` (with a timeout and log)
    before processing.
  - Attach the submission's increment to the Metal command buffer: `addCompletedHandler` → `syncpoints.complete(id, 1)`.
    For CPU→GPU data safety it is enough to signal at the end of encoding, because Hydra snapshots at encode time.
    Readbacks need completion, so signal on completion.
  - Host SYNCPOINTA/B and 3D `INCREMENT_SYNC_POINT` also increment through the same completion path.
  - In `nvhost_ctrl`, remove the `id == 0` shortcut, make `syncptWaitEvent` block up to the timeout, and fix
    `syncptRead(0)`.
- Test: a new `gpu_sync_tests.cpp` with a System using `GpuRenderer::Null`. Submit an empty GPFIFO with FenceGet,
  check the returned fence, wait through the NvHostCtrl ioctl, and expect it reached after PFIFO processing. With
  Metal, a long compute dispatch must not signal before the command buffer completes.
- Risk: **high**. A missed increment deadlocks games. Add a `HYDRA_FAKE_GPU_FENCES=1` escape hatch and a watchdog log.

**P1-2. Host (puller) methods (B2, C9–C17).**
- How:
  - Add a `HostMethods` unit in gpu/ (regs 0x00-0x40, laid out like eden/engines/puller.h:80-121). In
    `Gpu::subchannelMethod` (gpu.cpp:43), route `method < 0x40` to it before engine dispatch.
  - SEMAPHORED release: 4 B payload, or 16 B `{payload, 0, u64 ticks}` per bit 24.
  - Acquire EQ/GEQ/AND: bounded spin with yield, then log and continue.
  - Reductions per bits 30:27 and the format bit.
  - SET_REFERENCE: store. SYNCPOINTA/B: per FenceAction (bit 0 op, bits 8-31 id; puller.h:62-71) into P1-1.
    WFI / MEM_OP / FB_FLUSH: no-op.
  - After a release, invalidate the buffer cache for the written range.
- Test: pushbuffer-level test. Map a host page in a GMmu, feed `Gpu::subchannelMethod` words for SEMAPHOREA-D
  release (both sizes), and check memory and the timestamp unit. An acquire against an already-satisfied value must not
  block. A reduction ADD/INC must produce the expected values.
- Risk: medium (acquire spin).

**P1-3. Global memory / SSBOs (B3, A26–A29, F18, B32).**
- How:
  1. Decoder: real IR ops for LDG/STG (all sizes), ATOM/ATOM.CAS/RED (global), and generic LD/ST with a global
     fallback.
  2. Analyzer: port Eden's tracking (global_memory_to_storage_buffer_pass.cpp:373-412). Trace the 64-bit address to
     an LDC (cbuf, offset), prefer cbuf0 0x100–0x700 with 16-byte alignment, and record `{cbuf, offset, written}`.
  3. `ResourceMapping` gets `storage_buffers` and host bindings. The MSL emitter declares `device uint* ssboN`
     plus sizes, bounds-checks accesses (loads out of range return 0, stores are dropped), and emits atomics through
     `device atomic_uint*`.
  4. Engines (compute.cpp, 3d.cpp configureShaderStage) read `{u64 addr, size}` from the CB as Eden does
     (buffer_cache.h:2006-2050: size from the next qword, capped at 8 MiB).
  5. BufferCache: writable GPU buffers that are *not* snapshots. Mark written ranges GPU-modified. A later
     `get()` of an overlapping range must copy GPU→GPU instead of from the CPU. Write back to guest memory on CPU
     access (stage-2), on engine reads (as `TextureCache::flushMemory` does), and at fence completion for readbacks.
     Invalidate overlapping texture-cache entries.
- Test:
  - shader_library_tests: decode real LDG/STG/ATOM.ADD/RED kernels and run them on Metal against real buffers.
  - engine-level: drive `Compute::launch` with a synthetic QMD in GMmu-mapped memory and read back the SSBO.
  - buffer cache: GPU write, then `get()` as a vertex buffer sees the GPU data, then write-back to guest memory
    before a copy-engine read.
- Risk: **high** (touches decoder, emitter and caches). Gate behind an env var at first and compare render hashes in
  game.

**P1-4. Route CPU-fault readbacks through the GPU thread (B11), then reconsider stage-2 as default (B12).**
- How: add a request queue to `Pfifo` (range + promise). `IRenderer::flushMemoryForCpu` enqueues a request and waits.
  The PFIFO thread services requests between GPFIFO entries and when idle: it commits its in-flight command buffer
  (as `debugCommitAndWait` does), then runs `texture_cache.flushMemory`. This mirrors eden/gpu.cpp:214-227.
- Test: extend memory_tracking_tests or texture_cache_tests. Encode a draw that writes a texture into an *uncommitted*
  command buffer, trigger the CPU flush path, and expect the new pixels in guest memory.
- Risk: medium (deadlock if the faulting thread holds a lock the GPU thread needs; never hold the texture lock while
  waiting).

**P1-5. Report semaphores and queries (B9, B29, D3–D11).**
- How:
  - Decode the reduction bits and do atomic RMW on guest memory.
  - Unknown counters write 1 (Eden).
  - Use `WallClock::getGpuTick()` with remainder-exact conversion.
  - Invalidate the buffer cache for the written range.
  - Defer fence-type releases (short-query releases and copy semaphores) to Metal command-buffer completion, through a
    completion-write list on `metal::CommandBuffer` that writes guest memory and queues buffer-cache invalidation.
  - Real occlusion: Metal visibility result buffers (`setVisibilityResultMode(Counting, offset)`) for ZPASS, resolved
    in the completion handler.
  - CLEAR_REPORT_VALUE: decode TYPE[4:0] and reset per counter.
  - Optional GPU-tick divisor setting, like Eden's GpuClockMultiplier (gpu.cpp:44-53), for dynamic-resolution games.
- Test: a GMmu unit test for each control-word variant (layout, reduction, units). A Metal test where an occluded quad
  yields 0 and a visible one yields >0, with the value present only after completion.
- Risk: medium (deferred values can stall games that spin, so make sure every command buffer commits).

**P1-6. Copy engine semaphores (B10).**
- How: add regs 0x90-0x92 to `RegsCopy`. After both copy paths (copy.cpp:142-347, `remapPitchCopy`), release one or
  four words, with reductions. Use deferred completion once P1-5 exists.
- Test: extend `checkGpuMappings` in xci_tests: copy with SEMAPHORE_TYPE=1/2 and check the payload, timestamp and
  reduction INC.
- Risk: low.

**P1-7. I2M rewrite (B8).**
- How: port Eden `Upload::State` into `InlineBase`.
  - `launchDma` resets `write_offset`, computes `copy_size = line_length_in * line_count`, and latches the layout from
    bit 0.
  - Byte-accurate `loadInlineData`.
  - Flush when `copy_size` is reached.
  - Linear: per-line with `pitch_out`, chunked by GMMU mapping (copy.cpp:276-303 pattern).
  - Block-linear: swizzle the sub-rectangle with `getBlockLinearOffset`, using dst width/height/depth/layer/origin
    (memory_util.hpp).
  - Keep the texture flush before writing and the invalidation after.
- Test: GMmu(nullptr) unit test (render_hash_tests style) for multi-line pitch, a 6-byte upload, LAUNCH_DMA reset
  after a partial upload, and block-linear output compared with `convertLinearToBlockLinear`.
- Risk: low to medium.

**P1-8. Render-target barriers (B22, D14).**
- How: add 3D methods 0x378, 0x3DD and 0x3DF to the method table. Each ends the current render encoder
  (`CommandBuffer::endEncoding`), so the next draw starts a new encoder with load actions. On Apple GPUs,
  `memoryBarrier(scope: renderTargets, after: fragment, before: fragment)` may be used instead. Also detect "sampled
  texture == current color attachment" in `drawInternal` and split.
- Test: Metal test. Draw red into RT A, send FRAGMENT_BARRIER, then draw sampling A into RT B; B must be red. Run
  with `MTL_DEBUG_LAYER=1`.
- Risk: low to medium (more encoders cost performance).

**P1-9. Per-channel GPU context (B15, C19).**
- How: introduce `GpuChannel { ThreeD, Compute, Inline, TwoD, Copy, subchannels, macro driver }`, created per
  nvhost-gpu fd (allocGpfifoEx / allocObjCtx). `GpfifoEntryList` carries the channel, and `Gpu::subchannelMethod`
  becomes a channel method. Renderer and caches stay shared.
- Test: two channels with different CB bindings and render targets, with interleaved submissions; assert each draw
  uses its own channel's registers. A Null-renderer System test is enough.
- Risk: medium to high (refactor; the hot-reload ABI changes and needs a full restart).

**P1-10. QMD completeness (B16, B17, A12).**
- How: decode `linked_tsc` (w0x0B bit 30) and use the raw handle as the sampler index. Add
  `GuestShaderState::local_memory_size = local_pos_alloc + local_crs_alloc`, include it in the shader cache key
  (shader_cache.cpp:251-257), and use it in decompiler.cpp:109-114 for compute. Log when the local size exceeds the
  emitter clamp (0x4000 bytes).
- Test: a unit test decoding a synthetic QMD with every field set to distinctive values. A shader_library_tests kernel
  using 2 KiB of local memory gives correct results.
- Risk: low (shaders recompile).

### P2: completeness and robustness

1. **SULD, SURED, SUATOM.CAS (and SUATOM 0xEA40 range) and graphics-stage images (A25).** Shader domain.
   Test with shader_library_tests image kernels.
2. **Conditional rendering (D16) and transform feedback (D17).** Use Metal vertex-stage buffer writes for TFB.
   Test with a Metal pixel test plus counter checks.
3. **GMMU unmap / MODIFY / zeroed reservations / VA regions / kinds (B23, F2–F7).**
   - Implement `unmapBuffer` (carveOut and disable write tracking) and MODIFY.
   - Back reservations with zero pages or mark them "reserved" (reads 0, writes dropped).
   - Return proper small/big VA regions.
   - Test with xci_tests GMMU cases.
4. **Shader cache correctness (B27).** Hash the whole program (size known from decoding), or track shader code pages
   with write tracking; handle INVALIDATE_SHADER_CACHES. Test: reuse a code address with different code whose 8
   sampled bytes match.
5. **DMA pusher state across GPFIFO entries (B28).** Move `method`/`count`/`increment` state into `Pfifo`.
   Test: a pushbuffer whose INC method's data spans two entries.
6. **Compute dispatch limits (B19, B20, B21).** Log every skipped dispatch (rate-limited). Reduce the MSL register
   footprint. Turn a predicated BAR into an unconditional barrier with a predicated body.
7. **GPU-indirect dispatch (A20) and dependent QMD (A18).** After P1-3, follow Eden's kepler_compute.cpp:65-81 and
   `dispatchThreadgroups(indirectBuffer:)`.
8. **Present wait semantics (B31).** Wait on the fence from P1-1 instead of a 250 ms encode timeout.
9. **CB_DATA hygiene.** Bound-check `load_const_buffer_offset` against the selector size. Batch the invalidation of
   contiguous words (3d.cpp:615-629, "TODO: invalidate as a whole").
10. **Texture-from-buffer hazard (E8).** Add a Metal test (SUST through a buffer-backed texture, then a blit from
    the backing buffer in the next encoder) to settle the UNKNOWN.

### Suggested order

P0-1, P0-2, P0-7 (one-line safety fixes) → P0-3, P0-4 (index correctness) → P0-5, P0-6 (coherency;
likely relevant to the open nondeterministic texture issues) → P1-2 + P1-1 (host methods and fences, together)
→ P1-7, P1-6, P1-5 → P1-3 (SSBO; largest effort) → P1-4, P1-8 → P1-9 → P2.

### Reference index (ground truth used)

- ogd/clb1c0.h: 51, 83-176, 206, 214-229, 260-312, 349-380, 482-489, 547-576, 593-631, 663-699, 701-747.
- ogd/clb06f.h: 66-151, 154-176, 179-253.
- ogd/clb197.h: 52, 284-290, 2722-2743, 2759-2765, 3183-3205, 3609-3711, 4067-4080.
- ogd/clb0b5.h: 39-44, 66-99.
- eden/engines/kepler_compute.h: 56-190, 249-262.
- eden/engines/kepler_compute.cpp: 43-108.
- eden/engines/puller.h: 62-157.
- eden/engines/puller.cpp: 32-205.
- eden/dma_pusher.h: 52-97.
- eden/dma_pusher.cpp: 43-193.
- eden/engines/maxwell_3d.cpp: 307-370, 463-631.
- eden/engines/maxwell_3d.h: 2209-2237, 3284-3610.
- eden/engines/engine_upload.cpp: 28-83.
- eden/engines/maxwell_dma.cpp: 347-368.
- eden/query_cache/query_cache.h: 232-310.
- eden/fence_manager.h: 57-130.
- eden/host1x/syncpoint_manager.cpp: 60-92.
- eden/gpu.cpp: 44-53, 109-230.
- eden/renderer_vulkan/vk_rasterizer.cpp: 585-636, 833-883.
- eden/renderer_vulkan/vk_compute_pipeline.cpp: 151-259.
- eden/renderer_vulkan/vk_buffer_cache.cpp: 579-595.
- eden/buffer_cache/buffer_cache.h: 497-536, 1309-1352, 1523-1533, 2006-2050.
- eden/shader_environment.cpp: 408-420.
- eden/textures/texture.h: 159-166.
- eden/memory_manager.cpp: 178-203, 320-349.
- eden/control/channel_state.h: 35-62.
- eden/rasterizer_interface.h: 55-134.
- sr/ir_opt/global_memory_to_storage_buffer_pass.cpp: 373-412.
- sr/frontend/maxwell/maxwell.inc: 9, 61-114, 196-234, 282-283.
