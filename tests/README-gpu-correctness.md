# Guest ordering and completion-aware content versions

This change implements the concrete gaps found while checking the supplied
Maxwell-to-Metal audit against this checkout. The report's speculative examples
are not treated as confirmed bugs or hardware specifications.

## Source mapping

| Concern | Implementation | Status |
| --- | --- | --- |
| Per-channel decoder and engine state | `PfifoDecodeState`, `GpuChannelContext`, `Pfifo::threadFunc` | Existing |
| Blocked acquire cannot be overtaken by its channel | `Pfifo::ready`, `Pfifo::threadFunc`, saved entry/word offsets | Existing cooperative scheduler |
| Semaphore ordering and WFI | `Gpu::subchannelMethod`, `ThreeD::setReportSemaphore`, renderer synchronization/report queue | Existing; operation-specific WFI behavior is preserved |
| Method identity and ordering | `Pfifo::processMethodArg`, `Pfifo::dispatchMethod` | Added channel IDs, monotonic method sequences, argument addresses and debug assertion |
| CPU/DMA content change and overlapping aliases | `IRenderer::invalidateMemory`, `BufferCache::invalidateMemoryHot1`, `TextureCache::invalidateMemory` and interval storage | Existing range invalidation |
| Shared content authority | `ContentVersions`, draw/clear/copy content manifests | Extended with GPU status; compute image/storage writes now participate |
| Native GPU completion | `CommandBuffer::attachGpuCompletionCallbacks`, `commitSubmission`, `debugCommitAndWaitHot5` | Added separate callbacks for native submissions, including checkpoints |
| Resource lifetime | Resource leases, `retainResource`, `commitManagedSubmission` | Existing resources retained through completion |
| Capture/replay | `capture/draw_capture.*` | Existing host-resolved draw/clear replay; **not full guest-event replay** |

Sequences count consumed method arguments, including filtered arguments. Headers
and skipped conditional segments do not count. Deferred macro arguments retain
their sequence and source address across packet/submission boundaries. Internal
methods emitted by a macro inherit the invoking PFIFO context; this does not
assign a separate sequence to every internal macro instruction. Trace events
named `pfifo_method` include channel, sequence, source address, method,
subchannel and argument when `HYDRA_VISUAL_DIR` diagnostics are enabled.

## Content semantics

`latest()` and `current()` continue to describe **issued** writes. Existing
prepared-command validation must observe a change as soon as a producer is
encoded; delaying that version until completion would incorrectly keep older
snapshots valid.

`issueGpuWrites()` assigns one epoch to a command's write ranges, initially
pending. Its token transitions exactly once to completed or failed. Native
completion callbacks hold only the token's shared atomic status, so they neither
dereference a destroyed renderer nor restamp its interval map. A CPU write or a
later GPU issue overlays the old owner immediately. Completing the old token
cannot reclaim those bytes, including when only part of its range was replaced.
Split intervals share the original token's status.

`inspect()` reports the newest issued and completed versions among the currently
tracked overlapping subranges, plus pending/failed flags across those subranges.
The completed maximum is **not a fence for the entire range**. The bounded map's
eviction floor preserves conservative change detection and sets `unknown`;
eviction never invents completed contents. These are metadata about tracked
owners, not a new policy allowing concurrent unsynchronized CPU/GPU accesses.

GPU callbacks follow each native command buffer, even when a checkpoint commits
it and continues with another. Logical submission/fence callbacks retain their
existing report-publication ordering. The destructor attaches GPU callbacks
before the logical callbacks. The Metal API behavior used here is documented by
Apple for [completion handlers](https://developer.apple.com/documentation/metal/mtlcommandbuffer/addcompletedhandler(_:))
and [waiting for completion](https://developer.apple.com/documentation/metal/mtlcommandbuffer/waituntilcompleted()).

## Validation

Executed on Windows with Zig 0.16.0's C++ compiler, against the production
`content_version.cpp`, production `ztd::Range` and integer aliases:

```powershell
uv run --no-project --with ziglang==0.16.0 python -m ziglang c++ -std=c++23 -O2 -Wall -Wextra -Werror -Wno-nullability-completeness -Isrc -Isrc/ztd/src -include tests/content_version_test_types.hpp tests/content_version_tests.cpp src/core/hw/tegra_x1/gpu/resource/content_version.cpp -o build/correctness/content-version-tests.exe
.\build\correctness\content-version-tests.exe
```

The output is:

```text
PASS: completion races, alias ranges, 128000 differential steps, 8000 concurrent GPU writes
```

Also passed with the repository's additional pedantic, conversion, sign,
shadowing, promotion and cast warnings enabled and treated as errors. Tests
cover pending aliases, adjacent ranges, CPU overwrite before a delayed GPU
completion, split tails, failed/duplicate/out-of-order completion, tracker
destruction before callback, bounded eviction, a byte-array differential oracle
and concurrent issue/completion. `git diff --check` passes.

On macOS, build and run the following before merging:

```sh
ninja -C build/swiftui-release hydra-content-version-tests hydra-resource-registry-tests hydra-gpu-engine-tests hydra-shutdown-tests
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-content-version-tests
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-resource-registry-tests
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-gpu-engine-tests
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-shutdown-tests --check-pfifo-decode
MTL_DEBUG_LAYER=1 build/swiftui-release/bin/hydra-shutdown-tests --check-channel-scheduling
```

The GPU-engine suite adds assertions for content completion at a checkpoint
and separate checkpoint/destructor GPU callbacks. The PFIFO suite adds sequence
continuation and independent channel identity assertions. These macOS checks
were **not run on the Windows host**; this change has no gameplay or hardware
validation result and does not establish complete Maxwell compatibility.

## Remaining report work

Full guest-event capture/replay (CPU memory snapshots, semaphore dependencies,
DMA, compute, channel switches and completion), a complete compute command IR,
device-backed differential ordering tests, and barrier/batching optimization
remain separate work. The existing synchronous scheduler and retained resources
are not replaced with a speculative multi-queue dependency graph. No claim is
made that metadata alone resolves every alias or unsynchronized access case.
