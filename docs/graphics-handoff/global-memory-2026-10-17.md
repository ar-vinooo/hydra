# Global memory / storage buffers — 2026-10-17

Implemented end to end and unit tested. **Not seen in a game**: no reachable shader in the Crash 4 (1634) or `build/board-live` (1137) corpora executes LDG/STG/ATOM/RED. An earlier opcode scan of the dumps reported ~13 shaders, but those hits were neighbouring shaders' bytes after the end of the dumped program (the decoder never reaches them). Expect Mario Kart 8 (compute heavy) or other titles to be the first users.

## What it does
- **Decode** (`decoder/memory.cpp`): `LDG` (U8/S8/U16/S16/B32/B64/B128), `STG`, `ATOM` (U32/S32 ops ADD/MIN/MAX/AND/OR/XOR/EXCH, F32 ADD) and `RED`. Only `.E` (64-bit) addresses with a register base are supported.
- **Address tracking** (`decoder/texture.cpp`, `trackStorageBuffer`): follows the low address word back through IADD/IADD3/ISCADD/LEA/IADD32I/MOV/LDC to a constant buffer word, preferring the NVN SSBO table (c0[0x110, 0x700), 16 byte aligned). Same linear backward scan (and limits) as the bindless texture tracer. Crash 4's own pattern is `IADD.CC R, Ra, c0[0x310]`.
- **IR** `LoadStorage` / `StoreStorage` / `AtomicStorage`: the offset is `addr_lo + imm - descriptor_lo`, the size is the descriptor's third word, both read from the constant buffer in the shader (so the descriptor only needs to be right in the cbuf).
- **MSL** (`emitter.cpp`): `hydra_ssbo_load/store/atomic` helpers; loads out of range read 0 and stores/atomics are dropped; small stores use a CAS loop on the word. Buffers are `device uint* ssbo<slot>_<word> [[buffer(n)]]`, `n` = first free slot below 15 (vertex) / 31 (others), listed in `ResourceMapping::storage_buffers`.
- **Binding** (`engines/storage_buffer.hpp`, 3D `configureShaderStage`, `Compute::launch`): reads `{u64 gpu_addr, u32 size}` from the bound cbuf, resolves it through the GMMU (clamped to the mapping) and binds `IRenderer::getGuestMemoryBuffer(range)`.
- **Metal** (`metal::Renderer::getGuestMemoryBuffer`): `newBuffer(bytesNoCopy)` over the host pages, so GPU writes land in guest memory with no copy. Cached by first page (cleared past 256 entries; views keep theirs alive).
- Written buffers invalidate the **buffer cache** for the range at bind time.
- Update 2026-10-18: graphics and compute wait for storage-writing work before
  invalidating the overlapping texture cache. Storage bindings flush earlier
  texture writes into guest memory first; graphics does this before marking
  the current draw's render targets. Partial guest overwrites are reconciled
  before both texture reads and render-target reuse. This is a synchronous
  correctness fallback; GPU-only transfers remain a future optimization.

## Tests
- `testGlobalMemory` (shader-library): every LDG/STG size, out-of-range reads/writes, 8-thread ATOM/RED, signed MIN, EXCH, F32 ADD, INC no-op.
- `testGuestMemoryBuffer` (gpu-engine): a kernel writes through the view into mmap'd memory; page-straddling range, neighbours untouched.
- Corpus: 1634 + 1137 shaders still compile.

## Open
- **Coherence optimization**: replace the synchronous storage-write checkpoint
  with ordered GPU transfers where possible. The current checkpoint is used
  only for shaders with writable storage buffers; read-only bindings do not
  introduce this wait. Cross-format copy completeness remains separate.
- The shader checks the descriptor's size, the host only guarantees the mapped part: a descriptor larger than its GMMU mapping can fault the GPU.
- `ATOM.CAS` (0xeef0), 64-bit and F16x2 atomics, `.E`-less 32-bit addresses, absolute addresses (`RZ` base), constant-buffer SSBO arrays (dynamic descriptor index), generic `LD`/`ST`, `SAFEADD`.
- Tracking is a linear backward scan: an address built across a branch join is not followed. A failed track logs a warning and the access is dropped (loads read 0).
- `HYDRA_PROFILE_COMPUTE` replays storage bindings, other debug replays do not.
