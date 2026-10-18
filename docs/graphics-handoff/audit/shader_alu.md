# Hydra Maxwell (SM 5.3 / GM20B) shader translation audit: ALU and non-memory instructions

> Evidence policy (2026-10-18): NVIDIA documentation/headers define guest
> behavior; Apple Metal documentation, SDK headers and validation define host
> behavior. Eden and other emulators are historical comparators only. Any
> conclusion based solely on them is unverified and must be corroborated.

Read-only audit of `src/core/hw/tegra_x1/gpu/renderer/shader_decompiler/` (decoder, IR, MSL codegen) and `tests/shader_library_tests.cpp`.
Nothing in the repository was modified, built or run. Real-game evidence comes from guest shader words that Hydra had already dumped under `build/` (see Appendix A).

**Snapshot:** working tree on branch `arvino` (HEAD 3e8c4cc2), 2026-10-17 between 09:45 and 10:25 WIB. The tree was being edited during the audit. Appendix D lists the files that changed while I was reading them. Line numbers refer to the snapshot and may drift. `msl/emitter.cpp` was edited three times during the audit, so its citations also name the function.

**References (treated as ground truth):**
- **Eden** (yuzu fork): `refs/yuzu/src/shader_recompiler/frontend/maxwell/{maxwell.inc, translate/impl/*.cpp}` and `backend/spirv/*`. Here `refs/` means `/private/tmp/claude-501/-Users-arvino-Documents-Project-hydra/0e906531-f45a-4e9b-b0bc-dcb303edaa1b/scratchpad/refs/`.
- **NAK** (Mesa): `/Users/arvino/Documents/Project/hydra/build/mesa-sm50.rs` (encoders) and `/Users/arvino/Documents/Project/hydra/build/mesa-ir.rs` (constant-fold semantics).
- **envydis**: `/Users/arvino/Documents/Project/hydra/build/gm107.c`.

The references disagree in a few places. Section (b) records each conflict and which side Hydra follows.

Hydra paths below are relative to `src/core/hw/tegra_x1/gpu/renderer/shader_decompiler/` unless written in full.

**Class legend**
- `VERIFIED`: decoded and emitted correctly, and a test in `tests/shader_library_tests.cpp` executes the guest encoding on Metal and checks the result.
- `IMPLEMENTED_UNVERIFIED`: decoded and emitted with matching bit positions and semantics, but no test checks it.
- `PARTIAL`: some forms or modifiers are missing, approximated or wrong.
- `MISSING`: not decoded (`COMMENT_NOT_IMPLEMENTED`, unknown instruction) or a modifier is silently ignored.
- `UNKNOWN`: correctness depends on unverified host (Metal) behaviour.
- `N/A`: not a Maxwell SM5x instruction. Excluded from the counts.

---

## Executive summary

**Rows in table (a): 163 classified, plus 3 N/A (HMNMX2, PLOP3, IDP).**

| Class | Rows |
|---|---|
| VERIFIED | 18 |
| IMPLEMENTED_UNVERIFIED | 55 |
| PARTIAL | 26 |
| MISSING | 62 |
| UNKNOWN | 2 |

These counts reflect the working tree at 10:25. Against HEAD, PRMT, IMUL, P2R and HSET2 would be MISSING, and FCHK MISSING instead of PARTIAL.

**Confirmed real-game breakage.** Both items below come from Crash Bandicoot 4 guest shaders dumped by Hydra (Appendix A):

1. **XMAD constant-buffer ("cr", 0x4E..) form ignores `.PSL` (bit 55) and `.MRG` (bit 56).**
   - Where: `XMAD.MRG Rx, Ra, c[..].H1, RZ` occurs 18 times, in 15 of 73 decoded Crash 4 shaders.
   - Scale: the MSL output of the IMUL-by-uniform idiom appears in 520 `board-live` and 563 `hot-debug` dumped shaders. 416 of those are vertex shaders, e.g. `instance_index * c5[0x04]`.
   - Effect: the 32-bit product is wrong whenever the register operand is 65536 or more.
2. **CC-conditioned branch is executed unconditionally.**
   - Where: Crash 4 fragment shader `0x1b56830`, at three sites.
   - Sequence: `FSET.EQ.FTZ.AND.CC RZ, R16, R17, PT` → `@P0 BRA CC.NEU`.
   - Hydra drops the FSET CC write (`float_comparison.cpp:7`) and ignores the branch flow test (`decoder.cpp:449`). The branch is therefore taken whenever P0 is set, instead of P0 AND CC.NEU.

**Other confirmed decode or semantic bugs** (quoted in section b):
- **I2I `.SAT`:** decoded from bit 51 instead of 50, and never applied.
- **HADD2 (register) `-A`:** read from bit 38 instead of 43, so it is ignored.
- **PSETP second destination:** computes `!(a op b)` instead of `(!a) op b`.
- **MOV (imm):** not sign-extended.
- **LEA.HI.X (cbuf):** not decoded; the mask is 0xFE instead of 0xFC.
- **Half-precision constant-buffer operands:** HADD2/HMUL2/HFMA2 C/CR/RC treat the 32-bit word as packed `half2`. Eden and old yuzu use it as one F32 scalar.
- **F2F/F2I with an F16 source:** the `.H1` selector (bit 41) is ignored.
- **F2I/F2F `.ROUND`:** emitted as MSL `round()` (ties away from zero) instead of round-to-nearest-even. 70 conversions use it in the dumps.
- **F2I clamping:** no clamp and no NaN→0 handling. 16-bit and 8-bit results are written to only part of the register (`state.r[n]._u16 = …`, 70 sites in the dumps).
- **Host crash risks:**
  - `MUFU.RCP64H/RSQ64H` emits an undefined value → `LOG_FATAL` → `abort()`.
  - `I2F` from S64/U64 reaches `unreachable()`, which is undefined behaviour in release builds.
  - `F2F.F16.SAT` emits `clamp(half, float, float)`, which probably fails MSL compilation.
  - A half2 `RZ` operand emits the literal `INVALID`, which fails MSL compilation.

**Already fixed in the working tree during the audit** (bugs present in HEAD, not yet committed):
- HSETP2 `.H_AND` was wrong: `~bool` plus the wrong formula.
- HSETP2 constant-buffer operand was read as half2.
- HSET2 was missing.

These now match Eden (`half_comparison.cpp` rewritten at 10:15). At 10:19, PRMT, IMUL/IMUL32I and P2R were wired in a new `decoder/integer_misc.cpp`, using MSL helpers added at 10:18. FCHK became a fast-path stub at the same time. These are all still untested.

**Test coverage of the ALU path is thin.** Only these are executed and checked:
- IADD (+CC, X via XMAD)
- XMAD R base (+CC/X)
- I2I.CC (U32)
- CSETP.NEU
- R2P (PR mode)
- SHL/SHR, SHF, BFI, LEA
- ICMP/FCMP (all variants)
- ISET/FSET (R form)
- FSWZADD (IR level only, not through the decoder)

Nothing tests FFMA/FMUL/FADD/MUFU/F2F/F2I/I2F/FMNMX/FSETP/ISETP/LOP/LOP3/BFE/IADD3/half ops, even though FFMA/FMUL/FADD/MUFU/F2F/F2I/I2F/FMNMX account for most instructions in real shaders.

---

## (a) Opcode × variant inventory and classification

Column "Ref" gives the semantic or bit-position reference. "E" means Eden `translate/impl/<file>`. "N" means NAK `mesa-sm50.rs`. "env" means envydis `gm107.c`.

### a.1 Float32 arithmetic

| Opcode / variant | Class | Hydra evidence | Ref | Notes |
|---|---|---|---|---|
| FADD R/C/I base (abs/neg A,B; .SAT) | IMPLEMENTED_UNVERIFIED | `decoder/float_arithmetic.hpp:7-22` (neg_b 45, abs_a 46, neg_a 48, abs_b 49, sat 50); `float_arithmetic.cpp:8-25,95-118` | E `floating_point_add.cpp:36-64`; N 504-528 (`set_reg_fmod_src(20..28, 49, 45…)`, `(8..16, 46, 48…)`); env 2006/2080/2126 | Bits match all three references. No test. |
| FADD32I (abs/neg A,B) | IMPLEMENTED_UNVERIFIED | `float_arithmetic.hpp:24-36` (neg_b 53, abs_a 54, ftz 55, neg_a 56, abs_b 57); `.cpp:120-125` | E `floating_point_add.cpp:66-79`; N 496-502; env 2167 | 578 uses in the Crash 4 sample. |
| FADD rounding .RM/.RP/.RZ | MISSING | `float_arithmetic.hpp:12` decoded; `.cpp:7` "TODO: round_mode" | E `floating_point_add.cpp:24-28` (FpControl.rounding) | Rare (0 non-RN FADD in the sample). |
| FADD .FTZ | MISSING | `.cpp:7` TODO; result depends on Metal's denormal mode | E `floating_point_add.cpp:27` | Used on 3567 FADDs in the sample. Effect only below 1.2e-38. |
| FADD .CC | MISSING | `float_arithmetic.hpp:16` decoded, never used | E `floating_point_add.cpp:19-21` (throws) | Silently ignored. |
| FMUL R/C/I base (neg_b, .SAT, scale D2…M2) | IMPLEMENTED_UNVERIFIED | `float_arithmetic.hpp:40-63`; `.cpp:28-70,127-150` | E `floating_point_multiply.cpp:13-44,86-112`; env tab5c68_1 (bits 41-43) | Scale is applied as a second multiply, which is exact for powers of two. 36 scaled FMULs in the sample. |
| FMUL32I (.SAT) | IMPLEMENTED_UNVERIFIED | `float_arithmetic.hpp:65-76`; `.cpp:152-156` | E `floating_point_multiply.cpp:114-124`; N 658-670 | 1650 uses in the sample. |
| FMUL .FMZ (fmz=2) | MISSING | `float_arithmetic.hpp:57` `BitField64<u32, 44, 2> fmz; // TODO`, `:72` (32I) | E `floating_point_multiply.cpp:71-79` | Silent: `0×Inf/NaN` gives NaN instead of 0. Not in the Crash 4 sample. |
| FMUL .FTZ / rounding / .CC | MISSING | `float_arithmetic.cpp:27` "TODO: round_mode, fmz, write_cc" | E `floating_point_multiply.cpp:54-70` | FTZ on 8033+1650 FMULs in the sample. |
| FFMA R / RC / CR / I base (neg_b, neg_c, .SAT) | IMPLEMENTED_UNVERIFIED | `float_arithmetic.hpp:78-124` (RC: b=reg39, c=cbuf); `.cpp:73-91,158-199` | E `floating_point_fused_multiply_add.cpp:46-76`; N 532-603; env 2034/2049/2104/2152 | Operand order matches Eden. Most frequent opcode (10 202 FFMAs in the sample). |
| FFMA32I (neg_a, neg_c, c=dst) | IMPLEMENTED_UNVERIFIED | `float_arithmetic.hpp:126-139`; `.cpp:201-207` | E `floating_point_fused_multiply_add.cpp:78-91`; env 2166 | |
| FFMA .FMZ | MISSING | `float_arithmetic.hpp:88` `fmz; // TODO`; `.cpp:72` | E `floating_point_fused_multiply_add.cpp:31-39` (select c when a or b is 0) | Silent. |
| FFMA .FTZ / rounding / .CC | MISSING | `.cpp:72` TODO | E `…fused_multiply_add.cpp:19-29` | |
| FMNMX R/C/I (abs/neg, pred selects min/max) | IMPLEMENTED_UNVERIFIED | `decoder/float_min_max.hpp:7-22`; `float_min_max.cpp:8-29` | E `floating_point_min_max.cpp:10-46`; N 605-637 | 1106 uses in the sample. |
| FMNMX NaN rule (return the non-NaN operand) | UNKNOWN | emitted as MSL `min()`/`max()` (`codegen/lang/emitter.cpp:455-463`) | Eden uses `OpFMin/OpFMax` (`backend/spirv/emit_spirv_floating_point.cpp:87-101`) | MSL guarantees the NaN rule only for `fmin`/`fmax`; see c.3. |
| FMNMX .FTZ / .CC | MISSING | `float_min_max.cpp:7` TODO | E `…min_max.cpp:25-27` | |
| FSET R base (16 cmp ops, bop, mask/.BF) | VERIFIED | `decoder/float_comparison.hpp:8-26`; `.cpp:8-40,131-138` | E `floating_point_compare_and_set.cpp:11-62`; env 2036 | `testSetRegisterResults` (shader_library_tests.cpp:1636). Covers only GT/LT, .BF, PT-AND. |
| FSET C/I; bop OR/XOR with a real predicate | IMPLEMENTED_UNVERIFIED | `float_comparison.cpp:140-160` | same | |
| FSET .CC | MISSING | `float_comparison.cpp:7` "TODO: write_cc, ftz" | E `…compare_and_set.cpp:51-61` (Z, S=!Z unless BF, C=O=0) | **Used in Crash 4** (3×, feeds `BRA CC.NEU`). |
| FSET / FSETP .FTZ | MISSING | `float_comparison.cpp:7,42` | E `…compare_and_set.cpp:31-35` | |
| FSETP R/C/I (cmp, bop, dst A/B, abs/neg) | IMPLEMENTED_UNVERIFIED | `float_comparison.hpp:28-45`; `.cpp:43-67,162-192` | E `floating_point_set_predicate.cpp:11-44`; N 866-903 | Shares `getFloatCmp` with FCMP, which is tested. Bits match NAK (neg_b 6, abs_a 7, neg_a 43, abs_b 44). |
| FCMP R / RC / CR / I, all 16 ops incl. NaN and FTZ-subnormal | VERIFIED | `float_comparison.cpp:71-129` | E `floating_point_compare.cpp:11-52`; env 2025/2044/2096/2143 | `testCompareSelect(device, true)` (shader_library_tests.cpp:1446). |
| FSWZADD (swizzle modes, operands) | PARTIAL | `decoder/decoder.cpp:670-688`; MSL `hydra_swizzle_add` (`msl/emitter.cpp`, emitHeader, `lut_a {1,1,-1,0}`, `lut_b {1,-1,1,1}`) | E `floating_point_swizzled_add.cpp:10-41`; Eden LUT `backend/spirv/spirv_emit_context.cpp:1487-1489` `{-1,1,-1,0}/{-1,-1,1,-1}`; N 905-945 | Modes 1/2 (derivatives, the only ones in the dumps: 0x99/0xA5) agree with Eden. Modes 0/3 conflict with Eden (section b). .NDV, rounding, .FTZ and .CC are ignored. Tested at IR level only (`testQuadDerivatives`, :901). |
| FCHK R/C/I | PARTIAL | HEAD: NOTIMPL (predicate left stale). Working tree (10:19): `decoder/integer_misc.cpp:154-169` always writes `false` (take the fast path); non-DIVIDE checks are logged | Eden also missing (`not_implemented.cpp`); env 2001 (check bits 39-44) | Approximation: never selects the precise-division path. Rare. |
| MUFU COS/SIN | IMPLEMENTED_UNVERIFIED | `decoder/multifunction.cpp:32-37`; MSL `precise::sin/cos` | E `floating_point_multi_function.cpp:24-67` | RRO is a pass-through, so the full-range sin of the original value is correct. |
| MUFU EX2/LG2/RCP/RSQ/SQRT | IMPLEMENTED_UNVERIFIED | `multifunction.cpp:38-58`; MSL `exp2/log2/(1.0/x)/rsqrt/sqrt` | same | Precise (no fast math) and more accurate than MUFU. EX2 359, LG2 385, RCP 314, RSQ 148, SQRT 191 in the sample. |
| MUFU abs/neg/.SAT | IMPLEMENTED_UNVERIFIED | `multifunction.hpp:35-44` (abs 46, neg 48, sat 50) | N 743-770; env 2062 | |
| MUFU RCP64H / RSQ64H | MISSING | `multifunction.cpp:50-55`: logs, then copies an **undefined** value; `codegen/lang/emitter.hpp` `getValueStr` default → `LOG_FATAL` → `abort()` | E throws (`…multi_function.cpp:51-54`) | **Host process abort** if any shader uses it. |
| RRO R/C/I (SINCOS/EX2) | IMPLEMENTED_UNVERIFIED | `multifunction.hpp:12-21`, `.cpp:8-21,70-90` | E `floating_point_range_reduction.cpp:15-38` (imm throws); N 708-741 | |

### a.2 Conversions

| Opcode / variant | Class | Hydra evidence | Ref | Notes |
|---|---|---|---|---|
| F2F F32↔F32 FLOOR/CEIL/TRUNC | IMPLEMENTED_UNVERIFIED | `decoder/conversion.hpp:21-42`; `conversion.cpp:22-53` | E `floating_point_conversion_floating_point.cpp:37-123`; env tab5ca8_0 | 293 F2F.FLOOR in the sample. |
| F2F ROUND (round-to-nearest-even integer) | PARTIAL | `conversion.cpp:32-33` → `opRound` → MSL `round()` (`codegen/lang/emitter.cpp:472-474`) | E uses `FPRoundEven` (`…floating_point.cpp:88-89`) | Ties go away from zero instead of to even. |
| F2F F32→F16 / F16→F32 size conversion | PARTIAL | `conversion.cpp:46-49` writes `state.r[n]._f16` (upper half kept) | E packs `(value, 0)` (`…floating_point.cpp:108-112`) | Rounding-mode bits 39-40 ignored. The dumped NVN code clears the upper half with BFI itself (Appendix A), so impact is low. |
| F2F F16 source .H1 (bit 41) | MISSING | `conversion.hpp:28` `BitField64<bool, 41, 1> sh;` never read | E `…floating_point.cpp:131-139` (selector); N 1791 | `unpackHalf2x16(x).y` reads the wrong half. |
| F2F .SAT | PARTIAL | `conversion.cpp:48` uses `saturateIf` with **F32** constants even when the destination is F16 | E `…floating_point.cpp:104-106` | With an F16 destination this emits `clamp(half, float, float)`, an ambiguous MSL overload, likely a compile error. |
| F2F F64 source/destination | MISSING | `conversion.cpp:13-15` (treated as F32 and logged) | E handles F64 | Metal has no fp64. |
| F2F immediate (F16 source) | PARTIAL | `conversion.cpp:234` shifts imm20 left by 12 for every format | E `…floating_point.cpp:190-197` (low 16 bits) | Rare. |
| F2F .FTZ / .CC | MISSING | `conversion.cpp:21` "TODO: sh, ftz, write_cc" | | |
| F2I F32→S32/U32 TRUNC/FLOOR/CEIL | IMPLEMENTED_UNVERIFIED | `conversion.hpp:60-80`; `conversion.cpp:77-104,239-264` | E `floating_point_conversion_integer.cpp:117-208`; N 1800-1840 | 148 F2I.TRUNC in the sample (4723 trunc→int sites in the dumps). Out-of-range and NaN behaviour is UNKNOWN (c.2). |
| F2I clamp to range and NaN→0 | UNKNOWN | plain `i32(x)`/`u32(x)` cast (`conversion.cpp:98-100`) | E clamps and selects NaN→0 (`…integer.cpp:148-197`) | MSL does not specify out-of-range float→int; see c.2. |
| F2I ROUND (default rounding) | PARTIAL | `conversion.cpp:84-85` → MSL `round()` | E `FPRoundEven` (`…integer.cpp:135-136`) | Used in the dumps for unorm packing (`round(x*255)`) and array-layer selection. |
| F2I →U16/S16 | PARTIAL | `conversion.cpp:98-100` writes `._u16/._i16`; no clamp | E clamps to the 16-bit range and writes 32 bits (`…integer.cpp:62-93,174-203`) | 70 `state.r[n]._u16 = …` writes in the dumps. |
| F2I →U64/S64 | MISSING | `conversion.cpp:61-70` returns a 32-bit type and logs | E writes a register pair | |
| F2I F16 source .H1 (bit 41) | MISSING | `conversion.hpp:68` `sh` unused | E `…integer.cpp:221` (`f2i.base.half`) | |
| F2I abs/neg bit positions | IMPLEMENTED_UNVERIFIED | `conversion.hpp:70,72` (neg 45, abs 49) | N 1810 `set_reg_fmod_src(20..28, 49, 45…)`; env 1996 `ON(45, neg), ON(49, abs)` | Hydra matches NAK and envydis. **Eden has the bits swapped** (section b). |
| F2I .FTZ / .CC | MISSING | `conversion.cpp:76` TODO | E (`.CC` throws) | |
| I2F S32/U32→F32 | IMPLEMENTED_UNVERIFIED | `conversion.hpp:125-144`; `.cpp:193-208,294-319` | E `integer_floating_point_conversion.cpp:64-142`; N 1842-1881 | 140 uses in the sample. |
| I2F 8/16-bit sources with byte/half selectors | IMPLEMENTED_UNVERIFIED | `conversion.cpp:126-147` (`selectSourceBytes`) | E `…floating_point_conversion.cpp:72-85` | Crash 4 logs show `DEBUG byte select f (3, u8)` (B2/B3). The debug environment variable and per-decode `LOG_INFO` should be removed. |
| I2F abs/neg | PARTIAL | `conversion.cpp:198-200` applies neg/abs to the **integer** before conversion | E negates the float after conversion and keeps INT_MIN (`…:103-120`), and uses a small-abs wrap (`:56-62`) | `I2F.U32` with `-src` gives a huge positive value instead of a negative one. |
| I2F S64/U64 source | MISSING | `IntegerFormat2` has no value 3/7 → `toType()` default `unreachable()` (`conversion.cpp:120-121`) | E `…:145-172` | **Undefined behaviour in release builds** (`common/functions.hpp:15-21`). |
| I2F → F16 / F64 destination | PARTIAL | F16 writes `._f16` only; F64 is treated as F32 (`conversion.cpp:13-15,202-204`) | E `…:121-138` | |
| I2F rounding (.RM/.RP/.RZ) / .CC | MISSING | `conversion.cpp:192` TODO | E `…:97-101` | |
| I2I 32→32 base (abs/neg) | IMPLEMENTED_UNVERIFIED | `conversion.hpp:98-123`; `.cpp:149-190,266-292` | E `integer_to_integer_conversion.cpp:61-110` | |
| I2I .CC (Z/S set, C/O cleared) | VERIFIED | `conversion.cpp:167-186` | E `…:104-109` | `testConditionCodes` (:1363), U32→U32 register form only. Used in Crash 4 with CSETP.NEU. |
| I2I to 8/16-bit destination | PARTIAL | `conversion.cpp:159-165` writes `._u8/._u16/._i8/._i16` | E zero-extends to 32 bits (`…:29-34,103`) | The upper bits keep stale register contents. |
| I2I byte selectors | IMPLEMENTED_UNVERIFIED | `conversion.hpp:106`, `.cpp:126-147` | E `…:76-92` | |
| I2I .SAT | MISSING | `conversion.hpp:110` `BitField64<bool, 51, 1> sat;` and `.cpp:161-164` (logs only) | E `:73` sat=50; N 1922 `set_bit(50, self.saturate)`; env 1993 `ON(50, sat)` | Wrong bit and no implementation (section b). |

### a.3 Float64 (none of these are decoded)

| Opcode / variant | Class | Hydra evidence | Ref | Notes |
|---|---|---|---|---|
| DADD R/C/I | MISSING | `decoder.cpp:579,735,813` | E `double_add.cpp` | Metal has no `double`. Needs soft-fp64 (c.11). |
| DMUL R/C/I | MISSING | `decoder.cpp:576,732,810` | E `double_multiply.cpp` | |
| DFMA R/RC/CR/I | MISSING | `decoder.cpp:615,657,766,847` | E `double_fused_multiply_add.cpp` | |
| DMNMX R/C/I | MISSING | `decoder.cpp:585,741,819` | E `double_min_max.cpp` | |
| DSET R/C/I | MISSING | `decoder.cpp:629,779,861` | E `double_compare_and_set.cpp` | |
| DSETP R/C/I | MISSING | `decoder.cpp:612,763,844` | E `double_set_predicate.cpp` | |

### a.4 Half precision (FP16x2)

| Opcode / variant | Class | Hydra evidence | Ref | Notes |
|---|---|---|---|---|
| HADD2 R base (swizzles H1_H0/H0_H0/H1_H1, merge F16/F32/MRG_H0/MRG_H1, abs, neg_b, .SAT) | IMPLEMENTED_UNVERIFIED | `decoder/half_arithmetic.hpp:16-37`; `.cpp:7-70,115-121` | E `half_floating_point_add.cpp:8-80`, helper `half_floating_point_helper.cpp:21-59`; env 1987 | The merge logic matches Eden `MergeResult`. |
| HADD2 R `-A` | PARTIAL | `half_arithmetic.hpp:34` `BitField64<bool, 38, 1> neg_a;` | E `:59` `BitField<43, 1, u64> neg_a;`; env 1987 `ON(43, neg), ON(44, abs)` | Wrong bit (section b). |
| HADD2 C (cbuf operand) | PARTIAL | `half_arithmetic.cpp:128-130` reads cbuf as `VectorType(F16,2)` | E `:90` `Swizzle::F32` (one f32, promoted) | Section b. |
| HADD2 I / HADD2_32I | IMPLEMENTED_UNVERIFIED | `half_arithmetic.hpp:51-78`; `.cpp:134-149` | E `:94-123`; env 1977/2155 | The imm10 packing (`<<6`, sign at bit 29/56) matches Eden. Used in the dumps as constant loads. |
| HADD2 .FTZ | MISSING | `half_arithmetic.cpp:52` TODO | E `:35-39` | |
| HADD2/HMUL2/HFMA2 `.F32` swizzle | PARTIAL | `decoder/const.hpp` `getSwizzledHalf` F32 case converts the operand to f16 first | E promotes the other operands to f32 and rounds once at the end (`half_floating_point_add.cpp:18-28,46-49`) | Double rounding; f32 values above 65504 become Inf before the add. |
| HMUL2 R/I/32I | IMPLEMENTED_UNVERIFIED | `half_arithmetic.hpp:80-139`; `.cpp:73-90,151-185` | E `half_floating_point_multiply.cpp:68-140` | |
| HMUL2 C | PARTIAL | `half_arithmetic.cpp:164-166` (half2 cbuf) | E `:105-106` `Swizzle::F32` | |
| HMUL2 .FMZ / .FTZ | MISSING | `half_arithmetic.hpp:85,136` `fmz; // TODO` | E `:43-56` | |
| HFMA2 R/I/32I | IMPLEMENTED_UNVERIFIED | `half_arithmetic.hpp:141-221`; `.cpp:93-111,187-240` | E `half_floating_point_fused_multiply_add.cpp:88-166`; env 1989/1982/2157 | Bit positions match envydis. |
| HFMA2 RC / CR | PARTIAL | `half_arithmetic.cpp:203-205,214-216` (half2 cbuf) | E `:113,127` `Swizzle::F32` | |
| HFMA2 .FMZ / .FTZ | MISSING | `half_arithmetic.hpp:158,173,187,201,218` `// TODO` | E `:49-62` | |
| Half op with `RZ` source and H1_H0 swizzle | PARTIAL | `getRegisterStr(RZ, half2)` → `getConstantStr` has no vector case → emits `"INVALID"` (`codegen/lang/emitter.hpp:221-253`, `#define INVALID_VALUE "INVALID"` at :20) | | The whole shader fails MSL compilation. Not in the dumps (0 `INVALID`). |
| HSET2 R/C/I | IMPLEMENTED_UNVERIFIED | Was MISSING in HEAD. Working tree: `decoder/half_comparison.cpp:130-150`, dispatched at `decoder.cpp:531,534,550` | E `half_floating_point_set.cpp:9-115` | Added at 10:15 during the audit. Bits (bf 49/53, ftz 50/54) match Eden. No test. |
| HSETP2 R/I (cmp, bop, H_AND, swizzles, FTZ) | IMPLEMENTED_UNVERIFIED | Working tree `half_comparison.cpp:57-126,152-177` | E `half_floating_point_set_predicate.cpp:9-116` | HEAD had a wrong `.H_AND` (section b, B13). Rewritten at 10:15 to match Eden. Compares in f32 (exact). |
| HSETP2 C | PARTIAL | Working tree `half_comparison.cpp:89-98` (cbuf as F32, correct); `:161-162` abs(b) (bit 54) only logged | E `:84-95` applies abs_b 54 | |
| HMNMX2 | N/A | – | not in `maxwell.inc` (SM5x has no HMNMX2) | |

### a.5 Integer arithmetic

| Opcode / variant | Class | Hydra evidence | Ref | Notes |
|---|---|---|---|---|
| IADD R/C/I/32I base + NegA/NegB | VERIFIED | `decoder/integer_arithmetic.hpp:14-39`; `.cpp:8-33,298-325` | E `integer_add.cpp:58-102`; N 1229-1289 | `testIntegerConditions` (:124), five boundary operands. |
| IADD .PO (both neg bits) | IMPLEMENTED_UNVERIFIED | `integer_arithmetic.cpp:20` (+1) | E `integer_add.cpp:35-38,69` | The test loop stops at mode 2. |
| IADD32I .PO (bits 55+56) | MISSING | `decoder.cpp:878` mask `0xfe80` excludes bit 55; PO handler commented out (`decoder.cpp:~874-877`) | E `:94-101`; env 2161 `0x1d80… "po"` | Becomes an unknown instruction (no write). |
| IADD .CC (Z/S/C/O) | VERIFIED | `integer_arithmetic.cpp:21-30` | E `integer_add.cpp:39-53` | Tested. The negated-operand carry follows Eden, which conflicts with NAK; see the IADD.CC-with-negation row and section b. |
| IADD .CC carry with a negated operand | PARTIAL | `integer_arithmetic.cpp:15-16,25` (32-bit `-b`, then carry of `a + (-b)`) | N fold `mesa-ir.rs:3805-3817` "very literal interpretation of 2's compliment … `sum += !src + 1`" | Carry differs when the negated operand is 0. The test (`:157-160`) asserts Hydra's model. |
| IADD .X (carry-in) | PARTIAL | `integer_arithmetic.cpp:19`; NegA/NegB with .X use arithmetic negation | N `IAdd2X` uses **bitwise NOT** for bits 48/49 (`mesa-sm50.rs:1324,1339`; fold `mesa-ir.rs:3842-3853`) | Eden also uses INeg. A 64-bit subtract `IADD.CC lo,-b; IADD.X hi,~b` gives a high word off by one. |
| IADD .SAT | MISSING | `integer_arithmetic.cpp:7` "TODO: saturating mode" | E throws (`integer_add.cpp:18-20`) | Silent. |
| IADD3 R (H0/H1 halves, RS/LS shift), C, I, neg A/B/C | IMPLEMENTED_UNVERIFIED | `integer_arithmetic.hpp:179-226`; `.cpp:187-247,277-296` | E `integer_add_three_input.cpp:50-119`; env 1994 + tab5cc0_* | Uses **I32** adds (c.7). The RS 33-bit carry edge case is missing (E `:38-43`). |
| IADD3 .X / .CC | MISSING | `integer_arithmetic.cpp:203` "TODO: x, write_cc" | E `:72-94` | Silent. |
| ISCADD R/C/I/32I (shift, neg, .PO) | IMPLEMENTED_UNVERIFIED | `integer_arithmetic.hpp:41-51,100-110`; `.cpp:36-56,327-348,453-457` | E `integer_scaled_add.cpp:10-83` | I32 arithmetic (c.7). |
| ISCADD .CC | MISSING | `integer_arithmetic.cpp:35` "TODO: write_cc" | E `:39-46` | |
| IMNMX R/C/I signed/unsigned | IMPLEMENTED_UNVERIFIED | `integer_arithmetic.hpp:228-239`; `.cpp:165-185,251-275` | E `integer_minimum_maximum.cpp:10-48`; N 1453-1492 | Used for BRX bounds; 34 uses in the sample. |
| IMNMX .XLO/.XMED/.XHI (bits 43-44) | MISSING | not decoded | E throws (`:26-28`); env tab5c20_0 | 64-bit min/max only. |
| IMNMX .CC | MISSING | `integer_arithmetic.cpp:164` TODO | E throws | |
| IMUL R/C/I, IMUL32I (.HI, signedness, .CC) | IMPLEMENTED_UNVERIFIED | HEAD: NOTIMPL. Working tree (10:19): `decoder/integer_misc.cpp:67-118` (hi 39 / 53, signs 40-41 / 54-55, cc 47 / 52), low word `u32*u32`, high word via the `IMulHi` helper (`msl/intrinsics.cpp:55-61`); dispatched at `decoder.cpp:591,747,825,872` | Eden missing; N 1405-1451; env 2010/2084/2130/2159 | Bits match NAK and envydis. CC = Z/S with C=O=0 is unreferenced. No test. |
| IMAD / IMADSP / IMAD32I | MISSING | `decoder.cpp:622,625,663,666,772,775,854,857,882` | Eden also missing. IMAD32I location conflict: Eden `"1000 00--"` vs env 2165 `0x1000/0xfc00` | NVN uses XMAD instead. |
| XMAD R base (H0/H1, signedness, full/CLO/CHI) | IMPLEMENTED_UNVERIFIED | `integer_arithmetic.hpp:112-142`; `.cpp:59-162,459-469` | E `integer_short_multiply_add.cpp:28-92`; env 2031 | Only H0×H0 unsigned is tested. |
| XMAD R/I .PSL / .MRG | IMPLEMENTED_UNVERIFIED | `integer_arithmetic.cpp:84-85,149-155` | E `:47-50,67-71` | 40 XMAD_reg.PSL and 67 XMAD_imm.PSL in the sample. |
| XMAD .CBCC | IMPLEMENTED_UNVERIFIED | `integer_arithmetic.cpp:97-102` | E `:59-60` | Every IMUL idiom uses it (40 in the sample). |
| XMAD .CSFU | PARTIAL | `integer_arithmetic.cpp:103-118` (heuristic) | E throws (`:61-62`) | Unverified guess. |
| XMAD .CC / .X (R form) | VERIFIED | `integer_arithmetic.cpp:121-147` | (Eden throws) | `testConditionCodes` (:1363). |
| XMAD RC (b=reg39, c=cbuf) | IMPLEMENTED_UNVERIFIED | `integer_arithmetic.hpp:144-154`; `.cpp:471-483` | E `:94-104`; env 2050 | |
| XMAD CR (cbuf B) .PSL/.MRG | PARTIAL | `integer_arithmetic.hpp:156-164` (no psl/mrg fields); `.cpp:487` passes `false, false` | E `:107-117` `psl 55, mrg 56`; env 2064 `ON(55, psl), ON(56, mrg)` | **Confirmed in Crash 4** (XMAD_cr.MRG: 18 uses in 15 shaders). Section b. |
| XMAD I (imm16) | IMPLEMENTED_UNVERIFIED | `integer_arithmetic.hpp:168-177`; `.cpp:497-506` | E `:120-132`; env 2149 | |
| LEA lo R/C/I (neg, shift) | VERIFIED | `integer_arithmetic.hpp:53-65`; `.cpp:351-390` | E `load_effective_address.cpp:42-105` | `testLea` (:1235). |
| LEA.HI R/C (neg, shift) | VERIFIED | `integer_arithmetic.hpp:67-98`; `.cpp:396-451` | E `:10-40,73-93` | `testLea`. |
| LEA .X / .CC / predicate output | MISSING | `integer_arithmetic.cpp:353-354,399-400` (logged) | E throws | |
| LEA.HI.X cbuf (bit 57) | MISSING | `decoder.cpp:879` `INST(0x1800000000000000, 0xfe00000000000000)` | env 2163 `0x1800…, 0xfc00…, ON(57, x)`; Eden `"0001 10--"` | Becomes an unknown instruction. |
| POPC R/C/I (+inv) | IMPLEMENTED_UNVERIFIED | `decoder/compute.cpp:244-256,465-475` | E `integer_popcount.cpp`; N 1616-1642 | |
| FLO R/C/I (signed, .SH, inv) | IMPLEMENTED_UNVERIFIED | `compute.cpp:258-277,477-487`; MSL `hydra_find_umsb/smsb` (`msl/emitter.cpp` emitHeader) | E `find_leading_one.cpp:10-32`; N 1197-1227 | Semantics match FindUMsb/FindSMsb. |
| FLO .CC | MISSING | not decoded | E throws | |
| BFE R/C/I signed/unsigned | PARTIAL | `decoder/bitfield.cpp:8-48` → MSL `extract_bits(x, pos, len)` | E `bitfield_extract.cpp:10-58` (count 0 → 0; offset ≥32 → sign replicate; offset+count ≥32 → sign fix) | MSL leaves those edge cases undefined. 36 BFEs with register control in the dumps. |
| BFE .BREV (bit 40) | MISSING | `bitfield.cpp:7` "TODO: brev, write_cc" | E `:33-35`; N 1188-1190 (`set_bit(40, true)` when reversing) | Silent. The dumps use SHR.BREV instead (138 sites). |
| BFE .CC | MISSING | same TODO | E `:52-57` | |
| BFI R / RC / CR / I | VERIFIED | `bitfield.hpp:19-28`; `bitfield.cpp:50-107` | E `bitfield_insert.cpp:10-59` | `testBitOperations` (:456), including the predicate. |
| BFI .CC | MISSING | `bitfield.cpp:81-82` (logged) | E `:36-41` | |
| SHF L/R reg/imm (U32/U64/S64, .W, clamp) | VERIFIED | `decoder/shift.cpp:105-153` | N 1644-1693 and fold `mesa-ir.rs:4497-4542` | `testBitOperations`. Hydra matches NAK. Eden's 64-bit mask (62/63) is wrong, see section b. |
| SHF.R .HI (bit 48) | MISSING | `shift.cpp:148-150` (logged) | N 1688 `set_bit(48, self.dst_high && self.right)` | The high word of a 64-bit shift is wrong. |
| SHF .X / .CC | MISSING | same | E throws | |
| SHL R/C/I (+.W wrap, clamp ≥32) | VERIFIED | `shift.cpp:7-58,71-86` | E `integer_shift_left.cpp:10-55`; N 1695-1723 | `testIntegerShifts` (:337). |
| SHL .X / .CC | MISSING | `shift.cpp:55-57` (logged) | E throws | |
| SHR R/C/I (+.W, signed, .BREV) | VERIFIED | `shift.cpp:7-50,60-67,88-103` | E `integer_shift_right.cpp:10-51` | `testIntegerShifts`. The `x_mode` bit is 43 in Hydra/Eden and 44 in env; it only drives a log. |
| SHR .X / .CC | MISSING | `shift.cpp:64-66` (logged) | E throws | |

### a.6 Logic, compare, predicate, move

| Opcode / variant | Class | Hydra evidence | Ref | Notes |
|---|---|---|---|---|
| LOP R/C/I (AND/OR/XOR/PASS_B, inv A/B) | IMPLEMENTED_UNVERIFIED | `decoder/integer_logical.hpp:21-35`; `.cpp:8-90` | E `logic_operation.cpp:34-100`; N 1545-1614; env 2009 | 714 uses in the sample. |
| LOP predicate output (T/Z/NZ → P48) | IMPLEMENTED_UNVERIFIED | `integer_logical.cpp:39-58` | E `:55-58` | A write to PT is harmless (`state.p[7]`). |
| LOP .X / .CC | MISSING | `integer_logical.cpp:7` "TODO: x, write_cc" | E `:43-45,59-69` | Silent. |
| LOP32I | IMPLEMENTED_UNVERIFIED | `integer_logical.hpp:37-50`; `.cpp:92-96` | E `:102-114`; env 2168 | |
| LOP32I .X / .CC | MISSING | not used | E | |
| LOP3 R (LUT bits 28-35, pred op 36-37 → P48), C/I (LUT 48-55) | IMPLEMENTED_UNVERIFIED | `compute.cpp:174-242,450-463` (sum of minterms, a=0xF0, b=0xCC, c=0xAA) | E `logic_operation_three_input.cpp:546-595` | The LUT convention checks out by derivation. No test. |
| LOP3 .X / .CC | MISSING | not decoded | E throws | |
| ICMP R / RC / CR / I signed/unsigned, all 8 ops | VERIFIED | `decoder/integer_comparison.cpp:89-134` | E `integer_compare.cpp:11-45` | `testCompareSelect(device, false)`. |
| ISET R base (cmp, mask/.BF) | VERIFIED | `integer_comparison.cpp:34-62,136-142` | E `integer_compare_and_set.cpp:17-64` | `testSetRegisterResults`. Covers only NE/EQ. |
| ISET C/I; bop OR/XOR | IMPLEMENTED_UNVERIFIED | `integer_comparison.cpp:144-162` | same | |
| ISET .X / .CC | MISSING | `integer_comparison.cpp:33` "TODO: x, write_cc" | E `:13-15,50-63` | Silent. |
| ISETP R/C/I (8 ops, signedness, bop, dst A/B) | IMPLEMENTED_UNVERIFIED | `integer_comparison.hpp:36-50`; `.cpp:65-85,164-193` | E `integer_set_predicate.cpp:17-55`; N 1494-1543 | 69 uses in the sample. |
| ISETP .X (extended compare using CC) | MISSING | `integer_comparison.cpp:64` "TODO: x, write_cc" | E `common_funcs.cpp:31-72` (`ExtendedIntegerCompare`) | Silent: compares only 32 bits. |
| PSET (bop1/bop2, .BF) | IMPLEMENTED_UNVERIFIED | `decoder/predicate_comparison.hpp:8-24`; `.cpp:8-43` | E `predicate_set_register.cpp:10-50` | |
| PSET .CC | MISSING | `predicate_comparison.cpp:7` TODO | E `:39-49` | |
| PSETP destination A | IMPLEMENTED_UNVERIFIED | `predicate_comparison.cpp:45-74` | E `predicate_set_predicate.cpp:10-36`; N 2084-2101 | |
| PSETP destination B | PARTIAL | `predicate_comparison.cpp:63-67` `res1 = opNot(res0)` before op2 | E `:30` `lhs_b{PredicateCombine(ir, ir.LogicalNot(pred_a), pred_b, pset.bop_1)}`; N fold `mesa-ir.rs:5325` | Wrong for AND/OR bop1 (section b). |
| CSETP NEU / F / T | VERIFIED | `compute.cpp:281-306` | E `condition_code_set.cpp:45-63`, flow tests `ir_emitter.cpp:208-277` | NEU is tested in `testIntegerConditions`. |
| CSETP other flow tests (LT, EQ, LE, GT, NE, GE, NUM, NaN, LTU…GEU, OFF, LO, SFF, LS, HI, SFT, HS, OFT, RLE, RGT) | MISSING | `compute.cpp:283-286` logs and **returns without writing** | E `ir_emitter.cpp:208-265` | |
| CSET | MISSING | `decoder.cpp:705` | E `condition_code_set.cpp:11-43` | |
| P2R R/C/I (PR/CC mode, byte select) | IMPLEMENTED_UNVERIFIED | HEAD: NOTIMPL. Working tree (10:19): `decoder/integer_misc.cpp:121-149`, dispatched at `decoder.cpp:563,719,797` | E `move_predicate_to_register.cpp:24-63` (imm only); env 1992/2066/2112 | Hydra truncates the mask to 7 (PR) or 4 (CC) bits. Eden clears every masked bit, so they differ only for masks with higher bits set. No test. |
| R2P R/C/I PR mode (byte select, mask) | VERIFIED | `compute.cpp:308-328` | E `move_register_to_predicate.cpp:30-68` | `testRegisterPredicates` (:173). |
| R2P CC mode | IMPLEMENTED_UNVERIFIED | `compute.cpp:309,319,324` | E `:15-28,52` | |
| PLOP3 | N/A | – | SM70+ | |
| MOV R/C | IMPLEMENTED_UNVERIFIED | `decoder/move.cpp:10-18,37-46` | E `move_register.cpp:11-34` | 3255 uses in the sample. |
| MOV I (imm20) | PARTIAL | `move.cpp:48-52` `getIntImm20(inst.imm20_0, inst.imm20_19, false)` | E `move_register.cpp:36-38` + `impl.cpp:184-197` (sign-extends) | Negative immediates are wrong (section b). |
| MOV lane mask (bits 39-42 / 12-15) | MISSING | `move.cpp:9` "TODO: mask" | E `move_register.cpp:19-23` (skips the write for masks other than 0x1/0xF) | |
| MOV32I | IMPLEMENTED_UNVERIFIED | `move.cpp:54-57` | E `:40-42` | |
| SEL R/C/I | IMPLEMENTED_UNVERIFIED | `move.cpp:20-33,93-112` | E `select_source_with_predicate.cpp:11-42`; N 1999-2031 | |
| PRMT R/RC/CR/I, all modes (IDX, F4E, B4E, RC8, ECL, ECR, RC16) | IMPLEMENTED_UNVERIFIED | HEAD: NOTIMPL. Working tree (10:19): `decoder/integer_misc.cpp:46-63` (a=R8, selector=src_b, c=R39 or cbuf for RC, mode bits 48-51) + helper `hydra_prmt` (`msl/intrinsics.cpp:11-52`); dispatched at `decoder.cpp:608,652,759,840` | Eden missing; N 1955-1997; env 2023/2043/2094/2141 + tab5bc0_0 | Operand order matches envydis. The helper's mode tables and sign-replicate match PTX `prmt`. No test. |
| IDP (imm/reg) | N/A | `decoder.cpp:642,648` NOTIMPL | env marks it `F_SM60` | Not on SM53. |

### a.7 Video SIMD

| Opcode / variant | Class | Hydra evidence | Ref | Notes |
|---|---|---|---|---|
| VMAD | MISSING | `decoder.cpp:544` | E `video_multiply_add.cpp:10-61` (no SAT/scale/neg) | |
| VMNMX | MISSING | `decoder.cpp:787` | E `video_minimum_maximum.cpp:34-83` (MIN/MAX second op) | |
| VSETP | MISSING | `decoder.cpp:690` | E `video_set_predicate.cpp:47-89` | |
| VADD / VABSDIFF / VABSDIFF4 / VSET / VSHL / VSHR | MISSING | `decoder.cpp:868,639,711,783,633,636` | Eden also missing | |

### a.8 Condition codes and special encodings (cross-cutting)

| Item | Class | Hydra evidence | Ref | Notes |
|---|---|---|---|---|
| CC writers implemented: IADD/IADD32I/XMAD (Z,S,C,O), I2I (Z,S; C=O=0), IMUL (working tree) | IMPLEMENTED_UNVERIFIED | `integer_arithmetic.cpp:21-30,131-147`; `conversion.cpp:167-186`; `integer_misc.cpp:81-96` | E (`SetZFlag/SetSFlag…`) | IADD, XMAD and I2I are tested (rows above). |
| CC writers missing: FSET, ISET, PSET, CSET, LOP, LOP3, BFE, BFI, FLO, ISCADD, IADD3, SHL/SHR/SHF, IMNMX, F2F/F2I/I2F, FADD/FMUL/FFMA/FMNMX | MISSING | TODO comments listed above | E implements FSET/ISET/PSET/CSET/LOP/BFE/BFI/ISCADD/IADD3; throws for the rest | Silent wrong flags. FSET.CC is used in Crash 4. |
| CC consumers missing: ISETP.X, ISET.X, IADD3.X, LEA.X, SHL/SHR/SHF.X, CSET, P2R.CC, CSETP (non-NEU) | MISSING | see rows above | E `common_funcs.cpp:31-72`, `ir_emitter.cpp:208-277` | |
| Branch/exit/kill/sync/brk on CC (`ccc` field, bits 0-4) | MISSING | `decoder.cpp:449` "TODO: ccc, ca, lmt, u" (BRA), `:175,314,359` (SYNC, BRK, EXIT) | Eden honours `Condition(pred, flow_test)` (`ir_emitter.cpp:279-…`) | **Confirmed in Crash 4**: `@P0 BRA CC.NEU` is treated as `@P0 BRA`. Hand this to the control-flow owner. |
| `RZ` / `PT` operands | IMPLEMENTED_UNVERIFIED | `codegen/lang/emitter.hpp:261-276` (a read of RZ is the constant 0; PT reads as true; writes go to r[255]/p[7] and are never read back) | E `IR::Reg::RZ` | Except the half2 vector `RZ` case (row in a.4). |
| imm20 integer (sign-extended) / imm20 float (`<<12`, sign at bit 56) / imm32 | IMPLEMENTED_UNVERIFIED | `decoder/const.hpp` `getIntImm20`, `getFloatImm20` | E `impl.cpp:184-240`; N `set_src_imm_i20/f20` | MOV I is the exception (a.6). ICMP/FCMP immediate forms are tested. |
| Bound cbuf operand `c[slot][imm]` | VERIFIED | `CMem(slot, RZ, off*4)` everywhere | E `impl.cpp:124-150` | Exercised by the IADD, ICMP/FCMP, LEA, BFI, R2P and SHL/SHR tests. |
| Integer arithmetic overflow (wrap) | PARTIAL | IADD/XMAD use U32 (wraps). ISCADD/IADD3/LEA lo use **I32** (C++ signed overflow is UB) | E uses U32 everywhere | See c.7. |
| Shift counts ≥ 32 (SHL/SHR clamp unless .W; SHF clamp/wrap) | VERIFIED | `shift.cpp:27-46,116-146` | E/N as above | Boundary counts 31/32/33/63/64/0xffffffff are tested. |

---

## (b) Bit-position mismatches and semantic bugs (with exact code)

Severity: **S1** means silently wrong in shipped games or a host crash; **S2** means a wrong result in plausible code; **S3** is an edge case.

**B1. XMAD constant-buffer form drops `.PSL`/`.MRG` (S1, confirmed in Crash 4).**
- Hydra `decoder/integer_arithmetic.hpp:156-164`:
  ```cpp
  union InstXmadC {
      InstXmadBase base;
      BitField64<u32, 20, 14> cbuf_offset;
      BitField64<u32, 34, 5> cbuf_slot;
      BitField64<reg_t, 39, 8> src_c;
      BitField64<XmadCop, 50, 2> cop;
      BitField64<bool, 52, 1> hilo_b;
      BitField64<bool, 54, 1> x;
  };
  ```
  `integer_arithmetic.cpp:485-488`:
  ```cpp
  void emitXmadC(DecoderContext& context, InstXmadC inst) {
      emitXmad(
          context, inst.base.pred, inst.base.pred_inv, inst.cop, false, false,
  ```
- Eden `integer_short_multiply_add.cpp:107-117`: `BitField<55, 1, u64> psl; BitField<56, 1, u64> mrg;` … `XMAD(*this, insn, GetCbuf(insn), GetReg39(insn), xmad.select_mode, xmad.half_b, xmad.psl != 0, xmad.mrg != 0, xmad.x != 0);`
- envydis 2064: `{ 0x4e00…, … N("xmad"), T(5b00_0), ON(55, psl), ON(56, mrg), …`
- Effect: the NVN IMUL-by-uniform idiom is `XMAD Rlo, Ra, c[b], RZ` / `XMAD.MRG Rt, Ra, c[b].H1, RZ` / `XMAD.PSL.CBCC Rd, Ra.H1, Rt.H1, Rlo`. It computes `a.lo*b.lo + ((a.hi * ((a.lo*b.hi)>>16)) << 16) + ((a.lo*b.hi) << 16)` instead of `a*b`. The result is wrong whenever `a ≥ 65536` and `b.lo ≠ 0`. In the dumped MSL the merge `(r & 0xffff) | (c << 16)` is missing at every such site, for example `build/board-live/shaders/0015_compute_0x7c9f586400.metal:146-151`.

**B2. FSET (and ISET/PSET/LOP…) `.CC` dropped, and BRA ignores `ccc` (S1, confirmed in Crash 4).**
- Hydra `float_comparison.cpp:7`: `// TODO: write_cc, ftz`.
- Hydra `decoder.cpp:448-449`: `INST(0xe240000000000000, 0xfff0000000000020) { // bra` / `// TODO: ccc, ca, lmt, u`. For a non-PT predicate it emits `opBranchConditional(pred, target, pc+1)`, ignoring bits 0-4.
- Eden `floating_point_compare_and_set.cpp:51-61`: `if (fset.cc != 0) { … v.SetZFlag(is_zero); if (fset.bf != 0) v.ResetSFlag(); else v.SetSFlag(v.ir.LogicalNot(is_zero)); v.ResetCFlag(); v.ResetOFlag(); }`
- Eden flow test NEU `ir_emitter.cpp:239-240`: `return ir.LogicalOr(ir.GetSFlag(), ir.LogicalNot(ir.GetZFlag()));`
- Real code (shader `0x1b56830`, three sites; decoded words in Appendix A):

  | Word | Instruction |
  |---|---|
  | `0x5bb2838001271107` | `FSETP.EQ.FTZ.AND P0, PT, R17, R18, PT` |
  | `0x58828380011710ff` | `FSET.EQ.FTZ.AND.CC RZ, R16, R17, PT` |
  | `0x5bb280000117100f` | `FSETP.EQ.FTZ.AND P1, PT, R16, R17, P0` |
  | `0xe24000002200000d` | `@P0 BRA CC.NEU, +544` |

  Hydra branches whenever `R17==R18`, regardless of `R16==R17`.

**B3. I2I `.SAT` bit position and missing saturation (S2).**
- Hydra `decoder/conversion.hpp:110`: `BitField64<bool, 51, 1> sat;`
- Hydra `conversion.cpp:161-164`: `// TODO: saturate` … `if (saturate) LOG_NOT_IMPLEMENTED(ShaderDecompiler, "Saturation");`
- Eden `integer_to_integer_conversion.cpp:73`: `BitField<50, 1, u64> sat;`. Saturation logic is at `:36-59`.
- NAK `mesa-sm50.rs:1922`: `e.set_bit(50, self.saturate);`
- envydis 1993: `ON(50, sat)`
- Effect: `I2I.U8.S32.SAT` and similar are never clamped. A garbage bit 51 instead produces a spurious log.

**B4. I2I/F2I/F2F/I2F narrow results are written to part of the register (S2).**
- Hydra `conversion.cpp:165`: `context.builder.opCopy(ir::Value::createRegister(dst, dst_type), res);` with `dst_type` U8/U16/I8/I16/F16. This emits `state.r[n]._u16 = …` into `union Reg` (`codegen/lang/emitter.cpp:19-29`), so the upper bytes keep whatever was there before.
- Eden I2I `integer_to_integer_conversion.cpp:29-34,99-103`: `ConvertInteger(... BitFieldExtract(src, zero, count, false))` followed by a full 32-bit `X(dest_reg, result)`.
- Eden F2I clamps, then `ConvertFToI(max(32, bitsize), …)` (`floating_point_conversion_integer.cpp:148-203`).
- Eden F2F/I2F F16 `PackFloat2x16(CompositeConstruct(value, 0))` (`floating_point_conversion_floating_point.cpp:108-112`).
- Evidence: 70 `._u16 =` writes in the dumps. In the observed cases they come from F2I.U16.ROUND for array layers, and the compiler masks with `& 0xffff`. One F2F.F16 is followed by an explicit BFI clear. So the impact is latent but real for I2I.

**B5. F2F/F2I `.H1` source selector ignored (S2).**
- Hydra `conversion.hpp:28` and `:68`: `BitField64<bool, 41, 1> sh;`. It is never read. The source register is read as `_f16`, the low half.
- Eden F2F `floating_point_conversion_floating_point.cpp:131-139`: `auto [lhs_a, rhs_a]{Extract(ir, GetReg20(insn), Swizzle::H1_H0)}; src_a = f2f.selector != 0 ? rhs_a : lhs_a;`
- Eden F2I `floating_point_conversion_integer.cpp:221`: `CompositeExtract(ir.UnpackFloat2x16(X(f2i.src_reg)), f2i.base.half)`
- NAK `mesa-sm50.rs:1791,1836`: `e.set_bit(41, self.src.src_swizzle == SrcSwizzle::Yy);`

**B6. F2I/F2F `.ROUND` is not round-half-to-even (S3).**
- Hydra `codegen/lang/emitter.cpp:472-474`: `storeValue(dst, "round({})", getValueStr(src));`. MSL `round()` rounds halfway cases away from zero.
- Eden `floating_point_conversion_integer.cpp:135-136` and `…floating_point.cpp:88-89`: `FPRoundEven` → `OpRoundEven` (`emit_spirv_floating_point.cpp:197-199`).
- Fix: emit `rint()`.

**B7. F2I range clamping and NaN handling absent (S2, host-dependent).**
- Hydra `conversion.cpp:98-100`: `const auto res = context.builder.opCast(src_v, dst_type);` emits `(i32(x))`, `(u32(x))` or `(u16(x))`.
- Eden `floating_point_conversion_integer.cpp:148-197`: `FPClamp(rounded_value, min_val, max_val)`, then for signed results `Select(FPIsNan(op_a), Imm32(0U), result)`, plus the 0x80000000 special case when one side is 64-bit.

**B8. HADD2 register form reads `-A` from bit 38 (S2).**
- Hydra `half_arithmetic.hpp:27-35`: `union InstHadd2R { … BitField64<bool, 32, 1> sat; BitField64<bool, 38, 1> neg_a; };`
- Eden `half_floating_point_add.cpp:59`: `BitField<43, 1, u64> neg_a;`. This applies to all HADD2 forms; the C and I forms in Hydra correctly use 43.
- envydis 1987: `{ 0x5d10…, … N("hadd2"), ON(39, ftz), ON(32, sat), T(5d10_0), REG_00, ON(43, neg), ON(44, abs), …`

**B9. Half-precision constant-buffer operands interpreted as packed half2 (S2).**
- Hydra `half_arithmetic.cpp:128-130`: `ir::Value::createConstMemory(CMem(inst.cbuf_slot, RZ, …), ir::VectorType(ir::ScalarType::F16, 2))`. The same pattern appears at `:164-166` (HMUL2), `:203-205` (HFMA2 RC) and `:214-216` (HFMA2 CR).
- Eden `half_floating_point_add.cpp:90`: `HADD2(*this, insn, …, Swizzle::F32, GetCbuf(insn));`. Also `half_floating_point_multiply.cpp:105-106`, `half_floating_point_fused_multiply_add.cpp:113,127` and `half_floating_point_set_predicate.cpp:93` (all `Swizzle::F32`). The F32 operand promotes the computation to f32, then converts back.
- HSETP2 was fixed in the working tree (`half_comparison.cpp:89-98`, comment "The constant is a single F32 compared with both halves"). The other three opcodes still have the old behaviour.

**B10. PSETP second destination (S2).**
- Hydra `predicate_comparison.cpp:63-67`:
  ```cpp
  auto res0 = getLogical(context, b_op_ab, src_a_v, src_b_v);
  auto res1 = context.builder.opNot(res0);
  res0 = getLogical(context, b_op_c, res0, src_c_v);
  res1 = getLogical(context, b_op_c, res1, src_c_v);
  ```
- Eden `predicate_set_predicate.cpp:29-32`: `lhs_b{PredicateCombine(ir, ir.LogicalNot(pred_a), pred_b, pset.bop_1)}`
- NAK `mesa-ir.rs:5325-5326`: `let tmp = self.ops[0].eval(!srcs[0], srcs[1]); let dst1 = self.ops[1].eval(srcs[2], tmp);`
- `!(a&b)` differs from `(!a)&b`, and the same holds for OR. XOR happens to agree.

**B11. MOV immediate not sign-extended (S3).**
- Hydra `move.cpp:48-52`: `ir::Value::createConstantU(getIntImm20(inst.imm20_0, inst.imm20_19, false))`
- Eden `move_register.cpp:36-38`: `MOV(*this, insn, GetImm20(insn));`. `GetImm20` (`impl.cpp:191-193`) gives `if (imm.is_negative != 0) { … return ir.Imm32(static_cast<s32>(-(1LL << 19) + raw)); }`.

**B12. LEA.HI cbuf `.X` not decoded (S3).**
- Hydra `decoder.cpp:879`: `INST(0x1800000000000000, 0xfe00000000000000) { EMIT(LeaHiC); }`. The mask covers bit 57 (`.X`).
- envydis 2163: `{ 0x1800000000000000ull, 0xfc00000000000000ull, … N("lea"), N("hi"), ON(57, x), …`
- Eden `maxwell.inc`: `INST(LEA_hi_cbuf, "LEA (hi cbuf)", "0001 10-- ---- ----")`
- Result: `LEA.HI.X` with a cbuf operand (the 64-bit address idiom) becomes an "Unknown instruction". The same applies to `IADD32I.PO` (`decoder.cpp:878`, mask `0xfe80`; envydis 2161 `0x1d80 … "po"`).

**B13. HSETP2 `.H_AND` (S2). Present in HEAD; fixed in the working tree at 10:15.**
- HEAD `half_comparison.cpp`:
  ```cpp
  if (h_and) {
      res0 = context.builder.opBitwiseAnd(res0, res1);
      res0 = context.builder.opBitwiseNot(res0);
  }
  ```
  `opBitwiseNot` on a `bool` emits `(~b)`, which promotes to int (-1 or -2), so it is always truthy. Destination B also kept `res1`.
- Eden `half_floating_point_set_predicate.cpp:60-63`: `auto result = v.ir.LogicalAnd(bop_result_lhs, bop_result_rhs); v.ir.SetPred(hsetp2.dest_pred_a, result); v.ir.SetPred(hsetp2.dest_pred_b, v.ir.LogicalNot(result));`
- The working tree now has `result = {both, b.opNot(both)}` (`half_comparison.cpp:164-168`). This is correct. Keep a regression test.

**B14. MUFU.RCP64H/RSQ64H abort the host (S1, low frequency).**
- Hydra `multifunction.cpp:30,50-55`: `ir::Value res = ir::Value::createUndefined();` … `case MultifunctionOp::Rcp64h: LOG_NOT_IMPLEMENTED(…); break;`. It then calls `opCopy(dst, res)`.
- At emission, `codegen/lang/emitter.hpp` `getValueStr` hits `default: LOG_FATAL(ShaderDecompiler, "Invalid value type {} for src", …)`. `LOG_FATAL` calls `abort()` (`common/log.hpp:42-47`).
- Related undefined behaviour in release builds (`unreachable()` is `__builtin_unreachable()`, `common/functions.hpp:15-21`):
  - `conversion.cpp:17,72,121`: an I2F/I2I source of S64/U64 (`IntegerFormat2` value 3 or 7), or src_fmt bit 14 set (src_fmt_2 is 2 bits wide at `conversion.hpp:103,129`).
  - an invalid F2I destination format (0 or 4).
  - `getLogical` falling off the end for bop 3 (`comparison_helper.hpp:13-24`).

**B15. `F2F … .SAT` with an F16 destination emits an ill-typed clamp (S3).**
- Hydra `conversion.cpp:47-48`: `auto res = context.builder.opCast(src_v, dst_type); res = saturateIf(context.builder, res, saturate);`
- `saturateIf` (`decoder/const.hpp`) uses `createConstantF(0.0f)` and `createConstantF(1.0f)`, which are F32. The builder only checks types in debug builds (`ASSERT_DEBUG`). The emitted `clamp(half, float, float)` is an ambiguous MSL overload, so the shader fails to compile.

**B16. Half op with an `RZ` operand and the default H1_H0 swizzle emits `INVALID` (S3).**
- `getSwizzledHalf` (`decoder/const.hpp`) returns `createRegister(src, VectorType(F16,2))`. For `src == RZ`, `getRegisterStr` calls `getConstantStr(0, type)`. That function handles only scalar kinds and otherwise returns `INVALID_VALUE` (`"INVALID"`, `codegen/lang/emitter.hpp:20,221-253`).

**B17. IADD `.X` negation and `.CC` carry semantics differ from NAK (S3; the references conflict).**
- Hydra `integer_arithmetic.cpp:15-16,25`: `if (avg_mode == AvgMode::NegA) a = b.opNeg(a); if (avg_mode == AvgMode::NegB) other = b.opNeg(other);` … `carry = (sum < a) | (result < sum)`
- NAK `mesa-sm50.rs:1324,1339` (`IAdd2X`): `e.set_reg_bnot_src(20..28, 48, …)`, `e.set_reg_bnot_src(8..16, 49, …)`. The fold in `mesa-ir.rs:3844-3853` uses `get_u32_bnot_src`. The `IAdd2` fold (`mesa-ir.rs:3807-3811`) is `sum += u64::from(!srcs[i]) + 1;` with the comment "This is not -u64::from(src)".
- Eden matches Hydra (`integer_add.cpp:26-27,70-72`).
- The test `shader_library_tests.cpp:157-160` encodes Hydra's model (`if(mode==1) bb=0u-bb; … sum=u64(aa)+bb`). Resolve this with a hardware-documented case (NAK) before changing anything.

**B18. FSWZADD modes 0 and 3 (S3; the references conflict).**
- Hydra `msl/emitter.cpp` (emitHeader): `lut_a[4] = {1.0, 1.0, -1.0, 0.0}`, `lut_b[4] = {1.0, -1.0, 1.0, 1.0}`, which gives mode 0 = a+b and mode 3 = b.
- Eden `spirv_emit_context.cpp:1487-1489`: `fswzadd_lut_a = …(F32[4], f32_minus_one, f32_one, f32_minus_one, f32_zero); fswzadd_lut_b = …(f32_minus_one, f32_minus_one, f32_one, f32_minus_one)`, which gives mode 0 = −(a+b) and mode 3 = −b.
- NAK names the modes `Add/SubLeft/SubRight/MoveLeft` (`mesa-ir.rs:2957-2971`), which supports Hydra.
- Only 0x99/0xA5 (modes 1 and 2, identical in both) occur in the dumps. Low priority. Settle it with an on-device NVN capture.

**B19. Eden mistakes that Hydra correctly avoids** (do not "fix" Hydra towards Eden here):
- F2I abs/neg: Eden `floating_point_conversion_integer.cpp:45-46` has `BitField<45, 1, u64> abs; … BitField<49, 1, u64> neg;`. NAK (`mesa-sm50.rs:1810`, `set_reg_fmod_src(20..28, 49, 45…)` meaning abs=49, neg=45) and envydis (1996: `ON(45, neg), ON(49, abs)`) agree with Hydra (`conversion.hpp:70,72`).
- SHF 64-bit: Eden `integer_funnel_shift.cpp:51-54` uses `max_shift = 63` and wrap mask `max_shift-1 = 62`. NAK's fold (`mesa-ir.rs:4503-4508`) uses `bits-1 = 63` and clamp `bits = 64`, which matches Hydra (`shift.cpp:119-121`).
- IMAD32I: Eden decodes it at `"1000 00--"` (shadowing LD), while envydis 2165 puts it at `0x1000/0xfc00`. Both are unimplemented in Hydra.

**Other silent gaps** (no quote needed beyond the TODO lines cited in table (a)):
- FMZ: FMUL/FFMA/HMUL2/HFMA2.
- FTZ: FADD/FMUL/FFMA/FMNMX/FSET/FSETP/F2F/F2I/HADD2.
- Rounding modes: FADD/FMUL/FFMA/F2F/I2F.
- `.X`/`.CC`: IADD3, ISCADD, ISETP, ISET, LOP, LOP3, BFE, IMNMX.
- BFE.BREV.
- IADD.SAT.
- CSETP non-NEU tests (they also skip the predicate write).
- MOV lane mask.

---

## (c) MSL numeric issues

Production compiles with `setFastMathEnabled(false)` and `setPreserveInvariance(true)` (`renderer/metal/shader_library_cache.cpp:57-62`). With fast math off, `isnan`, IEEE NaN comparisons and precise math functions behave as specified. Each IR op is emitted as a separate statement (`auto local = …;`), so `a*b + c` across statements is not contracted. FFMA uses an explicit `fma()` (`codegen/lang/emitter.cpp:449-453`).

1. **`round()` instead of `rint()`** for integer rounding (B6). Use `rint` for the RN rounding of F2F and F2I.
2. **float→int conversions are unspecified out of range.** MSL `i32(x)`, `u32(x)` and `u16(x)` do not define the result for NaN or out-of-range inputs, and Apple GPUs saturate only by convention. Maxwell F2I saturates to the destination range and maps NaN to 0 for signed results (Eden B7). Make this explicit:
   ```
   clamp(rounded, MIN, MAX)
   isnan(x) ? 0 : …
   ```
   For U16/S16, clamp to the 16-bit range and write the full 32-bit register. F2I.TRUNC is very common (4723 trunc→int sites in the dumps), so first add a one-off Metal test of the host behaviour.
3. **`min`/`max` for FMNMX.**
   - Maxwell FMNMX returns the non-NaN operand.
   - MSL guarantees that rule only for `fmin`/`fmax`. Common-function `min`/`max` on floats is specified as a comparison select with no NaN clause.
   - Fix: emit `fmin`/`fmax` for floating-point min/max (`codegen/lang/emitter.cpp:455-463`, or override in `MslEmitter`).
   - Saturate is safe: `clamp(NaN, 0, 1)` = `fmin(fmax(NaN, 0), 1)` = 0, which matches `.SAT`.
4. **Bitwise ops on `bool` locals.** `emitBitwiseNot` writes `(~x)` (`codegen/lang/emitter.cpp:493-495`), and the builder accepts `Bool` (`ir/builder.hpp:164-172`). For a bool that produces int −1/−2 (B13). `&`, `|` and `^` on bools promote to `int` 0/1, which is harmless. Make `emitBitwiseNot` emit `!` for Bool, or reject it in the builder.
5. **`auto` locals take C++-promoted types** (`storeValue` → `auto … = …`, `codegen/lang/emitter.hpp:318-323`). For example `u16+u16` becomes `int`, and `bool&bool` becomes `int`. Declaring locals with the IR type (`getTypeStr(dst.getType())`) would make IR typing authoritative and catch mismatches like B15 at compile time.
6. **Partial union writes** for 8/16-bit and F16 destinations (B4). Prefer explicit `as_type<u32>(…)` packing.
7. **Signed `I32` arithmetic.**
   - Where: ISCADD (`integer_arithmetic.cpp:41-52`), IADD3 (`:220-243`) and LEA lo (`:359-366`) add and shift in `int`.
   - Problem: C++ signed overflow is undefined, and the LLVM-based Metal compiler may use `nsw` assumptions.
   - Fix: do the arithmetic in `u32`, as Eden does (all IAdd/IMul on U32), and keep signedness only for compares, min/max and right shifts.
8. **`extract_bits` edge cases.** BFE passes `pos = c & 0xff` and `len = (c>>8)&0xff` straight to `extract_bits` (`bitfield.cpp:15-20`). MSL leaves `offset`/`bits` > 32 and `offset+bits` > 32 undefined. Implement Eden's guards. 36 BFEs with register control appear in the dumps.
9. **Denormals.** All FTZ bits are ignored, so behaviour follows the host. The Hydra comment at `float_comparison.cpp:75-77` notes that some Metal GPUs flush subnormals even without fast math. Guest code is overwhelmingly `.FTZ` (FFMA 10 202/10 202, FMUL 8033, FADD 3567 in the sample), so a host that flushes is actually closer. Mismatches appear only below 1.2e-38. Low priority.
10. **Half math.** `getSwizzledHalf(F32)` rounds the f32 operand to half before the operation (double rounding, overflow at 65504). Eden promotes to f32. The new HSETP2/HSET2 code compares in f32, which is correct.
11. **No fp64 in Metal.** All D* ops, F2F/F2I/I2F F64 and MUFU.*64H need a soft-float or double-float library emitted as MSL helpers. The IR has no 64-bit types at all (`ir/type.hpp:13-24`, "TODO: 64-bit types").
12. **Math precision is fine.**
    - `precise::sin/cos` (`msl/emitter.cpp` emitSin/emitCos) is exact for the RRO pass-through.
    - `1.0 / x` with fast math off is a correctly rounded division, more precise than MUFU.RCP. Eden also uses FDiv.
    - `exp2`, `log2`, `sqrt` and `rsqrt` are precise. The special values (±0, ±Inf, negative inputs) match MUFU.
13. **Redundant NaN masking (performance only).** `getFloatCmp` (`decoder/const.hpp`) adds `isnan(a)|isnan(b)` and `& !nan` even for ordered LT/LE/GT/GE/EQ, which IEEE already makes false on NaN. Only NE (ordered) and the unordered ops need it. This accounts for 17 440 `isnan` lines in the `board-live` dumps.
14. **Test fidelity.**
    - Several tests compile kernels with `device->newLibrary(source, nullptr, …)`: testIntegerConditions, testRegisterPredicates, testLocalMemory, testSharedAtomics, testIntegerShifts, testBitOperations, testQuadDerivatives, testLea and testConditionCodes. `nullptr` means default options, which have fast math **on**.
    - testCompareSelect and testSetRegisterResults use `ShaderLibraryCache`, which matches production options.
    - Any new floating-point test must use `ShaderLibraryCache` or explicit options.

---

## (d) Prioritized implementation backlog

Every test below goes into `tests/shader_library_tests.cpp` and follows the existing pattern:
1. Assemble guest words, with a scheduling word every 4th slot (see `append` in testLea/testBitOperations).
2. Run `decoder::Decoder` → `MemoryAnalyzer` → `MslEmitter`.
3. Append a `kernel` that seeds `state.r[]`/`state.c2`, calls `func_label0x0(state)` and copies `state.r[16+i]` to a buffer.
4. Dispatch on Metal and compare against a CPU reference.

Compile float tests through `ShaderLibraryCache`. "Freq" cites the Crash 4 sample (Appendix A) or the `.metal` dumps.

**P0: confirmed in shipped game code or crashes the host**

1. **XMAD CR `.PSL`/`.MRG` (B1).**
   - Freq: 18 uses in 15 of 73 Crash 4 shaders; the IMUL-by-uniform idiom appears in 520+ dumped shaders, mostly vertex shaders.
   - How: add `BitField64<bool, 55, 1> psl; BitField64<bool, 56, 1> mrg;` to `InstXmadC` (`integer_arithmetic.hpp:156`) and pass `inst.psl, inst.mrg` in `emitXmadC` (`integer_arithmetic.cpp:485-495`).
   - Test: the three-instruction sequence `XMAD c` → `XMAD.MRG c.H1` (0x4E… with bits 52 and 56) → `XMAD.PSL.CBCC` (0x5B…, mode 4, bit 36), with a ∈ {0, 1, 0xFFFF, 0x10000, 0x12345678, 0xFFFFFFFF} × c ∈ same. Expect `a*c mod 2^32`. Also test standalone `XMAD.PSL` (CR) and `.MRG` against the Eden formula.
   - Risk: very low; it only affects encodings with bits 55/56 set.
2. **FSET.CC (and ISET/PSET/CSET/LOP/LOP3/BFE/BFI/IADD3/ISCADD CC writers) plus honouring `ccc` in BRA/EXIT/KIL/SYNC/BRK (B2).**
   - Freq: Crash 4 fragment shader `0x1b56830`, three sites.
   - How:
     - Port Eden's per-instruction flag writes (`SetZFlag`/`SetSFlag`/`Reset*`) into `float_comparison.cpp:emitFloatSet`, `integer_comparison.cpp:emitIntSet`, `predicate_comparison.cpp:emitPredicateSet` and the others, using `ir::Value::createConditionFlag(0..3)`.
     - Add a shared `getFlowTest(builder, test)` helper next to `emitCsetp` (`compute.cpp:281`) with all 32 tests from Eden `ir_emitter.cpp:208-277`.
     - Use it in `emitCsetp`, a new CSET, and the BRA/EXIT/SYNC/BRK/KIL handlers (`decoder.cpp:174-209,313-346,358-374,448-473`). The condition becomes `pred AND flow_test` when `ccc != T(15)`. The control-flow owner should review the decoder changes.
   - Test: decode `FSET.EQ.CC RZ, R1, R2` + `CSETP.NEU P0` for equal/unequal/NaN pairs, and `ISET.CC` / `PSET.CC` + `CSETP` (all 16 FP tests and the integer LO/HI/LS/HS…). Also a two-block program `@P0 BRA CC.NEU` that writes different constants on each path, checked for all four (P0, CC) combinations.
   - Risk: medium (control flow). Keep `ccc == 15` on the fast path.
3. **Host-crash and undefined-behaviour guards (B14, B15, B16).**
   - MUFU.RCP64H/RSQ64H: emit a deterministic f32 fallback instead of an undefined value. For example, reinterpret the high word as the top half of a double and compute `rcp`/`rsqrt` of `float(hi-double)`, then re-pack it into the high word (this matches what DFMA Newton iterations expect within f32 precision). At minimum, write 0 and log once.
   - Replace `unreachable()` in `conversion.cpp` `toType()` with handled defaults (64-bit → log once + 32-bit path) and mask `src_fmt_2` to 1 bit.
   - `getLogical` default case.
   - `saturateIf` must build constants in the operand's type, F16 included.
   - `getConstantStr` must handle `vec<f16,2>` (zero → `half2(0)`).
   - Test: decoder-level compile tests that run the emitter and `newLibrary` on MUFU.RCP64H, I2F.F32.S64, F2F.F16.F32.SAT and `HFMA2 R0, R1, R2, RZ`. Each must compile with no "INVALID" in the source.
   - Risk: none.

**P1: silent wrong results in common idioms**

4. **F2I/F2F correctness (B4, B5, B6, B7).**
   - How:
     - Use `rint` for ROUND (`emitRound`, or a new `RoundEven` IR op).
     - Clamp to the destination range, map NaN to 0 for signed results (with Eden's 0x80000000 special case), and convert through 32 bits, then write the full register.
     - Honour bit 41 (`.H1`) for F16 sources (`createRegister(src)` → `>> 16` → `as_type<half>`).
     - F2F to F16: write `as_type<u32>(half2(v, 0))`.
   - Test: F2I.{S32,U32,S16,U16}.{ROUND,FLOOR,CEIL,TRUNC} on {±0.5, 1.5, 2.5, −2.5, 1e10, −1e10, NaN, ±Inf, 65535.6, −1} with register pre-filled with 0xDEADBEEF. F2F.F32.F16 H0/H1, F2F.F16.F32 (upper half), F2F.F32.ROUND on .5 values.
   - Risk: low (affects only edge values).
5. **I2I `.SAT` (bit 50) and full-width results (B3, B4).**
   - How: fix the bit in `conversion.hpp:110`, port Eden `SaturateInteger` (`integer_to_integer_conversion.cpp:36-59`), and zero-extend the result to 32 bits.
   - Test: all 36 (src, dst) size/sign pairs × {sat, no-sat} × selectors B0-B3 × {neg, abs}, with inputs {0, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF}. Also keep the I2I.CC test.
   - Risk: low-medium (changes register write width; Crash 4 uses I2I.CC).
6. **HADD2.R `-A` bit 43 (B8).**
   - How: `half_arithmetic.hpp:34` → 43.
   - Test: HADD2 R with ±A/±B, |A|, swizzles and merges, checked bit-exact as half2.
   - Risk: none.
7. **Half constant-buffer operands as F32 with promotion (B9).**
   - How: mirror the working-tree HSETP2 approach (`half_comparison.cpp` `extractHalves` → f32) in HADD2/HMUL2/HFMA2. Compute in f32 when any operand is F32 (cbuf or `.F32` swizzle), round once, then merge.
   - Test: HFMA2 RC/CR, HADD2 C and HMUL2 C with cbuf f32 = {2.5, −0.1, 70000}, checked against Eden-style promotion.
   - Risk: medium. Eden and old yuzu agree; NAK has no SM5x half-op encoder.
8. **PSETP destination B (B10).**
   - How: `res1 = getLogical(b_op_ab, opNot(src_a_v), src_b_v)` before op2.
   - Test: exhaustive 3 bops × 3 bops × 8 input combinations, checking both destinations.
   - Risk: none.
9. **FMZ for FMUL/FMUL32I/FFMA/FFMA32I/HMUL2/HFMA2.**
   - Freq: 0 in the Crash 4 sample, but Eden implements it because D3D-style shaders rely on it. NaN/Inf otherwise turns pixels black.
   - How: port `floating_point_multiply.cpp:71-79` and `…fused_multiply_add.cpp:31-39` (select 0 / c when a==0 or b==0, skipped when .SAT is set).
   - Test: FMUL.FMZ (0×Inf, 0×NaN, −0×5), FFMA.FMZ (0×Inf+c → c); check that FMUL.FTZ is unchanged.
   - Risk: low.
10. **FMNMX NaN via `fmin`/`fmax`, plus F2I host-behaviour probe (c.2, c.3).**
    - Test: FMNMX R/C/I with NaN in each operand and both predicates → expect the non-NaN operand.
    - Risk: none.
11. **MOV imm sign-extension (B11).**
    - How: `move.cpp:51` → `true`.
    - Test: `MOV R16, -1` (imm20 = 0x7FFFF | bit 56) → 0xFFFFFFFF.
    - Risk: none.

**P2: correctness for less common but real constructs**

12. **P2R.** Implemented in the working tree at 10:19 (`integer_misc.cpp:121-149`).
    - Remaining: decide whether mask bits ≥7 (PR) or ≥4 (CC) clear destination bits, as Eden does. Hydra currently keeps them.
    - Test: P2R → R2P round trip across all four byte selectors, masks {0x00, 0x55, 0x7f, 0xff}, and CC mode after an IADD.CC.
    - Risk: low.
13. **ISETP.X / ISET.X / IADD3.X+CC / LEA.X+CC / ISCADD.CC.**
    - How: port `ExtendedIntegerCompare` (`common_funcs.cpp:31-72`) and the CC logic.
    - Test: 64-bit compare `IADD RZ.CC, a.lo, -b.lo; ISETP.LT.X P0, a.hi, b.hi`, and IADD3/LEA 64-bit add chains, against `u64`/`i64` references.
    - Risk: medium. Decide B17 (IADD.X `~b`) together with this item.
14. **BFE `.BREV` + Eden edge guards (c.8).**
    - Test: BFE.{U32,S32}{,.BREV} with register control over offsets {0,1,31,32,33,255} × counts {0,1,16,31,32,33}.
    - Risk: low.
15. **U32 arithmetic in ISCADD/IADD3/LEA (c.7).**
    - Test: overflow cases (0x7FFFFFFF+1, −0x80000000 negation, shifted negatives).
    - Risk: low.
16. **IADD32I.PO decode and LEA.HI.X cbuf mask (B12).**
    - How: add `INST(0x1d80…, 0xff80…)` before IADD32I and change the LeaHiC mask to `0xfc00…`. Implement X and CC for LEA.HI with the CC infrastructure from item 2.
    - Risk: low.
17. **SHF.R `.HI` and `.X`; SHL/SHR `.X`.**
    - How: follow the NAK fold (`mesa-ir.rs:4510-4539`).
    - Risk: low.
18. **Video ops VMAD/VMNMX/VSETP (Eden subset).**
    - Risk: low; independent new handlers.
19. **CSET + full CSETP flow tests**: comes free with item 2.

**P3: completeness and precision**

20. **Double precision** (DADD/DMUL/DFMA/DMNMX/DSET/DSETP, F2F/F2I/I2F F64, MUFU 64H proper).
    - Requires 64-bit IR types (or u32-pair intrinsics) plus MSL soft-fp64 helpers, which can follow the new `ir/intrinsic.hpp` mechanism.
    - Large effort; justify it with a game that needs it.
21. **I2F/F2I 64-bit integers** (S64/U64), done with u32-pair helpers.
22. **FTZ and rounding modes.** Emulate FTZ by flushing inputs and outputs when the host keeps denormals. Emulate RM/RP/RZ for FADD/FMUL/FFMA/F2F/I2F.
23. **FSWZADD modes 0/3** (B18): resolve with an NVN trace. Also implement `.NDV`.
24. **Half `.F32` swizzle promotion** (c.10) and **HSETP2 cbuf abs(b)** (bit 54).
25. **IMUL/IMUL32I/PRMT tests.** These were implemented in the working tree at 10:19 (`integer_misc.cpp`); the bits match NAK and envydis.
    - Test: PRMT all 7 modes × selectors 0-3 plus the default mode with sign-replicate nibbles, for R/CR/RC/I forms; IMUL/IMUL32I lo and hi words for all signedness combinations against an `i64`/`u64` reference; FCHK stub behaviour.
26. **MOV lane mask**: follow Eden (skip the write unless the mask is 0x1 or 0xF).
27. **Hygiene.**
    - Remove the `HYDRA_NO_BYTESEL` environment variable and the per-decode `LOG_INFO` in `selectSourceBytes` (`conversion.cpp:130-138`).
    - Make the tests use production compile options (c.14).
    - Add a decoder-vs-maxwell.inc mask cross-check test; the script logic is in Appendix B.

---

## Appendix A: Real-game evidence

**Source data**
- Guest words decoded by Hydra and dumped as `*.decoded.txt` in `build/**/visual/capture-*` (Crash Bandicoot 4 sessions, 16-17 Oct). That gives 73 unique shaders.
- Generated MSL in `build/board-live/shaders/*.metal` (3368 files) and `build/swiftui-release/hot-debug/shaders/*.metal` (3982 files).
- Opcodes were classified with Eden's `maxwell.inc` patterns.

**Opcode counts** (instances / shaders out of 73)

| Opcode | Instances | Shaders |
|---|---|---|
| FMUL_reg | 4867 | 70 |
| FFMA_cr | 4606 | 72 |
| FFMA_reg | 3670 | 68 |
| FMUL_cbuf | 2903 | 70 |
| FADD_reg | 1948 | 70 |
| MOV_cbuf | 1760 | 72 |
| FMUL32I | 1650 | 58 |
| MOV_reg | 1495 | 55 |
| MUFU | 1437 | 60 |
| FFMA_imm | 1314 | 42 |
| MOV32I | 1143 | 61 |
| FADD_cbuf | 978 | 69 |
| FMNMX_reg | 700 | 52 |
| LOP_cbuf | 620 | 67 |
| FFMA_rc | 612 | 54 |
| FADD32I | 578 | 51 |
| FSETP_cbuf | 475 | 55 |
| RRO | 387 | 48 |
| F2F | 293 | 40 |
| F2I | 149 | 35 |
| I2F | 134 | 31 |
| XMAD_imm | 133 | 5 |
| XMAD_cr | 49 | 15 |
| XMAD_reg | 40 | 15 |
| ISETP_reg | 41 | 34 |
| ICMP_imm | 45 | 31 |
| HSETP2_reg | 5 | 1 |
| CSETP | 1 | 1 |
| I2I | 1 | 1 |

No PSETP, P2R, PRMT, IMUL, BFE, double or video ops appear in this sample.

**Modifiers**
- FTZ is on everything: FFMA fmz=1 10 202, FMUL fmz=1 8033, FMUL32I 1650, FADD.FTZ 3567.
- FMZ: 0.
- XMAD_cr.MRG 18 (15 shaders). XMAD_reg mode CBCC + PSL 40. XMAD_imm.PSL 67.
- F2F: all F32→F32 FLOOR. F2I: S32 TRUNC 142, U32 TRUNC 6, U16 ROUND 1.
- MUFU: COS 12, SIN 28, EX2 359, LG2 385, RCP 314, RSQ 148, SQRT 191. No 64H.
- FSET.CC 3 with `BRA ccc=13 (NEU)` 3, both in shader `0x1b56830`.
- I2I.CC 1 with CSETP test 13 (NEU) 1.
- FSWZADD only 0x99/0xA5.

**MSL dump patterns** (board-live / hot-debug)

| Pattern | board-live | hot-debug |
|---|---|---|
| `round(` | 41 files | 75 files |
| IMUL-by-cbuf idiom | 520 files | 563 files |
| `._u16 =` partial writes (combined) | 70 | |
| `._f16 =` partial writes (combined) | 24 | |
| bit-reverse sequence (SHR.BREV) | 138 | |
| `extract_bits` with register offset and length | 36 | |
| `isnan` lines | 17 440 | 25 658 |
| `INVALID` tokens | 0 | 0 |

- 70 conversions of `round(…)` results to int appear in total.
- No bool `~`: every `(~local` operand is an integer, so the HEAD HSETP2.H_AND path was not exercised by these shaders.
- The `state.cc[` sites form only the I2I.CC→CSETP.NEU pattern.

## Appendix B: Decode-mask cross-check (Hydra `decoder.cpp` vs Eden `maxwell.inc`)

Method: all 2^16 top-16-bit patterns, with bit 5 = 0 and 1, were checked against Eden's first-match order and Hydra's `INST()` if/else chain. The script is `/private/tmp/claude-501/-Users-arvino-Documents-Project-hydra/0e906531-f45a-4e9b-b0bc-dcb303edaa1b/scratchpad/scripts/xcheck.py`.

Every ALU opcode lands on the matching Hydra handler or NOTIMPL entry, except these patterns:
- **LEA_hi_cbuf**: 1024 of 2048 patterns (bit 57 = X) become UNKNOWN.
- **IADD32I**: 512 of 1024 patterns (bit 55, i.e. `.PO` and invalid) become UNKNOWN.
- **IMAD32I**: Eden's `1000 00--` lands on Hydra's LD (reference conflict, see B19).

The HSET2 forms mapped to NOTIMPL in HEAD and map to `emitHset2` in the working tree.

## Appendix C: Reference conflicts and how they were resolved

| Topic | Eden | NAK | envydis | Hydra | Verdict |
|---|---|---|---|---|---|
| F2I abs/neg | abs 45, neg 49 | abs 49, neg 45 | neg 45, abs 49 | neg 45, abs 49 | Hydra correct |
| SHF 64-bit wrap/clamp | 62 / 63 | 63 / 64 | – | 63 / 64 | Hydra correct |
| IADD.X neg bits | INeg | bitwise NOT | – | Neg | Follow NAK (needs test change) |
| IADD.CC carry with -0 | 32-bit negation | literal `~x+1` (carry=1) | – | 32-bit | Follow NAK |
| FSWZADD modes 0/3 | −(a+b), −b | names Add/MoveLeft | – | a+b, b | Undecided; not used in dumps |
| IMAD32I location | `1000 00--` | – | `0x1000/0xfc00` | NOTIMPL at 0x10 | envydis likely right |
| FSET cbuf neg_b | 53 | 6 (NAK typo) | 53 | 53 | Hydra correct |
| SHR `.X` bit | 43 | – | 44 | 43 (log only) | No functional impact |

## Appendix D: Working-tree drift during the audit (2026-10-17)

| Time | File | Change |
|---|---|---|
| 10:03–10:08 | `const.hpp/.cpp`, `decoder/memory.cpp`, `analyzer/memory_analyzer.*`, `codegen/lang/msl/emitter.cpp` | Memory-side work (outside this scope). |
| 10:12 | `decoder/move.cpp` | S2R non-compute stages; MOV imm is **still unsigned**. |
| 10:13 | `tests/shader_library_tests.cpp`, new `tests/shader_coverage_tests.cpp` | Stage I/O and special-register tests; no ALU tests. |
| 10:15 | `decoder/half_comparison.{hpp,cpp}`, `decoder/decoder.cpp` | **HSETP2 rewritten** (H_AND, cbuf F32, FTZ, f32 compare) and **HSET2 added**, both per Eden. Fixes B13 and part of B9 in the working tree. |
| 10:17–10:18 | `ir/intrinsic.hpp` (new), `ir/builder.hpp`, `ir/instruction.hpp`, `codegen/emitter.*`, `codegen/lang/msl/emitter.*`, `codegen/lang/msl/intrinsics.{hpp,cpp}` (new) | `Intrinsic` IR op with `Prmt` and `IMulHi` MSL helpers. The reviewed `hydra_prmt` mode tables (F4E/B4E/RC8/ECL/ECR/RC16 + sign replicate) match the PTX `prmt` definitions. |
| 10:18–10:19 | `decoder/integer_misc.{hpp,cpp}` (new), `decoder/decoder.cpp` | **PRMT (R/CR/RC/I), IMUL/IMUL32I, P2R (R/C/I) and FCHK (fast-path stub) wired.** Reviewed: operand and bit positions match envydis and NAK. P2R differs from Eden only in the high-bit mask case. No tests yet. |
| 10:25 (final check) | — | Still present: B1 (XMAD CR), B3 (I2I sat bit 51), B6 (`round()`), B8 (HADD2 bit 38), B10 (PSETP), B11 (MOV imm). |

Re-check every item marked PARTIAL or MISSING against the tree at merge time.
