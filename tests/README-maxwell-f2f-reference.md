# Maxwell F2F: exact Python reference vs Metal

```sh
ninja -C build/swiftui-release -j 6 hydra-maxwell-alu-reference-tests
PYTHONDONTWRITEBYTECODE=1 python3 tests/maxwell_f2f_reference.py \
  build/swiftui-release/bin/hydra-maxwell-alu-reference-tests \
  --report /tmp/hydra-f2f-reference.json
```

The oracle uses exact rational numbers and NVIDIA's [PTX cvt rules](https://docs.nvidia.com/cuda/parallel-thread-execution/#data-movement-and-conversion-instructions-cvt).
It covers all nine source/destination pairs of F16/F32/F64, all four precision
rounding modes, and nearest-even/floor/ceil/trunc integer rounding for equal
formats. It also checks abs/neg, SAT, F32 input/output FTZ, F16 upper-half
selection, signed zeros, NaNs/infinities, subnormals, rounding ties and their
immediate neighbors, overflow, underflow, random bits and instruction guards.

The initial register suite had 3,248 mismatches among 50,624 cases. The final
suite has **56,240 cases**, including immediate and cbuf encodings and destination
registers overlapping the low or high source word. All eight returned registers,
seven predicate bits and four condition flags are checked. Guarded-off
instructions must preserve all state exactly, including NaN payloads.

For executed conversions, NaN payloads and the sign of SAT zero are deliberately
excluded from the comparison. Other results are bit-exact, without tolerance or
known-gap allowances. This is a Python model vs real Apple Metal execution,
not a physical NVIDIA GPU comparison. PTX specifies numeric semantics, not
Maxwell SASS encodings; the latter follow the existing offline field layout.

Production fixes:

- Precision rounding fields reach all narrowing paths. Raw rounding value 1
  is RM when the sizes differ; its equal-size pass interpretation must not be
  applied to narrowing conversions. Older C++ nearest-rounding cases used raw
  value 1 and now use value 0.
- F64 -> F16 rounds once, avoiding the former F64 -> F32 -> F16 double rounding.
- All F2F helpers share integer bit conversion, preserving subnormals without
  depending on host fp32 behavior. SAT and single-precision FTZ also reach the
  F64 paths. NaN payload bits are retained where possible.
- The file-case runner accepts an optional `CBUF <hex words...>` header for
  constant slot 2. It feeds the same production decoder and MSL emitter as the
  register and immediate cases.

Conversion `.CC` remains **unimplemented**. NVIDIA's [Maxwell instruction list](https://docs.nvidia.com/cuda/archive/12.6.1/cuda-binary-utilities/index.html#maxwell-and-pascal-instruction-set)
names the instructions, but the consulted primary sources do not specify the
conversion side effects on Maxwell Z/S/C/O. PTX's numeric `cvt` rules cannot
establish those SASS flag rules. Implementation needs a primary specification
or reproducible NVIDIA execution traces, especially for NaN, saturation and
64-bit results. No assumed `.CC` oracle has been added.
