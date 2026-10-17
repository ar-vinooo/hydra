# Graphics capability coverage

Snapshot: 2026-10-17. This records inspectable implementation/test evidence, not blanket Switch compatibility. Gameplay runs belong to the user and were not performed during this continuation.

`VERIFIED` means the named subset has result-checking automated tests. `IMPLEMENTED_UNVERIFIED` means code exists without the corresponding result check. `PARTIAL` means the family still has missing modes or approximations. `MISSING` means the end-to-end implementation is absent. A verified subset does not make its entire subsystem verified.

| Subsystem / feature | Classification | Evidence / remaining limit |
|---|---|---|
| MME ALU, parameters, delay slots and nested calls | VERIFIED | `gpu_engine_tests.cpp` macro groups; shadow RAM/replay and every PFIFO burst mode are not established by these tests. |
| Register method storage and bounds | VERIFIED | Register-write tests and macro register reads; inline/restart method offsets have engine tests. |
| Inline-to-memory pitch/block-linear writes | VERIFIED | Engine tests check partial words, launch reset, pitch and block-linear byte placement. |
| Indexed topology conversion and restart | VERIFIED | Decoder buffers checked for u8/u16/u32, custom/disabled/fixed restart and rewritten sources; rendered pixel coverage remains unverified. |
| Inline indexed method accumulation / END lifecycle | VERIFIED | Engine tests inspect packed indices and single END consumption with rasterization disabled. |
| Inline indexed pixel rendering | IMPLEMENTED_UNVERIFIED | Shared regular indexed path is wired; no dedicated rasterized method-replay test yet. |
| Non-indexed restart, topology-change restart, inline alignment, DRAW_AUTO | MISSING | Explicitly open in the handoff backlog; DRAW_AUTO also depends on transform feedback. |
| Raster/viewport/scissor/instancing | PARTIAL | Metal raster-state and selected shader tests exist; guest-register viewport behavior has gameplay-dependent conventions and incomplete engine-level coverage. |
| Clear masks/layers/state isolation | VERIFIED | Engine suite performs Metal readback for clear areas and attachment preservation. |
| Depth compare and blend families | PARTIAL | GL depth enums, constant/SRC1 factors and integer-RT restrictions implemented; alpha/dual-source shader tests exist. Logic ops and MSAA are missing. |
| Shader stage I/O and selected system values | VERIFIED | Shader-library coverage checks front facing, point coordinates/size, clip distances, layer/viewport outputs and special registers. General geometry/tessellation remains absent. |
| Integer, conversion and half ALU | PARTIAL | Result tests cover conversions, PRMT/IMUL/P2R/XMAD, half operands, FMZ, BFE edges, IADD32I.PO and wrapping operations. Complete rounding, FTZ, CC and all modifier combinations are not established. |
| Ordinary CC flow, subroutines, PEXIT and direct JMP | VERIFIED | Shader-library GPU tests include predicates, flow flags, nesting, exit targets and program/header relocation. |
| All control-flow variants | PARTIAL | Constant-buffer/indirect targets and special CSM tests remain incomplete. |
| Texture instructions | PARTIAL | TXQ/TEXS/TLDS/TXD/TMML and surface-access tests cover selected variants; 3D/cube TXD and full AOFFI coverage remain open. |
| Unnormalized texture coordinates | VERIFIED | TEX/TXD GPU results, TMML fragment compilation and shader-key tests; integer reads and cube directions are preserved. |
| Sampler behavior | PARTIAL | Bias, anisotropy, CLAMP_OGL and integer nearest variants exist; arbitrary border colors are approximated by Metal presets. |
| Texture formats and upload layouts | PARTIAL | Allocation tests cover reachable formats, with selected packed-swizzle and depth-layout upload checks. G4R4/RGB32 upload expansion is tested; broad readback/cross-format coherence remains incomplete. |
| Cache collision handling | PARTIAL | Texture storage/views and selected descriptor caches are tested; not every cache family has exact-descriptor collision checks. |
| Shared/local memory and image atomics | PARTIAL | Shared CAS, subword stores and selected surface access have result tests. Complete image formats/atomic variants are not established. |
| Global LDG/STG/ATOM/RED/CAS and SSBO coherence | PARTIAL | LDG/STG/ATOM/RED run on storage buffers found through NVN descriptors and alias guest memory (shader and engine tests). GPU writes are visible to the buffer cache (live-memory views); texture-cache invalidation, 64-bit atomics and indirect descriptors are open; no dumped Crash 4 shader uses it. |
| DMA transfers | PARTIAL | Engine tests cover pitch/block-linear copies, no-transfer launches and semaphore releases. Slice/remap combinations and completion ordering remain incomplete. |
| 2D blits | PARTIAL | Signed fixed point, filter/integer pipelines, clipping and CORNER calculations exist. Clip tests check geometry, not pixels; ROP/operations and slice modes remain open. |
| ZPASS queries and completion-ordered payload/counter reports | VERIFIED | GPU engine tests check actual depth/scissor coverage, indexed draws, reset epochs, clears, cross-submission accumulation, result storage rollover/folding, report sizes, unmapped tails and preceding GPU writes. Report publication waits for GPU completion. |
| Local conditional rendering | VERIFIED | CONDITIONAL/EQ/NE, both report words, high address bits, split mappings and overrides have tests, including actual zero/nonzero query reports. |
| Remaining GPU fence/report semantics | PARTIAL | Host fences remain optional. Acquire/reduction/awaken/trap and other counters are incomplete; owned asynchronous report publication is not implemented. |
| Buffer queues, presentation and vsync | PARTIAL | Slot state machine, host-completion release and a fixed 60 Hz vsync thread exist and are unit tested; GPU-fence-accurate release and gameplay validation remain. |
| FP64 arithmetic / video instructions | PARTIAL | DADD/DMUL/DFMA/DMNMX/DSET/DSETP and exact F64 conversions are soft float, tested bit-exactly against the host in all rounding modes. VMAD/VMNMX/VSETP exist; the other video ops have unverified semantics and are not implemented. |
| General geometry / tessellation / transform feedback | MISSING | Layer-forwarding heuristic is a limited special case. |
| Logic ops / MSAA / alpha-to-coverage / sample shading | MISSING | No complete guest-to-Metal implementation or result-checking coverage. |

Build/test commands are in `docs/graphics-handoff/README.md`. Run GPU suites with `MTL_DEBUG_LAYER=1`. Current test passes do not replace Crash 4, Jump Force, CTR and Mario Kart 8 gameplay checks. XCI fixture/key and GPU-memory-init limitations remain as documented in the handoff.
