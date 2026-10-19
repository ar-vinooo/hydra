#!/usr/bin/env python3
"""TLDS texel fetches: Python model vs a decoded guest TLDS on Metal.

TLDS reads a texel at integer coordinates (and an explicit mip level for the .LL
forms) without a sampler. Targets covered: 1D.LZ, 1D.LL, 2D.LZ, 2D.LL (mip
chains), 3D.LZ and 2D array LZ, on RGBA32Float textures filled with random
floats. The model is a plain array lookup; coordinates and levels stay inside
the texture (out-of-range fetches are not asserted: the header and the sources
available here do not define them).

Operand layout (from the decoder / nvdisasm): 1D.LZ x=R4; 1D.LL x=R4 lod=R6;
2D.LZ x=R4 y=R6; 2D.LL x=R4 y=R5 lod=R6; 3D.LZ x=R4 y=R5 z=R6; array LZ
layer=R4 x=R6 y=R7.

Usage: texel_fetch_reference.py <hydra-maxwell-alu-reference-tests> [--seed N]
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def f32_bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def bits_f32(b):
    return struct.unpack('<f', struct.pack('<I', b))[0]


def make_case(rng, kind, target):
    if kind == '1d':
        w, h, d, levels = rng.choice([5, 8, 16, 3]), 1, 1, 1
    elif kind == '2d':
        w, h, d = rng.choice([8, 7, 16, 5]), rng.choice([8, 5, 4, 3]), 1
        levels = min(rng.randrange(1, 5), max(w, h).bit_length()) if target == 5 else 1
    elif kind == '2darray':
        w, h, d, levels = rng.choice([4, 6]), rng.choice([3, 5]), rng.choice([2, 3, 4]), 1
    else:
        w, h, d, levels = rng.choice([4, 3]), rng.choice([4, 3]), rng.choice([2, 4, 3]), 1
    slices = d if kind in ('2darray', '3d') else 1
    data = {}
    for slice_ in range(slices):
        for level in range(levels):
            lw, lh = max(w >> level, 1), max(h >> level, 1)
            data[(slice_, level)] = [[rng.random() for _ in range(4)] for _ in range(lw * lh)]
    samples = []
    for _ in range(12):
        level = rng.randrange(levels)
        lw, lh = max(w >> level, 1), max(h >> level, 1)
        x, y = rng.randrange(lw), rng.randrange(lh)
        z = rng.randrange(d)
        if kind == '1d':
            regs = (x, 0, level, 0)
        elif kind == '2d' and target == 5:
            regs = (x, y, level, 0)
        elif kind == '2d':
            regs = (x, 0, y, 0)
        elif kind == '3d':
            regs = (x, y, z, 0)
        else:
            regs = (z, 0, x, y)
        samples.append((regs, data[(z if kind in ('3d', '2darray') else 0, level)][y * lw + x]))
    return kind, w, h, d, levels, target, data, samples


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    parser.add_argument('--seed', type=int, default=20261020)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    targets = [('1d', 0), ('1d', 1), ('2d', 2), ('2d', 5), ('3d', 7), ('2darray', 8)]
    names = {('1d', 0): '1D.LZ', ('1d', 1): '1D.LL', ('2d', 2): '2D.LZ', ('2d', 5): '2D.LL',
             ('3d', 7): '3D.LZ', ('2darray', 8): '2D array LZ'}
    cases = [make_case(rng, kind, target) for kind, target in targets for _ in range(40)]
    lines = []
    for kind, w, h, d, levels, target, data, samples in cases:
        lines.append('TYPE %s %d %d %d %d %d' % (kind, w, h, d, levels, target))
        for (slice_, level), texels in data.items():
            lines.append('DATA %d %d %s' % (slice_, level, ' '.join('%x' % f32_bits(c) for t in texels for c in t)))
        for regs, _ in samples:
            lines.append('CASE %x %x %x %x' % regs)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'in.txt').write_text('\n'.join(lines) + '\n')
        subprocess.run([args.runner, '--texel-fetch', str(tmp / 'in.txt'), str(tmp / 'out.txt')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        results = (tmp / 'out.txt').read_text().strip().split('\n')
    stats = {}
    cursor = 0
    for kind, w, h, d, levels, target, data, samples in cases:
        entry = stats.setdefault(names[(kind, target)], [0, 0, None])
        for regs, want in samples:
            got = [bits_f32(int(x, 16)) for x in results[cursor].split()]
            cursor += 1
            entry[0] += 1
            if any(abs(g - e) > 1e-6 for g, e in zip(got, want)):
                entry[1] += 1
                entry[2] = entry[2] or {'size': (w, h, d, levels), 'regs': regs, 'want': want, 'got': got}
    failed = False
    for name, (count, bad, example) in stats.items():
        print('%-12s %4d fetches %3d mismatches %s' % (name, count, bad, example if bad else ''))
        failed |= bad != 0
    if failed:
        print('FAIL')
        sys.exit(1)
    print('PASS')


if __name__ == '__main__':
    main()
