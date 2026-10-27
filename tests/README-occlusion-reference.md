# Occlusion report reference: Python vs Metal

```sh
ninja -C build/swiftui-release -j 6 hydra-gpu-engine-tests
MTL_DEBUG_LAYER=1 PYTHONDONTWRITEBYTECODE=1 python3 -I tests/occlusion_reference.py \
  build/swiftui-release/bin/hydra-gpu-engine-tests \
  --report /tmp/hydra-occlusion-reference.json
```

`hydra-gpu-engine-tests --occlusion-cases <in> <out>` runs random programs on
the Metal renderer and the engine's report methods; the Python script models the
same programs independently and compares every report slot.

A program is a line of operations: `D<n>` draws, `C0/C1/C2` coverage of a draw
(16 samples, none behind depth, 4 under a 2x2 scissor), `X0/X1` counting off/on,
`R<s>`/`r<s>` long/short ZPASS report into slot `s`, `Z` counter reset
(`CLEAR_REPORT_VALUE`), `S` new submission, `E` end of the render encoder, `F`
wait for the pending reports. Reports are queued without waiting for the GPU and
reach guest memory when the GPU finished them, so a program mixes encoder
switches (new visibility pages), submissions, resets in flight, repeated reports
to one slot and counting that is switched off.

The model: a report holds the number of samples of the counted draws since the
last reset; the last report to a slot wins; a short report replaces only the low
word; the timestamps of the slots whose last writer was a long report never go
back in stream order (reports are published in stream order).

Seed 2026 generates 300 programs (six fixed ones cover visibility-page rollover
inside one encoder, a rollover across a blit, report right after a reset,
counting off, submissions between reports and a mix). All 300 match bit for bit
against the 64-bit counter values, with Metal API validation enabled. The
control (`--control`) miscounts scissored draws in the model on purpose and
fails 97 of the 300 programs, so the comparison does bite.

References: NVIDIA `clb197.h` for `SET_ZPASS_PIXEL_COUNT` (0x1514),
`CLEAR_REPORT_VALUE` (0x1530) and `SET_REPORT_SEMAPHORE_*`; Apple's
[visibility result buffer](https://developer.apple.com/documentation/metal/mtlrenderpassdescriptor/visibilityresultbuffer)
for the host side.

Limits: this is a model against Apple GPU execution, **not NVIDIA hardware**.
The model is the counter semantics the engine tests encode (a sum of samples
since the last reset), the in-memory report layout (value, then timestamp) is
the one of `tests/gpu_engine_tests.cpp`, and sample counts come from three
coverage setups. Failure handling (a failed command buffer publishes nothing),
unmapped or cancelled destinations and the ordering of fence callbacks behind
reports are covered by `testOcclusionReports`, `testReportCallbackOrdering` and
`testReportsLandBeforeLaterWrites` instead, because they need injected
failures and slow publishers rather than random programs.
