# Hydra audit: graphics OS services and frame presentation

> Evidence policy (2026-10-18): NVIDIA documentation/headers define guest
> behavior; Apple Metal documentation, SDK headers and validation define host
> behavior. Eden and other emulators are historical comparators only. Any
> conclusion based solely on them is unverified and must be corroborated.

Scope: nvdrv (nvservices) devices and ioctls, syncpoint and fence semantics, vi (vi:u/vi:s/vi:m),
IHOSBinderDriver and the IGraphicBufferProducer buffer queue, the compositor, frame pacing and the
host CAMetalLayer presentation path. Read-only audit of `/Users/arvino/Documents/Project/hydra`
(branch `arvino`, HEAD `3e8c4cc2` plus uncommitted work), 2026-10-17.

## Conventions

* Hydra paths are relative to the repository root. Line numbers are for the working tree as audited.
* `E/<file>` = Eden HLE source fetched from `https://git.eden-emu.dev/eden-emu/eden/raw/branch/master/src/core/hle/service/<dir>/<file>`,
  saved as `/private/tmp/claude-501/-Users-arvino-Documents-Project-hydra/0e906531-f45a-4e9b-b0bc-dcb303edaa1b/scratchpad/refs/eden-hle/<dir>__<file>`
  (for example `E/nvdrv__devices__nvhost_gpu.cpp:368`).
* `YZ/<file>` = `/private/tmp/claude-501/-Users-arvino-Documents-Project-hydra/0e906531-f45a-4e9b-b0bc-dcb303edaa1b/scratchpad/refs/yuzu/src/video_core/<file>`.
* `SB-NV:<line>`, `SB-DS:<line>`, `SB-NF:<line>` = raw wikitext of https://switchbrew.org/wiki/NV_services ,
  https://switchbrew.org/wiki/Display_services and https://switchbrew.org/wiki/Nvnflinger_services , saved under
  `/private/tmp/claude-501/-Users-arvino-Documents-Project-hydra/0e906531-f45a-4e9b-b0bc-dcb303edaa1b/scratchpad/refs/switchbrew/`.
* Game evidence comes from logs already present in `build/` (Crash Bandicoot 4 = title `010073401175e000`,
  Jump Force Deluxe = `0100183010f12000`). Those logs were produced by builds a few days older than the working tree;
  this is noted where it matters.
* Classes: **VERIFIED** (implemented and covered by a test, or documented as verified in game in `tests/README-*.md`),
  **IMPLEMENTED_UNVERIFIED**, **PARTIAL** (implemented but incomplete or deviating from the references),
  **MISSING**, **UNKNOWN**. A stub that does exactly what the references do (for example Eden also only logs) is
  IMPLEMENTED_UNVERIFIED; a stub where the reference does real work is PARTIAL or MISSING.

## Executive summary

1. **ALLOC_AS_EX is decoded with the wrong field order**, so NVN's `flags = 1` becomes the big page size.
   `as_big_page_size` becomes 1 and every REMAP (NVN virtual/sparse memory pools) is mapped at
   `virt_offset_in_pages * 1` instead of `* 0x10000`. Both Crash 4 and Jump Force logs show
   `allocAsEx (big page size: 1, as fd: 0, flags: 65536 ...)`. Jump Force logs show REMAP calls. Bug B1.
2. **ALLOC_SPACE reservations (including sparse ones) are backed by uninitialized `malloc` memory**, so any read
   of a reserved-but-unmapped GPU range returns nondeterministic host heap bytes. Together with (1), every
   virtual-pool page a UE4 title remaps is read from the wrong place, and the result is undefined garbage. This is a strong
   candidate for the open "nondeterministic garbage textures" symptom. It is confirmed that Jump Force uses REMAP. Whether Crash 4 does is
   unknown, because current builds log ALLOC_SPACE/REMAP only at debug level; check that before the larger work. Bug B2.
3. **GPU fences are fake.** SUBMIT_GPFIFO and ALLOC_GPFIFO_EX(2) return fence `{id 0, value 0}`. Syncpoint 0 is
   always "reached" in nvhost-ctrl. Host/puller syncpoint and semaphore methods are dropped. PFIFO runs
   asynchronously, so the guest CPU never waits for GPU work and can recycle command, uniform and staging memory or read back
   results before Hydra's GPU thread has executed the commands. Bug B3.
4. **The buffer queue has no Dequeued/Acquired states**, only 8 slots and no bounds checks. A buffer is
   handed back to the producer before the host has waited for its rendering and drawn it. Disconnect does
   not free slots. A blocking DequeueBuffer runs on the single "others" service thread shared with hid,
   am, audio, fs, time and socket. Bugs B9 to B13.
5. **Frame pacing follows the host display link.** The vsync event is signalled once per CADisplayLink callback
   (120 Hz on ProMotion), not at 60 Hz. Swap interval, timestamps and async mode are ignored. Vsync stops
   entirely while no drawable is available. Bug B14.
6. Smaller deviations: NvResult enum values 0xD to 0x10 are off by one, the async syncpoint wait returns
   `BadValue` instead of `Timeout` (and a test locks that in), SET_TIMESLICE is missing (Jump Force calls it), CloseLayer destroys the
   layer, ConvertScalingMode writes no output, vi:m command IDs are wrong, binders leak (pool of 16, then an exception), and
   `Gpu::memory_maps` is accessed from two threads without a lock.

---

## (a) Capability table

### A. nvdrv service (`INvDrvServices`, `nvdrv`, `nvdrv:a`, `nvdrv:s`, `nvdrv:t`)

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| Open (cmd 0) | **PARTIAL** | `src/core/horizon/services/nvdrv/nvdrv_services.cpp:26-76` | `E/nvdrv__nvdrv_interface.cpp:22-50`, SB-NV:47 | Known paths work. Unknown paths (`/dev/nvhost-prof-gpu`, `-dbg-gpu`, `-msenc`, `-tsec`, ...) return a CMIF error `MAKE_RESULT(Svc,0)` (:69-70). Eden returns IPC success plus NvResult `FileOperationFailed`/`NotSupported`. A CMIF failure usually aborts the guest's nvdrv client. |
| Ioctl (cmd 1) dispatch | **PARTIAL** | `nvdrv_services.cpp:78-86,153-189`; `ioctl/const.hpp:5-42,152-230` | `E/nvdrv__nvdrv_interface.cpp:58-82` | Dispatches on type/nr only and ignores the size and direction bits. It writes `Out` arguments even for In-only ioctls (for example ALLOC_GPFIFO_EX 0x40204818). An invalid fd returns a CMIF error. No NotInitialized handling. Device errors go in the payload (`:186-188`, good). |
| Close (cmd 2) | **IMPLEMENTED_UNVERIFIED** | `nvdrv_services.cpp:88-96` | `E/nvdrv__nvdrv_interface.cpp:139` | An invalid fd gives a CMIF error instead of a payload error. |
| Initialize (cmd 3) | **IMPLEMENTED_UNVERIFIED** | `nvdrv_services.cpp:98-107` | `E/nvdrv__nvdrv_interface.cpp:157-182`, SB-NV:58 | Transfer memory is ignored, as in Eden. |
| QueryEvent (cmd 4) | **PARTIAL** | `nvdrv_services.cpp:109-129` | `E/nvdrv__nvdrv_interface.cpp:184-210`, `E/nvdrv__nvdrv.cpp:269-287`, SB-NV:63-118 | A device failure becomes a CMIF `NotFound` error (Eden: IPC success plus NvResult). A null event is returned as Success with `INVALID_HANDLE` (see E-row "QueryEvent"). |
| MapSharedMem (5), GetStatus (6), DumpStatus/DumpGraphicsMemoryInfo (9), InitializeDevtools (10) | **MISSING** | command table `nvdrv_services.cpp:21-24` | `E/nvdrv__nvdrv_interface.cpp` table, SB-NV:119-139 | Eden implements 6 and 9 as stubs. Rarely used by games. |
| SetAruid (8) | **IMPLEMENTED_UNVERIFIED** | `nvdrv_services.hpp:32` (stub) | Eden stub | |
| Ioctl2 (11) inline input | **IMPLEMENTED_UNVERIFIED** | `nvdrv_services.cpp:131-140` | SB-NV:141, `E/nvdrv__nvdrv_interface.cpp:84` | Used for SUBMIT_GPFIFO2. Inline length is not checked against `num_entries`. |
| Ioctl3 (12) inline output | **IMPLEMENTED_UNVERIFIED** | `nvdrv_services.cpp:142-151` | SB-NV:144 | |
| SetGraphicsFirmwareMemoryMarginEnabled (13) | **IMPLEMENTED_UNVERIFIED** | `nvdrv_services.hpp:43` | Eden stub | Called by both games (logs). |
| NvResult error codes | **PARTIAL** | `src/core/horizon/services/nvdrv/const.hpp:7-32` | SB-NV:2944-3058, `E/nvdrv__nvdata.h:25-82` | `AlreadyAllocated/Busy/ResourceError/CountMismatch` = 12/13/14/15. The references give 0xD/0xE/0xF/0x10. `OverFlow` and the `Disp*` codes are missing. Currently unused, so the bug is latent (B4). |

### B. `/dev/nvmap`

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| NVMAP_IOC_CREATE 0xC0080101 | **IMPLEMENTED_UNVERIFIED** | `ioctl/nvmap.cpp:12-20`, `gpu/gpu.hpp:50-67` | SB-NV:558, `E/nvdrv__devices__nvmap.cpp` IocCreate | Size is not page-aligned (Eden: `AlignUp(size, 0x1000)`). Size 0 returns `InvalidSize` (Eden: `BadValue`). The tests call `Gpu::createMap` directly, not the ioctl. |
| NVMAP_IOC_FROM_ID 0xC0080103 | **PARTIAL** | `nvmap.cpp:22-26` | Eden IocFromId (validates the id and duplicates the handle) | No validation and no reference count. |
| NVMAP_IOC_ALLOC 0xC0200104 (heap mask, flags, align, kind, address) | **PARTIAL** | `nvmap.cpp:29-40`, `gpu.hpp:69-76` | SB-NV:577, Eden IocAlloc + `E/nvdrv__core__nvmap.cpp` Handle::Alloc | heap_mask and kind are ignored. The returned alignment is always overwritten with 0x20000. `flags == 1` is stored as "write" (it is the bit0 `map_uncached` flag). No double-alloc check (Eden returns `InsufficientMemory`). Address 0 is not supported. Layout is 32 bytes, which matches. |
| NVMAP_IOC_FREE 0xC0180105 | **PARTIAL** | `nvmap.cpp:42-55`, `gpu.hpp:78-95` | SB-NV:590, Eden IocFree | Frees immediately with no refcount or pin. Binder slots and REMAP can keep referencing a freed id, and freed ids are reused LIFO (`gpu.hpp:52-62`). |
| NVMAP_IOC_PARAM 0xC00C0109 | **PARTIAL** | `nvmap.cpp:57-88` | SB-NV:610-617, Eden IocParam | Alignment is a constant 0x20000. Kind is 0 (logged as not implemented). Base returns `NotSupported` (Eden returns Success with -22). Heap is constant even when unallocated. |
| NVMAP_IOC_GET_ID 0xC008010E | **IMPLEMENTED_UNVERIFIED** | `nvmap.cpp:90-94` | Eden IocGetId | Handle and id are the same value, as in Eden. |
| Other nvmap ioctls (CLAIM, MMAP, READ/WRITE, PIN/UNPIN_MULT, CACHE, IVC, RESERVE, EXPORT_FOR_ARUID...) | **MISSING** | table `nvmap.cpp:7-10` | SB-NV:513-673 | Eden also lacks them. |
| nvmap table thread-safety | **PARTIAL** | `gpu.hpp:50-132,204-207` (no mutex) | n/a | Mutated on the nvservices thread and read by the presenter on the main thread (`gpu.cpp:113`). Data race (B16). |

### C. `/dev/nvhost-as-gpu`

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| BIND_CHANNEL 0x40044101 | **PARTIAL** | `ioctl/nvhost_as_gpu.cpp:19-22` | SB-NV:1407, `E/nvdrv__devices__nvhost_as_gpu.cpp` BindChannel | Stub. Hydra uses one GMMU per process (`nvhost_gpu.cpp:50`), so a second address space aliases the first. |
| ALLOC_SPACE 0xC0184102 (sparse, fixed, page size) | **PARTIAL** | `nvhost_as_gpu.cpp:24-35`, `gpu/gmmu.cpp:56-128` (`malloc` at :114) | SB-NV:1414, `E/nvdrv__devices__nvhost_as_gpu.cpp:148-191` | `align`, `page_size` and `Sparse` are ignored. The reservation gets **uninitialized malloc host memory** (B2). Eden maps sparse ranges to zero (`MapSparse`) and leaves non-sparse ranges unmapped. |
| FREE_SPACE 0xC0104103 | **MISSING** | `nvhost_as_gpu.cpp:37-42` (log only) | `E/...nvhost_as_gpu.cpp:225-252` | VA and mappings are never released. |
| MAP_BUFFER 0xC0184104 | **MISSING** | not in table | SB-NV:1437 | Eden also lacks it. |
| UNMAP_BUFFER 0xC0084105 | **PARTIAL** | `nvhost_as_gpu.cpp:76-86` (uncommitted change) | `E/...nvhost_as_gpu.cpp:383-420` | Only invalidates caches. The GPU mapping stays (`// TODO: remove the GPU mapping`). Sparse ranges do not return to zero. |
| MAP_BUFFER_EX 0xC0284106 (fixed offset, kind, buffer offset, mapping size) | **PARTIAL** | `nvhost_as_gpu.cpp:89-124` | SB-NV:1455, `E/...nvhost_as_gpu.cpp:300-381` | Kind and page_size are ignored. The Remap/Modify flag (bit 8) returns Success with no action (logged 90-197 times per session). Eden remaps the same pages with the new kind, so this is functionally close today. The 40-byte layout matches. |
| ALLOC_AS 0x40104107, MAP_BUFFER_EX2 0xC038410A | **MISSING** | — | SB-NV:1482,1523 | Eden also lacks them. |
| GET_VA_REGIONS (Ioctl1) 0xC0404108 | **PARTIAL** | `nvhost_as_gpu.cpp:126-140` | SB-NV:1491, `E/...nvhost_as_gpu.cpp:422-440` | Hard-coded `{offset 0, page 0x20000, pages 0}` twice ("HACK"). Eden returns the small-page and big-page VA windows. |
| GET_VA_REGIONS (Ioctl3) | **PARTIAL** | `nvhost_as_gpu.hpp:81-88` | Eden GetVARegions3 copies to the inline output | Regions are computed into a local and discarded, so the inline output is never written. |
| ALLOC_AS_EX / "InitializeEx" 0x40284109 | **PARTIAL** (bug) | `nvhost_as_gpu.hpp:68-71`, `.cpp:142-162` | `E/nvdrv__devices__nvhost_as_gpu.h:76-85` (`flags, as_fd, big_page_size, ...`), `E/...nvhost_as_gpu.cpp:96-140`; SB-NV:1510 lists big_page_size first (contradicted by game data) | Field order is wrong, so `as_big_page_size = 1` for NVN (B1). A non-zero VA range would also `malloc(va_end - va_start)`. |
| REMAP 0xC0??4114 (batch) | **PARTIAL** (bug) | `nvhost_as_gpu.cpp:166-196` | SB-NV:1566-1580, `E/...nvhost_as_gpu.cpp:254-298` | Logic is correct but uses the corrupted `as_big_page_size`. Unbinding uses `calloc` per call and leaks (`gmmu.cpp:181-185`). No check that the target lies inside a sparse allocation. Flags and kind are ignored. Jump Force issues REMAP (`build/swiftui-release/jump-force-touch.log:673`). |
| Switch 2 AS ioctls (GET_SYNC_RO_MAP, MAPPING_MODIFY, REMAP2) | **MISSING** | — | SB-NV:1546-1598 | Not relevant for Switch 1 titles. |

### D. `/dev/nvhost-gpu` (GPU channel)

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| SET_NVMAP_FD 0x40044801 | **IMPLEMENTED_UNVERIFIED** | `ioctl/channel_base.cpp:214-217` | `E/nvdrv__devices__nvhost_gpu.cpp` SetNVMAPfd | Stub, effectively equivalent. |
| SET_TIMEOUT 0x40044803 | **IMPLEMENTED_UNVERIFIED** | `channel_base.cpp:219-222` | Eden logs only | |
| ALLOC_GPFIFO 0x40084805, CHANNEL_WAIT 0x40184806, CYCLE_STATS 0xC0044807 | **MISSING** | table `channel_base.cpp:18-23` | SB-NV:2361-2367 | Eden also lacks them. |
| SUBMIT_GPFIFO 0xC0??4808 (inline entries, fence flags) | **PARTIAL** | `nvhost_gpu.cpp:42-58`, `gpu/pfifo.cpp:110-125` | SB-NV:2604-2625, `E/...nvhost_gpu.cpp:341-386` | Entries are queued to the asynchronous PFIFO thread. FenceWait, FenceGet/increment, SuppressWfi and increment_value (Eden bit 8, `E/nvdrv__devices__nvhost_gpu.h:169`) are all ignored. The returned fence is `{0,0}` (B3). The 24-byte header layout matches. |
| SUBMIT_GPFIFO2 via Ioctl2 0xC018481B | **PARTIAL** | `nvhost_gpu.hpp:47-57`, `nvhost_gpu.cpp:19-20` | SB-NV:2801, `E/...nvhost_gpu.cpp:113,420-433` | Same as above. |
| SUBMIT_GPFIFO2/KICKOFF via Ioctl1 0x1B (entries at `gpfifo` pointer) | **PARTIAL** (bug) | `channel_base.cpp:23` (maps nr 0x1b to the inline variant) | `E/...nvhost_gpu.cpp:85-86,400-421` (kickoff reads guest memory at `params.address`) | Hydra reads `num_entries` entries past the 24-byte input buffer (B7). |
| ALLOC_OBJ_CTX 0xC0104809 (class IDs) | **IMPLEMENTED_UNVERIFIED** | `nvhost_gpu.cpp:60-67` | `E/...nvhost_gpu.cpp:261-302` | Stub. Engines are bound from push-buffer SetObject (`gpu.cpp:43-84`), so this is functionally fine. No class validation (Eden returns BadParameter or AlreadyAllocated). Observed class 0xB197 in the Jump Force log. |
| ZCULL_BIND 0xC010480B | **IMPLEMENTED_UNVERIFIED** | `nvhost_gpu.cpp:69-74` | Eden stores the parameters only | Jump Force: `address 0x10144a000, mode 2`. |
| SET_ERROR_NOTIFIER 0xC018480C | **IMPLEMENTED_UNVERIFIED** | `nvhost_gpu.cpp:76-81` | Eden stub | |
| SET_PRIORITY 0x4004480D | **IMPLEMENTED_UNVERIFIED** | `channel_base.cpp:224-227` | Eden validates 0x32/0x64/0x96 | |
| CHANNEL_ENABLE/DISABLE/PREEMPT/FORCE_RESET 0x0E-0x11, EVENT_ID_CONTROL 0x12, CYCLE_STATS_SNAPSHOT 0x13, GET_ERROR_INFO 0x16, SUBMIT_GPFIFO_RETRY 0x19, SUBMIT_GPFIFO2_RETRY 0x1C | **MISSING** | table `channel_base.cpp:18-23` | SB-NV:2379-2397 | Eden lacks most of them too. |
| SET_USER_DATA/GET_USER_DATA 0x40084714/0x80084715 | **IMPLEMENTED_UNVERIFIED** | `channel_base.cpp:204-212` | Eden Set/GetClientData | |
| GET_ERROR_NOTIFICATION 0xC0104817 | **PARTIAL** | `nvhost_gpu.cpp:83-93` | SB-NV:2769-2777 (`status` always 0xFFFF) | Hydra returns status 0. |
| ALLOC_GPFIFO_EX 0x40204818 / EX2 0xC020481A | **PARTIAL** | `nvhost_gpu.cpp:95-106` | SB-NV:2779-2799, `E/...nvhost_gpu.cpp:183-245` (`fence_out = GetSyncpointFence(channel_syncpoint)`) | Returns fence `{0,0}`. For the In-only 0x18 Hydra still writes an output fence. Jump Force: `num_entries 8192, num_jobs 1`. |
| SET_TIMESLICE 0xC004481D | **MISSING** | not in table | SB-NV:2807, `E/...nvhost_gpu.cpp:87,446-456` | Jump Force calls it: `Unknown ioctl nr 0x1d for type 0x48` (`build/swiftui-release/jump-force-binder.log`). Guest gets NotImplemented (B8). |
| GET_WAITBASE 0xC0080003 | **IMPLEMENTED_UNVERIFIED** | `channel_base.cpp:138-144` | SB-NV:2480 ("Always returns 0") | |
| GET_SYNCPOINT 0xC0080002 on the GPU channel | **PARTIAL** | `channel_base.cpp:125-136` | Eden allocates a channel syncpoint per GPU fd (`E/...nvhost_gpu.cpp:39`) | GPU path returns 0. |
| Channel events (QueryEvent 1/2/3) | **IMPLEMENTED_UNVERIFIED** | `nvhost_gpu.cpp:22-39` | SB-NV:90-93, Eden QueryEvent | Never signalled, as in Eden. |

### E. `/dev/nvhost-ctrl`

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| SYNCPT_READ 0xC0080014 | **PARTIAL** | `ioctl/nvhost_ctrl.cpp:121-126` | SB-NV:385 | Only video syncpoints. Id 0 (the GPU "syncpoint") returns BadValue. |
| SYNCPT_INCR 0x40040015, SYNCPT_WAIT 0xC00C0016, SYNCPT_WAITEX 0xC0100019, SYNCPT_READ_MAX 0xC008001A | **MISSING** | table `nvhost_ctrl.cpp:18-23` | SB-NV:393-445 | Eden lacks them too. Needed once real GPU syncpoints exist. |
| MODULE_MUTEX 0x40080017 / MODULE_REGRDWR 0xC0180018 | **MISSING** | — | SB-NV:409-427 | |
| GET_CONFIG 0xC183001B | **PARTIAL** | `nvhost_ctrl.cpp:49-77` | SB-NV:447 ("Not available in production mode"), `E/nvdrv__devices__nvhost_ctrl.cpp:86-90` (always `ConfigVarNotFound`) | Returns `nv!` values from the set:sys table (more than 1500 keys) with Success. No NUL terminator after the copy. |
| SYNCPT_CLEAR_EVENT_WAIT 0xC004001C | **MISSING** | — | SB-NV:456, `E/...nvhost_ctrl.cpp:48,275-298` | |
| SYNCPT_WAIT_EVENT (EVENT_WAIT) 0xC010001D | **PARTIAL** | `nvhost_ctrl.cpp:79-89` | SB-NV:463-471, `E/...nvhost_ctrl.cpp:92-211` | Id 0 always returns Success. Video ids return `Timeout` without allocating an event or encoding `value` as `slot + ((id & 0xFFF) << 16) + (1 << 28)`. |
| SYNCPT_WAIT_EVENT_EX (EVENT_WAIT_ASYNC) 0xC010001E | **PARTIAL** (tested, but the test asserts non-reference behaviour) | `nvhost_ctrl.cpp:91-119`; `tests/shutdown_tests.cpp:643-680` | SB-NV:473-481, `E/...nvhost_ctrl.cpp:170,197,210` (returns `Timeout`, `value = slot + (id << 4)`) | A pending armed wait returns **BadValue**, and the test requires BadValue (`shutdown_tests.cpp:665-666,675-676`). On success the output value is not updated. Id 0 always succeeds (B5). |
| SYNCPT_ALLOC_EVENT (EVENT_REGISTER) 0xC004001F | **VERIFIED** | `nvhost_ctrl.cpp:128-144`; `shutdown_tests.cpp:655-660` | SB-NV:483, Eden IocCtrlEventRegister | |
| SYNCPT_FREE_EVENT (EVENT_UNREGISTER) 0xC0040020 | **VERIFIED** | `nvhost_ctrl.cpp:146-157`; `shutdown_tests.cpp:677-679` | SB-NV:490 | Eden masks `& 0xFF`. Hydra does not. |
| SYNCPT_FREE_EVENT_BATCH 0x40080021 | **MISSING** | — | SB-NV:497, `E/...nvhost_ctrl.cpp:58,259-273` | |
| SYNCPT_GET_SHIFT 0xC0040022 | **MISSING** | — | SB-NV:504 | |
| QueryEvent for syncpoint events | **PARTIAL** | `nvhost_ctrl.cpp:25-47` | SB-NV:94-96, `E/...nvhost_ctrl.cpp:300-326` | Returns Success with a **null** event for unregistered slots (becomes `INVALID_HANDLE`). The old-format slot is taken as 8 bits; switchbrew packs `slot + (id << 4)`. The syncpoint id is not checked (B6). |

### F. `/dev/nvhost-ctrl-gpu`

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| ZCULL_GET_CTX_SIZE 0x80044701 | **IMPLEMENTED_UNVERIFIED** | `ioctl/nvhost_ctrl_gpu.cpp:39-45` | `E/...nvhost_ctrl_gpu.cpp:245-249` returns 0x1 | Hydra returns 0x20000, which only wastes guest memory. |
| ZCULL_GET_INFO 0x80284702 | **IMPLEMENTED_UNVERIFIED** | `nvhost_ctrl_gpu.cpp:47-64` | Eden ZCullGetInfo (same values) | |
| ZBC_SET_TABLE 0x402C4703 | **IMPLEMENTED_UNVERIFIED** | `nvhost_ctrl_gpu.cpp:66-75` | Eden keeps a table | Called 12 times per boot (logs). |
| ZBC_QUERY_TABLE 0xC0344704 | **MISSING** | — | `E/...nvhost_ctrl_gpu.cpp:324-365` | |
| GET_CHARACTERISTICS (Ioctl1) 0xC0B04705, all fields | **IMPLEMENTED_UNVERIFIED** | `nvhost_ctrl_gpu.hpp:9-45`, `.cpp:77-129` | SB-NV:1819, `E/...nvhost_ctrl_gpu.cpp` GetCharacteristics1 | All 0xA0 bytes match Eden field for field. `buf_addr` is not written back (Eden writes 0xdeadbeef). |
| GET_CHARACTERISTICS (Ioctl3) | **PARTIAL** | `nvhost_ctrl_gpu.hpp:95-103` | `E/...nvhost_ctrl_gpu.cpp:166-209` | Only the inline output is filled. The fixed output's `gc` is left unwritten. |
| GET_TPC_MASKS 0xC0184706 (Ioctl1/3) | **IMPLEMENTED_UNVERIFIED** | `nvhost_ctrl_gpu.cpp:131-143`, `.hpp:104-111` | Eden (0x3 when the size is non-zero) | |
| FLUSH_L2 0x40084707 | **MISSING** | — | SB-NV:1949, `E/...nvhost_ctrl_gpu.cpp:367-374` | Eden returns Success. |
| INVAL_ICACHE..GET_TPC_EXCEPTION_EN_STATUS 0x0D-0x11, NUM_VSMS 0x12, VSMS_MAPPING 0x13, CG/PG controls 0x16-0x19, ELPG 0x1A, GET_ERROR_CHANNEL_USER_DATA 0x1B, GET_CPU_TIME_CORRELATION_INFO 0x1D | **MISSING** | — | SB-NV:1724-1770 | Eden lacks them too. |
| ZBC_GET_ACTIVE_SLOT_MASK 0x80084714 | **IMPLEMENTED_UNVERIFIED** | `nvhost_ctrl_gpu.cpp:145-152` | Eden (7, 1) | |
| PMU_GET_GPU_LOAD 0x80044715 | **IMPLEMENTED_UNVERIFIED** | `nvhost_ctrl_gpu.cpp:154-160` | Eden returns 100 | Hydra returns 0. |
| GET_GPU_TIME 0xC010471C | **IMPLEMENTED_UNVERIFIED** | `nvhost_ctrl_gpu.cpp:162-166`, `src/core/hw/wall_clock.cpp:30-32`, `wall_clock.hpp:6` | SB-NV:2088-2095 (PTIMER ticks of 1.625 ns) | 614.4 MHz ticks, which matches switchbrew. Eden returns ns. |
| QueryEvent 1 (ErrorEvent) / 2 (SemaphoreEvent) | **IMPLEMENTED_UNVERIFIED** | `nvhost_ctrl_gpu.cpp:22-37` | SB-NV:98-100 | |

### G. Host1x channels (`/dev/nvhost-nvdec`, `/dev/nvhost-vic`, `/dev/nvhost-nvjpg`), ioctl level only

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| CHANNEL_SUBMIT 0xC0??0001 (nvdec, vic) | **VERIFIED** | `channel_base.cpp:25-123`; `tests/shutdown_tests.cpp:107-430`; `tests/README-video.md:39-44,55-60` | SB-NV:2434-2470, `E/nvdrv__devices__nvhost_nvdec_common.cpp` | Relocations return NotSupported. Fences complete synchronously at submit time (README notes FFmpeg reordering). |
| GET_SYNCPOINT 0xC0080002 (video) | **VERIFIED** | `channel_base.cpp:125-136`; `shutdown_tests.cpp:344-349` | SB-NV:2472 | Independent non-zero video syncpoints. |
| MAP_CMD_BUFFER 0xC0??0009 / UNMAP_CMD_BUFFER 0xC0??000A | **VERIFIED** | `channel_base.cpp:151-202`; `shutdown_tests.cpp:137-161` | SB-NV:2506-2536 | Stable Host1x addresses. Unmap keeps the assignment. |
| GET_WAITBASE / SET_SUBMIT_TIMEOUT 0x40040007 | **IMPLEMENTED_UNVERIFIED** | `channel_base.cpp:138-149` | SB-NV:2480,2491 | |
| SET_TIMEOUT_EX 0x13, GET_CLK_RATE 0x23 (0x14 on 1.0-7.0.1), SET_CLK_RATE 0x08, SUBMIT_EX/MAP_EX/UNMAP_EX 0x24-0x26, GET_MODMUTEX 0x04 | **MISSING** | table `channel_base.cpp:10-23` | SB-NV:2488-2560; `E/...nvhost_nvdec.cpp` (implements GET_CLK_RATE) | |
| `/dev/nvhost-nvjpg` | **PARTIAL** | `ioctl/nvhost_nvjpg.hpp:7-11` (ChannelBase with no engine) | `E/...nvhost_nvjpg.cpp` (SetNVMAPfd only) | Submits are stub successes. |

### H. Other nv devices

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| `/dev/nvdisp-ctrl` NUM_OUTPUTS 0x80040212 / GET_DISPLAY_PROPERTIES 0xC0140213 | **IMPLEMENTED_UNVERIFIED** | `ioctl/nvdisp_ctrl.cpp:5-22` | SB-NV:675-713 | Stubs. Only system modules use these (NvDrvPermission). |
| `/dev/nvdisp-disp0/1` GET_WINDOW 0x40040201 / SET_CMU 0xC99A020E, everything else (FLIP, GET/SET_MODE, GET_VBLANK_SYNCPT ...) | **PARTIAL** | `ioctl/nvdisp_disp.cpp:5-21` | SB-NV:716-1068, `E/nvdrv__devices__nvdisp_disp0.cpp` (no ioctls; composite helper) | System-only. |
| `/dev/nvsched-ctrl` | **MISSING** | `ioctl/nvsched_ctrl.cpp:5` (empty table) | SB-NV:1146-1299 | System-only. |
| prof-gpu, dbg-gpu, msenc, tsec, nvcec, nvhdcp, nvdcutil, nverpt | **MISSING** | `nvdrv_services.cpp:67-71` | SB-NV:1070-1370,1600-1712 | Open returns a CMIF error. Eden returns NotSupported for prof-gpu. |

### I. Syncpoint and fence semantics, end to end

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| Per-channel GPU syncpoint allocation | **MISSING** | `nvhost_gpu.hpp:8-58` (no syncpoint member) | `E/...nvhost_gpu.cpp:39`, `E/nvdrv__core__syncpoint_manager.cpp` | |
| Syncpoint max tracking at submit (`IncrementSyncpointMaxExt`) | **MISSING** | `nvhost_gpu.cpp:54-56` | `E/...nvhost_gpu.cpp:368-372` | |
| GPU increments syncpoints when work completes (puller SyncpointPayload 0x1C / SyncpointOperation 0x1D, WFI 0x1E) | **MISSING** | `gpu/gpu.cpp:43-94` (only method 0 is special); `engines/engine_base.hpp:10-22` (other low methods become plain `writeReg`) | `E/...nvhost_gpu.cpp:301-339` (wait/increment command lists), Eden `video_core/engines/puller.cpp` | Puller methods sent to any subchannel are stored as engine registers and never executed. |
| Puller semaphores (0x04-0x07 acquire/release, 0x1A/0x1B), NonStallInterrupt, RefCnt | **MISSING** | same as above | Eden puller | The 3D-class report semaphore is implemented separately (`engines/3d.cpp:545-600`). |
| Fence returned by SUBMIT_GPFIFO (FenceGet, increment, increment_value) | **MISSING** | `nvhost_gpu.cpp:54-56` | `E/...nvhost_gpu.cpp:368-385` | Always `{0,0}`. |
| FenceWait flag (GPU waits for a fence before the entries) | **MISSING** | `nvhost_gpu.cpp:42-58`, `pfifo.cpp:159` (`// TODO: flags`) | `E/...nvhost_gpu.cpp:358-366` | |
| CPU waits on GPU fences (nvhost-ctrl event waits for the channel syncpoint) | **MISSING** | `nvhost_ctrl.cpp:84,96` (`if (id == 0) ... return NvResult::Success`) | `E/...nvhost_ctrl.cpp:92-211` | Every GPU fence wait completes immediately. Observed: Jump Force `syncptWaitEventEx (ID: 0, treshold: 13, timeout: 100)`. |
| Video (Host1x) syncpoints: increment, read, wait, async event | **VERIFIED** | `src/core/video/syncpoints.hpp:14-75`, `channel_base.cpp:101-111`; `shutdown_tests.cpp:341-374,643-680`; `README-video.md:39-44` | `YZ/host1x/syncpoint_manager.*` | Synchronous completion. The pending async status is wrong (see E). |
| Compositor waits for the buffer acquire fence (QueueBuffer NvMultiFence) | **PARTIAL** | `display/binder.cpp:47-55` (records the PFIFO submitted count), `display/layer.cpp:24-25`, `pfifo.cpp:132-139` (250 ms timeout) | `E/nvnflinger__hardware_composer.cpp:117`, `E/nvdrv__devices__nvdisp_disp0.cpp:58-83`, `YZ/gpu.cpp:254-292` (RegisterGuestAction per fence) | A conservative heuristic: wait for all GPFIFO work submitted before QueueBuffer. The multi-fence is ignored. On timeout it presents an incomplete frame (B15). |
| Release fence returned by DequeueBuffer | **IMPLEMENTED_UNVERIFIED** | `hos_binder_driver.cpp:170-174` (`num_fences = 0`) | `E/nvnflinger__ui__fence.h` NoFence (ids -1) | Equivalent because num_fences is 0. |
| Vblank syncpoints (26/27), NVDISP_GET_VBLANK_SYNCPT | **MISSING** | — | `E/nvdrv__core__syncpoint_manager.cpp` (reserves 26/27), SB-NV:742 | System-only. |

### J. vi root services

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| vi:u GetDisplayService (0) / ...WithProxyNameExchange (1) | **IMPLEMENTED_UNVERIFIED** | `visrv/application_root_service.cpp:7-21` | SB-DS:334-350 | The input u32 (privilege) is ignored. |
| vi:s GetDisplayService (1) / ...ProxyNameExchange (3) | **IMPLEMENTED_UNVERIFIED** | `visrv/system_root_service.cpp:7-20` | SB-DS:352-368 | |
| vi:m GetDisplayService (2) / ...ProxyNameExchange (3) | **PARTIAL** (bug) | `visrv/manager_root_service.cpp:7-9` (`0, getDisplayService, 2, getDisplayServiceWithProxyNameExchange`) | SB-DS:370-394 | Cmd 2 works by accident (same object type). Cmd 3 is unknown. Cmd 0 does not exist on vi:m (B19). |

### K. `IApplicationDisplayService`

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| GetRelayService (100) | **IMPLEMENTED_UNVERIFIED** | `visrv/application_display_service.cpp:32-53` | SB-DS:458, `E/vi__application_display_service.cpp:66-70` | Connects a new dispdrv session. |
| GetSystemDisplayService (101) / GetManagerDisplayService (102) | **IMPLEMENTED_UNVERIFIED** | `application_display_service.cpp:55-65` | SB-DS:464-468 | No per-port privilege check (switchbrew: error 0xA72 on vi:u). |
| GetIndirectDisplayTransactionService (103) | **PARTIAL** | `application_display_service.cpp:67-75` | SB-DS:461 | Returns the relay service. Eden does the same. |
| ListDisplays (1000) | **PARTIAL** | `application_display_service.cpp:77-90` | SB-DS:996-1016 ("1080p even when in handheld-mode"), `E/vi__vi_types.h:65-68` | Reports the current resolution (1280x720 in handheld). Should report 1920x1080. |
| OpenDisplay (1010) | **PARTIAL** | `application_display_service.cpp:92-104`, `display/driver.hpp:21-27` (always id 1, "not implemented") | `E/vi__application_display_service.cpp:92-114` (validates names) | |
| OpenDefaultDisplay (1011) | **MISSING** | — | `E/...application_display_service.cpp:116-119` | |
| CloseDisplay (1020) | **IMPLEMENTED_UNVERIFIED** | `application_display_service.cpp:106-112` | | No-op. |
| SetDisplayEnabled (1101) | **MISSING** | — | `E/...application_display_service.cpp:126-132` (no-op success) | |
| GetDisplayResolution (1102) | **PARTIAL** | `application_display_service.cpp:114-127`, `horizon/os.cpp:359-385` | `E/...application_display_service.cpp:134-143` (fixed 1280x720); SB-DS:1081-1082 | Returns the docked or handheld or host-window-derived resolution. |
| OpenLayer (2020) | **PARTIAL** | `application_display_service.cpp:129-152`, `hosbinder/parcel.hpp:165-177` | SB-DS:494-501,1023-1067, `E/...application_display_service.cpp:176-204` | aruid and display are ignored, with no owner or open-state checks. An unknown id hits `LOG_FATAL` (`driver.hpp:46-51`). The NativeWindow parcel layout matches (0x28 bytes). Word 1 is 0 (Eden writes 1). |
| CloseLayer (2021) | **PARTIAL** (bug) | `application_display_service.cpp:154-157` | `E/vi__container.cpp:259-273` (close only, can reopen) | Destroys the layer, and its binder leaks (B17). |
| CreateStrayLayer (2030) | **IMPLEMENTED_UNVERIFIED** | `display_service_base.cpp:8-32` | SB-DS:506 | Flags and display id are ignored. |
| DestroyStrayLayer (2031) | **PARTIAL** | `application_display_service.cpp:168-174` | `E/vi__container.cpp` DestroyStrayLayer | The binder is never destroyed. |
| SetLayerScalingMode (2101) | **PARTIAL** | `application_display_service.hpp:44` (STUB) | `E/...application_display_service.cpp:145-160` (validates) | Ignored by the compositor. |
| ConvertScalingMode (2102) | **PARTIAL** (bug) | `application_display_service.cpp:176-179` | `E/...application_display_service.cpp:268-292` | Takes no input and writes **no output** (B18). |
| GetIndirectLayerImageMap (2450), ...CropMap (2451), ...RequiredMemoryInfo (2460) | **MISSING** | — | SB-DS:515-528, `E/...application_display_service.cpp:299-324` | Used by library applets. |
| GetDisplayVsyncEvent (5202) | **PARTIAL** | `application_display_service.cpp:181-189`, `display/display.hpp:10`, `display/driver.cpp:73-79` | SB-DS:530, `E/...application_display_service.cpp:253-266` (per-session event, PermissionDenied on a repeat call) | One event is shared by every caller and returned as a Move handle. Signalled at the host display-link rate (S row). |
| GetDisplayVsyncEventForDebug (5203) | **MISSING** | — | SB-DS:533 | |

### L. `ISystemDisplayService`

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| GetZOrderCountMin (1200) | **MISSING** | — | SB-DS:536+ | |
| GetZOrderCountMax (1202) | **IMPLEMENTED_UNVERIFIED** | `visrv/system_display_service.cpp:12-19` | Eden nullptr | Returns 15. |
| GetDisplayLogicalResolution (1203), SetDisplayMagnification (1204) | **MISSING** | — | SB-DS | |
| SetLayerPosition (2201) | **PARTIAL** | `system_display_service.cpp:21-28`; `display/layer.hpp:40` | Eden nullptr | Stored but never used by `Layer::present` (`layer.cpp:68-80`). |
| SetLayerSize (2203) | **PARTIAL** | `system_display_service.cpp:30-37`; `layer.cpp:74-75` | | Applied as `size * dst_scale` and anchored at the letterbox origin, with no scaling-mode logic. |
| GetLayerZ (2204) | **MISSING** | — | `E/vi__system_display_service.cpp:26` | |
| SetLayerZ (2205) | **IMPLEMENTED_UNVERIFIED** | `system_display_service.cpp:39-45`; `driver.cpp:31-46` | Eden | Used for z sorting. |
| SetLayerVisibility (2207) | **PARTIAL** | `system_display_service.cpp:56-58` → `display_service_base.cpp:34-39` (log only) | `E/vi__container.cpp:115` | Never calls `Layer::setVisible`. |
| SetLayerAlpha (2209), indirect layers (2400-2402) | **MISSING** | — | SB-DS | |
| CreateStrayLayer (2312) | **IMPLEMENTED_UNVERIFIED** | `system_display_service.cpp:47-54` | | |
| ListDisplayModes (3000) .. SetDisplayCrcMode (3218) other than GetDisplayMode | **MISSING** | — | SB-DS | Display settings, system-only. |
| GetDisplayMode (3200) | **IMPLEMENTED_UNVERIFIED** | `system_display_service.cpp:60-72` | `E/vi__system_display_service.cpp` GetDisplayMode | Resolution and 60 Hz. |
| GetLayerPresentationSubmissionTimestamps (6013), shared buffer/layer (8225, 8250-8258) | **MISSING** | — | SB-DS | System applets. |

### M. `IManagerDisplayService`

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| CreateManagedLayer (2010) | **IMPLEMENTED_UNVERIFIED** | `visrv/manager_display_service.cpp:12-32` | `E/vi__manager_display_service.cpp` | Flags and aruid are ignored. |
| DestroyManagedLayer (2011) | **PARTIAL** | `manager_display_service.cpp:34-38` | | Binder leak. |
| CreateStrayLayer (2012) | **IMPLEMENTED_UNVERIFIED** | `manager_display_service.cpp:40-47` | | |
| AddToLayerStack (6000) | **PARTIAL** | `manager_display_service.cpp:49-53` (stub) | `E/nvnflinger__hwc_layer.h:28-43` (layer stacks) | Layer stacks are not modelled. |
| SetLayerVisibility (6002) | **PARTIAL** | `manager_display_service.cpp:55-58` (stub) | | |
| Other manager commands (200/201, 1102, 2050-2055, 2300-2303, 4201-4209, 6001-6015, 7000, 8000+) | **MISSING** | — | SB-DS:683-860 | System-only. |

### N. `IHOSBinderDriver` (`dispdrv`) and parcels

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| TransactParcel (0) | **PARTIAL** | `hosbinder/hos_binder_driver.cpp:75-84,130-278` | SB-NF:28-31, `E/nvnflinger__hos_binder_driver.cpp` | Flags (oneway) are ignored. A bad binder id hits `LOG_FATAL` (`driver.hpp:65-70`). The reply size is not checked against the buffer. |
| AdjustRefcount (1) | **IMPLEMENTED_UNVERIFIED** | `hos_binder_driver.cpp:86-102` | SB-NF:33-54 | Counted, never acted on. |
| GetNativeHandle (2) | **IMPLEMENTED_UNVERIFIED** | `hos_binder_driver.cpp:105-116`; `display/binder.hpp:117` | SB-NF:56, `E/nvnflinger__buffer_queue_producer.cpp` GetNativeHandle | The buffer-available event. Observed `Infinite wait on [Display event ...]` 2174 times in the Jump Force log, so NVN gates DequeueBuffer on this event. |
| TransactParcelAuto (3) | **PARTIAL** | `hos_binder_driver.cpp:118-127` | SB-NF | Same as cmd 0. |
| Parcel reader/writer | **PARTIAL** | `hosbinder/parcel.hpp:26-204` | `E/nvnflinger__parcel.h:28-190` | Layout is compatible. Alignment padding is never written (a bool is written as 1 byte and 3 stale bytes; Eden writes a zero-padded u32). Flattened-size checks are `ASSERT_DEBUG` only, so a mismatched length misparses silently in release builds. |

### O. `IGraphicBufferProducer` transactions

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| REQUEST_BUFFER (1) | **PARTIAL** (bug) | `hos_binder_driver.cpp:151-164` | `E/nvnflinger__buffer_queue_producer.cpp:41-64` | `slot > MAX` lets slot 8 and negative slots through (out-of-bounds read). No Dequeued-state check. Status is always OK. |
| SET_BUFFER_COUNT (2) | **MISSING** | default branch `:271-273` | `E/...buffer_queue_producer.cpp:66-124` | |
| DEQUEUE_BUFFER (3) (async, w/h/format/usage) | **PARTIAL** | `hos_binder_driver.cpp:165-177`, `binder.cpp:16-45` | `E/...buffer_queue_producer.cpp:126-322,823-837` | The request is not parsed. It blocks on a condition variable on the shared server thread, and blocks forever if no buffers are set. It always returns the lowest free slot (Eden: the oldest free slot). There is no Dequeued state, no `WouldBlock`, and no realloc flags. |
| DETACH_BUFFER (4), DETACH_NEXT_BUFFER (5), ATTACH_BUFFER (6) | **MISSING** | default branch | `E/...buffer_queue_producer.cpp:324-433` | |
| QUEUE_BUFFER (7) (timestamp, auto-timestamp, crop, scaling, transform, sticky, async, swap interval, fence) | **PARTIAL** | `hos_binder_driver.cpp:178-196`, `binder.cpp:47-67`, `display/binder.hpp:56-71` | `E/...buffer_queue_producer.cpp:435-552`, `E/nvnflinger__graphic_buffer_producer.h:22-52` | The 84-byte layout matches (`_unknown` is `async`). Only crop and transform are consumed. Scaling mode, timestamps, async/droppable, swap interval and the fence are ignored. The slot is unvalidated (out-of-bounds write). The reply reports `num_pending_buffers = 8` ("HACK") and display-resolution width/height (B13). |
| CANCEL_BUFFER (8) | **MISSING** | default branch | `E/...buffer_queue_producer.cpp:554-580` | The release fence is dropped. Harmless only because there is no Dequeued state. |
| QUERY (9) | **PARTIAL** | `hos_binder_driver.cpp:197-220` | `E/...buffer_queue_producer.cpp:582-631`, `E/nvnflinger__window.h` | Answers only width/height/format, with the display resolution instead of the preallocated buffer size. MinUndequeued (3), ConsumerRunningBehind (9), ConsumerUsageBits (10), StickyTransform (11), DefaultDataSpace/max count (12) and BufferAge (13) return 0. |
| CONNECT (10) | **PARTIAL** | `hos_binder_driver.cpp:221-231` | `E/...buffer_queue_producer.cpp:633-685,801-815` | The request (listener, api, producerControlledByApp) is not parsed. No connected-API state. `num_pending_buffers = 8` hack. No `dequeue_buffer_cannot_block`. |
| DISCONNECT (11) | **PARTIAL** (bug) | `hos_binder_driver.cpp:232-239`, `binder.cpp:113-130` | `E/...buffer_queue_producer.cpp:687-738` (`FreeAllBuffersLocked`) | Drops the queue but keeps every slot, so stale GraphicBuffers can be dequeued and presented later (B12). |
| SET_SIDEBAND_STREAM (12), ALLOCATE_BUFFERS (13) | **MISSING** | — | Eden also missing | |
| SET_PREALLOCATED_BUFFER (14) (GraphicBuffer/NvGraphicBuffer) | **PARTIAL** (bug) | `hos_binder_driver.cpp:240-270`, `binder.cpp:7-14` | `E/...buffer_queue_producer.cpp:740-770` | A non-null buffer with slot ≥ 8 or < 0 writes out of bounds. No nvmap pin or duplicate. Default size, format and max count are not updated. A null buffer frees the slot (good). |
| GET_BUFFER_HISTORY (17) | **PARTIAL** | default branch writes only the status | `E/...buffer_queue_producer.cpp:895-925` | Equivalent to Eden with history disabled. |
| GraphicBuffer / NvGraphicBuffer layout (0x16C, planes, block height log2, kind, pitch, offset, nvmap id) | **IMPLEMENTED_UNVERIFIED** | `display/binder.hpp:8-26`, `gpu/const.hpp:685-722` | `E/nvnflinger__ui__graphic_buffer.h:24-101` | Offsets match: buffer_id at 0x2C = `nvmap_id`, offset at 0x78 = `planes[0].offset`, handle at 0x74. Planes 1-2 and `num_planes` are ignored. |
| QueueBufferOutput (16 B) / NvMultiFence (36 B) | **IMPLEMENTED_UNVERIFIED** | `binder.hpp:39-78` | `E/nvnflinger__graphic_buffer_producer.h:54-79`, `E/nvnflinger__ui__fence.h` | |

### P. Buffer queue state machine

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| Slot states Free / Dequeued / Queued / Acquired | **PARTIAL** | `binder.hpp:28-32` (`initialized`, `queued`) | `E/nvnflinger__buffer_slot.h:18-23` | No Dequeued or Acquired, so one slot can be handed out twice and a buffer being presented can be re-dequeued (B10). |
| 64 slots | **PARTIAL** | `binder.hpp:81` (`MAX_BINDER_BUFFER_COUNT = 8`) | `E/nvnflinger__buffer_queue_defs.h:15` | Indices are not validated (B9). |
| Max acquired / min undequeued / async (`WouldBlock`) | **MISSING** | — | `E/...buffer_queue_producer.cpp:126-228` | |
| Frame dropping for droppable (async) buffers | **MISSING** | `binder.cpp:47-55` (always appends) | `E/...buffer_queue_producer.cpp:504-529` | |
| Buffer-available event | **IMPLEMENTED_UNVERIFIED** | `binder.cpp:36-38,95,129` | Eden `buffer_wait_event` | Signalled on consume; cleared when the last free slot is handed out. |
| Dequeue blocking isolation | **PARTIAL** (bug) | `binder.cpp:19`; `horizon/os.cpp:232` (dispdrv on the shared "others" server); `services/server.cpp:57-140` (single thread) | Real vi has dedicated threads; Eden runs a per-service server | A blocking dequeue stalls hid, am, audout, audren, opus, fs, time, socket, nifm and others (B11). |

### Q. Compositor (nvnflinger equivalent)

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| Acquire queued buffers | **PARTIAL** | `display/driver.cpp:12-24`, `layer.cpp:13-31`, `binder.cpp:69-98` | `E/nvnflinger__hardware_composer.cpp:50-153,192-245` | At most one buffer per layer per host frame. The slot is released before the GPU wait and the draw. |
| Hold the acquired buffer for swap_interval vsyncs, release afterwards | **MISSING** | — | `E/...hardware_composer.cpp:158-176,195-208` | |
| Multi-layer composition, z-order | **IMPLEMENTED_UNVERIFIED** | `driver.cpp:26-71` | `E/...hardware_composer.cpp:143-148` | Stable z sort. Layers are composited from creation, not only after OpenLayer. |
| Layer visibility | **PARTIAL** | `layer.hpp:44`, `driver.cpp:18,33`; vi visibility is stubbed (L/M rows) | `E/...hardware_composer.cpp:103` | Only the internal recording-layer flag (`am/self_controller.cpp:76-79`) works. |
| Layer position | **MISSING** | `layer.hpp:40` (unused in `layer.cpp:68-80`) | vi SetLayerPosition | |
| Layer size | **PARTIAL** | `layer.cpp:74-75` | | |
| Crop rectangle (empty → full buffer) | **IMPLEMENTED_UNVERIFIED** | `layer.cpp:32-50`, `renderer/metal/surface_compositor.cpp:53-63` | `YZ/framebuffer_config.cpp:33-77` (NormalizeCrop) | Equivalent result. Jump Force queues an empty crop (`Invalid src width/height` warnings). No clamping or `IsEmpty` handling of inverted rectangles. |
| Scaling modes (freeze / scale-to-window / scale-crop / no-scale-crop / preserve-aspect) | **PARTIAL** | `driver.cpp:48-65` (always stretch crop to a 16:9 canvas, then letterbox) | `E/nvnflinger__window.h` NativeWindowScalingMode | No Freeze and no NoScaleCrop. |
| Transform FlipH / FlipV / Rot180 | **IMPLEMENTED_UNVERIFIED** | `layer.cpp:52-59` | `YZ/framebuffer_config.cpp:51-68` | Negative source size; the same approach as Eden. |
| Transform Rot90 / Rot270 | **MISSING** | `layer.cpp:60-63` (logged once) | Eden also unimplemented | |
| InverseDisplay, sticky transform, transform hint | **PARTIAL** | ignored; hint 0 (`hos_binder_driver.cpp:191,226`) | `E/...buffer_queue_producer.cpp:500-501` | Eden masks InverseDisplay. |
| Per-layer blending (None / Premultiplied 0x105 / Coverage 0x405) | **PARTIAL** | `driver.cpp:68-70` (`transparent = i != 0`) | `E/nvnflinger__hwc_layer.h:16-26`, `E/nvdrv__devices__nvdisp_disp0.cpp:24-35` | |
| Pixel formats RGBA8888 / RGBX8888 / BGRA8888 / RGB565 / RGBA4444 | **IMPLEMENTED_UNVERIFIED** | `renderer/const.cpp:178-196` | `E/nvnflinger__pixel_format.h` | |
| RGBA5551, RGB888, YUV / NV12 multi-plane | **PARTIAL** | `const.cpp:185,189` (marked "Doesn't work" / Invalid), planes 1-2 ignored | Eden has no YUV either | |
| Block-linear framebuffer decode via the texture cache | **IMPLEMENTED_UNVERIFIED** | `gpu/gpu.cpp:96-122` | `YZ/renderer_vulkan/present/layer.cpp` | Block-linear decode is tested in general (`texture_cache_tests.cpp`), but not through the present path. |
| Display blit pipeline | **VERIFIED** (pipeline only) | `renderer/metal/surface_compositor.cpp:27-81`; `tests/texture_cache_tests.cpp:862-880`; `tests/README-crash4-render.md:97-101` | — | Pipeline creation and cache key only; no geometry, crop or transform test. |
| Stale or freed nvmap at present | **PARTIAL** (bug) | `gpu.cpp:113` (`getMap(...).value()`) | Eden pins the handle (`E/nvnflinger__ui__graphic_buffer.cpp:22-36`) | Throws `std::bad_optional_access` (B12). |

### R. Frame pacing

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| 60 Hz vsync event | **PARTIAL** (bug) | `system.cpp:536-538,748`; `frontend/swiftui/MetalView.swift:29-31` | `E/vi__conductor.cpp:19,40,51` (fixed 1/60 s timer) | Follows the host display link (120 Hz ProMotion, 144 Hz monitors). Skipped when `nextDrawable` returns nil or the view is occluded (B14). |
| Swap interval 0/1/2 | **MISSING** | `binder.hpp:69` (parsed, never read) | `E/nvnflinger__hardware_composer.cpp:22-41,124-140`, `E/vi__conductor.cpp:124` | |
| Presentation timestamps (`timestamp`, `is_auto_timestamp`) | **MISSING** | — | `E/...buffer_queue_producer.cpp:490-491` | |

### S. Host presentation (Metal, frontends)

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| setSurface / acquireNextSurface (CAMetalLayer) | **IMPLEMENTED_UNVERIFIED** | `renderer/metal/renderer.cpp:108-129` (hot routes `src/core/hot/routes.inc:3,6`) | — | |
| Opaque layer (ignore guest alpha) | **IMPLEMENTED_UNVERIFIED** | `renderer.cpp:111-115` | — | Matches the console, which ignores alpha. |
| Aspect-preserving letterbox into the drawable | **IMPLEMENTED_UNVERIFIED** | `driver.cpp:48-65`, `surface_compositor.cpp:42-48` | — | |
| Drawable pixel format / colorspace | **PARTIAL** | `renderer.cpp:116` (`// TODO: set pixel format`) | — | BGRA8Unorm default, colorspace unset. Raw bytes are passed through, which is acceptable. |
| Frame driving (SwiftUI CADisplayLink, SDL loop) | **PARTIAL** | `MetalView.swift:23-32,45-72,95-99`; `frontend/sdl3/window.cpp:80-91` | — | No `preferredFrameRateRange`. `updateNSView` → `setView` invalidates and recreates the display link and re-sets the surface on every SwiftUI update; the window title FPS changes once per second (`ContentView.swift:32`). |
| Behaviour when no drawable or window hidden | **PARTIAL** | `system.cpp:536-538` | Eden composes on a CoreTiming timer | No vsync and no consumption, so the guest stalls in its event wait. |

### T. Surface lifecycle and resolution

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| Buffer reallocation (new SetPreallocatedBuffer on a mode change) | **PARTIAL** | `binder.cpp:7-14,100-111` | `E/...buffer_queue_producer.cpp:740-770,687-738` | Works when the slot count stays the same. Leftover slots stay live and can point to a freed or reused nvmap. |
| Docked/handheld resolution (am GetDefaultDisplayResolution, OperationMode) | **PARTIAL** | `am/common_state_getter.cpp:34-52`, `os.cpp:110-122,359-385` | Eden 1920x1080 docked / 1280x720 handheld; SB-DS:1081-1082 | Handheld is correct (and the default config). Docked "Auto" derives 1280x720 to 7680x4320 from the host window size and changes when the window is resized (`os.cpp:354-357`). |
| DefaultDisplayResolutionChangeEvent | **MISSING** | `am/common_state_getter.hpp:18,27`, `.cpp:54-58` (never signalled) | Eden signals it on operation mode change | |
| OperationModeChanged / PerformanceModeChanged messages | **IMPLEMENTED_UNVERIFIED** | `os.cpp:340-352` | | |
| Layer and binder destruction, pool limits | **PARTIAL** (bug) | `display/driver.hpp:34-63,86-90`; `destroyBinder` has no callers | Eden frees the buffer queue in DestroyLayerLocked | 16 binder creations, then `std::bad_expected_access` (B17). |

### U. am display-related

| Capability | Class | Hydra evidence | Reference | Notes |
|---|---|---|---|---|
| CreateManagedDisplayLayer (ISelfController 40) | **IMPLEMENTED_UNVERIFIED** | `am/self_controller.cpp:48-58` | Eden | |
| CreateManagedDisplaySeparableLayer (44) | **IMPLEMENTED_UNVERIFIED** | `self_controller.cpp:66-82` | Eden | The recording layer shares the binder and is hidden (good). |
| IsSystemBufferSharingEnabled (41) | **IMPLEMENTED_UNVERIFIED** | `self_controller.cpp:60-64` | Eden fails for applications too | |
| IDisplayController capture buffers (GetLastForegroundCaptureImage, Acquire/Release*CaptureBuffer, TakeScreenShotOfOwnLayer, ClearCaptureBuffer, ...) | **MISSING** | `am/display_controller.cpp:5` (empty table) | Eden `am/service/display_controller.cpp` | Library applets (swkbd, error, web) use these for background images. |

---

## (b) Confirmed bugs, with exact code

Severity: P0 = corrupts rendering or memory in shipped games now; P1 = wrong behaviour likely to affect games; P2 = protocol deviation or latent.

### B1 (P0) ALLOC_AS_EX fields decoded in the wrong order, breaking REMAP for NVN virtual memory pools

`src/core/horizon/services/nvdrv/ioctl/nvhost_as_gpu.hpp:68-71`
```cpp
    NvResult allocAsEx(kernel::Process* process, u32 big_page_size, i32 as_fd,
                       u32 flags, [[maybe_unused]] u32 reserved,
                       u64 va_range_start, u64 va_range_end,
                       u64 va_range_split);
```
`nvhost_as_gpu.cpp:153-154` and `:172-174`
```cpp
    if (big_page_size != 0)
        as_big_page_size = big_page_size;
...
        const gpu_vaddr_t gpu_addr =
            static_cast<u64>(entry.virt_offset_in_pages) * as_big_page_size;
        const u64 size = static_cast<u64>(entry.num_pages) * as_big_page_size;
```
Eden (`E/nvdrv__devices__nvhost_as_gpu.h:76-84`): `u32_le flags{}; // usually passes 1`, `s32_le as_fd{}`, `u32_le big_page_size{}`, ...
Game evidence: `build/hang-debug-20261016-172325/run.log:631` (Crash 4) and `build/swiftui-release/jump-force-touch.log:559` (Jump Force) both log
`allocAsEx (big page size: 1, as fd: 0, flags: 65536, ...)`. Offset 0 holds 1 and offset 8 holds 0x10000. So `as_big_page_size` becomes **1**.
Jump Force issues REMAP with `virtual offset (in pages): 0x10629, number of pages: 0x1` (`jump-force-touch.log:673`). The correct GPU VA is
0x1_0629_0000 and the size 64 KiB; the current code maps 1 byte at GPU VA 0x10629. (Switchbrew SB-NV:1510 lists big_page_size first, but the game data
contradicts it.) The REMAP code and the `as_big_page_size` assignment come from commit `0ba775bc` (2026-10-11); the parameter order is older.

### B2 (P0) GPU VA reservations are backed by uninitialized host memory

`src/core/hw/tegra_x1/gpu/gmmu.cpp:112-127` (reached from `allocatePrivateAddressSpace`, used by ALLOC_SPACE `nvhost_as_gpu.cpp:32-33` and ALLOC_AS_EX `:159-160`)
```cpp
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    const uptr ptr = reinterpret_cast<uptr>(malloc(range.getSize()));
```
The sparse flag is never consulted (`nvhost_as_gpu.cpp:24-35`). Eden maps sparse allocations so they read as zero (`E/...nvhost_as_gpu.cpp:186-188`)
and keeps non-sparse ones unmapped. In Hydra, any GPU read of a reserved range that has not been REMAPped returns stale heap bytes that differ
from run to run. With B1 that is every virtual-pool page. A non-zero ALLOC_AS_EX VA range would `malloc` up to 128 GiB.

### B3 (P0) GPU fences and syncpoints are fake while PFIFO runs asynchronously

`ioctl/nvhost_gpu.cpp:49-57`
```cpp
    system->getGpu().getPfifo().submitEntries(
        process->getGMmu(),
        std::span<const hw::tegra_x1::gpu::GpfifoEntry>(entries, num_entries),
        inout_flags_and_detailed_error);

    // HACK
    inout_flags_and_detailed_error = 0;
    inout_fence = hw::tegra_x1::gpu::Fence{};
```
`nvhost_gpu.cpp:104`: `*out_fence = {};` (ALLOC_GPFIFO_EX/EX2).
`ioctl/nvhost_ctrl.cpp:84` and `:96`
```cpp
    if (id == 0) { *out_value = 0; return NvResult::Success; } // Existing GPU path.
    if (id == 0) { out_value = 0; return NvResult::Success; } // Existing GPU path.
```
`gpu/gpu.cpp:43-93`: only method 0 is special. Puller methods 0x04-0x1F (semaphores, SyncpointPayload/Operation, WaitForIdle) go to `engine->method()`
and end up in `writeReg` (`engines/engine_base.hpp:16-20`). `pfifo.cpp:110-125` only queues the work; the GPU thread runs it later.
Effect: every guest CPU wait on a GPU fence (nvnSyncWait, queue finish, ring-buffer reuse, readback waits) returns before Hydra has executed the
work. The guest can overwrite push buffers, uniform/vertex rings and staging buffers that are still queued, or read results early. This matches
the run-to-run variation in `memory/crash4-3d-render-status.md` (AverageBrightness readback, "whiteness varies between runs").
Eden: `E/nvdrv__devices__nvhost_gpu.cpp:368-385`.

### B4 (P2) NvResult values off by one

`src/core/horizon/services/nvdrv/const.hpp:19-23`
```cpp
    BadValue = 11,
    AlreadyAllocated = 12,
    Busy = 13,
    ResourceError = 14,
    CountMismatch = 15,
```
References: AlreadyAllocated 0xD, Busy 0xE, ResourceError 0xF, CountMismatch 0x10 (SB-NV:2971-2981, `E/nvdrv__nvdata.h:38-41`). Unused today.

### B5 (P1) EVENT_WAIT and EVENT_WAIT_ASYNC return the wrong status and value

`nvhost_ctrl.cpp:101-118`
```cpp
    if (slot < EVENT_COUNT && events[slot].event != nullptr && timeout != 0) {
        ...
        return video::Syncpoints::reached(*value, tresh)
                   ? NvResult::Success : NvResult::BadValue;
    }
    out_value = *value;
    return video::Syncpoints::reached(*value, tresh) ? NvResult::Success : NvResult::Timeout;
```
Switchbrew SB-NV:473-475: a pending wait returns 0x05 Timeout with `value = slot | (syncpt_id << 4)`. Eden returns `Timeout` (`E/...nvhost_ctrl.cpp:197,210`).
`tests/shutdown_tests.cpp:665-666` asserts `== nv::NvResult::BadValue` ("Pending async video wait returned wrong status"), which encodes the deviation in the test.
EVENT_WAIT (0x1D, `nvhost_ctrl.cpp:79-89`) returns `Timeout` with `*out_value = current value` instead of an allocated event id (SB-NV:463-465).

### B6 (P2) nvhost-ctrl QueryEvent returns Success with a null event, and decodes the old format wrongly

`nvhost_ctrl.cpp:31-46`
```cpp
    } else { // Old format
        slot = extractBits(event_id_u32, 0, 8);
        syncpoint_id = extractBits(event_id_u32, 4, 28);
    }
...
    // TODO: check if syncpoint IDs are equal
    (void)syncpoint_id;

    out_event = event.event;
    return NvResult::Success;
```
For an unregistered slot `event.event == nullptr`; `Process::addHandle(nullptr)` then returns `INVALID_HANDLE` while the payload says Success. Eden returns
BadParameter (`E/nvdrv__nvdrv.cpp:282-286`). The old format packs `slot | id<<4` (SB-NV:96), so the slot occupies only bits 0-3.

### B7 (P2) Ioctl1 SUBMIT_GPFIFO2 (0x1B) reads entries from the wrong place

`channel_base.cpp:18-23`: `0x08, submitGpfifo, ... 0x1b, submitGpfifo` maps both commands to the inline-entries handler (`const GpfifoEntry* entries` read from `in_stream`).
0xC018481B has a fixed 24-byte input, so `num_entries` entries are read past the input buffer. `(void)gpfifo;` (`nvhost_gpu.cpp:47`) discards the real pointer.
Eden reads guest memory at `params.address` (`E/...nvhost_gpu.cpp:85-86,402-416`).

### B8 (P2) SET_TIMESLICE (0xC004481D) missing

The ioctl is absent from `channel_base.cpp:18-23`. Jump Force logs `Unknown ioctl nr 0x1d for type 0x48` (`build/swiftui-release/jump-force-binder.log`) and receives
`NotImplemented`. Eden validates the range and returns Success (`E/...nvhost_gpu.cpp:446-456`).

### B9 (P1) Binder slot indices unchecked (out-of-bounds read and write)

`display/binder.hpp:81`: `constexpr usize MAX_BINDER_BUFFER_COUNT = 8; // TODO: what should this be?`
`hos_binder_driver.cpp:152-161`
```cpp
        i32 slot = parcel_reader.read<i32>();
        if (slot > static_cast<i32>(display::MAX_BINDER_BUFFER_COUNT)) {
```
This lets slot == 8 and negative slots reach `binder.getBuffer(slot)`. SET_PREALLOCATED_BUFFER (`:267`, `binder.addBuffer(slot, *buffer);`) and QUEUE_BUFFER (`:180-184`)
have no check at all, so `buffers[slot]` is written out of bounds. Android/Eden use 64 slots with `slot < 0 || slot >= NUM_BUFFER_SLOTS` checks (`E/...buffer_queue_producer.cpp:50,744`).

### B10 (P1) The buffer is released to the producer before the host has drawn it, and there is no Dequeued state

`binder.cpp:88-95` (consume)
```cpp
        queued_buffers.pop();
        buffers[slot].queued = false;
    }

    queue_cv.notify_all();

    // Signal event
    event->signal();
```
followed later in `layer.cpp:24-30`
```cpp
    // Wait for the GPU to finish rendering the buffer
    system.getGpu().getPfifo().waitForCompletion(gpu_fence);
    const auto& buffer = binder.getBuffer(slot);

    // Texture
    present_texture = system.getGpu().getTexture(
```
`binder.cpp:23-33` returns the lowest `initialized && !queued` slot, so a slot that was just consumed, or one that is still dequeued (not tracked),
can be handed out again. The guest can then render frame N+1 into the buffer being presented. Eden holds the buffer as Acquired until
`release_frame_number` (`E/nvnflinger__hardware_composer.cpp:158-176,195-208`) and returns the oldest free slot (`E/...buffer_queue_producer.cpp:166-172`).

### B11 (P1) A blocking DequeueBuffer stalls every service on the shared server thread

`binder.cpp:18-19`
```cpp
    std::unique_lock<std::mutex> lock(queue_mutex);
    queue_cv.wait(lock, [&] { return queued_buffers.size() != buffer_count; });
```
This runs inside `IHOSBinderDriver`, registered on the "others" server (`horizon/os.cpp:232`). That server is one `HostThread` running
`Server::mainLoop` (`services/server.cpp:8-20,57-140`) and also hosts hid, am, audout, audren, hwopus, time, fs, socket, nifm and more.
If `buffer_count == 0` (dequeue before any SetPreallocatedBuffer) the wait never ends. libnx-style async dequeue (wait on the event, then dequeue with async=1
and expect `WouldBlock`) blocks here instead. NVN normally waits on the buffer event first (observed), so the hazard is intermittent rather than per-frame.

### B12 (P1) Disconnect keeps stale slots, nvmap ids are not pinned, and the presenter throws on a freed nvmap

`binder.cpp:113-124` (Disconnect only clears the queue), `gpu.cpp:112-114`
```cpp
    const renderer::TextureDescriptor descriptor(
        mmu->unmapAddr(getMap(static_cast<u32>(buff.nvmap_id)).value()->addr +
                       plane.offset),
```
After a window is recreated with fewer buffers, or a framebuffer nvmap is freed, a live slot keeps its old `nvmap_id`. Ids are reused LIFO
(`gpu.hpp:52-62`), so the slot either shows unrelated memory or `std::optional::value()` throws `std::bad_optional_access` on the main thread.
Eden frees all slots on Disconnect (`E/...buffer_queue_producer.cpp:711-714`) and pins handles for the lifetime of a GraphicBuffer (`E/nvnflinger__ui__graphic_buffer.cpp:22-36`).

### B13 (P2) num_pending_buffers hard-coded to 8

`hos_binder_driver.cpp:188-193` (QueueBuffer) and `:223-228` (Connect)
```cpp
            .transform_hint = 0,                                     // HACK
            .num_pending_buffers = display::MAX_BINDER_BUFFER_COUNT, // HACK
```
Eden reports `core->queue.size()` (`E/...buffer_queue_producer.cpp:539,670`). A producer that throttles on pending buffers sees a permanently full queue.

### B14 (P1) Vsync follows the host display link, swap interval is ignored, and vsync stops with no drawable

`system.cpp:536-538` and `:748`
```cpp
    auto compositor = gpu.getRenderer().acquireNextSurface();
    if (compositor == nullptr)
        return;
...
    os.getDisplayDriver().signalVSync();
```
`frontend/swiftui/MetalView.swift:29-31` creates `view.displayLink(target:selector:)` with no `preferredFrameRateRange`, so it runs at the display maximum (120 Hz ProMotion).
`display/binder.hpp:69` `u32 swap_interval; // TODO: float?` is never read. A 60 Hz game on a 120 Hz Mac gets a buffer back and a vsync every 8.3 ms. Frame-locked game
logic (swap interval 1 or 2) can run up to 2x or 4x too fast, and pacing that relies on vsync breaks. When the drawable is unavailable (occluded window,
1 s `nextDrawable` timeout), the guest gets no vsync and no buffer release. Eden: fixed 60 Hz conductor (`E/vi__conductor.cpp:19,40,124`) and swap-interval hold (`E/nvnflinger__hardware_composer.cpp:200-208`).
`MetalView.swift:95-99` also calls `context.coordinator.setView(view)` on every `updateNSView`, which invalidates and recreates the display link and re-sets
the surface. The title FPS update (`ContentView.swift:32`) triggers this about once per second; this probably causes a periodic missed callback (not measured).

### B15 (P2) The presenter gives up on the GPU after 250 ms

`gpu/pfifo.cpp:135-138`
```cpp
    // Time out to avoid hanging the presentation if the GPU gets stuck
    completed_cond_var.wait_for(lock, std::chrono::milliseconds(250), [&] {
        return stop || completed_count >= count;
    });
```
During long shader or pipeline compilation stalls, a partly rendered frame is presented. Combined with B10 the slot is already back with the producer.

### B16 (P1) Data race on `Gpu::memory_maps`

`gpu/gpu.hpp:50-102,204` define a `std::map<u32, MemoryMap> memory_maps;` with no mutex. It is mutated by nvmap CREATE/ALLOC/FREE on the nvservices server thread
(`ioctl/nvmap.cpp:15,36,48`) and read by `Gpu::getTexture` on the main thread during presentation (`gpu.cpp:113`, called from `layer.cpp:29`). Concurrent
inserts and erases can corrupt the tree.

### B17 (P1) CloseLayer destroys the layer, and binders are never freed (16-binder cap, then an exception)

`application_display_service.cpp:154-157`
```cpp
result_t IApplicationDisplayService::closeLayer(System* system, u64 layer_id) {
    system->getOs().getDisplayDriver().destroyLayer(static_cast<u32>(layer_id));
```
`display/driver.hpp:54-57`: `return binder_pool.insert().value();` with `StaticHandlePool<Binder, 16> binder_pool;` (`:90`). `destroyBinder` (`:59-63`) has no callers.
In Eden, CloseLayer only removes the layer from the display stack and allows OpenLayer again (`E/vi__container.cpp:240-273`). In Hydra a re-open after CloseLayer
hits `LOG_FATAL("Failed to get layer")`. The 17th layer creation throws `std::bad_expected_access`.

### B18 (P2) ConvertScalingMode writes no output; SetLayerScalingMode and SetLayerVisibility are no-ops; SetLayerPosition is ignored

`application_display_service.cpp:176-179`
```cpp
result_t IApplicationDisplayService::convertScalingMode() {
    LOG_FUNC_NOT_IMPLEMENTED(Services);
    return RESULT_SUCCESS;
}
```
`display_service_base.cpp:34-39` logs and returns success without calling `Layer::setVisible`. `layer.cpp:68-80` never reads `position`.
Eden: `E/vi__application_display_service.cpp:145-160,268-292`.

### B19 (P2) vi:m command IDs wrong

`visrv/manager_root_service.cpp:8-9`
```cpp
DEFINE_SERVICE_COMMAND_TABLE(IManagerRootService, 0, getDisplayService, 2,
                             getDisplayServiceWithProxyNameExchange)
```
Switchbrew: vi:m 2 = GetDisplayService, 3 = GetDisplayServiceWithProxyNameExchange (SB-DS:370-394).

### B20 (P2) Resolution reporting: deviating values, and a change event that is never signalled

`application_display_service.cpp:80-87` (ListDisplays) and `:123-125` (GetDisplayResolution) use `getOs().getDisplayResolution()`.
SB-DS:1016 states the Default display's DisplayInfo is 1080p even in handheld mode. Eden's GetDisplayResolution is a fixed 1280x720
(`E/vi__application_display_service.cpp:134-143`). `os.cpp:354-357`
```cpp
void OS::setSurfaceResolution(uint2 resolution) {
    // TODO: signal resolution change event if changed
    surface_resolution = resolution;
```
In docked "Auto" mode (`os.cpp:363-365`) the guest-visible resolution follows the host window (up to 7680x4320). `default_display_resolution_change_event`
(`am/common_state_getter.hpp:18,27`) is never signalled, not even by `OS::notifyOperationModeChanged` (`os.cpp:340-352`).

### B21 (P2) GET_CONFIG answers like a development unit

`nvhost_ctrl.cpp:55-75` returns Success and copies set:sys values (for example `nv!nvn_no_vsync_capability`) with no NUL terminator. Retail returns an error
(SB-NV:447-448); Eden always returns `ConfigVarNotFound` (`E/...nvhost_ctrl.cpp:86-90`).

### B22 (P2) Device errors reported as CMIF errors

`nvdrv_services.cpp:67-70` (unknown path: `*out_error = MAKE_RESULT(Svc, 0); return MAKE_RESULT(Svc, 0);`), `:89-92` (close), `:112-113,125-127` (queryEvent), `:162-163` (ioctl, bad fd).
Eden always returns IPC success and puts the NvResult in the payload (`E/nvdrv__nvdrv_interface.cpp:22-50,184-210`).

### B23 (P2) GET_VA_REGIONS returns empty regions; the Ioctl3 variant drops its output

`nvhost_as_gpu.cpp:134-138`
```cpp
    inout_buffer_size = 2 * sizeof(VaRegion);
    // HACK
    (*out_va_regions)[0] =
        VaRegion{.addr = 0x0, .page_size = hw::tegra_x1::gpu::GPU_PAGE_SIZE};
```
`nvhost_as_gpu.hpp:84-87` computes the regions into a local array and discards it, so the Ioctl3 inline output is never written. Eden: `E/...nvhost_as_gpu.cpp:422-440`.

### B24 (P2) nvmap ALLOC, FROM_ID and PARAM deviations

`nvmap.cpp:36-38`
```cpp
    if (!system->getGpu().allocateMap(handle, addr, flags == 1))
        return NvResult::BadParameter;
    inout_alignment = hw::tegra_x1::gpu::GPU_PAGE_SIZE; // TODO: correct?
```
`nvmap.cpp:22-26`: FROM_ID returns the id without validation. PARAM returns a constant alignment, kind 0, and `NotSupported` for Base. FREE has no refcount.
Eden: `E/nvdrv__devices__nvmap.cpp` IocAlloc / IocFromId / IocParam / IocFree.

### B25 (P2) Parcel writer leaves padding unwritten

`hosbinder/parcel.hpp:135-161`
```cpp
        auto span = stream.writeReturningSpan<T>(count);
        // Align
        usize size = count * sizeof(T);
        stream.seekBy(align(size, static_cast<usize>(4)) - size);
...
    void writeStrongPointer(const T* ptr) {
        write(ptr != nullptr);
```
The 1-byte bool leaves 3 stale bytes in the guest reply buffer. Eden writes a `u32` and zero-pads every value (`E/nvnflinger__parcel.h:133-142,175-181`). The guest only
tests for non-zero today, so this is not observable yet.

### B26 (P2) GET_ERROR_NOTIFICATION status

`nvhost_gpu.cpp:90`: `*out_status = /*0xffff*/ 0x0; // TODO: correct?`. SB-NV:2769-2777 documents `status` as always 0xFFFF.

---

## (c) Prioritized implementation backlog

Each item lists what to change, why it matters for real games, how to do it in Hydra, how to test it, and the regression risk.

### P0-1: Fix the ALLOC_AS_EX layout (B1)
* **What/why:** Every NVN title that uses virtual memory pools (sparse textures and buffers; Jump Force confirmed, UE4 generally) remaps to the wrong VA. Fixing this is a small change and a possible root cause of garbage or black textures.
* **How:** In `nvhost_as_gpu.hpp:68-71` and `.cpp:142-162`, reorder the parameters to `(u32 flags, i32 as_fd, u32 big_page_size, u32 reserved, u64 va_start, u64 va_end, u64 va_split)`.
  Accept only `big_page_size` values that are a power of two and in 0x30000, otherwise keep 0x10000/0x20000 as Eden does (`E/...nvhost_as_gpu.cpp:96-120`).
  Stop calling `allocatePrivateAddressSpace(va_end - va_start, ...)`; record the VA window instead (it is also needed for P2 GET_VA_REGIONS). Add a
  `static_assert` on a packed 40-byte struct.
* **Test:** Unit test (new `tests/nvdrv_tests.cpp`, same style as `shutdown_tests.cpp::testVideoEvents`): call `NvHostAsGpu::ioctl(0x41, 0x09)` with `{1,0,0x10000,0,0,0,0}`,
  then REMAP `{flags 4, kind 0xfe, handle h, mem_off 0, virt 0x10629, pages 1}` and check `GMmu::unmapAddr(0x106290000)` equals the nvmap address. In game, check that
  Jump Force and Crash 4 textures stay stable across runs.
* **Risk:** Low. Only REMAP uses `as_big_page_size`.

### P0-2: Correct sparse and reserved VA semantics in the GMMU (B2)
* **What/why:** Reads of reserved-but-unmapped GPU memory must be deterministic (zero for sparse). Today they read uninitialized heap, which gives run-to-run garbage.
* **How:** In `gmmu.cpp:112-127`, do not `malloc` for reservations. Add an `AddressSpace` flag `sparse` and map one shared zero page (or `calloc`/anonymous `mmap`) for sparse ALLOC_SPACE
  (`AllocSpaceFlags::Sparse`, `nvhost_as_gpu.hpp:13`). Leave non-sparse reservations unmapped (null `ptr`, so accesses read zero and log). Implement
  FREE_SPACE (remove the reservation), UNMAP_BUFFER (restore sparse or unmapped instead of keeping the mapping), and REMAP unbind without leaking `calloc`.
  Honour `align` and `page_size` in ALLOC_SPACE. Writes to sparse zero pages must be discarded (read-only shared page).
* **Test:** GMMU unit tests for allocate sparse → read zero → REMAP → read data → unbind → read zero → FREE_SPACE → unmapped. Check that `readMappedBytes` stops correctly.
* **Risk:** Medium. Games that accidentally rendered into reserved-but-unbacked VA will now lose those writes (correct behaviour); add a one-time log.

### P0-3: Real GPU syncpoints and fences (B3)
* **What/why:** This makes guest CPU and GPU synchronization real: ring-buffer reuse, readbacks (UE4 AverageBrightness, occlusion), present fences and NVN queue
  finish. It is probably behind several nondeterministic rendering bugs.
* **How:**
  1. Generalize `video::Syncpoints` (`src/core/video/syncpoints.hpp`) into a Host1x syncpoint manager with `min` (completed) and `max` (scheduled) per id, 192 ids,
     ids 26/27 reserved, owned by `Gpu`. Video keeps using it unchanged.
  2. In `NvHostGpu` allocate a channel syncpoint in the constructor. In `submitGpfifo` implement Eden's `SubmitGPFIFOImpl` (`E/...nvhost_gpu.cpp:341-386`):
     handle FenceWait (enqueue a wait on the PFIFO list), compute increments (`FenceGet ? 2 : 0` plus `increment_value ? fence.value : 0`, flag bit 8),
     return `{channel_syncpoint, max}`, and append an increment (optionally with WFI) to the entry list. Return `GetSyncpointFence` from ALLOC_GPFIFO_EX/EX2.
  3. In `Gpu::subchannelMethod` (`gpu.cpp:43`) route methods below 0x40 to a new puller handler: SyncpointPayload 0x1C, SyncpointOperation 0x1D (increment or wait),
     SemaphoreAddressHigh/Low/Sequence/Trigger 0x04-0x07 (acquire, release, acquire-GEQ), WaitForIdle 0x1E, Nop, RefCnt. Increment `min` when the PFIFO thread
     reaches the increment, after the current Metal command buffer is committed. For strict CPU readback correctness, defer to that command buffer's
     `addCompletedHandler`.
  4. In nvhost-ctrl, handle GPU ids through the same manager in `syncptRead/WaitEvent/WaitEventEx`. Add CLEAR_EVENT_WAIT (0x1C), FREE_EVENT_BATCH (0x21),
     SYNCPT_INCR/WAIT/WAITEX/READ_MAX. Delete the `id == 0` shortcuts.
  5. Keep an escape hatch (`HYDRA_FAKE_GPU_FENCES=1`) and a watchdog that force-signals and logs any GPU fence wait longer than, for example, 2 s.
* **Test:** Unit test: submit a GPFIFO list containing a 0x1D increment while the PFIFO thread is paused, check that EVENT_WAIT returns Timeout, then resume and check that
  it returns Success and the event is signalled. In game, run Crash 4 boot and gameplay for N runs comparing render hashes (`render_hash_tests` infrastructure) and confirm the
  2026-10-14 AverageBrightness readback is stable without the DMA flush workaround.
* **Risk:** High. A missed increment turns into a guest hang instead of garbage. Mitigate with the toggle and watchdog and land it behind a flag first.

### P0-4: Buffer queue state machine (B9, B10, B12, B13)
* **What/why:** Prevents memory corruption from bad slots, double-dequeue, the producer overwriting the frame being presented, and stale or freed buffers after a window rebuild.
* **How:** In `display/binder.{hpp,cpp}`, replace `Buffer{initialized, queued}` with Eden-style `BufferSlot{state Free|Dequeued|Queued|Acquired, frame_number, fence, request_buffer_called, is_preallocated}`
  and 64 slots. Validate every slot index (`hos_binder_driver.cpp` RequestBuffer, QueueBuffer, CancelBuffer, SetPreallocatedBuffer) and return BadValue.
  Choose the oldest Free slot on dequeue. In `consumeBuffer` move the slot to Acquired; release it to Free only after the host present command buffer has been
  committed and after `swap_interval` vsyncs (P1-5). Implement CancelBuffer, SetBufferCount, Disconnect → free all slots, Connect parsing (api,
  producerControlledByApp → `dequeue_buffer_cannot_block`) and `num_pending_buffers = queue.size()`. Implement the Query fields from `E/...buffer_queue_producer.cpp:582-631`
  (MinUndequeued = 1, ConsumerRunningBehind = `queue.size() > 1`, and so on), and default width/height/format from the preallocated buffer.
  Pin nvmaps for a slot's lifetime (refcount in `Gpu::MemoryMap`) and make `Gpu::getTexture` return null instead of calling `.value()`.
* **Test:** New `tests/display_tests.cpp` that builds parcels and calls `IHOSBinderDriver::transactParcelImpl` (or drives `display::Binder` directly). Cover:
  out-of-range slots return BadValue with no crash; dequeue→queue→consume keeps the slot unavailable until release; two dequeues return two different slots;
  Disconnect followed by RequestBuffer on an old slot returns null; a freed nvmap does not crash the presenter.
* **Risk:** Medium. NVN behaviour depends on the dequeue and event interplay, so validate on Crash 4 and Jump Force menus and gameplay.

### P1-5: Fixed 60 Hz vsync and composition, decoupled from the host display (B14, B15)
* **What/why:** Correct game speed and frame pacing on 120 Hz or 144 Hz Macs. Swap interval 2 (30 fps) titles run correctly. The guest keeps running while the window
  is hidden. Partly rendered frames are no longer shown.
* **How:** Add a vsync thread in `display::Driver` with a steady 16.666 ms period. On each tick it acquires and composes per layer, honouring `swap_interval`
  (hold the Acquired buffer for N ticks; drop droppable/async frames), signals the vsync events, and publishes the latest composed texture. `System::progressFrame`
  only blits the latest composed frame and never gates vsync on `nextDrawable`. Replace the 250 ms `waitForCompletion` timeout with a fence wait (after P0-3),
  or log the timeout and present the previous frame. In `MetalView.swift`, set `displayLink.preferredFrameRateRange` and only create the display link
  in `makeNSView`, not in every `updateNSView`. Give each vi session its own vsync event (Eden `E/vi__application_display_service.cpp:253-266`).
* **Test:** Unit test with an injectable clock: vsync count over 1 s is 60 ± 1 regardless of how often `progressFrame` is called. A buffer queued with swap interval 2 is released
  after exactly 2 ticks. In game, measure guest frame time on a ProMotion display with `HYDRA_PERF_STATS=1`.
* **Risk:** Medium. Games that run uncapped today will be capped to their requested rate, which is correct but visible to users. Threading between the vsync thread
  and the Metal present needs care.

### P1-6: Move blocking dispdrv work off the shared "others" server thread (B11)
* **What/why:** A blocked dequeue must not stall hid, audio, fs and am. It removes a possible cause of audio stutter and hangs when the producer runs ahead.
* **How:** Register `IHOSBinderDriver` (and vi) on a dedicated `services::Server` instance in `horizon/os.cpp:232` (the same mechanism as `nvservices_server`), or support deferred
  replies in `Server::mainLoop`. Implement `WouldBlock` for async dequeue and `dequeue_buffer_cannot_block`. Never wait when `buffer_count == 0`; return `BUFFER_NEEDS_REALLOCATION`/BadValue.
* **Test:** Unit test with two client sessions where one dequeues with all buffers queued and the other's hid or time request still completes. Run a libnx-style async dequeue and expect `WouldBlock`.
* **Risk:** Medium. It adds a thread, so check for lock-order issues between `display::Driver` mutexes and the kernel.

### P1-7: Compositor feature completeness (Q rows, B18)
* **What/why:** Correct output for games or applets that use layer position, size, visibility, scaling modes, Rot90/270 or premultiplied overlays (software keyboard and error applets).
* **How:** In `Layer::present` (`layer.cpp:68-80`), map position and size from the 1280x720 vi space to the output. Implement scaling modes (Freeze keeps the last frame,
  ScaleToWindow, ScaleCrop, NoScaleCrop, PreserveAspect). Apply SetLayerVisibility (`display_service_base.cpp:34-39`), SetLayerScalingMode and ConvertScalingMode output,
  per-layer blending (None/Premultiplied/Coverage, matching `E/nvnflinger__hwc_layer.h:16-26`), and Rot90/Rot270 through blit UV rotation. Wait on the QueueBuffer NvMultiFence once P0-3 exists.
* **Test:** Compositor unit tests with the null renderer that record `drawTexture` src and dst rectangles for each transform, crop and scaling mode, plus one Metal readback test for Rot90.
* **Risk:** Low to medium.

### P1-8: Thread safety and lifetime of nvmap objects (B16, B24)
* **What/why:** Removes a heap-corruption race between presentation and nvmap ioctls.
* **How:** Add a `std::shared_mutex` around `Gpu::memory_maps`, or move the table into an `NvMapCore` class with refcounts (CREATE=1, FROM_ID +1, binder pin +1, FREE −1).
  Return copies instead of `MemoryMap*` across threads. Implement Eden's ALLOC/PARAM/FROM_ID checks.
* **Test:** Stress test that runs CREATE/FREE on one thread while another calls `getTexture`, under TSan if available. Add ioctl-level unit tests.
* **Risk:** Low.

### P2-9: nvdrv ioctl conformance sweep (B4-B8, B21-B23, B26)
* **What/why:** Robustness for titles that use less common ioctls, and correct error semantics.
* **How:**
  * Correct `NvResult` values (`nvdrv/const.hpp`).
  * EVENT_WAIT/EX: return Timeout with Eden's value encoding, allocate the event in 0x1D, and update `shutdown_tests.cpp:665-676` to expect Timeout.
  * QueryEvent: return BadParameter for a null event, decode the old-format slot from 4 bits, and check the syncpoint id.
  * Add SET_TIMESLICE (0x48/0x1D). Make Ioctl1 0x1B read entries from `gpfifo`.
  * GET_CONFIG always returns `ConfigVarNotFound`.
  * Return NvResult in the payload instead of CMIF errors (`FileOperationFailed` for unknown devices).
  * GET_VA_REGIONS reports the real windows and the Ioctl3 variant writes its inline output.
  * GetCharacteristics3 also fills the fixed output. Add FLUSH_L2, ZBC_QUERY_TABLE and NUM_VSMS/VSMS_MAPPING.
  * Honour the ioctl direction bits (do not write the output for In-only ioctls). Check `num_entries` against the input length.
  * Set GET_ERROR_NOTIFICATION status to 0xFFFF.
* **Test:** One table-driven unit test per ioctl with packed struct `static_assert`s and the byte-exact expected output from Eden and switchbrew.
* **Risk:** Low. The test change in `shutdown_tests.cpp` is intentional.

### P2-10: vi and surface lifecycle conformance (B17, B19, B20)
* **What/why:** Prevents crashes when games close and reopen layers or recreate them repeatedly, and gives correct resolution reporting and mode-change notification.
* **How:**
  * CloseLayer only closes: keep the layer, mark it closed, and stop compositing it. DestroyManagedLayer/DestroyStrayLayer also call `destroyBinder`.
    Grow the pools or return errors instead of `.value()` (`driver.hpp:34-63`).
  * Fix the vi:m table to `2, getDisplayService, 3, getDisplayServiceWithProxyNameExchange`.
  * OpenDisplay validates names. Add OpenDefaultDisplay and SetDisplayEnabled.
  * ListDisplays reports 1920x1080. GetDisplayResolution returns 1280x720 as Eden does (decide deliberately).
  * Signal `default_display_resolution_change_event` from `OS::notifyOperationModeChanged` and whenever the docked "Auto" resolution changes; or clamp docked Auto to 1080p by default.
  * Add GetIndirectLayerImageRequiredMemoryInfo and stub GetIndirectLayerImageMap.
* **Test:** Unit tests for CreateManagedLayer → OpenLayer → CloseLayer → OpenLayer (works) and 32 create/destroy cycles (no throw). Check that the resolution event fires on a mode toggle.
* **Risk:** Low. Changing GetDisplayResolution values can change how a game chooses its render size, so verify on Crash 4 and Jump Force in both modes.

### P3-11: Applet- and system-facing display features
* **What/why:** Library applet backgrounds and screenshots.
* **How:** am `IDisplayController` capture buffers (copy the last composed frame into the shared buffer), ISystemDisplayService/IManagerDisplayService shared-layer and indirect-layer
  commands as stubs that report success, layer stacks (AddToLayerStack), and the parcel writer zero padding (B25).
* **Test:** Run a library applet such as swkbd and check its background. Byte-compare parcel replies with Eden's serializer.
* **Risk:** Low.

## Appendix: counts

Rows per class in table (a), counted from the Class column: VERIFIED 7, IMPLEMENTED_UNVERIFIED 58, PARTIAL 78, MISSING 52, UNKNOWN 0
(195 rows in total; several rows group related ioctls or commands, so per-command counts would be higher, mostly in MISSING).

VERIFIED rows: nvhost-ctrl EVENT_REGISTER and EVENT_UNREGISTER, Host1x CHANNEL_SUBMIT, video GET_SYNCPOINT, MAP/UNMAP_CMD_BUFFER, video syncpoints
(all through `tests/shutdown_tests.cpp` and `tests/README-video.md`), and creation of the display blit pipeline (`tests/texture_cache_tests.cpp:862-880`). No test
covers the binder, vi, the compositor geometry, frame pacing or GPU fences.
