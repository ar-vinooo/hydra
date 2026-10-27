# Stability follow-ups

The existing audio clock/PCM implementation is retained. The user reported normal
CTR audio and FPS after repeated Alt-Tab in the baseline session. The final samples
while switching apps and closing the game are excluded from race performance.

Shutdown now stops services while their managers and client MMUs are alive, joins
all process threads (including a child started during shutdown), and joins PFIFO
before freeing guest/GPU state. Kernel process cleanup runs before kernel locks
are destroyed. Closed sessions drain requests before releasing their services.
Cloned service sessions own references; closing the last session releases the
domain's self-reference and its child services.

Metal MSL source persistence complements existing binary pipeline archives. On a
subsequent session a utility-priority worker precompiles up to 64 stored sources;
draws use ready libraries or synchronously compile a miss. This does not skip
draws, serialize MTLLibrary binaries, or eliminate first-use compilation. Resident
libraries are limited to 64, pending source writes to 16 MiB, and stored sources
to 128 files / 64 MiB per GPU/OS/options cache directory, evicting the oldest
source files as new ones arrive. A library's retained
functions may outlive its cache entry. HYDRA_DISABLE_SHADER_CACHE=1 disables both
source prewarming and pipeline archives.

HYDRA_PERF_STATS=1 additionally logs process physical footprint, Metal resource
allocation, and library cache hits. Process footprint and Metal allocations are
separate overlapping measurements on unified memory; do not add them together.
Buffer cache bytes exclude textures, in-flight resources, and retained views.
These limits are not a global emulator memory budget or proof that every texture
cache lifetime issue is resolved.

Run after building with HYDRA_METAL_CACHE_TESTS=ON:

```
build/swiftui-release/bin/hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests
build/swiftui-release/bin/hydra-shader-library-tests
build/swiftui-release/bin/hydra-buffer-snapshot-tests
HYDRA_DISABLE_BUFFER_SNAPSHOTS=1 build/swiftui-release/bin/hydra-buffer-snapshot-tests
build/swiftui-release/bin/hydra-texture-cache-tests
build/swiftui-release/bin/hydra-raster-state-tests
```

Shutdown tests use a real System, service workers/PFIFO, a process with live host
threads, TLS accessed until callback return, late child startup, destructor-only
and repeated explicit stop, and CMIF domain conversion/cloning/child requests.
The test bundle supplies the default avatar resource required by System startup.
The library test checks persistence across sessions, a warm hit executing a real
GPU compute kernel, bounded resident entries, disk eviction preserving recent sources, and invalid-source failure.

HYDRA_AUDIO_STATS=1 enables five-second audio clock/queue diagnostics without
writing PCM dumps. macOS application termination explicitly stops the active
system before C++ global destructors run.

For CTR validation: use the same track/mode/resolution, repeat Alt-Tab, leave the
app open for memory trends, then close normally to verify game-session teardown.
A second session is required to measure prewarm hits; the first seeds sources.

## Game validation observed in this run

- Baseline audio/FPS was reported normal after repeated Alt-Tab.
- The two final quit-hook sessions ended with user serialization and cache
  shutdown messages, without the previous mutex exception.
- Cortex Castle was named as the intended track, but the subsequent session
  already dropped FPS in the loading screen. A separate Claude Code session
  launched `ninja -C build/swiftui-dev hydra-xci-tests`, with many concurrent
  compiler processes; these samples are not an isolated performance comparison.
- Audio diagnostics captured an empty PCM queue and growing hardware missing
  frames during the large FPS drops. Audio recovery after that load is still
  unverified; do not mark the audio/performance issue resolved.
- The shader prewarm worker was sleeping in the sampled call graph, so this
  profile does not establish prewarming as the sustained slowdown's cause.
- After the user stopped the other coding session, the same release was opened
  without rebuilding (`isolated-cortex.log`). Early gameplay intervals measured
  58.5–60 FPS, CPU encode 3.9–6.3 ms and Metal GPU 8.6–11.0 ms at 430–814
  draws/frame. The 32 kHz PCM queue held 1440–1760 frames; cumulative missing
  frames stopped increasing between the 45- and 50-second audio samples. The
  user confirmed stable 60 FPS and normal audible output in this session.
  A one-minute Alt-Tab recovery test was requested, but the reply only explicitly
  confirmed normal audio, so prolonged-background recovery remains unverified.
