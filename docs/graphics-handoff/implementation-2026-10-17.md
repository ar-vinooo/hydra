# Graphics implementation continuation — 2026-10-17

Status: implementation and validation in progress. No commits or staging; no GUI/game launch.

## Changes in this continuation

- FSET/ISET/PSET `.CC`: write Z/S and clear C/O, including RZ destinations and BF results.
- LOP/LOP32I `.CC`: zero/sign result flags and cleared C/O.
- Flow tests: Z/S/C/O evaluation combined with instruction predicates for BRA, EXIT, KIL, SYNC and BRK. Ordinary tests 0–23, 30–31; special CSM tests remain unsupported.
- PSETP second output uses `(!a op b) op c`; PSET/PSETP predicate modifiers use logical inversion.
- MOV immediate sign extension; LEA.HI constant-buffer decode mask includes X.
- IADD3, ISCADD and LEA low-word arithmetic use unsigned operations to preserve guest wraparound.
- Floating min/max emit fmin/fmax; bitfield extraction clamps its arguments and returns zero for empty/out-of-range extracts.
- TSC decode factored into `decodeSampler`: signed 13-bit/256 LOD bias, anisotropy table, filter-sensitive CLAMP_OGL. MIRROR_ONCE_BORDER follows the reference fallback to mirror clamp edge (not exact border emulation).
- Sampler cache identities include bias and anisotropy. Integer sampled views select nearest samplers in the render binding path. Arbitrary border colors still need shader emulation; exact white matching corrected.
- GPU test found native sampler LOD bias insufficient with instruction bias on this host. Bias is specialized per used binding and combined in MSL, including gradient scaling. The native sampler does not apply bias again. Explicit-level accesses ignore bias.
- GL depth compare encodings map to their D3D equivalents. Added constant/secondary-source blend factor decoding, Add/One/Zero reset values, and disabled blending on integer targets.
- Fragment SPH output map: active targets consume four registers each, depth uses the following reserved sample-mask slot plus one, exported components are masked in the Metal pipeline.

## Validation

In progress. Temporary build/test logs are `/tmp/hydra-handoff-*` and will be copied to the final validation directory when checks complete. New regression tests cover all ordinary flow tests over every CC bit combination, predicate/inversion guards, SET.CC to RZ, the Crash 4 FSET→BRA idiom, signed sampler bias, anisotropy and GL depth functions.

Existing straight-line test EXIT fixtures used flow test F (zero) while intending unconditional T (15); those encodings are corrected.

## Still open

The master backlog is not complete. This continuation does not implement global memory/SSBO, complete GPU fence/syncpoint semantics, buffer-queue/vsync redesign, full subroutine control flow, image load/atomic families, arbitrary border emulation, geometry/tessellation, FP64 arithmetic, MSAA or transform feedback. Existing known failures remain tracked in README section 6. Gameplay validation belongs to the user.

## GPU trace incident from earlier

The previous trace is unusable: Xcode reports a missing index. macOS crash report `Hydra-2026-10-17-093544.ips` shows SIGSEGV in Apple GPUToolsCapture `GTTraceStream_deviceObject` during `stopCapture`, called from `Renderer::endCaptureHot51`. This establishes where finalization failed, not its root cause. No successful replacement capture has been made. Do not present the existing ZIP as a valid trace.
