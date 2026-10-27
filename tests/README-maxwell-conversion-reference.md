# Maxwell conversions: Python vs Metal

```sh
ninja -C build/swiftui-release -j 6 hydra-maxwell-alu-reference-tests
PYTHONDONTWRITEBYTECODE=1 python3 tests/maxwell_conversion_reference.py \
  build/swiftui-release/bin/hydra-maxwell-alu-reference-tests \
  --report /tmp/hydra-conversion-reference.json
```

The oracle uses NVIDIA's [PTX cvt semantics](https://docs.nvidia.com/cuda/parallel-thread-execution/#data-movement-and-conversion-instructions-cvt)
for rounding, clamping, FTZ, sign extension and NaN conversion. PTX does not
specify Maxwell SASS encodings: these use the offline layout, as in the ALU
reference. No physical NVIDIA GPU is used. Metal validation is enabled by the
script; the C++ runner executes actual Maxwell decoder -> IR -> MSL output.

Python represents binary32 inputs as exact rational values before rounding to
integers. Integer-to-float references quantize exact Python integers to the
destination precision, then pack an exactly representable F16/F32/F64 result.
Comparisons are bit-exact, with no tolerance or known-gap exemptions.

Seed 19 covers **197,184 cases**:

| Group | Cases |
| --- | ---: |
| F32 -> S/U16/32/64, no FTZ | 33,984 |
| F32 -> S/U16/32/64, FTZ | 33,984 |
| S/U8/16/32/64 -> F16/F32/F64, RN | 32,304 |
| Same, RM | 32,304 |
| Same, RP | 32,304 |
| Same, RZ | 32,304 |

Cases include half-way ties, mantissa carry, F16 overflow, integer saturation,
NaN, infinity, signed zero, binary32 subnormals, integer boundaries, seeded
random words, byte/half selection and normal/inverted instruction guards.
F2I abs/neg combinations are included. Each result checks all eight returned
registers, seven predicates and four CC flags, including preserved state and
the high output word for 64-bit destinations.

The initial smaller 83,952-case suite failed on 60 F2I cases and 899 I2F cases.
The former lost subnormal inputs during host floor/ceil; the latter ignored
RM/RP/RZ. F2I now substitutes a normal signed magnitude below 0.5 before integer
rounding when FTZ is off: all integer rounding modes have the same result as
for the original subnormal. FTZ instead substitutes signed zero. I2F carries
the rounding field into shared integer-based significand rounding for all
three float destination widths, including the existing FP64 software path.

The expanded suite adds 10,560 cases for immediate/cbuf operands and destinations
overlapping either source word, for a total of **207,744 cases**. These additional
cases cover F32 -> S/U16/32/64 and S/U8/16/32/64 -> F16/F32/F64 with four rounding
modes. The file header `CBUF <hex words...>` supplies slot 2; constants and
immediate fields are independently constructed in Python. The 197,184-case
register baseline above remains unchanged.

Limits: source F16/F64 F2I, conversion CC output, RZ boundaries,
I2F abs/neg and F2F rounding are outside this Python suite. Existing C++ shader
regressions still cover some of those paths. PTX's NaN rule for 64-bit integer
destinations is documented explicitly in the current specification; older
manuals described NaN conversion less precisely. This is reference-model
validation, not a Maxwell hardware trace or gameplay certification.

F2F now has its own [exact Python suite](README-maxwell-f2f-reference.md).
