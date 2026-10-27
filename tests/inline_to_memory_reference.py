#!/usr/bin/env python3
"""Inline-to-memory (class A140 registers 0x60..0x6d) model vs Hydra.

The engine is driven through the Maxwell 3D class, where it lives at methods
0x60-0x6d. Each case programs LINE_LENGTH_IN, LINE_COUNT, OFFSET_OUT, PITCH_OUT,
the destination surface and LAUNCH_DMA, then pushes LOAD_INLINE_DATA words; the
model writes the payload to the destination with plain loops (pitch rows, or
the block-linear layout of tests/copy_engine_reference.py) and the memory after
the engine ran is compared byte for byte. Registers are audited against
cla140.h.

Not defined by NVIDIA text and not asserted: the effect of data words beyond
LINE_LENGTH_IN * LINE_COUNT bytes, the SET_DST_LAYER and depth for volumes (Hydra
leaves the slice to the address, like Eden), LAUNCH_DMA completion types,
semaphores and interrupts (not implemented).

Usage: inline_to_memory_reference.py <hydra-copy-engine-reference-tests>
           [--header cla140.h] [--seed N]
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
from copy_engine_reference import BASE, MEMORY, Surface, random_image  # noqa: E402

REGISTERS = {  # header name -> method number
    'LINE_LENGTH_IN': 0x60, 'LINE_COUNT': 0x61, 'OFFSET_OUT_UPPER': 0x62, 'OFFSET_OUT': 0x63,
    'PITCH_OUT': 0x64, 'SET_DST_BLOCK_SIZE': 0x65, 'SET_DST_WIDTH': 0x66, 'SET_DST_HEIGHT': 0x67,
    'SET_DST_DEPTH': 0x68, 'SET_DST_LAYER': 0x69, 'SET_DST_ORIGIN_BYTES_X': 0x6a,
    'SET_DST_ORIGIN_SAMPLES_Y': 0x6b, 'LAUNCH_DMA': 0x6c, 'LOAD_INLINE_DATA': 0x6d,
}
PITCH, BLOCKLINEAR = 1, 0


def audit(header):
    text = header.read_text()
    for name, method in REGISTERS.items():
        match = re.search(r'^#define\s+NVA140_%s\s+(0x[0-9A-Fa-f]+)' % name, text, re.M)
        if not match or int(match[1], 16) // 4 != method:
            raise SystemExit('NVIDIA header mismatch for %s' % name)
    for name, value in (('LAUNCH_DMA_DST_MEMORY_LAYOUT_PITCH', 1), ('LAUNCH_DMA_DST_MEMORY_LAYOUT_BLOCKLINEAR', 0)):
        match = re.search(r'^#define\s+NVA140_%s\s+(0x[0-9A-Fa-f]+)' % name, text, re.M)
        if not match or int(match[1], 16) != value:
            raise SystemExit('NVIDIA header mismatch for ' + name)
    match = re.search(r'^#define\s+NVA140_LAUNCH_DMA_DST_MEMORY_LAYOUT\s+(\d+):(\d+)', text, re.M)
    if not match or (int(match[1]), int(match[2])) != (0, 0):
        raise SystemExit('NVIDIA header mismatch for DST_MEMORY_LAYOUT bits')
    return len(REGISTERS) + 3


def case(rng):
    layout = rng.choice([PITCH, BLOCKLINEAR])
    length = rng.randrange(1, 90)
    lines = rng.randrange(1, 12)
    total = length * lines
    payload = bytes(rng.randrange(256) for _ in range(total))
    if layout == PITCH:
        pitch = length + rng.randrange(0, 24)
        dst_off = 0x20000 + rng.randrange(0, 0x1000)
        writes = [(0x62, 0), (0x63, BASE + dst_off), (0x64, pitch)]
        surface = None
    else:
        # Rows of a block-linear surface at least as wide as the writes
        width = length + rng.randrange(0, 40)
        origin_x = rng.randrange(0, 20)
        width += origin_x
        height = lines + rng.randrange(0, 20)
        origin_y = rng.randrange(0, height - lines + 1)
        needed = (height + 7) // 8
        bh = rng.randrange(0, (needed - 1).bit_length() + 1)  # never taller than the image needs
        bd = rng.randrange(0, 2)
        surface = Surface(width, height, 1, 0, origin_x, origin_y, bh, bd)
        dst_off = 0x20000
        writes = [(0x62, 0), (0x63, BASE + dst_off), (0x65, (bd << 8) | (bh << 4)), (0x66, width),
                  (0x67, height), (0x68, 1), (0x69, 0), (0x6a, origin_x), (0x6b, origin_y)]
        pitch = 0
    writes += [(0x60, length), (0x61, lines), (0x6c, layout)]
    padded = payload + bytes(-total % 4)
    writes += [(0x6d, struct.unpack_from('<I', padded, i)[0]) for i in range(0, len(padded), 4)]
    return layout, length, lines, pitch, dst_off, surface, payload, writes


def expected(mem, layout, length, lines, pitch, dst_off, surface, payload):
    for line in range(lines):
        row = payload[line * length:(line + 1) * length]
        if layout == PITCH:
            mem[dst_off + line * pitch:dst_off + line * pitch + length] = row
        else:
            for i, byte in enumerate(row):
                mem[dst_off + surface.offset(surface.origin_x + i, surface.origin_y + line, 0)] = byte


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    parser.add_argument('--header', type=Path)
    parser.add_argument('--seed', type=int, default=20261020)
    args = parser.parse_args()
    audited = audit(args.header) if args.header else 0
    rng = random.Random(args.seed)
    cases = []
    for _ in range(200):
        c = case(rng)
        cases.append((c, random_image(rng)))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        with open(tmp / 'in.bin', 'wb') as out:
            for c, image in cases:
                writes = c[7]
                out.write(struct.pack('<II', len(image), len(writes)))
                for method, value in writes:
                    out.write(struct.pack('<II', method, value & 0xffffffff))
                out.write(bytes(image))
        subprocess.run([args.runner, str(tmp / 'in.bin'), str(tmp / 'out.bin'), 'b197'], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        actual = (tmp / 'out.bin').read_bytes()
    stats = {PITCH: [0, 0], BLOCKLINEAR: [0, 0]}
    example = {}
    for index, (c, image) in enumerate(cases):
        layout = c[0]
        model = bytearray(image)
        expected(model, *c[:7])
        got = actual[index * MEMORY:(index + 1) * MEMORY]
        stats[layout][0] += 1
        if got != bytes(model):
            stats[layout][1] += 1
            first = next(i for i in range(MEMORY) if got[i] != model[i])
            example.setdefault(layout, {'case': index, 'byte': first, 'expected': model[first], 'actual': got[first]})
    print('NVIDIA header fields audited: %d' % audited)
    failed = False
    for layout, name in ((PITCH, 'pitch destination'), (BLOCKLINEAR, 'block-linear destination')):
        cases_n, bad = stats[layout]
        print('%-26s %4d cases %4d mismatches  %s %s' % (name, cases_n, bad, 'ok' if not bad else 'MISMATCH',
                                                       example.get(layout, '') if bad else ''))
        failed |= bad != 0
    if failed:
        print('FAIL')
        sys.exit(1)
    print('PASS')


if __name__ == '__main__':
    main()
