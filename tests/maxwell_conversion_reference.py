#!/usr/bin/env python3
"""NVIDIA cvt scalar model in Python vs Maxwell -> MSL -> Metal.

Semantic reference: NVIDIA PTX ISA, Data Movement and Conversion: cvt
https://docs.nvidia.com/cuda/parallel-thread-execution/#data-movement-and-conversion-instructions-cvt
PTX is not the Maxwell SASS encoding specification. Encodings follow the
offline Maxwell layout used by the other reference runners. No NVIDIA GPU.
Scope: F32 -> S/U16/32/64 and S/U8/16/32/64 -> F16/F32/F64;
Register/cbuf/immediate operands and overlapping registers; no CC output or
I2F abs/neg modifiers.
"""
import argparse
import collections
import itertools
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile

from maxwell_alu_reference import INT_SPECIAL, SPECIAL_F, absf, classify, f_bits, flush, neg


def float_to_int(word, width, signed, rounding, ftz, mods):
    if ftz:
        word = flush(word)
    if mods & 1:
        word = absf(word)
    if mods & 2:
        word = neg(word)
    kind, value, sign = classify(word)
    low = -(1 << (width - 1)) if signed else 0
    high = (1 << (width - int(signed))) - 1
    if kind == 'nan':
        return 1 << 63 if width == 64 else 0
    if kind == 'inf':
        result = low if sign else high
    else:
        result = (round, math.floor, math.ceil, math.trunc)[rounding](value)
        result = max(low, min(high, result))
    return result & ((1 << max(width, 32)) - 1)


def int_to_float(word, width, signed, byte, rounding, destination):
    value = (word >> (byte * 8)) & ((1 << width) - 1)
    if signed and value & (1 << (width - 1)):
        value -= 1 << width
    magnitude = abs(value)
    precision = {16: 11, 32: 24, 64: 53}[destination]
    shift = max(0, magnitude.bit_length() - precision)
    step = 1 << shift
    q, remainder = divmod(magnitude, step)
    if rounding == 0:
        increment = remainder * 2 > step or (remainder * 2 == step and q & 1)
    elif rounding == 1:
        increment = value < 0 and remainder != 0
    elif rounding == 2:
        increment = value > 0 and remainder != 0
    else:
        increment = False
    rounded = (q + int(increment)) * step
    if destination == 16 and rounded > 65504:
        to_inf = rounding == 0 or (rounding == 1 and value < 0) or (rounding == 2 and value > 0)
        return (0x8000 if value < 0 else 0) | (0x7c00 if to_inf else 0x7bff)
    formats = {16: ('<e', '<H'), 32: ('<f', '<I'), 64: ('<d', '<Q')}
    packed, unpacked = formats[destination]
    return struct.unpack(unpacked, struct.pack(packed, -rounded if value < 0 else rounded))[0]


def cases(seed):
    rng = random.Random(seed)
    floats = SPECIAL_F + [0xffc00001, 0x7f800001]
    floats += [f_bits(v) for v in (-65536.5, -32768.5, -32767.5, -2.5, -1.5,
                                  -0.5, 0.5, 1.5, 2.5, 32766.5, 32767.5,
                                  65534.5, 65535.5, 2147483520., 2147483648.,
                                  -2147483904., 4294967040., 4294967296.)]
    floats += [rng.getrandbits(32) for _ in range(128)]
    integers = INT_SPECIAL + [rng.getrandbits(32) for _ in range(128)]
    wide_integers = [0, 1, 0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff]
    wide_integers += [rng.getrandbits(64) for _ in range(128)]
    for exponent in range(24, 33):
        for offset in (-129, -65, -3, -2, -1, 0, 1, 2, 3, 65, 129):
            integers.append(((1 << exponent) + offset) & 0xffffffff)
            integers.append((-(1 << exponent) + offset) & 0xffffffff)
    for exponent in (11, 16, 24, 32, 53, 54, 63, 64):
        for offset in (-129, -3, -2, -1, 0, 1, 2, 3, 129):
            wide_integers.append(((1 << exponent) + offset) & 0xffffffffffffffff)
            wide_integers.append((-(1 << exponent) + offset) & 0xffffffffffffffff)
    for width, signed, rounding, ftz, mods in itertools.product(
            (16, 32, 64), (0, 1), range(4), (0, 1), range(4)):
        inv = rounding & 1
        inst = (0x5cb0000000000000 | 3 | ((width.bit_length() - 4) << 8) |
                (2 << 10) | (signed << 12) | (2 << 16) | (inv << 19) |
                (rounding << 39) | (ftz << 44) |
                ((mods & 1) << 49) | (((mods >> 1) & 1) << 45))
        for index, word in enumerate(floats):
            for guard in (0, 1):
                preds, cc = (index & 0x7b) | (guard << 2), index & 15
                expected = float_to_int(word, width, signed, rounding, ftz, mods)
                yield 'f2i' + ('_ftz' if ftz else ''), inst, word, preds, cc, expected, guard != inv, 1, width == 64
    for width, signed, rounding, destination in itertools.product(
            (8, 16, 32, 64), (0, 1), range(4), (16, 32, 64)):
        for byte in (range(4) if width == 8 else (0, 2) if width == 16 else (0,)):
            inv = rounding & 1
            inst = (0x5cb8000000000000 | 3 | ((destination.bit_length() - 4) << 8) |
                    ((width.bit_length() - 4) << 10) | (signed << 13) |
                    (2 << 16) | (inv << 19) | (rounding << 39) | (byte << 41))
            for index, word in enumerate(wide_integers if width == 64 else integers):
                for guard in (0, 1):
                    preds, cc = (index & 0x7b) | (guard << 2), index & 15
                    expected = int_to_float(word, width, signed, byte, rounding, destination)
                    yield 'i2f_' + ('rn', 'rm', 'rp', 'rz')[rounding], inst, word & 0xffffffff, preds, cc, expected, guard != inv, word >> 32 if width == 64 else 1, destination == 64


SOURCE_VALUES = [0, 1, 0x80000001, 0x3fc00000, 0xbfc00000, 0x7fc00000,
                 0x7fffffff, 0xffffffff, 0x8000000000000000,
                 0xffffffffffffffff, 0x20000000000001]


def extra_cases():
    for family, width, signed, rounding, destination, form in itertools.product(
            ('f2i', 'i2f'), (8, 16, 32, 64), (0, 1), range(4), (16, 32, 64),
            ('cbuf', 'immediate', 'overlap_lo', 'overlap_hi')):
        if family == 'f2i' and (width != 32):
            continue
        for index, original in enumerate(SOURCE_VALUES):
            effective = original
            opcode = {'f2i': 0x5cb0000000000000, 'i2f': 0x5cb8000000000000}[family]
            inst = 3 | (2 << 16) | ((rounding & 1) << 19) | (rounding << 39)
            if family == 'f2i':
                inst |= ((destination.bit_length() - 4) << 8) | (2 << 10) | (signed << 12)
            else:
                inst |= ((destination.bit_length() - 4) << 8) | ((width.bit_length() - 4) << 10) | (signed << 13)
            dst = 3
            if form == 'cbuf':
                inst |= (opcode ^ 0x1000000000000000) | ((index * 2) << 20) | (2 << 34)
            elif form == 'immediate':
                opcode = 0x38b0000000000000 if family == 'f2i' else 0x38b8000000000000
                field = (original >> 12) & 0xfffff if family == 'f2i' else original & 0xfffff
                effective = field << 12 if family == 'f2i' else (
                    (field - (1 << 20) if field & 0x80000 else field) & 0xffffffffffffffff)
                inst |= opcode | ((field & 0x7ffff) << 20) | ((field >> 19) << 56)
            else:
                dst = int(form == 'overlap_hi')
                inst = (inst & ~255) | dst | opcode
            expected = (float_to_int(effective & 0xffffffff, destination, signed, rounding, False, 0)
                        if family == 'f2i' else int_to_float(effective, width, signed, 0, rounding, destination))
            pair = destination == 64
            for guard in (0, 1):
                yield family + '_' + form, inst, original & 0xffffffff, guard << 2, index & 15, expected, guard != (rounding & 1), original >> 32, pair, dst


def all_cases(seed):
    for case in cases(seed):
        yield (*case, 3)
    yield from extra_cases()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--seed', type=int, default=19)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    stats = collections.defaultdict(lambda: [0, 0])
    examples = collections.defaultdict(list)
    with tempfile.TemporaryDirectory() as tmp:
        input_path, output_path = Path(tmp) / 'cases', Path(tmp) / 'results'
        with input_path.open('w') as stream:
            words = [word for value in SOURCE_VALUES for word in (value & 0xffffffff, value >> 32)]
            stream.write('CBUF ' + ' '.join(f'{v:x}' for v in words) + '\n')
            for _, inst, word, preds, cc, _, _, hi, _, _ in all_cases(args.seed):
                stream.write(' '.join(f'{v:x}' for v in
                                     [inst, preds, cc, word, hi, 2, 0x12345678, 4, 5, 6, 7]) + '\n')
        subprocess.run([str(args.binary), str(input_path), str(output_path)],
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'), check=True)
        with output_path.open() as stream:
            for group, inst, word, preds, cc, expected, active, hi, pair, dst in all_cases(args.seed):
                row = [int(v, 16) for v in stream.readline().split()]
                result = [word, hi, 2, 0x12345678, 4, 5, 6, 7, preds, cc]
                if active:
                    result[dst] = expected & 0xffffffff
                    if pair: result[dst + 1] = expected >> 32
                good = row == result
                stats[group][0] += good
                stats[group][1] += 1
                if not good and len(examples[group]) < 4:
                    examples[group].append(dict(inst=f'{inst:016x}', input=f'{word:08x}',
                                                expected=result, got=row))
            if stream.readline():
                raise RuntimeError('Extra Metal results')
    for group, (passed, total) in stats.items():
        print(f'{group}: {passed}/{total}')
        for example in examples[group]:
            print(example)
    if args.report:
        args.report.write_text(json.dumps(dict(seed=args.seed, groups=stats,
                                              examples=examples), indent=2))
    return int(any(p != t for p, t in stats.values()))


if __name__ == '__main__':
    raise SystemExit(main())
