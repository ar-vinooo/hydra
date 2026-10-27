#!/usr/bin/env python3
"""Reference check for blend + render-target format behaviour.

Expected values are computed here from the blend equations (NVIDIA
SET_BLEND_* in clb197.h == OpenGL/D3D rules) and the storage format rules
(IEEE half, unorm clamp, unsigned 11/10-bit float). tests/blend_reference_tests
runs the same cases through Hydra's format table and Metal; this script
compares the raw bytes.

Usage: blend_reference.py <path to hydra-blend-reference-tests>
"""
import itertools, math, subprocess, sys, tempfile, os
import struct

def f32(x):
    return struct.unpack('f', struct.pack('f', x))[0]

FORMATS = ["RGBA16Float", "RG11B10Float", "RGBA8Unorm"]
FACTORS = ["zero", "one", "src_alpha", "inv_src_alpha", "src_color", "dst_color"]
OPS = ["add", "sub", "rsub", "min", "max"]
# (dst, src) pairs, chosen away from x.5/255 rounding ties; src above 1.0 and below 0 are the HDR cases
VALUES = [
    ((0.31, 0.21, 0.11, 1.0), (2.0, 1.5, 0.5, 0.5)),
    ((0.0, 0.0, 0.0, 1.0), (4.0, 4.0, 4.0, 1.0)),
    ((0.5, 0.5, 0.5, 0.5), (0.25, 0.5, 0.75, 0.25)),
    ((0.91, 0.91, 0.91, 1.0), (0.91, 0.91, 0.91, 1.0)),
]

def factor(name, src, dst):
    return {
        "zero": [0.0] * 4, "one": [1.0] * 4,
        "src_alpha": [src[3]] * 4,
        "inv_src_alpha": [f32(1 - src[3])] * 4,
        "src_color": list(src), "dst_color": list(dst),
    }[name]

def blend(op, s, d, sf, df):
    a = [f32(x * y) for x, y in zip(s, sf)]
    b = [f32(x * y) for x, y in zip(d, df)]
    if op == "add": return [f32(x + y) for x, y in zip(a, b)]
    if op == "sub": return [f32(x - y) for x, y in zip(a, b)]
    if op == "rsub": return [f32(y - x) for x, y in zip(a, b)]
    if op == "min": return [min(x, y) for x, y in zip(s, d)]
    return [max(x, y) for x, y in zip(s, d)]

def small_float(x, exp_bits, man_bits):
    """Unsigned float with 5 exponent bits (R11G11B10): negative -> 0."""
    if math.isnan(x): return float("nan")
    if x <= 0: return 0.0
    bias = 15
    max_val = (2 - 2.0 ** -man_bits) * 2.0 ** (31 - 1 - bias + 1 - 1)  # exp 30
    max_val = (1 + (2 ** man_bits - 1) / 2 ** man_bits) * 2.0 ** 15
    if x >= max_val: return max_val
    e = math.floor(math.log2(x))
    if e < -14:  # denormal
        step = 2.0 ** (-14 - man_bits)
        return round(x / step) * step
    step = 2.0 ** (e - man_bits)
    return round(x / step) * step

def half(x):
    try:
        return struct.unpack('e', struct.pack('e', x))[0]
    except OverflowError:
        return math.copysign(float("inf"), x)

def quantize(fmt, v):
    if fmt == "RGBA16Float":
        return [half(x) for x in v]
    if fmt == "RGBA8Unorm":
        return [round(min(max(x, 0.0), 1.0) * 255) / 255 for x in v]
    return [small_float(v[0], 5, 6), small_float(v[1], 5, 6),
            small_float(v[2], 5, 5), 1.0]  # alpha reads as 1

def decode(fmt, hexstr):
    b = bytes.fromhex(hexstr)
    if fmt == "RGBA16Float":
        return list(struct.unpack('<4e', b[:8]))
    if fmt == "RGBA8Unorm":
        return [x / 255 for x in b[:4]]
    w = int.from_bytes(b[:4], "little")
    def f(bits, m):
        ex, ma = bits >> m, bits & ((1 << m) - 1)
        if ex == 0: return ma * 2.0 ** (-14 - m)
        if ex == 31: return float("inf") if ma == 0 else float("nan")
        return (1 + ma / 2 ** m) * 2.0 ** (ex - 15)
    return [f(w & 0x7ff, 6), f((w >> 11) & 0x7ff, 6), f((w >> 22) & 0x3ff, 5), 1.0]

def main():
    binary = sys.argv[1]
    cases = [(f, sf, df, op, d, s) for f in FORMATS
             for sf, df in itertools.product(FACTORS, FACTORS)
             for op in OPS for d, s in VALUES]
    with tempfile.TemporaryDirectory() as t:
        cin, cout = os.path.join(t, "c"), os.path.join(t, "o")
        with open(cin, "w") as fh:
            for f, sf, df, op, d, s in cases:
                fh.write(f"{f} {sf} {df} {op} " +
                         " ".join(map(repr, d + s)) + "\n")
        subprocess.run([binary, cin, cout], check=True)
        got = open(cout).read().split()
    assert len(got) == len(cases), (len(got), len(cases))
    bad = 0
    for (f, sf, df, op, d, s), g in zip(cases, got):
        dq = quantize(f, d)               # destination as stored
        sa = [f32(x) for x in s]
        if f == "RGBA8Unorm":
            sa = [min(max(x, 0.0), 1.0) for x in sa]  # unorm clamps the source first
        e = quantize(f, blend(op, sa, dq, factor(sf, sa, dq), factor(df, sa, dq)))
        o = decode(f, g)
        tol = {"RGBA16Float": 2e-3, "RGBA8Unorm": 1.5 / 255,
               "RG11B10Float": 4e-2}[f]
        ok = all(abs(x - y) <= tol * max(1, abs(x)) or x == y for x, y in zip(e, o))
        if not ok:
            bad += 1
            if bad <= 15:
                print(f"MISMATCH {f} {op} src={sf} dst={df} d={d} s={s}\n"
                      f"   expected {e}\n   metal    {o}")
    print(f"{len(cases) - bad}/{len(cases)} cases match the reference")
    return 1 if bad else 0

sys.exit(main())
