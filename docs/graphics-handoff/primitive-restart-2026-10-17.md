# Indexed primitive restart — 2026-10-17

Implemented and tested at decoder level. Gameplay validation is pending. No commits, staging, GUI or games were launched.

## Behavior

- `SET_DA_PRIMITIVE_RESTART` (method 0x591, enable bit 0) and `SET_DA_PRIMITIVE_RESTART_INDEX` (0x592) now reach `IndexDescriptor` for indexed draws, including compact draws.
- Enabled restart compares the zero-extended input index against the full 32-bit restart register. Each marker ends primitive assembly; leading, trailing and consecutive markers produce no vertices. Incomplete list primitives are dropped at segment boundaries, and triangle-strip winding starts again for each segment.
- Converted fans, polygons, quads, quad strips and line loops assemble each segment separately. Converted strips become lists, preserving maximum-valued vertex indices when guest restart is disabled.
- Native fixed-index restart remains a direct buffer-cache path for u16/u32 line and triangle strips. Other decoded buffers retain source-content checks and include restart state in their identity. Cache hits restore the decoded count without regenerating indices.
- Generated index buffers no longer form a range from a null source pointer.

## Evidence

Guest register semantics were checked against NVIDIA `clb197.h`, definitions
`NVB197_SET_DA_PRIMITIVE_RESTART` / `_INDEX` (byte offsets 0x1644 / 0x1648).
Secondary-emulator agreement is not used as proof. Metal's documented
fixed-index restart constraints determine the host-side conversion path.

Metal's fixed maximum-index strip restart constraint is recorded in the original front-end audit, B23. List conversion avoids relying on a Metal restart-disable toggle.

## Validation

Built `hydra` and all nine test targets with `ninja -C build/swiftui-release -j 6`. Ran every suite below with `MTL_DEBUG_LAYER=1`; all exited 0:

- gpu-engine (13 PASS groups, including the new primitive-restart group)
- shader-library
- texture-cache
- raster-state
- render-hash
- register-write
- frame-trace
- visual-diagnostic
- buffer-snapshot

`checkPrimitiveRestart<T>` covers all three index widths and ten topologies, custom markers, repeated markers, incomplete segments, winding reset, marker/enable changes, source rewrites, all-marker draws, disabled maximum indices, full-width register comparison and native u16/u32 strip dispatch. These checks inspect index buffers allocated by the Metal renderer; they do not assert rendered pixel coverage or guest-engine register replay.

Build and test logs: `/tmp/hydra-restart-*.log`. The build retains existing linker warnings about Homebrew dylib deployment targets and duplicate libc++ linkage. No compilation errors or Metal validation errors occurred.

## Remaining scope and risks

Non-indexed restart (0x37A), restart topology-change modes, inline indexed draws and DRAW_AUTO remain open. Adjacency topologies, geometry shaders and tessellation are outside this change.

Converted strips require more output indices than native strips and incur decoding on a cache miss. Native fixed-index restart strips retain their direct path; decoded cache hits reuse the existing buffer. Gameplay performance has not been measured.

The descriptor/cache layout changed: use the rebuilt app and restart it rather than hot-reloading into an older app process. The user should check Crash 4 and Jump Force geometry, plus CTR and Mario Kart 8 regressions, particularly disconnected strips and missing geometry.
