# Graphics capability coverage

Snapshot: 2026-10-18. This records inspectable implementation/test evidence, not blanket Switch compatibility. Gameplay runs belong to the user and were not performed during this continuation.

`VERIFIED` means the named subset has result-checking automated tests. It only
claims NVIDIA guest semantics when the evidence explicitly cites NVIDIA
documentation or hardware traces. `IMPLEMENTED_UNVERIFIED` means code exists
without the corresponding result check. `PARTIAL` means the family still has
missing modes or approximations. `MISSING` means the end-to-end implementation
is absent. Metal tests prove host execution, not undocumented guest behavior;
another emulator is never semantic proof.

| Subsystem / feature | Classification | Evidence / remaining limit |
|---|---|---|
| MME ALU, parameters, delay slots and nested calls | VERIFIED | `gpu_engine_tests.cpp` macro groups; shadow RAM/replay and every PFIFO burst mode are not established by these tests. |
| Register method storage and bounds | VERIFIED | Register-write tests and macro register reads; inline/restart method offsets have engine tests. |
| Inline-to-memory pitch/block-linear writes | VERIFIED | Engine tests check partial words, launch reset, pitch and block-linear byte placement. |
| Indexed topology conversion and restart | VERIFIED | Decoder buffers checked for u8/u16/u32, custom/disabled/fixed restart and rewritten sources; rendered pixel coverage remains unverified. |
| Inline indexed method accumulation / END lifecycle | VERIFIED | Engine tests inspect packed indices and single END consumption with rasterization disabled. |
| Inline indexed pixel rendering | IMPLEMENTED_UNVERIFIED | Shared regular indexed path is wired; no dedicated rasterized method-replay test yet. |
| DRAW_AUTO byte-count fallback | VERIFIED | NVIDIA `clb197.h` defines `DRAW_AUTO_BYTE_COUNT` and the 12-bit stride; the method test checks the Metal-facing `byte_count / stride` conversion, vertex-zero fallback and state restoration. GPU-produced transform-feedback counts remain absent. |
| Non-indexed restart, topology-change restart and inline alignment | MISSING | Non-indexed restart is a hardware no-op; topology-change and alignment transitions remain open pending verified semantics. |
| Raster/viewport/scissor/instancing | PARTIAL | Metal raster-state and selected shader tests exist; guest-register viewport behavior has gameplay-dependent conventions and incomplete engine-level coverage. |
| Clear masks/layers/state isolation | VERIFIED | Engine suite performs Metal readback for clear areas and attachment preservation. |
| Depth compare and blend families | PARTIAL | GL depth enums, constant/SRC1 factors and integer-RT restrictions implemented; alpha/dual-source shader tests exist. Logic ops and MSAA are missing. |
| Shader stage I/O and selected system values | VERIFIED | Shader-library coverage checks front facing, point coordinates/size, clip distances, layer/viewport outputs and special registers. General geometry/tessellation remains absent. |
| Integer, conversion and half ALU | PARTIAL | Result tests cover conversions, PRMT/IMUL/P2R/XMAD, half operands, FMZ, BFE edges, IADD32I.PO and wrapping operations. Complete rounding, FTZ, CC and all modifier combinations are not established. |
| Ordinary CC flow, subroutines, PEXIT and direct JMP | VERIFIED | Shader-library GPU tests include predicates, flow flags, nesting, exit targets and program/header relocation. |
| All control-flow variants | PARTIAL | Constant-buffer/indirect targets and special CSM tests remain incomplete. |
| Texture instructions | PARTIAL | TXQ/TEXS/TLDS/TXD/TMML and surface-access tests cover selected variants, including 3D/cube TXD and AOFFI cases; sparse, multisample and other modifier combinations remain open. |
| Unnormalized texture coordinates | VERIFIED | TEX/TXD GPU results, TMML fragment compilation and shader-key tests; integer reads and cube directions are preserved. |
| Sampler behavior | PARTIAL | Bias, anisotropy, CLAMP_OGL, integer nearest and shader-emulated custom float/integer/sRGB/depth borders have Metal result tests. NVIDIA confirms the TSC fields and address-mode encodings, but several border conversion/swizzle rules remain guest-semantics unverified. |
| Texture formats, copies and readback layouts | PARTIAL | Allocation and packed-swizzle tests cover reachable formats. G4R4/RGB32 upload expansion, equal-byte compressed alias copies, packed depth/stencil readback and 3D block-linear flushes are tested; every cross-format pair is not supported. |
| Cache collision handling | PARTIAL | Texture storage/views and selected descriptor caches are tested; not every cache family has exact-descriptor collision checks. |
| Shared/local memory and image atomics | PARTIAL | Shared CAS, subword stores and selected surface access have result tests. Complete image formats/atomic variants are not established. |
| Global LDG/STG/ATOM/RED/CAS and SSBO coherence | PARTIAL | LDG/STG/ATOM/RED run on storage buffers found through NVN descriptors and alias guest memory (shader and engine tests). GPU writes are visible to the buffer cache (live-memory views); texture-cache invalidation, 64-bit atomics and indirect descriptors are open; no dumped Crash 4 shader uses it. |
| DMA transfers | PARTIAL | Engine tests cover pitch/block-linear copies, no-transfer launches and semaphore releases. Slice/remap combinations and completion ordering remain incomplete. |
| 2D blits | PARTIAL | Signed fixed point, filter/integer pipelines, clipping and CORNER calculations exist. Clip tests check geometry, not pixels; ROP/operations and slice modes remain open. |
| ZPASS queries and completion-ordered payload/counter reports | VERIFIED | GPU engine tests check actual depth/scissor coverage, indexed draws, reset epochs, clears, cross-submission accumulation, result storage rollover/folding, report sizes, unmapped tails and preceding GPU writes. Report publication waits for GPU completion. |
| Local conditional rendering | PARTIAL | CONDITIONAL/EQ/NE, both report words, high address bits, split mappings and overrides have host tests, including actual query reports. NVIDIA documents the methods, but the comparison structure layout and EQ/NE interpretation still need hardware/NVN confirmation. |
| Remaining GPU fence/report semantics | PARTIAL | Acquire and reduction have engine tests. Host fences remain optional; awaken/trap, other counters and owned asynchronous report publication are not implemented. |
| Buffer queues, presentation and vsync | PARTIAL | Slot state machine, host-completion release and a fixed 60 Hz vsync thread exist and are unit tested; GPU-fence-accurate release and gameplay validation remain. |
| FP64 arithmetic / video instructions | PARTIAL | DADD/DMUL/DFMA/DMNMX/DSET/DSETP and exact F64 conversions are soft float, tested bit-exactly against the host in all rounding modes. VMAD/VMNMX/VSETP exist; the other video ops have unverified semantics and are not implemented. |
| General geometry / tessellation / transform feedback | MISSING | Layer-forwarding heuristic is a limited special case. |
| Logic ops / MSAA / alpha-to-coverage / sample shading | MISSING | No complete guest-to-Metal implementation or result-checking coverage. |

Build/test commands are in `docs/graphics-handoff/README.md`. Run GPU suites with `MTL_DEBUG_LAYER=1`. Current test passes do not replace Crash 4, Jump Force, CTR and Mario Kart 8 gameplay checks. XCI fixture/key and GPU-memory-init limitations remain as documented in the handoff.
