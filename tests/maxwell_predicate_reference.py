#!/usr/bin/env python3
"""Python NVIDIA setp model vs Maxwell decoder -> MSL -> Apple Metal.

Reference: NVIDIA PTX ISA, Comparison and Selection Instructions: setp
https://docs.nvidia.com/cuda/parallel-thread-execution/#comparison-and-selection-instructions-setp
PTX specifies the comparison, FTZ and two Boolean results; it does not specify
Maxwell SASS bit encodings. Encodings follow the offline Maxwell field layout,
as in maxwell_alu_reference.py. No physical NVIDIA GPU is used.
Register operands only; no .X, cbuf, immediate, half/double or destination alias.
"""
import argparse
import collections
import itertools
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile

from maxwell_alu_reference import INT_SPECIAL, SPECIAL_F, absf, bits_f, flush, is_nan, neg


def compare(op, a, b, floating):
    if floating:
        unordered = is_nan(a) or is_nan(b)
        if op == 7:
            return not unordered
        if op == 8:
            return unordered
        if op == 15:
            return True
        if op == 0:
            return False
        if unordered:
            return op >= 9
        a, b = bits_f(a), bits_f(b)
        if op >= 9:
            op -= 8
    return (False, a < b, a == b, a <= b, a > b, a != b, a >= b, True)[op]


def boolean(op, a, b):
    return (a and b, a or b, a != b)[op]


def cases(seed):
    rng = random.Random(seed)
    for floating in (False, True):
        pool = SPECIAL_F + [0xffc00001, 0x7f800001] if floating else INT_SPECIAL
        anchors = [0, 0x80000000, 0x3f800000, 0x7fc00000] if floating else [0, 0x80000000]
        pairs = [(a, b) for a in pool for b in anchors]
        pairs += [(b, a) for a in pool for b in anchors]
        pairs += [(a, a) for a in pool]
        pairs += [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(64)]
        configs = [(op, bop, mode, inv, 0, 0)
                   for op, bop, mode, inv in itertools.product(
                       range(16 if floating else 8), range(3), range(2), range(2))]
        if floating:
            # abs/neg combinations and inverted instruction predicate.
            configs += [(op, 0, 1, 0, mods, guard_inv)
                        for op, mods, guard_inv in itertools.product(
                            (1, 2, 5, 13), range(1, 16), (0, 1))]
        else:
            configs += [(op, 2, mode, 0, 0, 1)
                        for op, mode in itertools.product(range(8), range(2))]
        for op, bop, mode, inv, mods, guard_inv in configs:
            inst = (0x5bb0000000000000 if floating else 0x5b60000000000000)
            # P0|P1 <- R0 cmp R1, combined with P2; guarded by P3.
            inst |= 1 | (3 << 16) | (guard_inv << 19) | (1 << 20)
            inst |= (2 << 39) | (inv << 42) | (bop << 45)
            if floating:
                inst |= (op << 48) | (mode << 47)
                inst |= ((mods & 1) << 7) | (((mods >> 1) & 1) << 43)
                inst |= (((mods >> 2) & 1) << 44) | (((mods >> 3) & 1) << 6)
            else:
                inst |= (mode << 48) | (op << 49)
            for index, (a, b) in enumerate(pairs):
                # Both source/guard states; rotate old destination values.
                for flags in range(4):
                    low = (flags << 2) | (index & 3)
                    preds = low | ((index & 7) << 4)
                    cc = index & 15
                    x, y = a, b
                    if floating:
                        if mode:
                            x, y = flush(x), flush(y)
                        if mods & 1: x = absf(x)
                        if mods & 2: x = neg(x)
                        if mods & 4: y = absf(y)
                        if mods & 8: y = neg(y)
                    elif mode:
                        x = x - (1 << 32) if x >> 31 else x
                        y = y - (1 << 32) if y >> 31 else y
                    expected = preds
                    if bool(preds & 8) != bool(guard_inv):
                        t = compare(op, x, y, floating)
                        c = bool(preds & 4) != bool(inv)
                        expected = ((preds & ~3) | int(boolean(bop, t, c)) |
                                    (int(boolean(bop, not t, c)) << 1))
                    group = ('fsetp_ftz' if mode else 'fsetp') if floating else (
                        'isetp_signed' if mode else 'isetp_unsigned')
                    regs = [a, b, 0xdeadbeef, 0x12345678, 0, 0xffffffff, 42, 7]
                    yield group, inst, preds, cc, regs, expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--seed', type=int, default=19)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    stats = collections.defaultdict(lambda: [0, 0])
    examples = []
    with tempfile.TemporaryDirectory() as tmp:
        input_path, output_path = Path(tmp) / 'cases', Path(tmp) / 'results'
        with input_path.open('w') as stream:
            for _, inst, preds, cc, regs, _ in cases(args.seed):
                stream.write(' '.join(f'{v:x}' for v in [inst, preds, cc, *regs]) + '\n')
        env = dict(os.environ, MTL_DEBUG_LAYER='1')
        subprocess.run([str(args.binary), str(input_path), str(output_path)],
                       env=env, check=True)
        with output_path.open() as stream:
            for case in cases(args.seed):
                group, inst, preds, cc, regs, expected = case
                row = [int(v, 16) for v in stream.readline().split()]
                good = row == [*regs, expected, cc]
                stats[group][0] += good
                stats[group][1] += 1
                if not good and len(examples) < 12:
                    examples.append(dict(group=group, inst=f'{inst:016x}',
                                         inputs=[preds, cc, *regs],
                                         expected=[*regs, expected, cc], got=row))
            if stream.readline():
                raise RuntimeError('Extra Metal result rows')
    for group, (passed, total) in stats.items():
        print(f'{group}: {passed}/{total}')
    for example in examples:
        print(example)
    if args.report:
        args.report.write_text(json.dumps(dict(seed=args.seed, groups=stats,
                                              examples=examples), indent=2))
    return int(any(p != t for p, t in stats.values()))


if __name__ == '__main__':
    raise SystemExit(main())
