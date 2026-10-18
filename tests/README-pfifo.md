# PFIFO instruction stage — current status

The instruction stage remains **open**. Passing the tests below establishes
specific behavior; it does not establish complete Maxwell/NVN compatibility.
No game or GUI is launched by these tests.

## Implemented and tested

- GPFIFO submission validates the header, output and complete entry array.
  Ioctl1 accepts the guest pointer in `args.gpfifo`, reading CPU pages
  separately; ioctl2 reads inline entries. The existing zero-pointer,
  appended-array path remains supported for internal callers. Invalid pointers,
  truncated arrays, unaligned buffers and empty submissions are covered.
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
  copies ioctl1 entries from the userspace pointer and publishes an error
  notification with timestamp, info32 and status 0xffff. The
  [nvhost.h notification definition](https://android.googlesource.com/kernel/tegra/+/android-tegra-3.10/include/linux/nvhost.h)
  defines the 16-byte layout, MMU error 31 and illegal notification 25.
- [Apple waitUntilCompleted](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilcompleted())
  specifies waiting for GPU execution and completion handlers.

  Mapping arbitrary Metal failures to 25 is Hydra's conservative fallback,
  not a verified NVIDIA error classification.

## Validation

On 2026-10-18, the app build, all nine checks below and the full repeated
System teardown suite passed with Metal validation enabled. No gameplay
validation or deliberately faulting Metal shader was run.

```sh
ninja -C build/swiftui-release -j 6 hydra-shutdown-tests hydra-gpu-engine-tests hydra \
  hydra-texture-cache-tests hydra-raster-state-tests

MTL_DEBUG_LAYER=1 ./build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-gpfifo-input
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

1. GPFIFO conditional/subroutine execution and subdevice-mask instructions.
   They currently fault explicitly instead of executing with incorrect rules.
2. Additional submit flags and full ring/allocation semantics. SyncFence,
   HwFormat, skip-refcounting and unknown flags are rejected; SuppressWfi uses
   stronger completion ordering. Counter release/reuse and additional NVN
   increment-count semantics need further work. The AS_GPU bind-channel ioctl
   is still a stub; complete address-space binding/isolation needs an audit.
3. Mapped `SET_ERROR_NOTIFIER` registration needs owned/pinned backing storage;
   it returns NotSupported. The event and polling paths work. Recovery/reset
   after a fault is not implemented; opening a new channel is required.
4. Semaphore reductions and words split across host mappings remain unsupported.
   More precise host-to-guest error classification needs evidence.
5. Concurrent unmap/remap during live execution and complete backing-allocation
   pinning still need review. Process destruction is covered; that is not the
   same guarantee as every mutation of memory while a process is running.
6. Gameplay/hardware-trace validation and the instruction-stage exit audit.
   Synchronous checkpoints can add stalls; performance has not been measured.

Display presentation and renderer-feature completeness are separate later
stages; their existing limitations are not marked fixed by this work.
