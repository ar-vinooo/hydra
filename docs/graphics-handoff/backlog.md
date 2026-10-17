# Master backlog (from audits + evidence)

Legend: [x] done in tree + tested, [~] done untested, [ ] open

## Done this session
- [x] Stage I/O: front facing, point coord (+origin), primitive ID, layer/viewport (FS in + VS out), point size (fixed/program), clip distances (mask), instance ID without base instance, per-instance divisor (constant step + offset bind)
- [x] S2R: Y direction, thread kill, invocation info defaults, lane ID in FS, clocks 0; CS2R
- [x] PRMT, IMUL/IMUL32I (+CC), P2R, HSET2, HSETP2 rewrite (H_AND, cbuf F32, F32 swizzle), LDC U8/S8/U16/S16 + signed reg offset, FCHK fast path
- [x] writeReg/getReg bounds in all builds (2D B1, compute B18)
- [~] ALLOC_AS_EX argument order (services B1)
- [~] GMMU reservations zero-filled (services B2)
- [x] MME interpreter rewrite (Eden semantics), Regs3D MME scratch, nested macro calls
- [x] Clear state (rect, stencil mask/layer), clears independent of draw state (3D-BE B1/B9/B10)
- [x] XMAD cbuf PSL/MRG; LDC small ints; TEXS/TLDS/TLD4S/TXD/TXQ fixes; shared CAS/8-16 bit stores
- [x] Format crashes: RGBA32Sint, X8Z24, 0x40-0x47/unknown → Invalid (no abort), A2R10G10B10 → BGR10A2, X1R5G5B5/Z/O variants, invalid RT skipped (3D/2D)
- [x] TIC table: +26 Eden encodings (BC6H, BC7 sRGB, BC4 snorm, RG8 U/S, RGB10A2 U, E5B9G9R9, ETC2/EAC x10, depth alternates, S8Z24, X8Z24, R32B24G8, A5B5G5R1)
- [x] Packed 16-bit formats: A1B5G5R5/B5G6R5/A4B4G4R4/A5B5G5R1 value-remapped swizzles (TEX D-3/D-4)
- [x] Z24S8 (stencil low) vs S8Z24 (stencil high) separate host formats + upload decoders (TEX D-19); Z32F separate cache group (D-6)
- [x] Sampler/swizzle converters total; TSC mag filter 3 bits (D-11)
- [x] Compute launch takes buffer cache lock (B4); GMMU shared_mutex + per-thread lookup cache (B5)
- [x] Index buffer FIRST/limit (B6); decoded index buffers content-checked (B7); u8/u32 input types, fan/polygon→list, quad strip, line loop
- [x] Texture shapes legalized for Metal (array promotion, level clamp, 1D single level, non-square cube/oversize rejected) (TEX D-15)
- [x] Collision-safe caches: exact texture group keys, storage/view probing with descriptor checks, CacheBase equals for sampler/pipeline/render pass/Metal caches (TEX D-7)
- [x] Conversions rewritten per Eden: F2I (RN-even, clamp, NaN→0, H1, 16/64-bit), F2F (H1, F16 low half, SAT, F64<->F32 exact), I2I (SAT bit 50, sign bit 13, selector, zero-extended truncation), I2F (selector, 64-bit source, least-value neg/abs)
- [x] MUFU.RCP64H/RSQ64H seeds (no host abort); getLogical/getConstantStr vectors total; half cbuf operands as F32; HADD2.R neg_a bit 43
- [x] Inline-to-memory rewrite: LAUNCH_DMA reset, byte-exact, pitch lines, block linear, multi-mapping writes (2D B2 / compute B8)
- [x] DMA pitch destination range for single-line copies (2D B4)
- [x] 2D compressed alias copy only with compressed overlap and flushed source (2D B3)
- [x] (Codex) CC writes + BRA/EXIT CC flow tests; GL depth funcs; blend constant/SRC1 factors, no blend on integer RTs; sampler LOD bias/anisotropy/wrap; SPH omap fragment outputs + depth register; PSETP 2nd dst; MOV imm sext; LEA.HI cbuf mask; BFE extract guards; fmin/fmax
- [x] MSL block dispatch: tests run blocks through run_program; unused temp union removed; default dispatch case
- [x] BFE.BREV; `!` for bool NOT
- [x] CLAMP_OGL → border with linear filtering, edge with nearest; nearest sampler variant for integer textures; no per-sampler logs
- [x] Cross-format texture copies: same format blit, same texel size via view, single-plane depth/color via buffer, others skipped with log (TEX D-5)
- [x] Alpha test (func/ref in fragment shader key, discard on RT0 alpha)
- [x] Viewport: LOWER_LEFT offset flips by surface height, scissor flipped, viewport transform disable (window-space VS positions converted in the shader, tested), depth range defaults 0..1 + clamp (3D-FE B18-B20)
- [x] SSCALED attributes signed; vertex sizes G8R8/X8B8G8R8/A8; compact draws 0x485/0x486 and 0x5F9-0x5FE (3D-FE B9 partly, B21, B22)
- [x] Dual-source blending: SRC1 factors on RT0 export shader output 1 as `color(0), index(1)` (3D-BE B8)
- [x] 32-bit signed IR arithmetic wraps through unsigned bits (no signed-overflow UB in MSL)
- [x] Closest Metal border color preset (was snapping wrongly, TEX D-9 partly)
- [x] Perf/stability: TIC format conversion cached (was a ~130-way compare chain per texture per draw), nearest samplers memoized, GPU lookup benchmark in xci tests (5 ns vs 8.6 ns bare map), warning spam removed (texture combining, MODIFY mappings), buffer-snapshot and thread-wait tests updated to current semantics
- [x] Raster: SET_RASTER_ENABLE discard (power-on 1), cull FRONT_AND_BACK skips polygons, winding always updated, depth bias halved + POINT/LINE/FILL enables, depth clamp from VIEWPORT_CLIP_CONTROL geometry clip (Eden), vertex stream index bound (3D-FE B15-B17, B24, B25, B28)

- [x] DMA: semaphores (1/4 word releases), BL→BL, transfer NONE; puller semaphores + host fences behind `HYDRA_GPU_FENCES` (default off)
- [x] Control flow CAL/RET/PRET/PCNT/CONT; TMML; SULD/SURED/SUST.D/SUATOM variants
- [x] Conditional rendering TRUE/FALSE/override (compare modes still need visibility buffers)
- [x] 2D: signed fixed point, exact du/dx extents, SAMPLE_MODE filter, integer blit pipelines, no source layer from word 0x90
- [x] Shader corpus tool (`hydra-shader-corpus-tests`)

## P0 — crash / UB / gross corruption

## Known test limitations
- hydra-xci-tests FIXTURES: the XCI part needs `prod.keys` in the fixture dir (fails there by design here); `--check-gpu-memory` aborts on DiskFile("") during system init (environment)

## P1 — widespread correctness
- [ ] Primitive restart index remap / disable (3D-FE B23); inline index draws 0x57A/0x57C/0x4C1 and DRAW_AUTO (B9)
- [ ] ALU: FMZ; IADD3/ISCADD/LEA in u32; BFE signed replicate for offset >= 32; IADD32I.PO
- [ ] Exact sampler border colors (shader emulation, TEX D-9); unnormalized coordinates (TEX P2-10)
- [ ] Global memory + SSBO end to end
- [ ] Report semaphores deferred to completion, real occlusion counts
- [ ] 2D: sample origin CORNER, clip rectangle, ROP/operations (SRCCOPY only), 2D slices
- [ ] DMA slices; PEXIT/JMP; TXD dims, TEX AOFFI
- [ ] Buffer queue rewrite (services B9-B13), vsync 60 Hz pacing (B14)

## P2 — completeness
- [ ] FP64 soft-float (DADD/DMUL/DFMA/DMNMX/DSET/DSETP)
- [ ] Video ops (VMAD/VADD/VABSDIFF/VMNMX/VSET/VSETP/VSHL/VSHR)
- [ ] Geometry shader emulation, tessellation
- [ ] Logic op, MSAA, alpha-to-coverage, sample shading
- [ ] Transform feedback
- [ ] Texture formats: BC6H, BC7 sRGB, ETC2/EAC, E5B9G9R9...
