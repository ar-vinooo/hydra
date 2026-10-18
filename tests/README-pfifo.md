# PFIFO instruction stage — current status

The instruction stage remains **open**. Passing the tests below establishes
specific behavior; it does not establish complete Maxwell/NVN compatibility.
No game or GUI is launched by these tests.

## Implemented and tested

- GPFIFO submission validates the header, output and complete entry array.
  Switch ioctl1 reads entries appended to the IPC header and ignores
  `args.gpfifo`; ioctl2 reads its separate inline input. Nonzero/invalid ignored
  pointers, truncated arrays, unaligned buffers and empty submissions are covered.
- AS_GPU MapBufferEx validates the complete 40-byte input/output and copies
  unaligned IPC through aligned scratch storage. Mapping ranges must stay in
  their nvmap allocation and have readable CPU backing on every page. Remap
  validates all backing ranges before applying the batch; descriptors use the
  remaining input at the current stream cursor and cannot be partially present.
- Each AS_GPU FD owns its own GMMU. BindChannel resolves the target FD in the
  nvdrv session and accepts GPU channels only. A channel binds once; several
  channels may share an AS. Actual channel submits use the bound AS and reject
  unbound or foreign-process callers. Queued and host-completing work retains
  its AS after FD closure; process cleanup cancels/drains all live owned spaces.
- PFIFO consumes one word within the entry's declared length. Split host
  mappings are checked instead of dereferenced across allocations. Partial
  packets and macro arguments persist across entries and submissions.
- Each `NvHostGpu` owns its decoder, five engine instances, subchannel bindings,
  macro interpreter state and puller registers. Resources in the renderer's
  memory caches remain shared; compute accesses its channel's 3D helpers.
- Cooperative scheduling retains the word offset when an acquire or command
  syncpoint wait blocks. Another channel can run to release it; later jobs in
  the blocked channel cannot overtake it. Retirement only advances through a
  contiguous prefix, even when other channels finish earlier.
- Fence counters are allocated per channel. Another channel's completion
  cannot increment the blocked channel's fence. Allocation returns that
  channel's initial counter without incrementing it. Exhaustion fails rather
  than returning a successful zero fence.
- WFI and WFI-enabled releases wait for earlier Metal work. Four-byte releases
  leave timestamp bytes untouched; WFI-disabled releases do not force a wait.
  Acquire submits earlier work before polling/yielding. Time elapsed on the
  host does not satisfy either semaphore or fence waits.
- Tegra syncpoint A/B uses payload method 0x1c, operation bits 1:0 and index
  bits 15:8. Wait and increment operate on allocated counters. Waits retain
  their threshold while another channel runs.
- An unmapped command/semaphore faults its channel instead of dispatching
  partial data or invoking the success callback. The error event is signaled;
  `GET_ERROR_NOTIFICATION` returns a timestamp, error code and status. MMU
  failures use code 31; illegal/unsupported command and generic host execution
  failures use code 25. Faulted channels reject new submissions.
- Metal completion callbacks publish success only for completed work on a
  non-faulted channel. Error-event lifetime survives FD closure while queued
  work holds the channel state.
- Process cleanup cancels queued work, waits for an active decoder to stop
  referencing its GMMU and drains previously committed work on the Metal
  queue before freeing guest allocations. New work on a cancelled GMMU or a
  stopped PFIFO is rejected.

## Evidence

- [NVIDIA Maxwell clb06f.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/host/clb06f.h)
  defines GPFIFO lengths, conditional/level fields, modern/legacy headers,
  SEMAPHORED operation and size/WFI bits, and WFI.
- [NVIDIA dev_pbdma.ref, PB_COUNT](https://github.com/NVIDIA/open-gpu-doc/blob/master/manuals/turing/tu104/dev_pbdma.ref.txt)
  documents pending counts and words from separate segments. This later-family
  reference is not a GM20B hardware trace.
- [NVIDIA Tegra channel_sync_gk20a.c](https://android.googlesource.com/kernel/tegra/+/android-tegra-3.10/drivers/gpu/nvgpu/gk20a/channel_sync_gk20a.c)
  emits syncpoint A/B wait/increment commands; the adjacent
  [hw_pbdma_gk20a.h](https://android.googlesource.com/kernel/tegra/+/android-tegra-3.10/drivers/gpu/nvgpu/gk20a/hw_pbdma_gk20a.h)
  defines the operation and index fields.
- [NVIDIA channel_gk20a.c](https://android.googlesource.com/kernel/tegra/+/android-tegra-3.10/drivers/gpu/nvgpu/gk20a/channel_gk20a.c)
  describes the Linux driver copying ioctl1 entries from the userspace pointer;
  this is not the Switch IPC contract. It also publishes an error notification
  with timestamp, info32 and status 0xffff. The
  [nvhost.h notification definition](https://android.googlesource.com/kernel/tegra/+/android-tegra-3.10/include/linux/nvhost.h)
  defines the 16-byte layout, MMU error 31 and illegal notification 25.
- [libnx nvchannel.c](https://github.com/switchbrew/libnx/blob/master/nx/source/nvidia/ioctl/nvchannel.c)
  provides the Switch caller contract: ioctl1 appends entries and ignores the
  pointer field; ioctl2 passes entries separately. Input parsing follows this
  contract instead of the Linux userspace-pointer ABI.
- [Apple waitUntilCompleted](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilcompleted())
  specifies waiting for GPU execution and completion handlers.

  Mapping arbitrary Metal failures to 25 is Hydra's conservative fallback,
  not a verified NVIDIA error classification.

## Validation

On 2026-10-18, the app build, all nine checks below and the full repeated
System teardown suite passed with Metal validation enabled. At that checkpoint,
no gameplay validation or deliberately faulting Metal shader was run. Later
user gameplay findings and subsequent fixes are recorded below.

```sh
ninja -C build/swiftui-release -j 6 hydra-shutdown-tests hydra-gpu-engine-tests hydra \
  hydra-texture-cache-tests hydra-raster-state-tests

MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-gpfifo-input
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-gpu-address-space
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-gpu-as-binding
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-gpu-as-lifecycle
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-gpu-heap-resize
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-video-events
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-pfifo-decode
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-channel-scheduling
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-channel-faults
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-gpu-process-cancel
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-semaphore-shutdown
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-gpu-engine-tests
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-texture-cache-tests
MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-raster-state-tests
```

The cancellation test holds real Metal work behind a shared event and checks
that process destruction waits until the event is signaled. Scheduling tests
include independent completion fences, a blocked channel with a later job,
semaphore release from another channel, puller state restoration and command
syncpoint dependencies. Fault tests verify the error event and notification,
no successful completion fence, invalid output sizes, unsupported-format
rejection and counter exhaustion.

## Gates still open before calling the stage complete

1. GPFIFO conditional fetch and subdevice-mask instructions still fault
   explicitly. Both MAIN and SUBROUTINE levels now execute; USERD
   TOP_LEVEL_GET progress reporting remains unimplemented.
2. Additional submit flags and full ring/allocation semantics. SyncFence,
   skip-refcounting and unknown flags are rejected; native HwFormat is accepted.
   SuppressWfi uses
   stronger completion ordering. Bit 8 now reserves the supplied guest
   increment count; wider NVN flag semantics and counter release/reuse still
   need further work. AS_GPU binding/isolation now has directed tests, while
   reservation/free/unmap semantics and concurrent backing pinning remain partial.
3. Switch notifier enable/disable and retained-event lifetime are implemented;
   registration does not map guest storage. Recovery/reset after a channel
   fault is not implemented; a new channel is required. See the ABI correction
   below; the earlier Linux-derived mapping approach was removed.
4. Semaphore reductions and words split across host mappings remain unsupported.
   More precise host-to-guest error classification needs evidence.
5. Concurrent unmap/remap during live execution and complete backing-allocation
   pinning still need review. Process destruction is covered; that is not the
   same guarantee as every mutation of memory while a process is running.
6. Gameplay/hardware-trace validation and the instruction-stage exit audit.
   Synchronous checkpoints can add stalls; performance has not been measured.

Display presentation and renderer-feature completeness are separate later
stages; their existing limitations are not marked fixed by this work.

## Startup regression found in gameplay — 2026-10-18

The user's selected title `0100f9f00c696000` remained at 0 FPS. The captured
log showed `SET_ERROR_NOTIFIER` (ioctl `0xc018480c`) returning device result 2,
followed immediately by an SDK data abort during graphics initialization,
before the first frame. The prior decision to reject this registration was
therefore a startup compatibility regression. The original log was preserved
as `/tmp/hydra-startup-before-notifier.log` (temporary evidence, not a fixture).

The first follow-up implemented a Linux-style owned Mach VM alias and its
lifetime tests. That was a mistaken application of Linux registration semantics
to Switch. It has been removed and superseded by the Switch enable/disable ABI
described below. The startup rejection was real, but the former mapping tests
were not proof that Switch requires mapped notifier storage.

## Native GPFIFO format rejection — 2026-10-18

After notifier registration was fixed, the user reported menu audio, working
input and a 60 FPS counter with a blank viewport. The live log repeatedly
reported device result 2 for submit ioctl `0xc018481b`. A read-only LLDB
inspection of the running submit call captured `flags = 0x4`, five entries,
and the ioctl2 caller. The process was detached/resumed after inspection.
Evidence was preserved in `/tmp/hydra-live-submit-debug.txt` and
`/tmp/hydra-before-hwformat.log`; these are temporary local files.

The whitelist had rejected `HwFormat` despite the decoder already consuming
native two-word GPFIFO entries. NVIDIA's
[nvhost_ioctl.h](https://android.googlesource.com/kernel/tegra/+/android-tegra-3.10/include/linux/nvhost_ioctl.h)
defines HW_FORMAT as bit 2. The submit loop in
[channel_gk20a.c](https://android.googlesource.com/kernel/tegra/+/android-tegra-3.10/drivers/gpu/nvgpu/gk20a/channel_gk20a.c)
copies entry0/entry1 directly to the hardware ring. Hydra now accepts this
native format through its existing validated entry path. Other unsupported
flags remain rejected and their numeric value is logged.

The PFIFO regression now executes native words through ioctl1 and inline
ioctl2, including the observed flags `0x4` without FenceGet, and checks actual
engine register effects. Native entries combined with FenceGet also complete
correctly. Build, decoder, GPFIFO input, channel-fault and scheduling checks
passed with Metal validation. User validation of visible output after this
fix is still pending; a running FPS counter alone does not verify GPU execution.

## Guest increment reservations and GP entry level — 2026-10-18

The next user run still had a blank viewport. Its log showed an initial
`Unsupported GPFIFO submit flags 0x104`, then the old combined
`Unsupported conditional/subroutine GPFIFO entry` error, followed by repeated
InvalidState (8) submissions on the faulted channel. The combined error did
not record which entry bit triggered it, so the log alone cannot distinguish
conditional fetch from SUBROUTINE level. Inspection found an independently
incorrect rejection of the latter.

Two instruction-stage changes follow:

- `IncrementWithValue` (bit 8) reserves the input fence value on the channel's
  target counter. Guest syncpoint commands perform those reserved increments;
  merely reserving them does not advance the completed counter. FenceGet,
  when also requested, retains its existing terminal host increment. A request
  combining FenceWait and the count field is rejected before reservation.
  The direct caller reference is
  [libnx gpu_channel.c](https://github.com/switchbrew/libnx/blob/master/nx/source/nvidia/gpu_channel.c):
  kickoff sets bit 8 for `fence_incr`, passes that count as input fence value,
  then advances its local fence by the count after successful submission.
  This is caller-ABI evidence, not NVIDIA hardware proof for every NVN flag.
- GP entry bit 41 is named `level_subroutine` instead of `is_push_buffer`.
  Both levels execute the same packet decoder. NVIDIA's
  [clb06f.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/host/clb06f.h)
  defines LEVEL and
  [dev_pbdma.ref](https://github.com/NVIDIA/open-gpu-doc/blob/master/manuals/turing/tu104/dev_pbdma.ref.txt)
  explains that it controls whether progress updates TOP_LEVEL_GET; it does not
  introduce call/return execution. The explanatory source is later-family,
  not a GM20B trace. USERD progress is not exposed by Hydra yet. Conditional
  fetch remains explicitly unsupported and now logs the raw offending entry.

Focused tests exercise flags 0x104 behind a blocked semaphore, confirm that
reservation alone cannot satisfy the fence, then release the wait and execute
three guest increments with no extra completion increment. A subsequent
reservation plus FenceGet checks accumulation on the same channel. Other tests
cover conflicting wait/count flags and a packet continuing from SUBROUTINE into
MAIN across submissions. Gameplay visible output after these fixes is pending;
the instruction stage remains OPEN.

The final app/test build, six instruction integration checks, engine/texture/
raster suites and full shutdown suite passed with Metal validation. Logs are
`/tmp/hydra-count-level-*.log`. Hydra was reopened for the user to select the
game, with `HYDRA_TRACE_DIR=/tmp/hydra-instruction-retest-2026-10-18` so a capture
can be triggered without another restart. No game was selected by the agent.

## 3D syncpoint method was missing — 2026-10-18

The user clarified the next symptom: the viewport alternated between old
frames and then remained there, while menu audio/input continued. The captured
run logged waits for syncpoint 1 thresholds 153, 155 and 158 with current value
0, followed by an unknown engine binding (class 0x40, subchannel 3), then
InvalidState (8) for subsequent submissions. The prior run faulted with class
0 on subchannel 0. These invalid bindings are faults, not proof of a supported
new engine class. The original log is preserved at
`/tmp/hydra-before-3d-syncpoint.log`.

Inspection found that 3D `INCREMENT_SYNC_POINT`, byte address 0x02c8 / method
0xb2, was stored as a register without incrementing any counter. This omission
was already listed as D12 in the historical compute/memory audit. Supporting
puller A/B increments alone did not cover the 3D engine's command family.
The live log establishes the stuck counter and later fault; it does not yet
prove why that binding was read or that this fix removes the visible symptom.

The method now validates INDEX[11:0], checkpoints prior Metal work and advances
the allocated counter once. Both CONDITION values (stream-out/ROP) use this
conservative full-completion path, including CLEAN_L2 settings: shared Metal
buffer writes are CPU-visible after completion. A failed Metal checkpoint
cannot publish success; an unallocated index faults the channel. This does not
claim complete emulation of guest L2/cache operations or optimal timing.
Primary encoding reference:
[NVIDIA clb197.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/3d/clb197.h),
`NVB197_INCREMENT_SYNC_POINT` and its INDEX/CLEAN_L2/CONDITION fields.

Validation passed with Metal validation: app/test build; real GPU shared-buffer
fills followed by 3D counter increments under all four CLEAN_L2/CONDITION
combinations; ioctl2 0x104 reservation followed by gated 3D commands; combined
FenceGet accounting; invalid index fault; GPFIFO input, channel faults,
scheduling and visual diagnostics. Logs: `/tmp/hydra-3d-syncpoint-*.log`.
Gameplay after this fix remains pending. Fault diagnostics now include the last
GPU header address/raw word and PFIFO method for an unknown engine binding,
so a recurrence can be investigated without treating result 8 as the root cause.

## Follow-up: syncpoint advances, invalid binding persists — 2026-10-18

The user's CTR screenshot remains on the autosave screen while audio/input
are already at the menu. The latest run confirms 3D counters now advance
(e.g. threshold 158/current 156, then threshold 162/current 160), so the missing
handler was real but does not fully explain or fix this symptom. The channel
still faults on class 0x40 (subchannel 1). New provenance identifies GPU header
address `0x187ec00e4`, raw header `0x2d953000`, decoder method 0 and argument
0x40. Subsequent result 8 is the failed channel rejecting more work.

The initial bounded visual capture is under
`/tmp/hydra-instruction-retest-2026-10-18/visual/capture-138517885765833`.
Live read-only LLDB inspection was attempted, but the app exited before the
memory snapshot completed. No process memory was modified by that inspection.
The exact cause of the invalid stream is still unconfirmed; decoder state,
command-buffer reuse and presentation ownership are hypotheses, not fixes.

`HYDRA_PFIFO_TRACE=1` now retains the last 64 read words on the GPU worker,
including address, raw value and pre-dispatch packet state. On a dispatch
fault it dumps that bounded history and re-reads the same mapped addresses to
expose any differences from the values consumed. This is opt-in diagnostic
code, not a workaround that accepts the invalid binding. App/test build,
tracing-enabled PFIFO regression and diff checks passed. Next step: user starts
CTR once more, then inspect this fault history before changing execution rules.

## Pending event-wait status and Switch notifier ABI correction — 2026-10-18

The tracing-enabled run still faulted, with entry range
`[0x186ec00d4, 0x186ec816c)`. Its final words were:

```text
0x186ec00d4  0x80000044  immediate method 0x44
0x186ec00d8  0x800004a2  immediate method 0x4a2
0x186ec00dc  0x91000002  immediate host NOP
0x186ec00e0  0x00000001  interpreted as a zero-count legacy header
0x186ec00e4  0x2d956000  interpreted as a new header
0x186ec00e8  0x00000040  rejected as SET_OBJECT class 0x40
```

All recorded words matched their re-read values at fault time. This does not
prove the contents were correct at submission time, but provides no evidence
of changes between their consumption and the diagnostic re-read. No arbitrary
skip or acceptance of class 0x40 was introduced.

A concrete wait-ABI bug was found: a successfully armed, pending async fence
wait returned BadValue (11), although
[libnx fence.c](https://github.com/switchbrew/libnx/blob/master/nx/source/nvidia/fence.c)
only enters its kernel event-wait path for Timeout. Its
[nv.c result conversion](https://github.com/switchbrew/libnx/blob/master/nx/source/services/nv.c)
identifies device result 5 as Timeout and 11 as BadValue. The implementation now
returns Timeout for pending waits, while invalid IDs retain BadValue. Expected
pending statuses no longer produce an ioctl failure warning.

EventSignal (0x1c) now cancels/clears a pending event wait. Rearm, cancellation,
unregistration and destruction invalidate a shared wait generation under its
mutex. A callback already extracted from the syncpoint queue cannot signal a
new wait reusing the same event slot. Event service operations are serialized.
A directed test holds the first callback after extraction, cancels/rearms,
releases that stale callback, then verifies only the new threshold signals.

The notifier registration was also corrected against direct caller evidence:
[libnx nvchannel.c](https://github.com/switchbrew/libnx/blob/master/nx/source/nvidia/ioctl/nvchannel.c)
passes zero offset/size and an enable value as `mem`, without allocating a
notifier nvmap. [Switchbrew NV services](https://switchbrew.org/wiki/NV_services#NVGPU_IOCTL_CHANNEL_SET_ERROR_NOTIFIER)
explicitly describes this Switch/Linux ABI difference. Nonzero enables, zero
disables; timestamp/error/status are fetched by GET_ERROR_NOTIFICATION. Hydra
now follows this protocol without mapping or writing guest storage. The
previous Linux/Mach alias implementation and its mapping-specific tests were
removed. Event ownership still survives FD closure while queued state exists.

Build passed. With Metal validation (and PFIFO tracing enabled), video-events,
PFIFO decode, GPFIFO input, channel faults, scheduling, process cancellation,
semaphore shutdown, engine/texture/raster/visual-diagnostic suites and full
shutdown all passed. Logs: `/tmp/hydra-fence-abi-*.log`. Tests now expect the
caller contract's pending result 5; the earlier expectation of 11 was incorrect.
Whether these fixes eliminate CTR's invalid stream and stale-frame alternation
is still pending user gameplay validation. The instruction stage remains OPEN.

Additional ABI audit item (resolved by the Switch submit correction below): Switch ioctl1 carries appended entries and ignores
the pointer field (libnx/Switchbrew). Hydra still has a Linux-style CPU-pointer
path for a nonzero pointer, plus its zero-pointer appended compatibility path.
This should be reconciled with the Switch contract separately; current CTR
captures use ioctl2's inline entry path.

### No-display cleanup and regression checkpoint

The user subsequently confirmed CTR renders normally through race. After that
confirmation, the no-display fixes were refactored: event arm/cancel state is
encapsulated in NvHostEvent, query/cancel share event-slot decoding, and notifier
arguments have an explicit 24-byte ABI layout with a checked enable-field offset.
The ChannelBase notifier override is restored behind the checked input adapter.
ThreeD increment and puller checkpoints now call IRenderer::synchronize instead
of inspecting Metal command-buffer status from guest engines. Full GPU waiting
remains conservative; separate STREAM_OUT/ROP completion timing is still open.

App and test build succeeded. All seven integration selectors above, gpu-engine
and full shutdown passed with Metal validation after cleanup. Logs:
`/tmp/hydra-no-display-refactor-*.log`. Gameplay after this refactor has not yet
been checked. Texture mismatch warnings remain under investigation, and the
instruction-stage gates above remain OPEN.

### Switch submit input correction

Removed the Linux-style guest-pointer copy from NvHostGpu's ioctl1 adapter.
Both zero and nonzero `args.gpfifo` now select entries appended to the IPC header;
ioctl2 continues to read its inline input. Whole-array bounds validation precedes
allocation and submission. There is no pointer-based fallback for a truncated
IPC payload. This fixes an input-stage ABI mismatch; prior CTR captures used
ioctl2, so it is not presented as another cause of the earlier CTR display issue.

The GPFIFO regression uses a mapped guest array spanning a CPU page boundary
whose words differ from the IPC payload. It verifies IPC words win for a mapped,
partially unmapped, overflowing, or zero ignored pointer. For each case every
truncated entry length is rejected before dispatch without changing the response.
Existing unaligned, zero-entry, header/output length and large-count checks remain.

App and shutdown-test builds passed. All seven integration selectors and full
shutdown passed with Metal validation and PFIFO tracing enabled. Logs:
`/tmp/hydra-switch-submit-*.log`; `git diff --check` passed. This checkpoint does
not establish gameplay behavior or close the remaining instruction-stage gates.

### AS_GPU mapping input audit

The bind-channel audit exposed a separate unchecked-input family before any
binding changes: MapBufferEx allowed offset/size beyond nvmap storage, and Remap
could apply an early entry before rejecting a later invalid handle/range. Remap
also computed its count from the full stream size and read an unaligned typed
array. These paths now validate allocation bounds, CPU backing and IPC lengths
before mapping. MapBufferEx errors preserve the caller's output. A zero map size
still means the whole allocation, so a nonzero buffer offset cannot silently
extend that implicit mapping past its end.

Layout/caller evidence:
[libnx nvhost-as-gpu.c](https://github.com/switchbrew/libnx/blob/master/nx/source/nvidia/ioctl/nvhost-as-gpu.c)
and [address_space.c](https://github.com/switchbrew/libnx/blob/master/nx/source/nvidia/address_space.c).
The whole-buffer default also appears in NVIDIA's
[gk20a mapping implementation](https://android.googlesource.com/kernel/tegra.git/+/5028b8088fd5743c23f4566678e2eac867fa50f3/drivers/gpu/nvgpu/gk20a/mm_gk20a.c).
Hydra's InvalidSize/InvalidAddress choices for invalid ranges are defensive
validation policy; exact Switch error classification has not been hardware verified.

The new `--check-gpu-address-space` regression checks all truncated map
input/output lengths, unaligned input, valid subranges, allocation overflow,
fixed-address overflow, unallocated backing, an unmapped CPU tail, an invalid
batch following an unbind, truncated remap descriptors, the stream cursor, and
the whole-allocation default. Build and all eight instruction integration
selectors, full shutdown, GPU-engine and texture-cache passed with Metal
validation. Logs: `/tmp/hydra-as-mapping-*.log`.

This does not implement AS_GPU bindChannel, per-AS isolation, freeSpace, actual
unmapBuffer removal, or backing pinning during concurrent remap/free. Mappings
remain process-wide. Modify/kind and AS reservation semantics remain partial.
Batch validation prevents known invalid-input prefixes from executing; it is
not a transaction or a guarantee against concurrent changes to backing memory.

### AS binding, isolation and queued lifetime

The prior bindChannel stub is replaced with an FD capability call. The ioctl
context resolves FDs through the same nvdrv session pool; non-channel devices
reject binding through the base interface, without RTTI. AS device state is
serialized and owned by its first caller process. Every AS FD lazily creates a
separate GMMU registered weakly with that process. Channels bind once and keep
a strong AS reference in their PFIFO state; the submission adapter no longer
falls back to Process::getGMmu. Low-level decoder fixtures explicitly bind
their existing test GMMU rather than enabling a production fallback.

PFIFO retains channel state through host completion for yielded/faulted work as
well as successful work. Process cleanup first marks every live registered AS
cancelled, then removes/joins its PFIFO work and drains the host queue before
releasing guest allocations. This preserves the existing process lifetime
guarantee for multiple AS instances. The weak registry permits a closed AS to
expire once its last channel/queued/host completion owner disappears.

Per-object identities now prevent GenericMmu's TLS cache from accepting stale
entries when a destroyed MMU's host address is reused. Geometry layer-source
memoization is scoped to each channel's ThreeD instance, so a GPU VA in another
AS cannot reuse that channel's result. Shader cache identity already includes
the resolved host code pointer; broader self-modifying/remapped shader behavior
remains outside this checkpoint.

The Switch bind-call layout follows
[libnx nvhost-as-gpu.c](https://github.com/switchbrew/libnx/blob/master/nx/source/nvidia/ioctl/nvhost-as-gpu.c).
The single-bind/shared-AS/reference-lifetime model is corroborated by NVIDIA's
[as_gk20a.c](https://android.googlesource.com/kernel/tegra.git/+/5028b8088fd5743c23f4566678e2eac867fa50f3/drivers/gpu/nvgpu/gk20a/as_gk20a.c).
This does not assert that every Switch error code matches Linux: BadParameter,
BadValue for rebinding, and InvalidState for unbound/stopped submits are Hydra's
explicit validation policy until exact invalid-call results are hardware verified.

`--check-gpu-as-binding` covers truncated/unaligned bind input, invalid/wrong
device FDs, same/different-AS rebinding rejection, shared AS across channels,
two AS mappings of the same GPU VA with different backing, actual PFIFO fetch
and semaphore stores, foreign-process rejection, AS FD closure with live
channels, both FDs closing behind a pending fence, eventual owner release, and
reconstruction of an MMU in the same host storage. Process-cancel now covers
blocked work in two additional registered AS instances plus a real Metal drain.

The foreign-process test exposed an independent kernel initialization bug:
`std::array<bool, 32>{true}` marked only slot zero free. Kernel construction now
fills every applet-resource slot with true, allowing the second process to be
created. The directed ownership test exercises allocation and release of that
second slot. Fixture-only `--check-mmu-lookup` exposes the existing range/cache,
concurrent remap and thread-wait checks without requiring encrypted XCI fixtures.

Still open: AS initialization/reservation range semantics, freeSpace, actual
unmapBuffer removal, global nvmap handle ownership and backing pinning against
live CPU/GPU remap/free. This checkpoint does not close the instruction stage
or establish game compatibility after the architectural change.

Validation: app, shutdown, XCI, engine, texture, shader and raster targets built.
All nine instruction integration selectors above (including video-events),
engine/texture/shader/raster suites and full shutdown passed on the final build
with Metal validation. `hydra-xci-tests --check-mmu-lookup` passed, including
the lookup cost guard and concurrent lookup/remap checks.
`--check-gpu-memory` also passed after supplying the default-avatar asset in a
temporary test bundle (`/tmp/hydra-as-binding-memory.app`). The bare executable
initially aborted because it had no bundle resource. Its two older DMA fixtures
were corrected from TRANSFER_TYPE_NONE (0x180/0x380) to NON_PIPELINED
(0x182/0x382), as defined by
[NVIDIA clb0b5.h](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/dma-copy/clb0b5.h);
the engine's valid no-transfer behavior was preserved. Logs:
`/tmp/hydra-as-binding-final-*.log`, `-mmu-lookup.log`,
`-gpu-memory-bundled.log`. `git diff --check` passed. The preexisting duplicate
libc++ and newer minimum-macOS Homebrew dependency linker warnings remain.

The configured `hydra-hot` module was rebuilt for the changed GPU state layout.
The hot-reload integration test also passed with live Metal output, production
module loading, cache revision changes, in-flight call pinning, deliberate ABI
rejection and rollback. Log: `/tmp/hydra-as-binding-hot-reload.log`.

### AS_GPU reservation, FreeSpace and UnmapBuffer lifecycle

Previously AllocSpace never recorded a reservation, FreeSpace was a stub,
UnmapBuffer only forgot its bookkeeping (the GPU mapping stayed), non-fixed
placement used a bump counter that could land on fixed mappings and never
reused space, AllocAsEx zero-backed its whole VA range, and every sparse
block was an unowned `calloc` (leaked).

Semantics follow NVIDIA's `gk20a_vm_alloc_space`, `gk20a_vm_free_space` and
`gk20a_vm_unmap_user`
([mm_gk20a.c](https://android.googlesource.com/kernel/tegra.git/+/5028b8088fd5743c23f4566678e2eac867fa50f3/drivers/gpu/nvgpu/gk20a/mm_gk20a.c)):

- AllocSpace records `{gpu address, size, sparse}`. Only sparse reservations
  are zero backed; plain ones claim VA and reads fault. Non-fixed placement is
  first-fit over reservations and mappings, honoring the alignment input.
  Fixed reservations may not overlap each other.
- FreeSpace requires an exact reservation (offset, pages x page size). Buffers
  mapped inside stay mapped as ordinary mappings; only the sparse zero backing
  of the holes is dropped (`GMmu::releaseSparse`).
- UnmapBuffer removes the GPU mapping, restores sparse zeros where a sparse
  reservation covers it, and invalidates the cached guest range. Unknown
  addresses succeed (the driver only logs them).
- A fixed mapping/remap replaces overlapped mappings and splits the records so
  remainders can still be unmapped. AllocAsEx records the VA range instead of
  backing it.
- The GMMU owns sparse zero blocks and frees them only when it is destroyed,
  because queued or in-flight GPU work may hold pointers into blocks that were
  already unmapped. This bounds correctness, not memory: repeated remap to
  sparse grows virtual (untouched) memory until the address space closes.

Error codes (BadValue for unknown flags, odd page size, empty size, mismatched
or missing reservations; InsufficientMemory when no VA fits) are Hydra policy;
the Linux driver returns EINVAL/ENOSYS/ENOMEM and has not been compared with
Switch hardware traces. Non-sparse reservations no longer read as zero, which
matches the driver but could surface MMU faults in a title that relied on the
old zero fill; gameplay has not been checked.

`--check-gpu-as-lifecycle` covers argument validation, plain vs sparse reads,
overlap rejection, unmap restoring sparse, repeated/unknown unmap, FreeSpace
mismatches and double free, buffers outliving their reservation, automatic
placement around reservations with reuse, and record splitting.

### Heap backing pinning (resizeHeap)

`Process::resizeHeap` was the only path that freed host memory under live GPU
mappings (it unmapped and reallocated the heap, also discarding its contents).
It now flushes GPU-written textures of the old range, allocates the new block,
copies the contents, remaps the guest, repoints every live GMMU
(`GMmu::rebaseHostRange`; mappings past a shrunk end become unmapped,
write tracking is re-enabled), invalidates the old range in the caches, and
keeps the old block alive in `retired_heaps` until process cleanup. Queued or
in-flight work that already holds old pointers therefore reads valid memory.
`--check-gpu-heap-resize` covers growth, content copy, mapping follow-up,
shrink, and the retired block staying readable.

Residual limits: GPU writes performed after the copy but before the repoint
land in the retired block and are lost (no quiesce; blocking on PFIFO could
deadlock on a semaphore the stalled CPU would release). Other guest unmap SVCs
only change the guest page table and keep the host memory alive, so they do not
dangle; SharedMemory destruction while GPU mapped, and nvmap handle ownership
across processes, remain unreviewed.

### Subdevice masks and conditional fetch

gk20a programs the PBDMA with SUBDEVICE_ID 1 and channel filtering enabled.
Per [dev_pbdma.ref](https://github.com/NVIDIA/open-gpu-doc/blob/master/manuals/turing/tu104/dev_pbdma.ref.txt)
a mask makes the subdevice ACTIVE when `ID & mask != 0`; while inactive
methods are not delivered and a CONDITIONAL GP entry behaves as a NOP. SET,
STORE and USE_SUBDEVICE_MASK (clb06f.h opcodes 1/2/3, value bits 15:4) and
conditional fetch are implemented on that basis; skipped methods still
consume their payload words. END_SUBDEVICE_MASK is not defined in the
published header and still faults. The gk20a ID/enable programming comes from
driver memory and was not re-verified against the cited kernel source this
session. USERD TOP_LEVEL_GET is not exposed (the Switch caller has no USERD
view); both levels execute.

### Semaphore reductions, split reports, syncpoint reuse

- SEMAPHORED REDUCTION (op 0x10; clb06f.h bits 30:27, FORMAT bit 31) is an
  atomic MIN/MAX/XOR/AND/OR/ADD on the 4-byte word, honoring RELEASE_WFI. INC
  and DEC are rejected: the header does not define their wrap rules.
- A 16-byte release resolves payload and timestamp separately, so reports
  straddling host mappings work.
- Channel fence counters are returned when the channel state is destroyed (after
  queued work), and video channels return theirs when the FD closes. A reused
  counter keeps its value; the next owner's target starts from it. Previously
  the 191 counters were never recycled. Existing video tests now compare
  relative to the counter's starting value.
- Submit flags: SYNC_FENCE creates an Android sync fd
  (channel_gk20a.c), which does not exist on Switch, so it stays rejected.
  SKIP_BUFFER_REFCOUNTING is absent from the available kernel sources; it stays
  rejected until its semantics can be cited.

Validation (2026-10-18): app, tests and `hydra-hot` build; the eleven instruction
selectors above, gpu-engine, texture-cache, raster-state, full shutdown and
`hydra-xci-tests --check-mmu-lookup` pass with Metal validation. Logs
`/tmp/hydra-lifecycle-*.log` and `/tmp/hydra-pin-*.log`. The hot-reload test needs
fixture arguments and was not run. `git diff --check` clean. No game was run.

### Audit sumber: Linux versus Switch (Switchbrew NV services / SVC)

Perilaku AS_GPU dan syncpoint sesi ini memakai driver Linux gk20a sebagai
dasar; [Switchbrew NV services](https://switchbrew.org/wiki/NV_services) hanya
memberi daftar struct dan satu kalimat per ioctl, tanpa kode error.

| Perilaku | Dasar | Status |
|---|---|---|
| BIND_CHANNEL: identik Linux, sekali ikat | Switchbrew eksplisit | cocok |
| ALLOC/FREE_SPACE, UNMAP_BUFFER semantik rinci | Linux gk20a | asumsi, belum terbukti di Switch |
| Kode error AS_GPU | kebijakan Hydra | tidak diverifikasi |
| MAP_BUFFER_EX size tidak aligned = Panic | Switchbrew | **tidak ditegakkan**: nilai alignment tidak disebut; pemeriksaan 0x1000 sempat dicoba dan memutus CTR saat init (MapBufferEx InvalidSize lalu pure virtual abort), jadi dicabut |
| MAP_BUFFER_EX memberi atribut DeviceShared | Switchbrew | **belum**: QueryMemory tidak menampilkannya; butuh hitungan referensi lintas mapping/AS |
| Flag submit bit 5 skip_buffer_refcounting ada di Switch | Switchbrew | tetap ditolak; semantiknya belum ada sumber |
| Daur ulang syncpoint | desain Hydra (Host1x) | Switchbrew tidak mendokumentasikan reuse |
| SetHeapSize: isi bertahan saat resize | tidak dinyatakan | Hydra menyalin isi (tafsiran aman) |
| SetHeapSize: kelipatan 0x200000, >4GB ditolak 0xCA01 | Switchbrew SVC | keduanya dicek (InvalidSize = 0xCA01), ada tes |

Halaman GPU / GPU Classes Switchbrew, envytools, dan Apple Metal belum dipakai
sebagai sumber untuk gate sesi ini. `getVaRegions` dan Modify/kind masih stub.

### nvmap ownership dan serialisasi tabel handle

`MemoryMap` mencatat proses pembuatnya (`owner`). Alamat guest di nvmap hanya
bisa di-resolve pada MMU proses itu, jadi MapBufferEx dan Remap menolak handle
milik proses lain dengan BadParameter (handle tanpa owner, seperti fixture tes,
tetap diizinkan). Tabel handle kini dilindungi mutex (create/alloc/free/get,
host1x); sebelumnya `std::map` diakses tanpa kunci dari beberapa thread. Pointer
`MemoryMap*` dari `getMap` tetap hanya valid sampai handle di-free; caller
yang menyimpannya lintas free tetap berisiko. Berbagi nvmap lintas proses
(FROM_ID dengan tabel handle per-klien seperti nvmap asli) belum dimodelkan:
`fromId` masih mengembalikan ID yang sama.

### Status gate tahap A setelah sesi ini

Selesai dan diuji: binding/isolasi AS, reservasi/freeSpace/unmapBuffer, pinning
heap, subdevice mask + conditional fetch, reduction semaphore, report terbelah,
daur ulang syncpoint, batas heap SetHeapSize, ownership nvmap.

Tidak dapat diselesaikan tanpa sumber: INC/DEC reduction, END_SUBDEVICE_MASK
(tidak ada di clb06f.h), skip_buffer_refcounting, error code AS_GPU di Switch.
Ditunda dengan alasan: atribut DeviceShared (hanya terlihat lewat QueryMemory;
perlu hitungan referensi termasuk remap dan lintas AS), getVaRegions/Modify
(stub, tanpa sumber), recovery channel setelah fault (tanpa ioctl pendukung di
sumber yang dibaca), SharedMemory yang dilepas saat ter-map GPU (bug umur objek
kernel yang lebih luas), FROM_ID per-klien. Validasi gameplay/hardware dan
performa belum ada, maka tahap A **belum boleh dinyatakan tertutup**.

### Regression: nvmap bound vs NVN big-page mappings (gameplay, 2026-10-18)

The strict "mapping must fit inside the nvmap" check from the AS mapping audit
made CTR fail during init: `MapBufferEx(handle 0xe, offset 0, size 0x10000,
flags 0x5)` returned InvalidSize (10), the game continued with unmapped
memory and aborted with "Pure virtual function called!" (0 FPS). HEAD had no such
check. The limit is now the nvmap size rounded up to the AS big page size, and
every mapped page must still be readable guest memory; out-of-range requests
log nvmap size, offset and size. After the fix the user confirmed CTR boots and
reaches a race. The same log showed no MapBufferEx/ioctl failures, no PFIFO
faults and no MMU faults; remaining warnings were texture mismatches (capped at
16) and missing save files. PERF (HYDRA_PERF_STATS) in the race: 28-42 FPS with
pfifo CPU encode 24-28 ms/frame, Metal GPU 13-14 ms/frame, 1000+ draws/frame;
about 60 FPS in menus. Not compared with an older build, so no regression
claim. `--check-gpu-as-lifecycle` now covers an nvmap smaller than the big page: a
mapping rounded up to the big page succeeds, mappings past that limit (size or
offset) are rejected, and the rounded tail still needs readable guest backing.
Restoring the strict bound makes the test fail, so it guards this regression.
Also a lesson recorded: a size-alignment check for MapBufferEx (Switchbrew
"panic") was tried first and removed, since the required alignment is not
documented.
