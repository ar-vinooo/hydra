# Maxwell predicate reference: Python vs Metal

```sh
ninja -C build/swiftui-release -j 6 hydra-maxwell-alu-reference-tests
PYTHONDONTWRITEBYTECODE=1 python3 tests/maxwell_predicate_reference.py \
  build/swiftui-release/bin/hydra-maxwell-alu-reference-tests \
  --report /tmp/hydra-predicate-reference.json
```

Python encodes Maxwell FSETP/ISETP register instructions and computes expected
comparison/predicate results independently. The C++ runner decodes those words,
emits MSL through Hydra and executes them on Apple Metal. The script enables
Metal API validation and compares every returned word: eight general registers,
seven predicate bits and four condition flags. Any mismatch or missing row fails.

Seed 19 generates 499,296 cases: 35,616 unsigned ISETP, 35,616 signed ISETP,
131,712 FSETP and 296,352 FSETP.FTZ. Cases cover all comparison encodings,
AND/OR/XOR, source-predicate inversion, instruction guards and inversion,
float abs/neg combinations, signed zeros, infinities, quiet/signaling NaN bit
patterns, subnormals, integer boundaries and seeded random operand words.
Destination predicates are distinct P0/P1, source is P2 and guard is P3;
unrelated predicates, registers and condition flags must remain unchanged.

The semantic reference is NVIDIA's [PTX ISA, setp](https://docs.nvidia.com/cuda/parallel-thread-execution/#comparison-and-selection-instructions-setp):
the two destinations combine the comparison and its complement separately with
the input predicate. Ordered comparisons reject NaN, unordered comparisons
accept it, and FTZ converts subnormal inputs to signed zero. Python binary64
represents every finite binary32 input exactly, so numeric comparisons require
no tolerance or rounding approximation.

PTX is a virtual ISA, not a published Maxwell SASS encoding specification.
Maxwell encodings here follow the offline field layout, as in the existing ALU
reference; they are not independently certified by PTX. This is a software
reference versus a real Apple GPU, **not physical NVIDIA GPU execution**.
Immediate/constant-buffer forms, .X, destination/source aliasing, half/double,
full guest programs and gameplay remain outside this Python suite.

Before the fix, 1,024 FSETP cases failed because native host fp32 comparison
flushed subnormals. Integer order keys now preserve them, normalize both zeros,
detect NaN from bits and implement FTZ explicitly. All 499,296 cases pass without
a known-gap allowance. The existing half comparison regression no longer
substitutes host-flushed zeros for F32-swizzled subnormal inputs.
