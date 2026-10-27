# CPU register writes, Metal buffer snapshots and merged cache ownership

## CPU

Hypervisor serialization still reads the complete guest state after every VM
exit. Restoration compares 31 general registers and 32 SIMD registers against
that freshly read state and writes only changes. It never compares against the
previous restore input, because the guest can modify registers during execution.
The first restore writes the full bank. SP, PC/ELR, PSTATE, FPCR and FPSR retain
the original unconditional writes; exception/trampoline and timer paths are
unchanged. `HYDRA_DISABLE_REGISTER_WRITE_FILTER=1` restores all bank writes.

`hydra-register-write-tests` is signed with the Hypervisor entitlement and uses
real VCPU register APIs to check initial writes, unchanged filtering, a guest-like
mutation before refreshing the baseline, changed FP/LR and high SIMD bits.
It does not replace gameplay validation of SVC, timer and debugger behavior.

## GPU submission and memory

Metal buffer-cache entries up to 64 KiB can use immutable Shared snapshots of
guest memory. On invalidation the resource is replaced, rather than overwriting
in-flight bytes or inserting a blit in the current render pass. Larger entries
and `HYDRA_DISABLE_BUFFER_SNAPSHOTS=1` use the original private-buffer copy path.
Its staging uploads now share the bounded upload pool and are retired only after
command completion. Snapshots have a per-resource limit, not a total cache cap.

Merged-away buffer entries previously erased raw resource pointers without
deleting them. Cache entries and their returned BufferViews now share ownership.
Existing views keep the old wrapper alive during a merge; once the final view is
released its resource reference is released too. Metal's normal retained command
buffers hold already encoded GPU resources until completion. This fixes that
specific leak without evicting active textures or changing guest-visible data.

`MTL_DEBUG_LAYER=1 bin/hydra-buffer-snapshot-tests` checks actual red/green GPU
pixels from two uniform updates, release of the first CPU wrapper before GPU
submission, merge ownership, cached-byte accounting and the size limit. Repeat
with `HYDRA_DISABLE_BUFFER_SNAPSHOTS=1`: the same pixels require two render
encoders on the copy path versus one on the snapshot path.

With profiling enabled, `CPU registers` counts writes/skips and `BUFFERS` reports
current buffer-cache bytes, snapshot count/bandwidth and render/blit encoders per
frame. These counters exclude buffers retained by external views or Metal
in-flight commands, textures, other caches and the rest of the process. The live
cache has no new arbitrary global eviction limit. Measure FPS, memory footprint
and swap activity during the same CTR race before claiming a gameplay benefit.
