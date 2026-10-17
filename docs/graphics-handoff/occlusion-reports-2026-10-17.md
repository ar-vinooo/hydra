# Occlusion queries and report completion — 2026-10-17

Implemented as the next continuation. No staging, commits, game launches or GUI actions. The full graphics backlog remains open.

## Behavior

- `SET_ZPASS_PIXEL_COUNT` (method 0x545, enable bit 0) now controls Metal counting for actual indexed and non-indexed draw commands. Engine-skipped/rejected draws and renderer clear operations do not add samples. `CLEAR_REPORT_VALUE` (0x54C, type 1) starts a fresh counter epoch.
- The old increment-on-every-draw approximation is removed. Visibility results are summed as u64 values for ZPASS reports 0x2 and 0x15. Repeated reports retain the accumulated count; reset discards pending earlier-epoch results without reusing their GPU storage.
- Each render encoder uses separate result storage. A new Metal render pass can reset a visibility buffer, so sharing one buffer across encoders loses earlier samples. Pages contain up to 4096 draw results; after 128 retained pages, a checkpoint folds completed results into the scalar counter. Queries disabled by the guest allocate no result pages.
- Payload release and supported counter reports wait until preceding GPU commands complete before writing guest data. Long reports include a completion timestamp; short reports write only four bytes. Reports mask reserved address bits, validate the entire destination, support split host mappings, flush overlapping texture data, then invalidate GPU caches. An unmapped tail leaves the earlier destination bytes untouched.
- Checkpoint submissions retain logical `onCompleted` callbacks until the whole wrapper is submitted, preserving PFIFO submission/fence lifetime. Completed texture-upload buffers can be recycled at the checkpoint. A failed current checkpoint is logged and does not publish a successful report; a failed counter epoch remains invalid until reset.
- Local render enable modes CONDITIONAL, RENDER_IF_EQUAL and RENDER_IF_NOT_EQUAL now read the comparison structure. EQ/NE compare the two sequence/mode pairs, skipping timestamp fields. ALWAYS/NEVER overrides take precedence. Reads support high GPU addresses and split mappings without per-draw allocation. Invalid comparison data disables the operation.

## References and evidence

- Local NVIDIA `clb197.h`: `SET_ZPASS_PIXEL_COUNT` (byte 0x1514), `CLEAR_REPORT_VALUE` (0x1530), `SET_REPORT_SEMAPHORE_*` (0x1B00–0x1B0C), report encodings 0x2/0x15, one-/four-word structure sizes, and `SET_RENDER_ENABLE_*` / override.
- Local Eden `maxwell_3d.cpp::ProcessQueryCondition` and `maxwell_3d.h::ReportSemaphore::Compare`: comparison field offsets and each conditional mode. The reference location is recorded in the main handoff README.
- Apple [visibility counting](https://developer.apple.com/documentation/metal/mtlvisibilityresultmode/counting), [visibilityResultBuffer](https://developer.apple.com/documentation/metal/mtlrenderpassdescriptor/visibilityresultbuffer), and [visibility result reset/accumulation](https://developer.apple.com/documentation/metal/mtlvisibilityresulttype).
- The SDK marks the explicit `visibilityResultType` property as macOS 26+. This implementation uses separate storage across render encoders, retaining the existing macOS 15 deployment target rather than requiring that newer property.

During development, the new pixel test exposed two issues: its host-only GMMU had been installed before texture-cache setup (fixed in the test), and reusing a visibility buffer across submissions returned 16 instead of the expected 32 samples (fixed by per-encoder result pages). These were not gameplay incidents.

## Validation

Built `hydra` plus all nine test targets with `ninja -C build/swiftui-release -j 6`. All suites passed with `MTL_DEBUG_LAYER=1`: gpu-engine (17 PASS groups), shader-library (50), texture-cache (29), raster-state, render-hash, register-write, frame-trace, visual-diagnostic and buffer-snapshot. No Metal validation errors were reported. `git diff --check` passes. Build logs retain the existing Homebrew dylib deployment-target / duplicate-libc++ linker warnings.

Logs: `/tmp/hydra-query-final-build.log` and `/tmp/hydra-query-final-<suite>.log`. XCI/key-dependent and gameplay checks were not run.

The new engine tests use a real Metal pipeline and issue renderer draws and guest report methods. They assert zero samples behind depth, 16 samples for visible 4×4 coverage, 4 samples under a 2×2 scissor, indexed accumulation, disabled counting, reset of an in-flight epoch, accumulation across submissions, exclusion of clear draws, a 5000-draw batch, and folding a long-lived counter across 160 render/blit transitions. Report checks cover real GPU writes before publication, one-/four-word layouts, timestamps, split mappings, unmapped tails and callback lifetime. Conditional tests cover all five local modes and overrides, including data from actual occlusion reports.

## Limits and next work

Reports currently synchronize the CPU/GPU at publication; they are not asynchronous. This prioritizes ordering and guest-memory lifetime, and may affect performance in query-heavy scenes. Gameplay/performance validation remains with the user. Restart the rebuilt app because renderer and engine layouts changed.

Other report counters, semaphore acquire/reduction/awaken/trap, global render-enable controls and full hardware pipeline-location semantics remain incomplete. Existing optional host-fence gating is unchanged. These results do not establish MSAA/sample-shading correctness or complete global-memory coherence.
