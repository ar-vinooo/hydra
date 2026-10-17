# Maxwell video instruction continuation — 2026-10-17

The video family is **partially implemented**. These are integer shader
instructions, unrelated to the emulator's NVDEC/video playback engine.

## Implemented

- VMAD: byte/halfword/word source selection, independent signed extensions,
  register or signed/unsigned 16-bit immediate B, wrapping multiplication and
  addition, negate-product/negate-addend/plus-one modes, instruction predication.
- VMNMX: same-sign source min/max, register/immediate B, signed/unsigned
  destination domain, primary-result saturation, secondary MRG_16H, MRG_16L,
  MRG_8B0, MRG_8B2, ACC, MIN, MAX, or raw result, instruction predication.
  Saturation precedes the secondary operation.
- VSETP: same-sign source comparisons F/LT/EQ/LE/GT/NE/GE/T, AND/OR/XOR with
  an optionally inverted predicate, complementary predicate destination,
  predicate source/destination aliasing, instruction predication.

The VSETP comparison field is split across bits43–44 and47. Bits45–46 select
its boolean operation. Taking the whole bits43–47 range as the comparison
incorrectly rejects OR/XOR. Immediate B never has a lane selector, including
when the immediate's upper bits coincide with the register selector fields.

## Explicitly remaining

- VADD, VABSDIFF, VSET, VSHL, VSHR.
- VMAD condition flags, scaling and saturation.
- VMNMX condition flags.
- Mixed source signedness in VMNMX/VSETP. Eden applies B's signedness to the
  comparison while archived Ryujinx uses independent source signedness and
  additional handling. These forms remain diagnosed until Maxwell hardware
  evidence resolves the disagreement. VMAD's mixed source signs are supported
  because the unscaled low result has unambiguous wrapping multiplication.
- VSETP boolean operation3 is reserved/unimplemented.

Unsupported modifiers produce an explicit shader diagnostic; they are not
silently ignored. This is not full video-family completion.

## References

- Local Eden `video_helper.cpp`, `video_multiply_add.cpp`,
  `video_minimum_maximum.cpp`, `video_set_predicate.cpp`, and `maxwell.inc`.
- [envytools Maxwell disassembler tables](https://github.com/envytools/envytools/blob/master/envydis/gm107.c):
  exact opcode masks, split VSETP comparison field, source widths and secondary
  operation encodings (`tab50f0_0`, `tab50f0_1_size`, `tab5700_3`).
- [Archived Ryujinx VMAD implementation](https://git.axenov.dev/Museum/ryujinx/src/commit/b6614c6ad5d7d19594b80f4917df27bf476e8f03/Ryujinx.Graphics.Shader/Instructions/InstEmitVideoArithmetic.cs):
  negate/plus-one modes.
- [Archived Ryujinx VMNMX/VSETP implementation](https://git.axenov.dev/Museum/ryujinx/src/commit/b6614c6ad5d7d19594b80f4917df27bf476e8f03/Ryujinx.Graphics.Shader/Instructions/InstEmitVideoMinMax.cs):
  secondary operation/saturation ordering and evidence of the mixed-sign
  disagreement. The archive preserves the original implementation and commit.

## Validation

`hydra-shader-video-tests` builds guest machine words, decodes them through the
production decoder, emits MSL, and executes it on Metal with CPU reference
expectations. It covers 80 source configurations (widths/selectors/signs,
register/immediate and predication), all four unscaled VMAD arithmetic modes,
all eight VMNMX secondary modes across 16 signedness/saturation/min-max
configurations, all 24 VSETP comparison/boolean combinations, aliased predicate
input/output, and signed immediate extraction.

These tests validate Hydra's translation against the stated reference
semantics. They do not constitute direct execution on Maxwell hardware or a
gameplay test. Build/test outcomes are recorded in the main continuation log.
