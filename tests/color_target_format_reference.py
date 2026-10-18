#!/usr/bin/env python3
"""Guest colour-target formats 0x40-0x47 (and A8) vs Hydra + Metal.

Expected texel bytes are computed here from the NVIDIA format names in clb197.h
(SET_COLOR_TARGET_FORMAT_V_*). NVIDIA lists components from the most significant
bits down, so the bytes in little-endian memory are in reverse order of the name:
A8R8 -> bytes R, A; B8G8R8A8 -> bytes A, R, G, B; R16_A16 -> R then A (16 bit each).
Controls (RGBA8 0xD5, R16F 0xF2) check the reference path with formats that already
worked. tests/color_target_format_tests runs the guest shader through Hydra's shader
cache and pipeline and prints the raw bytes. No physical NVIDIA execution.

Usage: color_target_format_reference.py <hydra-color-target-format-tests> [--header clb197.h]
"""
import argparse
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

NAMES = {
    0x40: 'A16', 0x41: 'AF16', 0x42: 'AF32', 0x43: 'A8R8', 0x44: 'R16_A16',
    0x45: 'RF16_AF16', 0x46: 'RF32_AF32', 0x47: 'B8G8R8A8',
}


def audit(header):
    text = header.read_text()
    for value, name in NAMES.items():
        match = re.search(r'^#define\s+NVB197_SET_COLOR_TARGET_FORMAT_V_%s\s+(0x[0-9A-Fa-f]+)' % name,
                          text, re.M)
        if not match or int(match[1], 16) != value:
            raise SystemExit('NVIDIA header mismatch for ' + name)
    return len(NAMES)


def f32(x):
    return struct.pack('<f', x)


def f16(x):
    return struct.pack('<e', x)


def unorm(x, bits):
    n = (1 << bits) - 1
    v = min(max(x, 0.0), 1.0) * n
    r = int(v)
    frac = v - r
    if frac > 0.5 or (frac == 0.5 and (r & 1)):
        r += 1
    return r


def u16(x):
    return struct.pack('<H', unorm(x, 16))


def u8(x):
    return bytes([unorm(x, 8)])


# format -> function(r, g, b, a) -> expected bytes
EXPECT = {
    0x40: lambda r, g, b, a: u16(a),
    0x41: lambda r, g, b, a: f16(a),
    0x42: lambda r, g, b, a: f32(a),
    0x43: lambda r, g, b, a: u8(r) + u8(a),
    0x44: lambda r, g, b, a: u16(r) + u16(a),
    0x45: lambda r, g, b, a: f16(r) + f16(a),
    0x46: lambda r, g, b, a: f32(r) + f32(a),
    0x47: lambda r, g, b, a: u8(a) + u8(r) + u8(g) + u8(b),
    0xF7: lambda r, g, b, a: u8(a),                          # A8
    0xD5: lambda r, g, b, a: u8(r) + u8(g) + u8(b) + u8(a),  # control: A8B8G8R8
    0xF2: lambda r, g, b, a: f16(r),                         # control: RF16
}


# 16-bit float lanes (byte offset / 2) of each format
HALF_LANES = {0x41: [0], 0x45: [0, 1], 0xF2: [0]}


def clear_expect(fmt, color, mask):
    """Clear colour words and mask the Metal format stores (R=1 G=2 B=4 A=8)."""
    r, g, b, a = color
    if fmt in (0x40, 0x41, 0x42, 0xF7):
        return [a, 0, 0, 0], 1 if mask & 8 else 0
    if fmt in (0x43, 0x44, 0x45, 0x46):
        return [r, a, 0, 0], (mask & 1) | (2 if mask & 8 else 0)
    if fmt == 0x47:  # bytes A, R, G, B
        return [a, r, g, b], (1 if mask & 8 else 0) | (2 if mask & 1 else 0) | \
            (4 if mask & 2 else 0) | (8 if mask & 4 else 0)
    return [r, g, b, a], mask


def bits(x):
    return struct.unpack('<I', f32(x))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--header', type=Path)
    parser.add_argument('--seed', type=int, default=3)
    args = parser.parse_args()
    if args.header:
        print('NVIDIA header values audited:', audit(args.header))
    rng = random.Random(args.seed)
    values = [0.0, 1.0, 0.25, 0.5, 0.75, 0.1, 0.9, 0.333, 0.6667, 100.0, -0.5, 2.0]
    values = [struct.unpack('<f', f32(v))[0] for v in values]
    render, clear = [], []
    for fmt in EXPECT:
        for _ in range(24):
            color = tuple(struct.unpack('<f', f32(rng.choice(values) if rng.random() < 0.3
                                                  else rng.uniform(0.01, 0.99)))[0] for _ in range(4))
            if fmt in (0x45, 0x46, 0x41, 0x42, 0xF2):
                color = tuple(rng.choice([c, rng.uniform(-8, 8)]) for c in color)
                color = tuple(struct.unpack('<f', f32(c))[0] for c in color)
            render.append((fmt, color))
    for fmt in list(EXPECT):
        for mask in range(1, 16):
            color = tuple(rng.getrandbits(32) for _ in range(4))
            clear.append((fmt, color, mask))
    with tempfile.TemporaryDirectory() as tmp:
        cases, out = Path(tmp) / 'cases.txt', Path(tmp) / 'out.txt'
        with cases.open('w') as f:
            for fmt, color in render:
                f.write('R %x %s\n' % (fmt, ' '.join('%x' % bits(c) for c in color)))
            for fmt, color, mask in clear:
                f.write('C %x %s %x\n' % (fmt, ' '.join('%x' % c for c in color), mask))
        completed = subprocess.run([str(args.binary), str(cases), str(out)])
        if completed.returncode != 0:
            return completed.returncode
        lines = out.read_text().splitlines()
    if len(lines) != len(render) + len(clear):
        print('incomplete readback')
        return 1
    failures, per_format, half_inexact = 0, {}, 0
    for (fmt, color), line in zip(render, lines):
        expected = EXPECT[fmt](*color)
        got = bytes.fromhex(line)
        ok = got == expected
        if not ok and fmt in HALF_LANES and len(got) == len(expected):
            # The GPU's fp32 -> fp16 render target conversion truncates (observed on
            # the RF16 control format too); RN-even would match NVIDIA. Allow 1 ulp.
            lanes = [(struct.unpack_from('<H', got, 2 * i)[0], struct.unpack_from('<H', expected, 2 * i)[0])
                     for i in HALF_LANES[fmt]]
            ok = all(abs(a - b) <= 1 for a, b in lanes)
            half_inexact += ok
        stat = per_format.setdefault(('render', fmt), [0, 0])
        stat[0] += ok
        stat[1] += 1
        if not ok and stat[1] - stat[0] <= 3:
            print('  render %#x color=%s expected=%s got=%s' % (fmt, color, expected.hex(), line))
    for (fmt, color, mask), line in zip(clear, lines[len(render):]):
        words = [int(x, 16) for x in line.split()]
        exp_color, exp_mask = clear_expect(fmt, color, mask)
        ok = words[:4] == exp_color and words[4] == exp_mask
        stat = per_format.setdefault(('clear', fmt), [0, 0])
        stat[0] += ok
        stat[1] += 1
        if not ok and stat[1] - stat[0] <= 3:
            print('  clear %#x mask=%x expected=%s/%x got=%s' % (fmt, mask, exp_color, exp_mask, words))
    for (kind, fmt), (passed, total) in sorted(per_format.items()):
        name = NAMES.get(fmt, {0xF7: 'A8', 0xD5: 'A8B8G8R8 (control)', 0xF2: 'RF16 (control)'}[fmt]
                         if fmt not in NAMES else '')
        print('%s %-6s %#04x %-20s %3d/%-3d' % ('ok ' if passed == total else 'BAD', kind, fmt, name, passed, total))
        failures += passed != total
    if half_inexact:
        print('note: %d half-float results were 1 ulp off RN-even (host truncation)' % half_inexact)
    print('%d format/path groups failed' % failures)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
