#!/usr/bin/env python3
"""TLD4S (texture gather): Python model vs a decoded guest TLD4S on Metal.

A gather returns one component of the four texels of the 2x2 footprint around
the sample position. Footprint: the texel centres surrounding (u * w - 0.5,
v * h - 0.5); result order (the Vulkan / OpenGL / Metal gather order): the
texels (x0, y1), (x1, y1), (x1, y0), (x0, y0), with the address mode of each
axis (wrap, mirror, clamp to edge) applied per texel. The sampler is a random
guest sampler (TSC) that goes through decodeSampler like a draw; its filters do
not matter for a gather. The component is the TLD4S component field (R, G, B, A).
The header names the instruction but has no prose; the order and footprint are the
standard ones, not NVIDIA text.

Usage: gather_reference.py <hydra-maxwell-alu-reference-tests> [--seed N]
"""
import argparse
import math
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sampler_reference import EDGE, MIRROR, WRAP, bits_f32, f32, f32_bits, pack_tsc, resolve  # noqa: E402


def gather(texels, w, h, u, v, modes, component):
    fx, fy = u * w - 0.5, v * h - 0.5
    x0, y0 = math.floor(fx), math.floor(fy)
    out = []
    for dx, dy in ((0, 1), (1, 1), (1, 0), (0, 0)):
        xi, yi = resolve(x0 + dx, w, modes[0], False), resolve(y0 + dy, h, modes[1], False)
        out.append(texels[yi * w + xi][component])
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    parser.add_argument('--seed', type=int, default=20261020)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    configs = []
    for _ in range(300):
        w, h = rng.choice([(4, 4), (8, 4), (5, 7), (16, 2), (3, 3)])
        modes = (rng.choice([WRAP, MIRROR, EDGE]), rng.choice([WRAP, MIRROR, EDGE]))
        component = rng.randrange(4)
        tsc = pack_tsc(ADDRESS_U=modes[0], ADDRESS_V=modes[1], MAG_FILTER=rng.choice([1, 2]),
                       MIN_FILTER=rng.choice([1, 2]), MIP_FILTER=1, MAX_LOD_CLAMP=4095)
        texels = [[rng.random() for _ in range(4)] for _ in range(w * h)]
        samples = []
        while len(samples) < 8:
            u, v = f32(rng.uniform(-0.7, 1.7)), f32(rng.uniform(-0.7, 1.7))
            if any(abs((c * s - 0.5) - round(c * s - 0.5)) < 2e-3 for c, s in ((u, w), (v, h))):
                continue  # a sample on a texel centre: the footprint is ambiguous in float
            samples.append((u, v))
        configs.append((w, h, component, tsc, modes, texels, samples))
    lines = []
    for w, h, component, tsc, modes, texels, samples in configs:
        lines.append('CONFIG %d %d %d %s' % (w, h, component, ' '.join('%x' % x for x in tsc)))
        lines.append('DATA ' + ' '.join('%x' % f32_bits(c) for t in texels for c in t))
        for u, v in samples:
            lines.append('CASE %x %x' % (f32_bits(u), f32_bits(v)))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'in.txt').write_text('\n'.join(lines) + '\n')
        subprocess.run([args.runner, '--gather', str(tmp / 'in.txt'), str(tmp / 'out.txt')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        results = (tmp / 'out.txt').read_text().strip().split('\n')
    bad = total = 0
    example = None
    cursor = 0
    for w, h, component, tsc, modes, texels, samples in configs:
        for u, v in samples:
            got = [bits_f32(int(x, 16)) for x in results[cursor].split()]
            cursor += 1
            want = gather(texels, w, h, u, v, modes, component)
            total += 1
            if any(abs(g - e) > 1e-6 for g, e in zip(got, want)):
                bad += 1
                example = example or {'size': (w, h), 'component': component, 'modes': modes, 'uv': (u, v),
                                      'want': want, 'got': got}
    print('Gathers: %d, mismatches: %d %s' % (total, bad, example or ''))
    if bad:
        print('FAIL')
        sys.exit(1)
    print('PASS')


if __name__ == '__main__':
    main()
