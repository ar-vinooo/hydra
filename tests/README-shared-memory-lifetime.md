# SharedMemory ownership and GPU lifetime

Shared CPU mappings now retain their SharedMemory object independently of
handles. Exact object/address/size matching is required to unmap a recorded
mapping. Closing the last handle therefore cannot delete a still-mapped
object, and an invalid unmap cannot drop another mapping's reference.

The allocation is shared separately from the kernel object. A process retains
each allocation it successfully maps until process cleanup has cancelled its
address spaces and drained GPU work. CPU unmap does not remove existing GPU
mappings or pointers already encoded into Metal. This follows the conservative
retirement used for resized heap allocations; it is not immediate reclamation
after the last unmap. Retained allocations can increase memory use during a
long-lived process. There is no new allocation or reference operation per draw.

The repeated tests also found a Hypervisor teardown defect: PageAllocator
freed its host blocks without removing their VM mappings. Reusing a process
page-table region aborted at `hv_vm_map`. Each block now records its physical
address and calls `hv_vm_unmap` before host deallocation.

## Reproduce

```sh
ninja -C build/swiftui-release -j 6 hydra hydra-shutdown-tests hydra-gpu-engine-tests
python3 tests/shared_memory_lifetime_reference.py \
  build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests \
  --report /tmp/hydra-shared-memory-reference.json
MTL_DEBUG_LAYER=1 HYDRA_HOT_RELOAD=0 build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests --check-shared-memory-lifetime
```

The Python runner enables Metal validation and disables hot reload. Its
independent ownership model checks 200 programs / 10,064 steps across two
processes, two objects and two mapping slots per process. It covers handle
transfer/close, multiple mappings, wrong-object/size unmaps, duplicate maps,
unaligned/undersized map rejection and process destruction. Observations are
actual object destruction, allocation ownership, CPU mapping state and bytes.

Every program gates a real Metal blit read of 16 KiB from a no-copy buffer
resolved through an owned GMMU. Handle and CPU mapping mutations happen while
that read is blocked. Before destruction, the gate is released and all bytes
are checked. A fixture-only backing reference makes failed tests safe to clean
up; it is explicitly subtracted from ownership observations, so correct GPU
bytes alone cannot hide a missing production owner.

The dedicated cleanup selector destroys a process asynchronously while Metal
is held, both with an active CPU mapping and after unmap/last-handle closure.
It checks that cleanup waits, production ownership remains while waiting, and
the object/allocation are released after the GPU completes. The fixture guard
is removed before checking final allocation expiration.

Repeat the Python command with `--control` to model immediate backing release
after the last handle/mapping, deliberately ignoring Hydra's retirement. This
must fail the comparison. On 2026-10-19: normal run 10,064/10,064 passed;
control detected 348 mismatches with the Metal runner still exiting normally.

## Evidence and limits

- [libnx SVC header](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h)
  supplies the caller API for map/unmap and handle closure. It does not by
  itself prove internal Horizon object lifetime or exact result precedence.
- [Switchbrew MapSharedMemory](https://switchbrew.org/wiki/SVC#MapSharedMemory)
  is the reverse-engineered description of mapping-held references. It is
  secondary evidence, not an official Nintendo specification.
- [Apple Metal no-copy buffers](https://developer.apple.com/documentation/metal/mtldevice/makebuffer(bytesnocopy:length:options:deallocator:))
  and actual API validation establish the host buffer path used by the tests.
- [Apple hv_vm_unmap](https://developer.apple.com/documentation/hypervisor/hv_vm_unmap(_:_:))
  and the installed SDK `Hypervisor.framework/Headers/hv_vm.h` specify removal
  of a page-aligned guest physical mapping. Actual repeated process creation
  verifies reuse after teardown.

This is a host lifetime regression test, not a NVIDIA instruction simulation
or physical NVIDIA execution. The conservative retirement policy is internal
Hydra behavior. Concurrent handle-table lookup/close, complete Horizon mapping
permission/error semantics, early allocation reclamation, and all other CPU
alias APIs remain outside certification. Stage A remains open; gameplay and
FPS have not been measured for these changes.
