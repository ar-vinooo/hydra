#!/usr/bin/env python3
"""Maxwell ALU instruction reference vs Hydra decoder -> MSL -> Metal.

Instructions are encoded here from the Maxwell field layout (envytools / nvdisasm
conventions); expected results are computed independently with exact rational
arithmetic (Fractions) rounded to IEEE binary32, or with integer math. Hydra runs
the same guest encodings through its decoder, MSL emitter and Metal compute, and
the raw 32-bit register results are compared. No physical NVIDIA execution: the
reference is the documented ISA/IEEE behaviour, not hardware traces.

Usage: maxwell_alu_reference.py <hydra-maxwell-alu-reference-tests> [--report f.json]
                                [--group name] [--seed n]
Exit status is non-zero when a group not listed in KNOWN_GAPS has a mismatch, or
when a KNOWN_GAPS group unexpectedly passes completely (so stale entries get removed).
"""
import argparse
import collections
import json
import math
import random
import struct
import subprocess
import sys
import tempfile
from fractions import Fraction
from pathlib import Path

# Groups where Hydra is known to differ from the reference. Each value says why.
# Filled in from observed mismatches; see the docs for the investigation status.
HOST_DENORMAL = ('Apple GPU flushes some fp32 denormal inputs/results (probe: c+0.0f -> 0 while '
                 'c*1.0f keeps it); Maxwell keeps denormals unless .FTZ')
KNOWN_GAPS = {name + '_denormal': HOST_DENORMAL for name in (
    'fadd', 'fadd_sat', 'fmul', 'fmul_sat', 'fmul_scale', 'fmul_scale_sat',
    'ffma', 'ffma_sat', 'fmnmx')}

PT = 7


# --------------------------------------------------------------------------- floats
def bits_f(b):
    return struct.unpack('<f', struct.pack('<I', b & 0xffffffff))[0]


def f_bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


NAN = 0x7fc00000
INF = 0x7f800000


def classify(b):
    e = (b >> 23) & 0xff
    m = b & 0x7fffff
    if e == 0xff:
        return ('nan', None, 0) if m else ('inf', None, b >> 31)
    sign = b >> 31
    if e == 0:
        v = Fraction(m, 1 << 149)
    else:
        v = Fraction(m | 0x800000) * Fraction(2) ** (e - 150)
    return ('fin', -v if sign else v, sign)


def flush(b):
    """FTZ: denormal -> zero of the same sign."""
    if (b >> 23) & 0xff == 0:
        return b & 0x80000000
    return b


def round_fraction(fr, sign_if_zero=0):
    """Round an exact rational to binary32 (RN-even); return bits."""
    if fr == 0:
        return sign_if_zero << 31
    sign = 1 if fr < 0 else 0
    a = abs(fr)
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if a < Fraction(2) ** e:
        e -= 1
    e_eff = max(e, -126)
    q = a / (Fraction(2) ** (e_eff - 23))
    m = q.numerator // q.denominator
    rem = q - m
    if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and (m & 1)):
        m += 1
    value = math.ldexp(m, e_eff - 23)
    if value > 3.4028234663852886e38:
        return (sign << 31) | INF
    return (sign << 31) | f_bits(value) & 0x7fffffff


def neg(b):
    return b ^ 0x80000000


def absf(b):
    return b & 0x7fffffff


def fadd(x, y):
    kx, vx, sx = classify(x)
    ky, vy, sy = classify(y)
    if kx == 'nan' or ky == 'nan':
        return NAN
    if kx == 'inf' and ky == 'inf':
        return NAN if sx != sy else x
    if kx == 'inf':
        return x
    if ky == 'inf':
        return y
    zero_sign = 1 if (sx and sy) else 0
    return round_fraction(vx + vy, zero_sign)


def fmul(x, y):
    kx, vx, sx = classify(x)
    ky, vy, sy = classify(y)
    if kx == 'nan' or ky == 'nan':
        return NAN
    if kx == 'inf' or ky == 'inf':
        if (kx == 'fin' and vx == 0) or (ky == 'fin' and vy == 0):
            return NAN
        return ((sx ^ sy) << 31) | INF
    return round_fraction(vx * vy, sx ^ sy)


def ffma(x, y, z):
    kx, vx, sx = classify(x)
    ky, vy, sy = classify(y)
    kz, vz, sz = classify(z)
    if 'nan' in (kx, ky, kz):
        return NAN
    prod_inf = kx == 'inf' or ky == 'inf'
    if prod_inf:
        if (kx == 'fin' and vx == 0) or (ky == 'fin' and vy == 0):
            return NAN
        ps = sx ^ sy
        if kz == 'inf' and sz != ps:
            return NAN
        return (ps << 31) | INF
    if kz == 'inf':
        return z
    p = vx * vy
    ps = sx ^ sy
    total = p + vz
    zero_sign = 1 if (ps and sz) else 0
    return round_fraction(total, zero_sign)


def sat_f(b):
    k, v, s = classify(b)
    if k == 'nan':
        return 0
    if k == 'inf':
        return 0x3f800000 if not s else 0
    if v <= 0:
        return 0
    if v >= 1:
        return 0x3f800000
    return b


def is_nan(b):
    return (b & 0x7f800000) == 0x7f800000 and (b & 0x7fffff) != 0


def fcompare_value(b):
    k, v, s = classify(b)
    return v


def fminmax(a, b, want_min):
    if is_nan(a) and is_nan(b):
        return NAN
    if is_nan(a):
        return b
    if is_nan(b):
        return a

    def key(x):
        k, v, s = classify(x)
        if k == 'inf':
            return float('-inf') if s else float('inf')
        return v

    ka, kb = key(a), key(b)
    if ka == kb:
        return a  # +-0 ambiguity is normalised by the comparison
    if want_min:
        return a if ka < kb else b
    return a if ka > kb else b


# ------------------------------------------------------------------- encodings
def base(op, dst, a, pred=PT):
    return (op << 48) | dst | (a << 8) | (pred << 16)


def enc_fadd(dst, a, b, neg_a=0, neg_b=0, abs_a=0, abs_b=0, sat=0, ftz=0):
    return (base(0x5c58, dst, a) | (b << 20) | (ftz << 44) | (neg_b << 45) |
            (abs_a << 46) | (neg_a << 48) | (abs_b << 49) | (sat << 50))


def enc_fmul(dst, a, b, neg_b=0, sat=0, scale=0):
    return base(0x5c68, dst, a) | (b << 20) | (scale << 41) | (neg_b << 48) | (sat << 50)


def enc_ffma(dst, a, b, c, neg_b=0, neg_c=0, sat=0):
    return (base(0x5980, dst, a) | (b << 20) | (c << 39) | (neg_b << 48) |
            (neg_c << 49) | (sat << 50))


def enc_fmnmx(dst, a, b, inv_pred=0, neg_a=0, neg_b=0, abs_a=0, abs_b=0):
    return (base(0x5c60, dst, a) | (b << 20) | (PT << 39) | (inv_pred << 42) |
            (neg_b << 45) | (abs_a << 46) | (neg_a << 48) | (abs_b << 49))


def enc_mufu(dst, a, op, neg_a=0, abs_a=0, sat=0):
    return (base(0x5080, dst, a) | (op << 20) | (abs_a << 46) | (neg_a << 48) | (sat << 50))


def enc_rro(dst, b, ex2=0):
    return base(0x5c90, dst, 0xff) | (b << 20) | (ex2 << 39)


def enc_iadd(dst, a, b, mode=0, sat=0):
    return base(0x5c10, dst, a) | (b << 20) | (mode << 48) | (sat << 50)


def enc_iadd32i(dst, a, imm):
    return base(0x1c00, dst, a) | (imm << 20)


def enc_lop(dst, a, b, op, inv_a=0, inv_b=0):
    return (base(0x5c40, dst, a) | (b << 20) | (inv_a << 39) | (inv_b << 40) |
            (op << 41) | (3 << 44) | (PT << 48))


def enc_lop3(dst, a, b, c, lut):
    return base(0x5be0, dst, a) | (b << 20) | (lut << 28) | (c << 39) | (PT << 48)


def enc_shl(dst, a, b, wrap=0):
    return base(0x5c48, dst, a) | (b << 20) | (wrap << 39)


def enc_shr(dst, a, b, signed=0, wrap=0):
    return base(0x5c28, dst, a) | (b << 20) | (wrap << 39) | (signed << 48)


def enc_iscadd(dst, a, b, shift):
    return base(0x5c18, dst, a) | (b << 20) | (shift << 39)


def enc_imnmx(dst, a, b, signed=0, inv_pred=0):
    return base(0x5c20, dst, a) | (b << 20) | (PT << 39) | (inv_pred << 42) | (signed << 48)


def enc_popc(dst, b, inv=0):
    return base(0x5c08, dst, 0xff) | (b << 20) | (inv << 40)


def enc_flo(dst, b, sh=0, signed=0, inv=0):
    return base(0x5c30, dst, 0xff) | (b << 20) | (inv << 40) | (sh << 41) | (signed << 48)


# ------------------------------------------------------------------- input pools
def f(x):
    return f_bits(x)


SPECIAL_F = [
    0x00000000, 0x80000000, f(1.0), f(-1.0), f(0.5), f(-0.5), f(1.5), f(2.0), f(3.0),
    f(0.1), f(-0.1), f(1e-3), f(123456.789), f(-98765.4), f(1e30), f(-1e30),
    f(1e-30), 0x7f7fffff, 0xff7fffff, INF, 0xff800000, NAN,
    0x00800000, 0x007fffff, 0x00000001, 0x80000001, 0x00400000, f(65504.0), f(16777216.0),
]

NORMAL_F = [x for x in SPECIAL_F if classify(x)[0] == 'fin' and
            (x & 0x7f800000) != 0]


DENORMALS = [0x00000001, 0x80000001, 0x00400000, 0x807fffff, 0x007fffff, 0x00012345]


def float_triples(rng, count, pool=SPECIAL_F):
    # Denormal-heavy cases first so every group's slice exercises them.
    front = []
    for d in DENORMALS:
        for other in (0x00000000, 0x80000000, f(1.0), f(-2.5), 0x00000001, 0x00400000):
            front.append((d, other, rng.choice(DENORMALS)))
            front.append((other, d, rng.choice(DENORMALS + [0])))
    rng.shuffle(front)
    out = []
    for a in pool[:12]:
        for b in pool[:12]:
            out.append((a, b, rng.choice(pool)))
    while len(out) < count:
        out.append(tuple(rng.choice(pool) if rng.random() < 0.6 else rng.getrandbits(32)
                         for _ in range(3)))
    rng.shuffle(out)
    return front[:24] + out[:count]


INT_SPECIAL = [0, 1, 2, 3, 31, 32, 33, 63, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff,
               0xfffffffe, 0x0000ffff, 0xffff0000, 0x12345678, 0xdeadbeef, 0x55555555, 0xaaaaaaaa]


def int_triples(rng, count):
    out = []
    for a in INT_SPECIAL:
        for b in INT_SPECIAL:
            out.append((a, b, rng.choice(INT_SPECIAL)))
    while len(out) < count:
        out.append(tuple(rng.getrandbits(32) for _ in range(3)))
    rng.shuffle(out)
    return out[:count]


# --------------------------------------------------------------------- case sets
Case = collections.namedtuple('Case', 'group inst regs expected tolerance')


def make_float_cases(rng):
    cases = []
    triples = float_triples(rng, 96)
    for ftz in (0, 1):
        for sat in (0, 1):
            for na in (0, 1):
                for nb in (0, 1):
                    for aa in (0, 1):
                        for ab in (0, 1):
                            inst = enc_fadd(3, 0, 1, na, nb, aa, ab, sat, ftz)
                            group = 'fadd_ftz' if ftz else 'fadd'
                            if sat:
                                group += '_sat'
                            for a, b, _ in triples[:24]:
                                x = flush(a) if ftz else a
                                y = flush(b) if ftz else b
                                if aa: x = absf(x)
                                if na: x = neg(x)
                                if ab: y = absf(y)
                                if nb: y = neg(y)
                                r = fadd(x, y)
                                if ftz: r = flush(r)
                                if sat: r = sat_f(r)
                                cases.append(Case(group, inst, (a, b, 0), r, 'zero_sign' if sat else None))
    for scale, mult in ((0, 1), (1, Fraction(1, 2)), (4, 8), (5, 4), (6, 2)):
        for sat in (0, 1):
            for nb in (0, 1):
                inst = enc_fmul(3, 0, 1, nb, sat, scale)
                group = 'fmul' + ('_scale' if scale else '') + ('_sat' if sat else '')
                for a, b, _ in triples[:48]:
                    y = neg(b) if nb else b
                    k1, v1, s1 = classify(a)
                    k2, v2, s2 = classify(y)
                    if k1 == 'fin' and k2 == 'fin':
                        r = round_fraction(v1 * v2 * mult, s1 ^ s2)
                    else:
                        r = fmul(a, y)
                        if mult != 1 and not is_nan(r) and absf(r) != INF:
                            r = fmul(r, f_bits(float(mult)))
                    if sat: r = sat_f(r)
                    cases.append(Case(group, inst, (a, b, 0), r, 'zero_sign' if sat else None))
    for sat in (0, 1):
        for nb in (0, 1):
            for nc in (0, 1):
                inst = enc_ffma(3, 0, 1, 2, nb, nc, sat)
                group = 'ffma' + ('_sat' if sat else '')
                for a, b, c in triples:
                    y = neg(b) if nb else b
                    z = neg(c) if nc else c
                    r = ffma(a, y, z)
                    if sat: r = sat_f(r)
                    cases.append(Case(group, inst, (a, b, c), r, 'zero_sign' if sat else None))
    for inv in (0, 1):
        for na in (0, 1):
            for nb in (0, 1):
                for aa in (0, 1):
                    for ab in (0, 1):
                        inst = enc_fmnmx(3, 0, 1, inv, na, nb, aa, ab)
                        for a, b, _ in triples[:32]:
                            x = absf(a) if aa else a
                            if na: x = neg(x)
                            y = absf(b) if ab else b
                            if nb: y = neg(y)
                            cases.append(Case('fmnmx', inst, (a, b, 0),
                                              fminmax(x, y, not inv), 'zero_sign'))
    return cases


def ulp_distance(x, y):
    def ordered(b):
        return -(b & 0x7fffffff) if b >> 31 else b
    return abs(ordered(x) - ordered(y))


def make_mufu_cases(rng):
    cases = []
    inputs = [f(v) for v in (0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 10.0, 100.0, 0.001,
                            12345.0, -0.5, -1.0, -3.5, 0.0, 1e-10, 1e10)]
    inputs += [f(rng.uniform(0.01, 8)) for _ in range(24)]
    inputs += [f(-rng.uniform(0.01, 8)) for _ in range(12)]
    inputs += [f(rng.uniform(-60, 60)) for _ in range(24)]

    def ex2(x):
        return 2.0 ** x if x < 128 else float('inf')

    def sign0(b):
        return -1.0 if b >> 31 else 1.0

    def rcp(b, x):
        if x == 0: return sign0(b) * float('inf')
        return 1.0 / x

    def rsq(b, x):
        if x == 0: return sign0(b) * float('inf')
        if x < 0: return float('nan')
        return 1.0 / math.sqrt(x)

    def sqrt(b, x):
        if x == 0: return x
        if x < 0: return float('nan')
        return math.sqrt(x)

    def lg2(b, x):
        if x == 0: return float('-inf')
        if x < 0: return float('nan')
        return math.log2(x)

    ops = {
        # Real Maxwell shaders run MUFU.SIN/COS on the output of RRO.SINCOS
        # (e.g. FMUL32I 2*pi; RRO.SINCOS; MUFU.SIN), so the pair is sin(x) in radians.
        'mufu_cos': (0, lambda b, x: math.cos(x)),
        'mufu_sin': (1, lambda b, x: math.sin(x)),
        'mufu_ex2': (2, lambda b, x: ex2(x)),
        'mufu_lg2': (3, lg2),
        'mufu_rcp': (4, rcp),
        'mufu_rsq': (5, rsq),
        'mufu_sqrt': (8, sqrt),
    }
    for group, (op, fn) in ops.items():
        inst = enc_mufu(3, 0, op)
        if op in (0, 1):
            inst = '%016x,%016x' % (enc_rro(3, 0), enc_mufu(3, 3, op))
        for b in inputs:
            x = bits_f(b)
            try:
                y = fn(b, x)
            except (OverflowError, ValueError):
                y = float('nan')
            if group in ('mufu_sin', 'mufu_cos'):
                expected = ('abs', y, 2e-6)
            else:
                expected = ('rel', y, 4)
            cases.append(Case(group, inst, (b, 0, 0), expected, 'mufu'))
    return cases


def s32(x):
    return x - (1 << 32) if x & 0x80000000 else x


def make_int_cases(rng):
    cases = []
    triples = int_triples(rng, 128)
    M = 0xffffffff
    for mode, name in ((0, 'iadd'), (1, 'iadd_neg_b'), (2, 'iadd_neg_a')):
        inst = enc_iadd(3, 0, 1, mode)
        for a, b, _ in triples:
            x = (-a if mode == 2 else a)
            y = (-b if mode == 1 else b)
            cases.append(Case(name, inst, (a, b, 0), (x + y) & M, None))
    inst = enc_iadd(3, 0, 1, 0, 1)
    for a, b, _ in triples:
        r = max(-(1 << 31), min((1 << 31) - 1, s32(a) + s32(b)))
        cases.append(Case('iadd_sat', inst, (a, b, 0), r & M, None))
    for imm in (0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x12345678):
        inst = enc_iadd32i(3, 0, imm)
        for a, _, _ in triples[:16]:
            cases.append(Case('iadd32i', inst, (a, 0, 0), (a + imm) & M, None))
    ops = {0: lambda x, y: x & y, 1: lambda x, y: x | y, 2: lambda x, y: x ^ y, 3: lambda x, y: y}
    for op, fn in ops.items():
        for ia in (0, 1):
            for ib in (0, 1):
                inst = enc_lop(3, 0, 1, op, ia, ib)
                for a, b, _ in triples[:24]:
                    x = a ^ M if ia else a
                    y = b ^ M if ib else b
                    cases.append(Case('lop', inst, (a, b, 0), fn(x, y) & M, None))
    for lut in list(range(0, 256, 5)) + [0x80, 0xe8, 0x96, 0xca, 0xf0, 0xcc, 0xaa, 0x01, 0xfe]:
        inst = enc_lop3(3, 0, 1, 2, lut)
        for a, b, c in triples[:16]:
            r = 0
            for bit in range(32):
                idx = (((a >> bit) & 1) << 2) | (((b >> bit) & 1) << 1) | ((c >> bit) & 1)
                r |= ((lut >> idx) & 1) << bit
            cases.append(Case('lop3', inst, (a, b, c), r, None))
    for wrap in (0, 1):
        inst = enc_shl(3, 0, 1, wrap)
        for a, b, _ in triples:
            sh = b & 31 if wrap else b
            cases.append(Case('shl', inst, (a, b, 0), (a << sh) & M if sh < 32 else 0, None))
        for signed in (0, 1):
            inst = enc_shr(3, 0, 1, signed, wrap)
            for a, b, _ in triples:
                sh = b & 31 if wrap else b
                if signed:
                    r = (s32(a) >> min(sh, 31)) & M
                else:
                    r = a >> sh if sh < 32 else 0
                cases.append(Case('shr' + ('_signed' if signed else ''), inst, (a, b, 0), r, None))
    for shift in (0, 1, 2, 5, 31):
        inst = enc_iscadd(3, 0, 1, shift)
        for a, b, _ in triples[:16]:
            cases.append(Case('iscadd', inst, (a, b, 0), ((a << shift) + b) & M, None))
    for signed in (0, 1):
        for inv in (0, 1):
            inst = enc_imnmx(3, 0, 1, signed, inv)
            for a, b, _ in triples[:48]:
                x, y = (s32(a), s32(b)) if signed else (a, b)
                r = (min(x, y) if not inv else max(x, y)) & M
                cases.append(Case('imnmx' + ('_signed' if signed else ''), inst, (a, b, 0), r, None))
    for inv in (0, 1):
        inst = enc_popc(3, 1, inv)
        for a, b, _ in triples[:48]:
            cases.append(Case('popc', inst, (a, b, 0), bin((b ^ M) if inv else b).count('1'), None))
    for sh in (0, 1):
        inst = enc_flo(3, 1, sh)
        for a, b, _ in triples[:48]:
            r = M if b == 0 else b.bit_length() - 1
            if sh and b != 0:
                r = 31 - r
            cases.append(Case('flo', inst, (a, b, 0), r, None))
    return cases


def is_denormal(b):
    return (b & 0x7f800000) == 0 and (b & 0x7fffff) != 0


def split_denormals(cases):
    """Cases touching a nonzero denormal (input or exact result) get their own group."""
    out = []
    for c in cases:
        if c.group.startswith('f') and not c.group.endswith('_ftz') and 'ftz' not in c.group:
            exp = c.expected if isinstance(c.expected, int) else 0
            if any(is_denormal(v) for v in (*c.regs, exp)):
                c = c._replace(group=c.group + '_denormal')
        out.append(c)
    return out


def run(binary, cases):
    with tempfile.TemporaryDirectory() as tmp:
        cases_path = Path(tmp) / 'cases.txt'
        out_path = Path(tmp) / 'out.txt'
        with cases_path.open('w') as f_:
            for c in cases:
                inst = c.inst if isinstance(c.inst, str) else '%016x' % c.inst
                f_.write('%s 0 0 %x %x %x 0 0 0 0 0\n' % (inst, *c.regs))
        completed = subprocess.run([str(binary), str(cases_path), str(out_path)])
        if completed.returncode != 0:
            raise SystemExit('runner failed: %d' % completed.returncode)
        rows = [list(map(lambda x: int(x, 16), line.split())) for line in out_path.read_text().splitlines()]
    if len(rows) != len(cases):
        raise SystemExit('incomplete readback: %d of %d' % (len(rows), len(cases)))
    return rows


def matches(case, row):
    regs, preds, cc = row[:8], row[8], row[9]
    if regs[0:3] != list(case.regs) or regs[4:] != [0] * 4 or preds != 0:
        return False, 'clobbered state'
    got = regs[3]
    exp = case.expected
    if case.tolerance == 'mufu':
        kind, y, tol = exp
        if y != y:
            return is_nan(got), None
        if kind == 'abs':
            return abs(bits_f(got) - y) <= tol, None
        if math.isinf(y):
            return got == (INF if y > 0 else 0xff800000), None
        expect_bits = f_bits(y) if abs(y) < 3.4e38 else INF
        return ulp_distance(got, expect_bits) <= tol, None
    if is_nan(exp):
        return is_nan(got), None
    if got == exp:
        return True, None
    if case.tolerance == 'zero_sign' and absf(got) == 0 and absf(exp) == 0:
        return True, None
    return False, None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--group')
    parser.add_argument('--seed', type=int, default=5)
    parser.add_argument('--show', type=int, default=3)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    cases = make_float_cases(rng) + make_mufu_cases(rng) + make_int_cases(rng)
    cases = split_denormals(cases)
    if args.group:
        cases = [c for c in cases if c.group == args.group]
    rows = run(args.binary, cases)
    stats = collections.OrderedDict()
    examples = collections.defaultdict(list)
    for case, row in zip(cases, rows):
        ok, _ = matches(case, row)
        s = stats.setdefault(case.group, [0, 0])
        s[0] += ok
        s[1] += 1
        if not ok and len(examples[case.group]) < args.show:
            examples[case.group].append({
                'inst': case.inst if isinstance(case.inst, str) else '%016x' % case.inst,
                'inputs': ['%08x' % r for r in case.regs],
                'expected': case.expected if not isinstance(case.expected, int) else '%08x' % case.expected,
                'got': '%08x' % row[3]})
    unexpected = 0
    for group, (passed, total) in stats.items():
        known = KNOWN_GAPS.get(group)
        status = 'ok ' if passed == total else ('GAP' if known else 'BAD')
        print('%s %-16s %5d/%-5d%s' % (status, group, passed, total, '  (' + known + ')' if known else ''))
        if passed != total and not known:
            unexpected += 1
            for e in examples[group]:
                print('      ', e)
        if passed == total and known:
            unexpected += 1
            print('       known gap now passes completely; remove it from KNOWN_GAPS')
    if args.report:
        args.report.write_text(json.dumps({'groups': stats, 'examples': examples,
                                           'cases': len(cases), 'seed': args.seed}, indent=2, default=str))
    print('%d cases, %d groups with unexpected results' % (len(cases), unexpected))
    return 1 if unexpected else 0


if __name__ == '__main__':
    sys.exit(main())
