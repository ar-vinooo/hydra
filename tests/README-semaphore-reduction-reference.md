# Puller semaphore reduction: Python reference and actual Metal I/O

`semaphore_reduction_reference.py` compares an independent Python model with
Hydra's production puller through `--semaphore-cases`. Metal compute kernels
initialize the shared guest buffer and read back the final semaphore plus seven
neighboring words. The reduction itself uses the production CPU atomic path;
this is not a GPU shader implementation of the reduction or physical NVIDIA execution.

The 3,272 cases cover all eight operations, signed/unsigned formats, both WFI
settings, boundary values, deterministic random inputs, repeated wrap chains,
and invalid operations. Checks include the result, fault status, neighboring
words, and checkpoint count. With WFI enabled, initialization is still pending
when the production reduction starts. With WFI disabled, initialization is
completed before the measurement so arithmetic has a defined input; that group
checks that the reduction does not force a checkpoint, not concurrent atomics.

## Reproduce on macOS with Metal

```sh
ninja -C build/swiftui-release -j 6 hydra hydra-gpu-engine-tests
curl -fL https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/host/clb06f.h -o /tmp/hydra-nvidia-clb06f.h
curl -fL https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/manuals/turing/tu104/dev_pbdma.ref.txt -o /tmp/hydra-nvidia-dev_pbdma.ref.txt
python3 tests/semaphore_reduction_reference.py build/swiftui-release/bin/hydra-gpu-engine-tests \
  --nvidia-header /tmp/hydra-nvidia-clb06f.h \
  --nvidia-manual /tmp/hydra-nvidia-dev_pbdma.ref.txt \
  --report /tmp/hydra-puller-reduction-reference.json
```

The Python runner enables `MTL_DEBUG_LAYER=1` and disables hot reload. It audits
12 numeric Maxwell header definitions and the manual equations before running,
and records source SHA-256 hashes in its report. Repeat the Python command with
`--control` to deliberately corrupt the INC reference; this must exit nonzero.

## Sources and limits

- [NVIDIA Maxwell channel header](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/host/clb06f.h)
  defines reduction opcodes 0–7 and the format encoding.
- [NVIDIA Turing PBDMA manual](https://github.com/NVIDIA/open-gpu-doc/blob/master/manuals/turing/tu104/dev_pbdma.ref.txt)
  supplies the detailed unsigned INC/DEC equations and marks their signed
  forms invalid. INC wraps to zero at/above the limit; DEC reloads the limit
  at zero or above the limit, otherwise decrementing.
- [NVIDIA PTX atomic instructions](https://docs.nvidia.com/cuda/parallel-thread-execution/#parallel-synchronization-and-communication-instructions-atom)
  independently document matching unsigned increment/decrement arithmetic.

The detailed channel manual is from a later GPU family. Matching Maxwell
encodings and PTX arithmetic do not certify all GM20B side effects. Invalid
forms leave memory unchanged and fault with Hydra error 25; that error mapping
is emulator policy, not a proven NVIDIA error enum. Concurrent CPU/GPU atomics,
timestamps and every semaphore modifier remain outside this fixture. Stage A
remains open, and gameplay/FPS changes require a separate game run.
