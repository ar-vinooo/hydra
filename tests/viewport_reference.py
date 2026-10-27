#!/usr/bin/env python3
"""Guest viewport, scissor and window origin registers vs the Metal viewport.

Random SET_VIEWPORT_SCALE/OFFSET, coordinate swizzle, SET_WINDOW_ORIGIN,
SET_VIEWPORT_Z_CLIP, surface clip and scissor registers are written to Hydra's 3D
engine and `ThreeD::getViewport` / `getScissor` build the Metal state. The model
applies the NVIDIA viewport transform to sample points of the normalised device
space:

    x_win = offset_x + scale_x * x_ndc        (NEG_X flips the sign of x_ndc)
    y_win = offset_y + scale_y * y_ndc        (counted from the bottom with
                                               WINDOW_ORIGIN_MODE LOWER_LEFT)
    z_win = offset_z + scale_z * z_ndc        (z_ndc in -1..1, or 0..1 with
                                               VIEWPORT_Z_CLIP ZERO_TO_POSITIVE_W)

and the points Metal's viewport transform gives (x_fb = originX + (x+1)/2 *
width, y_fb = originY + (1-y)/2 * height, z_fb = znear + z_m * (zfar - znear))
must land on the same pixels: the same x, the same row from the top and the same
depth. Scissors are compared as pixel ranges. Registers are audited against
clb197.h.

The header names the registers and has no prose; these are the usual
conventions, not NVIDIA text. Swizzles that swap axes (POS_Y into X ...) are not
supported by Hydra and not asserted; FLIP_Y of the window origin only changes the
winding in Hydra and is not asserted.

Usage: viewport_reference.py <hydra-viewport-reference-tests> [--header clb197.h]
"""
import argparse
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

POS_X, NEG_X, POS_Y, NEG_Y, POS_Z, NEG_Z, POS_W, NEG_W = range(8)


def audit(header):
    text = header.read_text()
    checks = {
        'SET_VIEWPORT_SCALE_X(j)': (0x0a00, 32), 'SET_VIEWPORT_SCALE_Y(j)': (0x0a04, 32),
        'SET_VIEWPORT_SCALE_Z(j)': (0x0a08, 32), 'SET_VIEWPORT_OFFSET_X(j)': (0x0a0c, 32),
        'SET_VIEWPORT_OFFSET_Y(j)': (0x0a10, 32), 'SET_VIEWPORT_OFFSET_Z(j)': (0x0a14, 32),
        'SET_VIEWPORT_COORDINATE_SWIZZLE(j)': (0x0a18, 32),
    }
    for name, (base, stride) in checks.items():
        match = re.search(r'^#define\s+NVB197_%s\s+\((0x[0-9a-fA-F]+)\+\(j\)\*(\d+)\)' % re.escape(name), text, re.M)
        if not match or (int(match[1], 16), int(match[2])) != (base, stride):
            raise SystemExit('NVIDIA header mismatch for ' + name)
    for name, value in (('SET_WINDOW_ORIGIN', 0x13ac), ('SET_VIEWPORT_Z_CLIP', 0x0d7c)):
        match = re.search(r'^#define\s+NVB197_%s\s+(0x[0-9a-fA-F]+)' % name, text, re.M)
        if not match or int(match[1], 16) != value:
            raise SystemExit('NVIDIA header mismatch for ' + name)
    for name, value in (('SET_WINDOW_ORIGIN_MODE_LOWER_LEFT', 1), ('SET_VIEWPORT_Z_CLIP_RANGE_ZERO_TO_POSITIVE_W', 1),
                        ('SET_VIEWPORT_Z_CLIP_RANGE_NEGATIVE_W_TO_POSITIVE_W', 0),
                        ('SET_VIEWPORT_COORDINATE_SWIZZLE_X_POS_X', 0), ('SET_VIEWPORT_COORDINATE_SWIZZLE_X_NEG_X', 1),
                        ('SET_VIEWPORT_COORDINATE_SWIZZLE_Y_POS_Y', 2), ('SET_VIEWPORT_COORDINATE_SWIZZLE_Y_NEG_Y', 3)):
        match = re.search(r'^#define\s+NVB197_%s\s+(0x[0-9a-fA-F]+)' % name, text, re.M)
        if not match or int(match[1], 16) != value:
            raise SystemExit('NVIDIA header mismatch for ' + name)
    return len(checks) + 2 + 7


def f32_bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def make_case(rng):
    width, height = rng.choice([(1280, 720), (1920, 1080), (640, 360), (512, 512), (300, 200)])
    scale_x, scale_y = f32(width / 2 * rng.uniform(0.3, 1.0)), f32(height / 2 * rng.uniform(0.3, 1.0))
    offset_x = f32(rng.uniform(scale_x, width - scale_x))
    offset_y = f32(rng.uniform(scale_y, height - scale_y))
    zero_to_one = rng.random() < 0.5
    znear, zfar = sorted((rng.uniform(0, 1), rng.uniform(0, 1)))
    if zero_to_one:
        scale_z, offset_z = f32(zfar - znear), f32(znear)
    else:
        scale_z, offset_z = f32((zfar - znear) / 2), f32((zfar + znear) / 2)
    neg_x, neg_y = rng.random() < 0.3, rng.random() < 0.3
    swizzle = (NEG_X if neg_x else POS_X) | ((NEG_Y if neg_y else POS_Y) << 4) | (POS_Z << 8) | (POS_W << 12)
    lower_left = rng.random() < 0.5
    enabled = rng.random() < 0.7
    min_x, min_y = rng.randrange(0, width // 2), rng.randrange(0, height // 2)
    max_x, max_y = rng.randrange(min_x + 1, width), rng.randrange(min_y + 1, height)
    return dict(scale=(scale_x, scale_y, scale_z), offset=(offset_x, offset_y, offset_z), swizzle=swizzle,
                neg=(neg_x, neg_y), lower_left=lower_left, zero_to_one=zero_to_one, width=width, height=height,
                scissor=(enabled, min_x, max_x, min_y, max_y))


def expected(case, x, y, z):
    """Window position of an NDC point, in framebuffer pixels from the top left."""
    sx = -case['scale'][0] if case['neg'][0] else case['scale'][0]
    sy = -case['scale'][1] if case['neg'][1] else case['scale'][1]
    x_fb = case['offset'][0] + sx * x
    y_win = case['offset'][1] + sy * y
    y_fb = case['height'] - y_win if case['lower_left'] else y_win
    z_win = case['offset'][2] + case['scale'][2] * z
    return x_fb, y_fb, z_win


def metal(view, x, y, z, zero_to_one):
    origin_x, origin_y, width, height, znear, zfar = view
    z_metal = z if zero_to_one else (z + 1) / 2
    return (origin_x + (x + 1) / 2 * width, origin_y + (1 - y) / 2 * height,
            znear + z_metal * (zfar - znear))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    parser.add_argument('--header', type=Path)
    parser.add_argument('--seed', type=int, default=20261020)
    args = parser.parse_args()
    audited = audit(args.header) if args.header else 0
    rng = random.Random(args.seed)
    cases = [make_case(rng) for _ in range(500)]
    lines = []
    for c in cases:
        words = [f32_bits(v) for v in c['scale'] + c['offset']] + [c['swizzle']]
        lines.append(' '.join('%x' % w for w in words + [int(c['lower_left']), int(c['zero_to_one']), c['height'],
                                                         *map(int, c['scissor'])]))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'in.txt').write_text('\n'.join(lines) + '\n')
        subprocess.run([args.runner, str(tmp / 'in.txt'), str(tmp / 'out.txt')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        results = (tmp / 'out.txt').read_text().strip().split('\n')
    bad_viewport = bad_scissor = 0
    example = None
    for case, line in zip(cases, results):
        v = list(map(float, line.split()))
        view, rect = v[:6], list(map(int, v[6:]))
        for x in (-1.0, -0.4, 0.0, 0.6, 1.0):
            for y in (-1.0, -0.2, 0.0, 0.8, 1.0):
                for z in ((0.0, 0.3, 1.0) if case['zero_to_one'] else (-1.0, 0.1, 1.0)):
                    want = expected(case, x, y, z)
                    got = metal(view, x, y, z, case['zero_to_one'])
                    if any(abs(a - b) > 2e-3 * max(1.0, abs(a)) for a, b in zip(want, got)):
                        bad_viewport += 1
                        example = example or (case, (x, y, z), want, got)
        enabled, min_x, max_x, min_y, max_y = case['scissor']
        height = case['height']
        if enabled:
            top = height - max_y if case['lower_left'] else min_y
            want_rect = [min_x, top, max_x - min_x, max_y - min_y]
        else:
            want_rect = [0, 0, 0xffff, 0xffff]
        if rect != want_rect:
            bad_scissor += 1
            example = example or (case, 'scissor', want_rect, rect)
    print('NVIDIA header values audited: %d' % audited)
    print('Viewport points: %d mismatches in %d cases' % (bad_viewport, len(cases)))
    print('Scissors: %d mismatches in %d cases' % (bad_scissor, len(cases)))
    if bad_viewport or bad_scissor:
        print('FAIL: e.g. %s' % (example,))
        sys.exit(1)
    print('PASS')


if __name__ == '__main__':
    main()
