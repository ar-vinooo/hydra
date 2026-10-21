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
| Compute IR | `command_ir/compute.*` | Added sealed bindings, resource/mapping checks, leases, access manifests and encoding failure propagation |
| Capture/replay | `capture/draw_capture.*` | Host-resolved draw/clear/copy/compute replay; **not full guest-event replay** |

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

An optional guest event recorder, semantic memory/dependency replay, native
PFIFO-word replay, differential comparator and seeded ordering stress are now
implemented; see [guest-event-stream.md](../docs/graphics-handoff/guest-event-stream.md).
Real-device reference collection and execution of the added checks remain
unperformed. Barrier/batching optimization remains gated on successful P0
correctness checks. The existing synchronous scheduler and retained resources
are not replaced with a speculative multi-queue dependency graph. No claim is
made that metadata alone resolves every alias or unsynchronized access case.

## Compute, capture and validation continuation

Compute now follows `sealCompute` / `validateCompute` / `executeCompute`. A seal
tracks shader, buffer, sampled texture, sampler and image lifetimes, retains
owners, records guest mappings, and builds conservative read/write manifests.
Validation checks the exact resolved binding pointers, offsets, sizes, indices,
write qualifiers and dispatch dimensions. Mutating those after sealing rejects
execution. Zero-sized dispatches keep their previous no-op behavior and issue
no write epoch. Nonempty dispatch dimensions use checked multiplication, and
buffer ranges are checked with subtraction to avoid overflow.

The backend returns whether it encoded the compute operation. Pipeline creation,
threadgroup-size or binding-index failures cannot publish successful write
metadata; the engine faults the channel with Hydra's existing generic error 25.
`HYDRA_PROFILE_COMPUTE` now measures the actual kernel once on the ordered queue:
the previous profiling branch encoded another dispatch on a separate command
buffer, which could duplicate storage/image side effects.

Capture format 4 adds copy/compute commands and guest origin metadata for all
command kinds. Version 3 remains readable. Compute capture normalizes bindings
of the same native buffer onto one allocation, preserving alias offsets, and
records written storage bytes immediately after that compute operation. Replay
compares these bytes as well as final textures. Cross-allocation aliases that
cannot be restored are explicitly rejected. Backend failures abort the live
capture. The replay tool identifies all four command kinds.

This remains a snapshot-based host command replay. It does not claim to replay
PFIFO scheduling, guest memory mappings or CPU/GPU races. Capturing snapshots
requires synchronization and can change timing. The separate guest event stream
reconstructs captured memory and ordering evidence without executing shaders.
The native `hydra-guest-replay` separately executes consumed PFIFO words through
the production decoder and engines with conservative checkpoints. It compares
decoded methods and recorded memory outputs; neither replay reconstructs
unobserved CPU execution or the original asynchronous timing. This continuation
was not built or tested at the user's request.

The origin fields carry channel, guest method sequence and source address through
draw, clear, copy and compute IR into captures. Visual diagnostics additionally
record `pfifo_blocked` and `pfifo_resumed` events. Check observed method ordering:

```sh
python3 tools/check_gpu_trace.py path/to/events.jsonl
```

The checker rejects duplicate/reversed method sequences, dispatch from a blocked
channel and inconsistent wakeups. It allows gaps from filtered methods and marks
partial capture boundaries. `--require-complete` rejects unmatched wakeups and
unfinished blocking. It checks scheduling evidence, not Maxwell hardware or
memory visibility.

Run all available checks with one command:

```powershell
uv run --no-project python tools/validate_gpu_correctness.py
```

The script compiles production C++ with warnings as errors, runs the content
version tests, compares compute dimension guards against Python arbitrary-
precision integers, checks the trace checker's failure cases, and checks the
diff. It uses clang++/g++ when available or pinned Zig 0.16.0 through uv, without
changing project dependencies. It writes per-check logs and a machine-readable
result to `build/correctness/validation.json`. A missing compiler, failed check
or timed-out subprocess exits nonzero. `--compiler` chooses an explicit compiler;
`--timeout-seconds` bounds each subprocess (default 900).

On Windows, the successful status is explicitly
`portable_passed_metal_unverified`. For the native merge gate, use a configured
macOS build:

```sh
python3 tools/validate_gpu_correctness.py --require-metal --build-dir build/swiftui-release
```

That gate also builds the replay tool and runs the native GPU, registry, PFIFO,
channel scheduling and process cancellation suites with Metal validation. A
separate `--check-compute-ir-profile` GPU-engine invocation verifies that profiling
does not execute the kernel twice. Native tests cover sealed-binding mutation,
invalid ranges, no-op dispatch, backend failure propagation, resource leases,
storage aliases and offsets, mixed compute/copy replay, corrupted expected
storage bytes, invalid captured offsets and version 3 compatibility.

Executed here: **128,000 content differential steps, 8,000 concurrent writes,
26,916 compute dimension cases and 7 trace-checker tests**, all passing. Missing-
compiler and unavailable-required-Metal paths were also checked to exit nonzero
and record failure. The native suites are implemented but remain **unrun on this
Windows host**; portable success is not a full-app compile or Metal runtime result.
