#!/usr/bin/env python3
"""1D, 2D array, 3D and cube textures through a guest TEXS: Python model vs Hydra.

Same path as tests/sampler_reference.py (random TSC words, decodeSampler, the
Metal sampler, a decoded guest TEXS) but for the texture types a TIC can
describe besides plain 2D. The models are the textbook ones: point and linear
taps at texel centres with the address mode of each axis (TSC U, V and P), array
layers selected by an integer register, and cube faces chosen by the major axis
with the usual sc/tc/ma table (+X, -X, +Y, -Y, +Z, -Z). Cube cases use point
sampling in the interior of a face: seam filtering (TSC CUBEMAP_INTERFACE_FILTERING)
is not asserted.

The header has no prose; these are conventions, not NVIDIA text.

Usage: texture_type_reference.py <hydra-maxwell-alu-reference-tests> [--seed N]
"""
import argparse
import math
import os
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sampler_reference import (EDGE, LINEAR, MIP_NONE, MIRROR, POINT, WRAP, bits_f32, f32,  # noqa: E402
                               f32_bits, pack_tsc, resolve)


def tsc_for(modes, mag_min):
    return pack_tsc(ADDRESS_U=modes[0], ADDRESS_V=modes[1], ADDRESS_P=modes[2], MAG_FILTER=mag_min,
                    MIN_FILTER=mag_min, MIP_FILTER=MIP_NONE, MAX_LOD_CLAMP=4095)


def lerp_taps(position, size, mode):
    """(index, weight) taps along one axis for linear filtering."""
    f = position - 0.5
    i0 = math.floor(f)
    a = f - i0
    return [(resolve(i0, size, mode, True), 1 - a), (resolve(i0 + 1, size, mode, True), a)]


def sample_nd(texel, sizes, coords, modes, linear):
    """N-dimensional point or multilinear sample; texel(*indices) -> RGBA."""
    axes = []
    for coord, size, mode in zip(coords, sizes, modes):
        position = coord * size
        if linear:
            axes.append(lerp_taps(position, size, mode))
        else:
            axes.append([(resolve(math.floor(position), size, mode, False), 1.0)])
    out = [0.0] * 4
    stack = [((), 1.0)]
    for axis in axes:
        stack = [(idx + (i,), w * wi) for idx, w in stack for i, wi in axis]
    for idx, weight in stack:
        color = texel(*idx)
        for c in range(4):
            out[c] += weight * color[c]
    return out


# (sc, tc, ma) for each face as functions of (x, y, z)
CUBE_FACES = [
    (lambda x, y, z: -z, lambda x, y, z: -y, lambda x, y, z: x),
    (lambda x, y, z: z, lambda x, y, z: -y, lambda x, y, z: -x),
    (lambda x, y, z: x, lambda x, y, z: z, lambda x, y, z: y),
    (lambda x, y, z: x, lambda x, y, z: -z, lambda x, y, z: -y),
    (lambda x, y, z: x, lambda x, y, z: -y, lambda x, y, z: z),
    (lambda x, y, z: -x, lambda x, y, z: -y, lambda x, y, z: -z),
]


def cube_face(x, y, z):
    ax, ay, az = abs(x), abs(y), abs(z)
    if ax >= ay and ax >= az:
        face = 0 if x > 0 else 1
    elif ay >= az:
        face = 2 if y > 0 else 3
    else:
        face = 4 if z > 0 else 5
    sc, tc, ma = (f(x, y, z) for f in CUBE_FACES[face])
    return face, (sc / abs(ma) + 1) / 2, (tc / abs(ma) + 1) / 2


def random_level(rng, count):
    return [[rng.random() for _ in range(4)] for _ in range(count)]


def make_case(rng, kind):
    mag_min = rng.choice([POINT, LINEAR])
    modes = [rng.choice([WRAP, MIRROR, EDGE]) for _ in range(3)]
    if kind == 'cube':
        mag_min, modes = POINT, [EDGE] * 3
    if kind == '1d':
        w, h, d = rng.choice([5, 8, 16, 3]), 1, 1
    elif kind == '2darray':
        w, h, d = rng.choice([4, 5, 8]), rng.choice([3, 4, 8]), rng.choice([2, 3, 4])
    elif kind == '3d':
        w, h, d = rng.choice([4, 3, 5]), rng.choice([4, 3]), rng.choice([2, 4, 3])
    else:
        w, h, d = 4, 4, 6
    slices = {'1d': 1, '2darray': d, '3d': d, 'cube': 6}[kind]
    data = [random_level(rng, w * (1 if kind == '1d' else h)) for _ in range(slices)]
    samples = []
    while len(samples) < 6:
        if kind == 'cube':
            while True:
                x, y, z = (rng.uniform(-1, 1) for _ in range(3))
                m = sorted([abs(x), abs(y), abs(z)])
                if m[2] > 1.25 * m[1]:
                    break
            face, s, t = cube_face(x, y, z)
            if any(abs(c * w - round(c * w)) < 1e-2 for c in (s, t)) or not (0.03 < s < 0.97 and 0.03 < t < 0.97):
                continue
            samples.append(((f32(x), f32(y), f32(z), 0.0), (x, y, z)))
            continue
        u, v, wz = (f32(rng.uniform(-0.6, 1.6)) for _ in range(3))
        layer = rng.randrange(d) if kind == '2darray' else 0
        sizes = (w, h, d)
        coords = (u, v, wz)
        if any(abs(c * s - round(c * s)) < 1e-3 for c, s in zip(coords[:{'1d': 1, '2darray': 2, '3d': 3}[kind]], sizes)):
            continue
        samples.append(((u, v, wz, layer), None))
    return kind, w, h, d, tsc_for(modes, mag_min), modes, mag_min == LINEAR, data, samples


def model(case, sample):
    kind, w, h, d, tsc, modes, linear, data, _ = case
    regs, direction = sample
    if kind == '1d':
        return sample_nd(lambda x: data[0][x], (w,), (regs[0],), modes[:1], linear)
    if kind == '2darray':
        layer = regs[3]
        return sample_nd(lambda x, y: data[layer][y * w + x], (w, h), (regs[0], regs[1]), modes[:2], linear)
    if kind == '3d':
        return sample_nd(lambda x, y, z: data[z][y * w + x], (w, h, d), regs[:3], modes, linear)
    face, s, t = cube_face(*direction)
    return sample_nd(lambda x, y: data[face][y * w + x], (w, h), (s, t), modes[:2], False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    parser.add_argument('--seed', type=int, default=20261020)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    cases = [make_case(rng, kind) for kind in ('1d', '2darray', '3d', 'cube') for _ in range(100)]
    lines = []
    for kind, w, h, d, tsc, modes, linear, data, samples in cases:
        lines.append('TYPE %s %d %d %d %s' % (kind, w, h, d, ' '.join('%x' % x for x in tsc)))
        for index, texels in enumerate(data):
            lines.append('DATA %d %s' % (index, ' '.join('%x' % f32_bits(c) for t in texels for c in t)))
        for regs, _ in samples:
            r3 = regs[3] if kind == '2darray' else f32_bits(regs[3]) if kind == 'cube' else 0
            # 2D array: R4 holds the layer, R5 and R6 the coordinates
            if kind == '2darray':
                words = (regs[3], f32_bits(regs[0]), f32_bits(regs[1]), 0)
            else:
                words = (f32_bits(regs[0]), f32_bits(regs[1]), f32_bits(regs[2]), r3)
            lines.append('CASE %x %x %x %x' % words)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'in.txt').write_text('\n'.join(lines) + '\n')
        subprocess.run([args.runner, '--texture-types', str(tmp / 'in.txt'), str(tmp / 'out.txt')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        results = (tmp / 'out.txt').read_text().strip().split('\n')
    stats = {}
    cursor = 0
    for case in cases:
        kind, linear = case[0], case[6]
        entry = stats.setdefault(kind, [0, 0, None])
        for sample in case[8]:
            got = [bits_f32(int(x, 16)) for x in results[cursor].split()]
            cursor += 1
            expected = model(case, sample)
            tolerance = 0.02 if linear else 2e-5
            entry[0] += 1
            if any(abs(g - e) > tolerance for g, e in zip(got, expected)):
                entry[1] += 1
                entry[2] = entry[2] or {'case': (kind, case[1:4], case[5], linear), 'regs': sample[0],
                                        'expected': expected, 'got': got}
    failed = False
    for kind, (count, bad, example) in stats.items():
        print('%-8s %4d samples %4d mismatches %s' % (kind, count, bad, example if bad else ''))
        failed |= bad != 0
    if failed:
        print('FAIL')
        sys.exit(1)
    print('PASS')


if __name__ == '__main__':
    main()
