# Autonomous visual debugging MVP

This is an evidence collection and controlled experiment workflow. It does not
claim to infer a proven root cause from a screenshot or a successful compilation.
The `hydra` and `hydra-hot` targets built successfully on 16 October 2026,
and the new application opened its library. Tests have not been run. GPU shader
execution, captures and gameplay still need validation; build success does not
establish rendering correctness. The commands below include manual test steps.

## Existing architecture and integration

- `nvhost_gpu.cpp` submits entry lists to `Pfifo::submitEntries`. PFIFO decodes
  pushbuffer headers, dispatches methods/macros, and owns the GPU worker context.
  Each list now carries one correlation ID assigned under the submission mutex.
  `cpu_submit`, `gpu_submit`, draw events, Metal signposts, and `gpu_completed`
  share that ID. This links CPU submission to GPU execution; it is not an ARM
  instruction trace. GPU completion runs without reading GPU-thread control state.
- `engines/3d.cpp` builds the render pass and pipeline, resolves fragment shader
  inputs before vertex outputs, binds resources, and submits indexed/nonindexed
  draws. Selection, register snapshots and temporary overrides live here.
- `ShaderCache` specializes guest programs and tracks constant-buffer branch
  dependencies. Diagnostics retain its hash and hot revision, plus FNV-1a content
  fingerprints (not cryptographic hashes). A bounded replay captures current
  guest bytes, decoder traversal, decoded IR, analyzed IR and regenerated MSL,
  including when the real shader is cached. Bound MSL is saved separately;
  replay differences can also come from existing overrides or changed guest state.
- The decoder listing includes shader-relative word PCs, raw instructions,
  operation names for macro-dispatched instructions and UNKNOWN markers. Direct
  control-flow emitters retain raw words and corresponding IR; this is not a
  complete Maxwell assembly disassembler. The guest binary includes scheduling
  words and the bounded mapped code window, not a proven exact shader length.
- The MSL emitter supports selected solid magenta, UV, normal and checker output.
  `input=N` is an existing fragment varying location. UV uses its XY components;
  normal maps its XYZ from [-1,1] into [0,1]. This requires identifying the right
  varying from shader evidence. Missing inputs produce `override_unavailable`.
  Solid mode bypasses guest execution, including discard and side effects, when
  the fragment shader has no explicit depth export. With an explicit depth export,
  it falls back to an output-only override and records `solid_coverage_unavailable`.
  UV, normal and checker modes retain guest execution and discard. Fixed-function
  depth, stencil, culling and color write masks still apply in every mode.
- Metal shader/pipeline creation records compiler outcomes. Cached pipelines
  need no rebuild for inspection: selected draws record encoding/rejection and
  signposts on the actual encoder. `gpu_completed` reports asynchronous execution
  status and GPU duration for the PFIFO command buffer.
- Texture alias/upload/writeback synchronization, tracked-page flushing, buffer
  snapshots and command-buffer retirement remain in their existing paths. TIC/TSC
  copies use mapped guest snapshots. Diagnostic readbacks use the existing Metal
  blit/completion path, with additional allocation and failed-command guards.
- The controller is compiled only into the host, outside the hot source glob.
  Existing immutable hot dispatch tables, per-operation pinning, retained old
  images and revision-aware caches remain in use. No hot loader was replaced.

## Install and activate

The new host API, PFIFO list field and shader specialization fields require
**one full build and app restart**. They cannot be injected into an older host.
Afterward diagnostic requests require no build/restart. Supported source edits
continue through the existing watcher. Never bypass its header/ABI rejection.

Use the existing `tools/hot_debug.py run` workflow. Its `HYDRA_TRACE_DIR` enables
this controller at `<session>/trace/visual`. Alternatively set
`HYDRA_VISUAL_DIR=/absolute/path` before launching Hydra. Without either environment
variable diagnostics are disabled. A running older host must first be restarted.

No new UI is added. The interface is a small command file, like the existing
frame-trace trigger, with a helper:

```sh
python3 tools/visual_debug.py discover
python3 tools/visual_debug.py status
```

`--directory /path` precedes the subcommand when using a different session.
Discovery records draw/program addresses and pass/pipeline object identities,
without detailed resource or image dumps. Inspect `events.jsonl`, then select a
zero-based draw ordinal in a presentation epoch, guest shader address, or guest
resource address:

```sh
python3 tools/visual_debug.py capture --draw 42 --frames 2
python3 tools/visual_debug.py capture --shader 0x123400 --frames 2
python3 tools/visual_debug.py capture --resource 0x567800 --frames 2
```

Selectors combine with AND; a shader address matches VS or FS. A resource selector
matches color/depth targets and sampled VS/FS texture base addresses. It does not
match arbitrary buffer ranges or texture subranges. Resource selection is resolved
following pipeline construction, so use the reported shader address in a second
capture to collect its decoder/IR replay. Bindless descriptor failures are reported
as unavailable evidence rather than inferred texture contents.

Each request gets an immutable `capture-<monotonic timestamp>` directory. Fields in
`events.jsonl` are strings except schema, time, submission, presentation epoch,
thread and draw identifiers. Time is host monotonic nanoseconds. Object addresses
are session-local identities. `present_epoch` is an observed CPU presentation
counter, **not** a guest frame ID, fence or proof of identical game state. A
submission may span presentation epochs. Draw ordinals are not reset by capture;
use shader/resource filters and compare draw signatures when order varies.

## Controlled experiment

Freeze or reproduce the same game state manually. Capture a baseline, inspect its
images, shader artifacts and register/binding state, then write at most three
hypotheses. Run one intervention:

```sh
python3 tools/visual_debug.py experiment --draw 42 --change no_cull \
  --baseline /path/to/baseline-capture --symptom 'mesh disappears from one side' \
  --hypothesis 'front-face winding or cull interpretation may be wrong'
```

Other changes: `skip`, `only`, `solid`, `uv`, `normal`, `checker`, `no_depth`,
`no_blend`. For varying output add `--input N`. Resource selectors support
capture, skip, only, no_depth and no_cull. Shader/blend interventions require a
draw/shader selector, because those states are specialized before resource
selection. All requests reject multiple simultaneous interventions.

`only` suppresses other 3D draw submissions; it does not suppress clears, compute,
copies or compositor draws. `no_depth` disables both test and writes. Guest
registers are never rewritten. Overrides expire after 1..8 observed presents or
10 seconds, checked at the next GPU submission/draw boundary. They also reset with:

```sh
python3 tools/visual_debug.py reset
```

A paused/idle GPU consumes pending requests when it resumes. Expiry never changes
an in-flight draw. The next draw rebuilds/binds guest-derived state and existing
cache keys include output mode/input and blend state. Cached diagnostic objects
may remain allocated, as do other cached objects; they are not selected afterward.
Diagnostic rollback restores settings, not prior framebuffer contents, query
results, shader side effects or guest memory. Reproduce the baseline state before
repeating an experiment.

For a small implementation patch use the existing watcher/reload path, and
`python3 tools/hot_debug.py rollback` to roll back code. Diagnostic reset and code
rollback are different operations. Each captured draw records its pinned hot
revision. Check the existing host status and compiler errors before comparing.
There is no automatic code edit, build, patch acceptance or game restart here.

## Evidence and before/after comparison

```sh
python3 tools/visual_debug.py diagnose /path/to/capture --symptom 'black surface'
python3 tools/visual_debug.py compare /path/before.png /path/after.png --diff /path/diff.png
python3 tools/visual_debug.py compare-captures /path/baseline /path/candidate --output /path/comparison.json --same-game-state
```

`diagnosis.json` contains affected draws, resources/shaders, evidence, at most
three evidence-backed hypotheses, proposed experiments, source candidates,
uncertainty and a request for more focused evidence. It leaves the recommended
correction empty and confidence insufficient until external analysis proves a
cause. The experiment manifest records symptom, hypotheses, baseline, intervention
and rollback instructions; results remain pending review. The difference command
requires Pillow in the Python environment you choose and measures pixel changes
without resizing. It never marks a production fix validated.

Check `capture_end` before analyzing a capture; GPU images can arrive later.
`completions.jsonl` preserves correlated execution results even after capture end
(up to 128 tracked submissions per capture). Rerun comparison after those results
arrive; missing completion is not proof of success. State comparison records
field differences, draw rejection and GPU failures without approving a fix.
`image_requested` is not success: confirm the PNG exists and inspect it. Unsupported
formats/dimensions or allocation/command failures may yield no PNG. The inherited
dumper may normalize/tone-map data, stitch cube faces or show only the first array
slice; PNG similarity is not an exact floating-point framebuffer comparison.
Use raw state/shader evidence and the dumper's statistics alongside images.

Readbacks are taken before/after selected draws on the ordered command stream.
They are attachment snapshots, not a guaranteed final composited screenshot or
pixel-provenance buffer. Solid output and skip experiments help localize an object
manually. Automatic ROI-to-draw picking, semantic UV/normal discovery, exact frame
replay, complete buffer snapshots, automatic visual anomaly detection and a proven
root-cause inference engine are outside this MVP.

Normal enabled tracing retains a 256-event submission/completion ring; it never
dumps ARM instructions. Detailed capture is bounded to 4096 events (+end marker), with summary events limited
to the first 512 slots so selected-draw detail retains capacity,
16 shader replays, 512 artifact files, 4 MiB per artifact/16 MiB total artifacts, 4 before-target images,
8 after-target images and 4 sampled-texture image requests. Readbacks are bounded
to 8 MiB each/64 MiB per capture before PNG encoding. These are per-capture limits;
old capture directories are deliberately not deleted. Archive/delete reviewed
captures manually. Timings during image/replay capture include diagnostic overhead.

## Manual validation

Run these yourself after ensuring sufficient disk space:

```sh
cmake -S . -B build/swiftui-release -G Ninja -DFRONTEND=SwiftUI -DMACOS_BUNDLE=ON -DCMAKE_BUILD_TYPE=Release -DHYDRA_HOT_RELOAD=ON -DHYDRA_HOT_TESTS=ON -DHYDRA_VISUAL_DIAGNOSTIC_TESTS=ON
cmake --build build/swiftui-release --target hydra hydra-hot hydra-visual-diagnostic-tests -j 4
build/swiftui-release/bin/hydra-visual-diagnostic-tests
python3 tests/visual_debug_tests.py
python3 tests/hot_debug_tests.py
```

Then use the existing Metal hot-reload integration command in `README-hot-debug.md`.
For gameplay validate: diagnostics disabled versus idle enabled FPS; summary-only
capture; selected indexed/nonindexed draws; before/after PNGs; source hashes and
resource mapping; compiler failure versus valid-but-wrong output; each intervention
and expiry/reset; shader override removal and existing code rollback; no changed
rendering after returning to the same baseline; and a second game for regressions.
Compare GPU duration and existing `--perf-stats` outside capture windows. Confirm
multiple requests cannot exceed per-capture budgets and record any skipped images.
Do not accept a production correction solely because similarity improves.

## Files changed for this MVP

- `CMakeLists.txt`, `src/core/CMakeLists.txt`: host controller and optional test target.
- `src/core/hw/tegra_x1/gpu/visual_diagnostics.{hpp,cpp}`: process-lifetime controller,
  request/expiry handling, bounded events/artifacts/readbacks, completion persistence.
- `src/core/hw/tegra_x1/gpu/pfifo.{hpp,cpp}`, `src/core/system.cpp`: submission IDs,
  command origin, presentation observations and asynchronous Metal completion.
- `src/core/hw/tegra_x1/gpu/engines/3d.{hpp,cpp}`: `selectVisualResources`,
  `captureVisualDraw`, draw isolation, temporary depth/cull/blend overrides.
- `src/core/hw/tegra_x1/gpu/renderer/shader_cache.{hpp,cpp}`: diagnostic replay,
  shader identity/source snapshots and output/input specialization in cache keys.
- `src/core/hw/tegra_x1/gpu/renderer/shader_decompiler/decompiler.cpp`,
  `decoder/decoder.cpp`, `codegen/lang/msl/emitter.cpp`: IR/decoder evidence,
  RAII emitter ownership for bounded replay lifetime and varying/solid output.
  Decoder/emitter paths in this item are relative to `shader_decompiler/`.
- `src/core/hw/tegra_x1/gpu/renderer/metal/{shader,pipeline,renderer}.cpp`: real
  compiler/encoding results, signposts and bounded/failure-checked readbacks.
- `tools/visual_debug.py`: discover/capture/experiment/reset/status, evidence
  diagnosis, image difference and capture state comparison.
- `tests/visual_diagnostics_tests.cpp`, `tests/visual_debug_tests.py`: prepared
  controller/evidence regression cases, not executed during implementation.
- `tests/README-visual-debug.md`, `tests/README-hot-debug.md`: architecture map,
  workflow, limitations, source inventory and manual validation instructions.
