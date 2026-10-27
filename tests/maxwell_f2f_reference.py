#!/usr/bin/env python3
"""Exact Python IEEE/PTX cvt model vs Maxwell F2F -> MSL -> Metal.

https://docs.nvidia.com/cuda/parallel-thread-execution/#data-movement-and-conversion-instructions-cvt
PTX specifies numeric behaviour, not Maxwell SASS encodings. No NVIDIA GPU.
NaN payloads and saturated zero signs are not certified by this reference.
"""
import argparse
import collections
from fractions import Fraction
import itertools
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile

FORMATS = {1: (10, 15), 2: (23, 127), 3: (52, 1023)}


def unpack(bits, fmt):
    fraction, bias = FORMATS[fmt]
    exponent = (bits >> fraction) & (2 * bias + 1)
    mantissa = bits & ((1 << fraction) - 1)
    sign = bits >> (fraction + (2 * bias + 1).bit_length())
    if exponent == 2 * bias + 1:
        return 'nan' if mantissa else 'inf', None, sign
    value = Fraction(mantissa + ((1 << fraction) if exponent else 0))
    value *= Fraction(2) ** ((exponent or 1) - bias - fraction)
    return 'finite', -value if sign else value, sign


def quantize(value, sign, fmt, mode):
    fraction, bias = FORMATS[fmt]
    sign_bits = sign << (fraction + (2 * bias + 1).bit_length())
    if value == 0:
        return sign_bits
    magnitude = abs(value)
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    if magnitude < Fraction(2) ** exponent:
        exponent -= 1
    step = Fraction(2) ** max(exponent - fraction, 1 - bias - fraction)
    result = (round, math.floor, math.ceil, math.trunc)[mode](value / step) * step
    if result == 0:
        return sign_bits
    magnitude = abs(result)
    maximum = (2 - Fraction(1, 1 << fraction)) * Fraction(2) ** bias
    if magnitude > maximum:
        infinity = mode == 0 or (mode == 1 and sign) or (mode == 2 and not sign)
        return sign_bits | (((2 * bias + 1) << fraction) if infinity else
                            (((2 * bias) << fraction) | ((1 << fraction) - 1)))
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    if magnitude < Fraction(2) ** exponent:
        exponent -= 1
    if exponent < 1 - bias:
        return sign_bits | int(magnitude / (Fraction(2) ** (1 - bias - fraction)))
    significand = int(magnitude / (Fraction(2) ** (exponent - fraction)))
    return sign_bits | ((exponent + bias) << fraction) | (significand & ((1 << fraction) - 1))


def reference(bits, source, destination, raw_mode, ftz, sat, mods):
    fraction, bias = FORMATS[source]
    sign_mask = 1 << (fraction + (2 * bias + 1).bit_length())
    if mods & 1:
        bits &= ~sign_mask
    if mods & 2:
        bits ^= sign_mask
    kind, value, sign = unpack(bits, source)
    df, db = FORMATS[destination]
    out_sign = sign << (df + (2 * db + 1).bit_length())
    if sat and (kind == 'nan' or sign):
        return 0
    if kind == 'nan':
        return out_sign | ((2 * db + 1) << df) | (1 << (df - 1))
    if kind == 'inf':
        return quantize(Fraction(1), 0, destination, 0) if sat else out_sign | ((2 * db + 1) << df)
    if ftz and source == 2 and (bits & 0x7f800000) == 0:
        value = Fraction(0)
    if source == destination and raw_mode >= 4:
        value = Fraction((round, math.floor, math.ceil, math.trunc)[raw_mode - 4](value))
    if sat:
        value = min(Fraction(1), max(Fraction(0), value))
    result = quantize(value, sign, destination, raw_mode if source != destination else 0)
    if ftz and destination == 2 and (result & 0x7f800000) == 0:
        result &= 0x80000000
    return result


def pools(seed):
    rng = random.Random(seed)
    result = {}
    for fmt, (fraction, bias) in FORMATS.items():
        sign = 1 << (fraction + (2 * bias + 1).bit_length())
        inf = (2 * bias + 1) << fraction
        values = [0, 1, (1 << fraction) - 1, 1 << fraction, inf - 1, inf,
                  inf | 1, inf | (1 << (fraction - 1)), bias << fraction]
        values += [sign | v for v in values]
        packed, unpacked = {1: ('<e', '<H'), 2: ('<f', '<I'), 3: ('<d', '<Q')}[fmt]
        for number in (-2.5, -1.5, -0.5, 0.5, 1.5, 2.5, 65504., 65520., 1e-5):
            if fmt == 1 and number > 65504:
                continue
            values.append(struct.unpack(unpacked, struct.pack(packed, number))[0])
        # Values immediately around rounding ties and destination boundaries.
        for destination in (1, 2):
            if destination >= fmt:
                continue
            df, db = FORMATS[destination]
            for number in (1 + 2. ** (-df - 1), 2. ** (1 - db), 2. ** (1 - db - df),
                           2. ** (-db - df), (2 - 2. ** (-df)) * 2. ** db):
                center = struct.unpack(unpacked, struct.pack(packed, number))[0]
                for delta in (-1, 0, 1):
                    values += [center + delta, sign | (center + delta)]
        values += [rng.getrandbits(sign.bit_length()) for _ in range(64)]
        result[fmt] = values
    return result


def cases(seed):
    values = pools(seed)
    for source, destination in itertools.product(FORMATS, repeat=2):
        modes = (1, 4, 5, 6, 7) if source == destination else range(4)
        configs = [(mode, ftz, 0, 0) for mode, ftz in itertools.product(modes, (0, 1))]
        configs += [(mode, 0, sat, mods) for mode, sat, mods in itertools.product(
            (1,) if source == destination else (0, 3), (0, 1), range(1, 4))]
        for mode, ftz, sat, mods in configs:
            for sh in ((0, 1) if source == 1 else (0,)):
                inst = (0x5ca8000000000000 | 3 | (destination << 8) | (source << 10) |
                        (2 << 16) | ((mode & 1) << 19) | ((mode & 3) << 39) |
                        (sh << 41) | ((mode >> 2) << 42) | (ftz << 44) |
                        (((mods >> 1) & 1) << 45) | ((mods & 1) << 49) | (sat << 50))
                for index, bits in enumerate(values[source]):
                    word = bits if source != 1 else bits | (values[1][-(index + 1)] << 16)
                    effective = (word >> (16 * sh)) & 0xffff if source == 1 else bits
                    expected = reference(effective, source, destination, mode, ftz, sat, mods)
                    for guard in (0, 1):
                        preds = (index & 0x7b) | (guard << 2)
                        regs = [word & 0xffffffff, word >> 32, 2, 0x12345678, 4, 5, 6, 7]
                        out = regs.copy()
                        if guard != (mode & 1):
                            out[3] = expected & 0xffffffff
                            if destination == 3:
                                out[4] = expected >> 32
                        yield f'f{16 << (source-1)}_f{16 << (destination-1)}', inst, preds, index & 15, regs, out, destination, sat


def constants(seed):
    words, offsets = [], {}
    for source, values in pools(seed).items():
        for bits in values[:18]:
            offsets[source, bits] = len(words)
            words += [bits & 0xffffffff, bits >> 32]
    return words, offsets


def extra_cases(seed):
    _, offsets = constants(seed)
    values = pools(seed)
    for source, destination in itertools.product(FORMATS, repeat=2):
        modes = (1, 4, 5, 6, 7) if source == destination else range(4)
        for form in ('cbuf', 'immediate', 'overlap_lo', 'overlap_hi'):
            for mode in modes:
                for index, original in enumerate(values[source][:18]):
                    bits = original
                    inst = (3 | (destination << 8) | (source << 10) | (2 << 16) |
                            ((mode & 3) << 39) | ((mode >> 2) << 42))
                    destination_register = 3
                    if form == 'cbuf':
                        inst |= 0x4ca8000000000000 | (offsets[source, bits] << 20) | (2 << 34)
                    elif form == 'immediate':
                        field = bits if source == 1 else bits >> (12 if source == 2 else 44)
                        bits = field if source == 1 else field << (12 if source == 2 else 44)
                        inst |= 0x38a8000000000000 | ((field & 0x7ffff) << 20) | ((field >> 19) << 56)
                    else:
                        inst |= 0x5ca8000000000000
                        destination_register = int(form == 'overlap_hi')
                        inst = (inst & ~255) | destination_register
                    expected = reference(bits, source, destination, mode, False, False, 0)
                    for guard in (0, 1):
                        regs = [original & 0xffffffff, original >> 32, 2, 0x12345678, 4, 5, 6, 7]
                        out = regs.copy()
                        if guard:
                            out[destination_register] = expected & 0xffffffff
                            if destination == 3:
                                out[destination_register + 1] = expected >> 32
                        yield f'{form}_f{16 << (source-1)}_f{16 << (destination-1)}', inst, guard << 2, index & 15, regs, out, destination, False, destination_register


def all_cases(seed):
    for case in cases(seed):
        yield (*case, 3)
    yield from extra_cases(seed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--seed', type=int, default=19)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    stats = collections.defaultdict(lambda: [0, 0])
    examples = collections.defaultdict(list)
    with tempfile.TemporaryDirectory() as tmp:
        inputs, outputs = Path(tmp) / 'cases', Path(tmp) / 'results'
        with inputs.open('w') as stream:
            stream.write('CBUF ' + ' '.join(f'{v:x}' for v in constants(args.seed)[0]) + '\n')
            for _, inst, preds, cc, regs, *_ in all_cases(args.seed):
                stream.write(' '.join(f'{v:x}' for v in [inst, preds, cc, *regs]) + '\n')
        subprocess.run([str(args.binary), str(inputs), str(outputs)],
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'), check=True)
        with outputs.open() as stream:
            for group, inst, preds, cc, regs, out, destination, sat, dst in all_cases(args.seed):
                row = [int(v, 16) for v in stream.readline().split()]
                expected = [*out, preds, cc]
                # Payloads of NaNs and the sign of saturated zero are not assumed.
                if len(row) == 10 and bool(preds & 4) != bool(inst & (1 << 19)):
                    want = out[dst] | ((out[dst + 1] << 32) if destination == 3 else 0)
                    got = row[dst] | ((row[dst + 1] << 32) if destination == 3 else 0)
                    kind, _, _ = unpack(want, destination)
                    if kind == 'nan' and unpack(got, destination)[0] == 'nan':
                        row[dst] = out[dst]
                        if destination == 3: row[dst + 1] = out[dst + 1]
                    elif sat and unpack(want, destination)[1] == 0 and unpack(got, destination)[1] == 0:
                        row[dst] = out[dst]
                        if destination == 3: row[dst + 1] = out[dst + 1]
                good = row == expected
                stats[group][0] += good
                stats[group][1] += 1
                if not good and len(examples[group]) < 3:
                    examples[group].append(dict(inst=f'{inst:016x}', inputs=regs, expected=expected, got=row))
            if stream.readline(): raise RuntimeError('Extra Metal results')
    for group, (passed, total) in stats.items():
        print(f'{group}: {passed}/{total}')
        for example in examples[group]: print(example)
    if args.report:
        args.report.write_text(json.dumps(dict(seed=args.seed, groups=stats, examples=examples), indent=2))
    return int(any(p != n for p, n in stats.values()))


if __name__ == '__main__':
    raise SystemExit(main())
