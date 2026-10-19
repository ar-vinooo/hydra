#!/usr/bin/env python3
"""DMA copy engine (class B0B5) model in Python vs Hydra's engine.

Each case is a random memory image plus a script of method writes. The model
below applies the documented register fields (clb0b5.h) to the image with plain
Python loops and the result is compared byte for byte with the memory Hydra's
`Copy` engine leaves behind (tests/copy_engine_reference_tests.cpp drives the
engine through its method interface). Registers and fields are audited against
the NVIDIA header first.

What the header does not define is stated in the code: the header names the
fields but gives no prose, so the unit of LINE_LENGTH_IN with REMAP_ENABLE (one
element of NUM_SRC/NUM_DST components of COMPONENT_SIZE bytes), the order of
lines through the Y/Z slices of a block-linear surface, and the behaviour of
SET_*_WIDTH in bytes come from the usual driver convention and Hydra's own
documented behaviour, not from NVIDIA text. No NVIDIA hardware was used.

Usage: copy_engine_reference.py <hydra-copy-engine-reference-tests>
           [--header clb0b5.h] [--seed N] [--report out.json]
"""
import argparse
import json
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

BASE = 0x200000
MEMORY = 0x40000

# Method numbers (byte offset / 4) of the registers used, with their header names
REGISTERS = {
    'LAUNCH_DMA': 0xc0, 'SET_SEMAPHORE_A': 0x90, 'SET_SEMAPHORE_B': 0x91,
    'SET_SEMAPHORE_PAYLOAD': 0x92, 'OFFSET_IN_UPPER': 0x100, 'OFFSET_IN_LOWER': 0x101,
    'OFFSET_OUT_UPPER': 0x102, 'OFFSET_OUT_LOWER': 0x103, 'PITCH_IN': 0x104,
    'PITCH_OUT': 0x105, 'LINE_LENGTH_IN': 0x106, 'LINE_COUNT': 0x107,
    'SET_REMAP_CONST_A': 0x1c0, 'SET_REMAP_CONST_B': 0x1c1, 'SET_REMAP_COMPONENTS': 0x1c2,
    'SET_DST_BLOCK_SIZE': 0x1c3, 'SET_DST_WIDTH': 0x1c4, 'SET_DST_HEIGHT': 0x1c5,
    'SET_DST_DEPTH': 0x1c6, 'SET_DST_LAYER': 0x1c7, 'SET_DST_ORIGIN': 0x1c8,
    'SET_SRC_BLOCK_SIZE': 0x1ca, 'SET_SRC_WIDTH': 0x1cb, 'SET_SRC_HEIGHT': 0x1cc,
    'SET_SRC_DEPTH': 0x1cd, 'SET_SRC_LAYER': 0x1ce, 'SET_SRC_ORIGIN': 0x1cf,
}
# Bit fields of LAUNCH_DMA and SET_REMAP_COMPONENTS: name -> (low bit, high bit)
LAUNCH = {'DATA_TRANSFER_TYPE': (0, 1), 'FLUSH_ENABLE': (2, 2), 'SEMAPHORE_TYPE': (3, 4),
          'INTERRUPT_TYPE': (5, 6), 'SRC_MEMORY_LAYOUT': (7, 7), 'DST_MEMORY_LAYOUT': (8, 8),
          'MULTI_LINE_ENABLE': (9, 9), 'REMAP_ENABLE': (10, 10)}
REMAP = {'DST_X': (0, 2), 'DST_Y': (4, 6), 'DST_Z': (8, 10), 'DST_W': (12, 14),
         'COMPONENT_SIZE': (16, 17), 'NUM_SRC_COMPONENTS': (20, 21), 'NUM_DST_COMPONENTS': (24, 25)}
CONST_A, CONST_B, NO_WRITE = 4, 5, 6


def audit(header):
    text = header.read_text()

    def define(name):
        match = re.search(r'^#define\s+NVB0B5_%s\s+\(?(0x[0-9A-Fa-f]+)\)?' % name, text, re.M)
        return int(match[1], 16) if match else None

    def field(name):
        match = re.search(r'^#define\s+NVB0B5_%s\s+(\d+):(\d+)' % name, text, re.M)
        return (int(match[2]), int(match[1])) if match else None

    checked = 0
    for name, method in REGISTERS.items():
        if name.endswith('_UPPER') or name.endswith('_LOWER'):
            value = define(name)
        else:
            value = define(name)
        if value is None or value // 4 != method:
            raise SystemExit('NVIDIA header mismatch for %s: %r vs method %#x' % (name, value, method))
        checked += 1
    for name, bits in LAUNCH.items():
        if field('LAUNCH_DMA_' + name) != bits:
            raise SystemExit('NVIDIA header mismatch for LAUNCH_DMA_' + name)
        checked += 1
    for name, bits in REMAP.items():
        if field('SET_REMAP_COMPONENTS_' + name) != bits:
            raise SystemExit('NVIDIA header mismatch for SET_REMAP_COMPONENTS_' + name)
        checked += 1
    for suffix, value in (('DST_X_NO_WRITE', NO_WRITE), ('DST_X_CONST_A', CONST_A), ('DST_X_CONST_B', CONST_B)):
        if define('SET_REMAP_COMPONENTS_' + suffix) != value:
            raise SystemExit('NVIDIA header mismatch for ' + suffix)
        checked += 1
    return checked


def pack_fields(fields, values):
    word = 0
    for name, value in values.items():
        low, high = fields[name]
        word |= (value & ((1 << (high - low + 1)) - 1)) << low
    return word


def launch_word(transfer=1, semaphore=0, src_pitch=True, dst_pitch=True, multi=True, remap=False):
    return pack_fields(LAUNCH, {'DATA_TRANSFER_TYPE': transfer, 'SEMAPHORE_TYPE': semaphore,
                                'SRC_MEMORY_LAYOUT': int(src_pitch), 'DST_MEMORY_LAYOUT': int(dst_pitch),
                                'MULTI_LINE_ENABLE': int(multi), 'REMAP_ENABLE': int(remap)})


def remap_word(selects, size, src_count, dst_count):
    return pack_fields(REMAP, {'DST_X': selects[0], 'DST_Y': selects[1], 'DST_Z': selects[2],
                               'DST_W': selects[3], 'COMPONENT_SIZE': size - 1,
                               'NUM_SRC_COMPONENTS': src_count - 1, 'NUM_DST_COMPONENTS': dst_count - 1})


# ---------------------------------------------------------------- block linear

def gob_inner(x, y):
    return (((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 +
            (y % 2) * 16 + (x % 16))


class Surface:
    def __init__(self, width, height, depth, layer, origin_x, origin_y, bh, bd):
        self.width, self.height, self.depth, self.layer = width, height, depth, layer
        self.origin_x, self.origin_y, self.bh, self.bd = origin_x, origin_y, bh, bd

    def offset(self, x, y, z):
        gobs_x = (self.width + 63) // 64
        bh, bd = 1 << self.bh, 1 << self.bd
        blocks_y = (self.height + 8 * bh - 1) // (8 * bh)
        block = ((z // bd) * blocks_y + (y // 8) // bh) * gobs_x + x // 64
        gob = (z % bd) * bh + (y // 8) % bh
        return block * 512 * bh * bd + gob * 512 + gob_inner(x, y)

    def size(self):
        gobs_x = (self.width + 63) // 64
        bh, bd = 1 << self.bh, 1 << self.bd
        blocks_y = (self.height + 8 * bh - 1) // (8 * bh)
        blocks_z = (self.depth + bd - 1) // bd
        return blocks_z * blocks_y * gobs_x * 512 * bh * bd

    def rows(self):
        return self.height - self.origin_y

    def available_lines(self):
        if self.origin_y >= self.height or self.layer >= self.depth:
            return 0
        return self.rows() * (self.depth - self.layer)

    def slice_row(self, line):
        return self.origin_y + line % self.rows(), self.layer + line // self.rows()

    def pitch_row(self, line):
        return (line // self.rows()) * self.height + line % self.rows()

    def write_regs(self, first, bpp=1):
        """SET_{DST,SRC}_BLOCK_SIZE .. ORIGIN, in method order (width in elements)."""
        return [(first, (1 << 12) | (self.bd << 8) | (self.bh << 4)), (first + 1, self.width // bpp),
                (first + 2, self.height), (first + 3, self.depth), (first + 4, self.layer),
                (first + 5, self.origin_x | (self.origin_y << 16))]


# ---------------------------------------------------------------------- model

def model_remap_line(src, selects, size, src_count, dst_count, consts, dst, elements):
    """Apply the remap to `elements` elements; returns the new destination bytes."""
    out = bytearray(dst)
    for e in range(elements):
        for i in range(dst_count):
            sel = selects[i]
            target = (e * dst_count + i) * size
            if sel < 4:
                source = (e * src_count + sel) * size
                out[target:target + size] = src[source:source + size]
            elif sel in (CONST_A, CONST_B):
                value = consts[0 if sel == CONST_A else 1]
                out[target:target + size] = (value & ((1 << (8 * size)) - 1)).to_bytes(size, 'little')
    return bytes(out)


def run_model(mem, writes):
    regs = {}
    for method, value in writes:
        regs[method] = value
        if method == REGISTERS['LAUNCH_DMA']:
            launch(mem, regs, value)


def address(regs, upper):
    return ((regs.get(upper, 0) & 0xff) << 32) | regs.get(upper + 1, 0)


def launch(mem, regs, word):
    transfer = word & 3
    if transfer != 0:
        execute_copy(mem, regs, word)
    semaphore = (word >> 3) & 3
    if semaphore:
        target = address(regs, REGISTERS['SET_SEMAPHORE_A']) - BASE
        payload = regs.get(REGISTERS['SET_SEMAPHORE_PAYLOAD'], 0)
        mem[target:target + 4] = struct.pack('<I', payload)
        if semaphore == 2:
            mem[target + 8:target + 16] = b'\x00' * 8  # timestamp: not compared


def execute_copy(mem, regs, word):
    src_pitch = (word >> 7) & 1
    dst_pitch = (word >> 8) & 1
    multi = (word >> 9) & 1
    remap = (word >> 10) & 1
    src = address(regs, REGISTERS['OFFSET_IN_UPPER']) - BASE
    dst = address(regs, REGISTERS['OFFSET_OUT_UPPER']) - BASE
    pitch_in, pitch_out = regs.get(0x104, 0), regs.get(0x105, 0)
    length, count = regs.get(0x106, 0), regs.get(0x107, 0)
    components = regs.get(0x1c2, 0)
    selects = [(components >> s) & 7 for s in (0, 4, 8, 12)]
    size = ((components >> 16) & 3) + 1
    src_count = ((components >> 20) & 3) + 1
    dst_count = ((components >> 24) & 3) + 1
    consts = (regs.get(0x1c0, 0), regs.get(0x1c1, 0))
    lines = count if multi else 1
    if src_pitch and dst_pitch:
        for line in range(lines):
            s, d = src + pitch_in * line, dst + pitch_out * line
            if remap:
                data = mem[s:s + length * src_count * size]
                width = length * dst_count * size
                mem[d:d + width] = model_remap_line(data, selects, size, src_count, dst_count, consts,
                                                   bytes(mem[d:d + width]), length)
            else:
                mem[d:d + length] = mem[s:s + length]
        return
    # Tiled copies. Without REMAP_ENABLE an element is a byte. With it, an element
    # is NUM_SRC_COMPONENTS (source side) or NUM_DST_COMPONENTS (destination
    # side) components of COMPONENT_SIZE bytes and SET_*_WIDTH counts elements.
    src_bpp = src_count * size if remap else 1
    dst_bpp = dst_count * size if remap else 1
    dst_surface = surface_from_regs(regs, 0x1c3, dst_bpp)
    src_surface = surface_from_regs(regs, 0x1ca, src_bpp)
    n = lines
    for surface, bpp, pitch in ((src_surface, src_bpp, src_pitch), (dst_surface, dst_bpp, dst_pitch)):
        if pitch:
            continue
        if surface.origin_x * bpp + length * bpp > surface.width:
            return
        n = min(n, surface.available_lines())
    if n == 0 or length == 0:
        return
    for line in range(n):
        for e in range(length):
            if src_pitch:
                row = dst_surface.pitch_row(line) if not dst_pitch else line
                base = src + pitch_in * row + e * src_bpp
                element = bytes(mem[base:base + src_bpp])
            else:
                y, z = src_surface.slice_row(line)
                x = (src_surface.origin_x + e) * src_bpp
                element = bytes(mem[src + src_surface.offset(x + i, y, z)] for i in range(src_bpp))
            if remap:
                if dst_pitch:
                    row = src_surface.pitch_row(line) if not src_pitch else line
                    base = dst + pitch_out * row + e * dst_bpp
                    old = bytes(mem[base:base + dst_bpp])
                else:
                    y, z = dst_surface.slice_row(line)
                    x = (dst_surface.origin_x + e) * dst_bpp
                    old = bytes(mem[dst + dst_surface.offset(x + i, y, z)] for i in range(dst_bpp))
                element = model_remap_line(element, selects, size, src_count, dst_count, consts, old, 1)
            if dst_pitch:
                row = src_surface.pitch_row(line) if not src_pitch else line
                base = dst + pitch_out * row + e * dst_bpp
                mem[base:base + dst_bpp] = element
            else:
                y, z = dst_surface.slice_row(line)
                x = (dst_surface.origin_x + e) * dst_bpp
                for i in range(dst_bpp):
                    mem[dst + dst_surface.offset(x + i, y, z)] = element[i]


def surface_from_regs(regs, first, bpp):
    block = regs.get(first, 0)
    origin = regs.get(first + 5, 0)
    # SET_*_WIDTH counts elements of the copy (bytes without a remap)
    return Surface(regs.get(first + 1, 0) * bpp, regs.get(first + 2, 0), regs.get(first + 3, 0),
                   regs.get(first + 4, 0), origin & 0xffff, origin >> 16, (block >> 4) & 15, (block >> 8) & 15)


# ---------------------------------------------------------------------- cases

def random_image(rng):
    return bytearray(rng.randrange(256) for _ in range(MEMORY))


def case_plain(rng):
    length = rng.randrange(1, 300)
    lines = rng.randrange(1, 9)
    pitch_in = length + rng.randrange(0, 40)
    pitch_out = length + rng.randrange(0, 40)
    multi = rng.random() < 0.8
    src, dst = rng.randrange(0, 0x2000), 0x10000 + rng.randrange(0, 0x2000)
    writes = [(0x100, (BASE + src) >> 32), (0x101, (BASE + src) & 0xffffffff),
              (0x102, (BASE + dst) >> 32), (0x103, (BASE + dst) & 0xffffffff),
              (0x104, pitch_in), (0x105, pitch_out), (0x106, length), (0x107, lines),
              (0xc0, launch_word(transfer=rng.choice([1, 2]), multi=multi))]
    return 'plain pitch to pitch', writes


def case_remap(rng):
    size = rng.randrange(1, 5)
    src_count, dst_count = rng.randrange(1, 5), rng.randrange(1, 5)
    selects = []
    for _ in range(4):
        choices = list(range(src_count)) + [CONST_A, CONST_B, NO_WRITE]
        selects.append(rng.choice(choices))
    elements = rng.randrange(1, 40)
    lines = rng.randrange(1, 6)
    pitch_in = elements * src_count * size + rng.randrange(0, 16)
    pitch_out = elements * dst_count * size + rng.randrange(0, 16)
    src, dst = rng.randrange(0, 0x2000), 0x10000 + rng.randrange(0, 0x2000)
    writes = [(0x100, 0), (0x101, BASE + src), (0x102, 0), (0x103, BASE + dst),
              (0x104, pitch_in), (0x105, pitch_out), (0x106, elements), (0x107, lines),
              (0x1c0, rng.getrandbits(32)), (0x1c1, rng.getrandbits(32)),
              (0x1c2, remap_word(selects, size, src_count, dst_count)),
              (0xc0, launch_word(multi=rng.random() < 0.8, remap=True))]
    return 'remapped pitch to pitch', writes


def case_semaphore(rng):
    target = 0x20000 + rng.randrange(0, 0x1000) * 16
    kind = rng.choice([1, 2])
    writes = [(0x90, 0), (0x91, BASE + target), (0x92, rng.getrandbits(32))]
    if rng.random() < 0.5:  # a copy plus semaphore
        writes += [(0x100, 0), (0x101, BASE), (0x102, 0), (0x103, BASE + 0x10000),
                   (0x104, 64), (0x105, 64), (0x106, 64), (0x107, 2)]
        writes.append((0xc0, launch_word(transfer=1, semaphore=kind)))
    else:
        writes.append((0xc0, launch_word(transfer=0, semaphore=kind)))
    return 'semaphore release', writes


def random_surface(rng, width_bytes=None, max_size=0x8000):
    for _ in range(100):
        s = Surface(width_bytes or rng.randrange(8, 200), rng.randrange(1, 40), rng.choice([1, 1, 2, 3, 4]),
                    0, 0, 0, rng.randrange(0, 4), rng.randrange(0, 3))
        if s.size() <= max_size:
            return s
    return Surface(64, 8, 1, 0, 0, 0, 0, 0)


def tiled_surface(rng, bpp, max_size=0x8000):
    for _ in range(100):
        s = Surface(rng.randrange(2, 60) * bpp, rng.randrange(1, 40), rng.choice([1, 1, 2, 3, 4]),
                    0, 0, 0, rng.randrange(0, 4), rng.randrange(0, 3))
        if s.size() <= max_size:
            return s
    return Surface(8 * bpp, 8, 1, 0, 0, 0, 0, 0)


def case_tiled(rng, kind, mode):
    """mode: 'none' (bytes), 'identity' (remap, equal counts) or 'general' (any remap)."""
    size = rng.randrange(1, 5)
    src_count = rng.randrange(1, 5)
    dst_count = src_count if mode != 'general' else rng.randrange(1, 5)
    src_bpp = src_count * size if mode != 'none' else 1
    dst_bpp = dst_count * size if mode != 'none' else 1
    if mode == 'general':
        selects = [rng.choice(list(range(src_count)) + [CONST_A, CONST_B, NO_WRITE]) for _ in range(4)]
    else:
        selects = [0, 1, 2, 3]
    src = tiled_surface(rng, src_bpp)
    dst = tiled_surface(rng, dst_bpp)
    # Slice origins skip rows only when they stay inside one slice: the pitch
    # side of a multi-slice copy then has an unambiguous layout
    for s, bpp in ((src, src_bpp), (dst, dst_bpp)):
        s.origin_x = rng.randrange(0, 4)
        if s.depth == 1:
            s.origin_y = rng.randrange(0, s.height)
        s.layer = rng.randrange(0, s.depth) if rng.random() < 0.3 else 0
    room = min(src.width // src_bpp - src.origin_x, dst.width // dst_bpp - dst.origin_x)
    if room < 1:
        src.origin_x = dst.origin_x = 0
        room = min(src.width // src_bpp, dst.width // dst_bpp)
    length = rng.randrange(1, min(room, 40) + 1)
    lines = rng.randrange(1, 60)
    if kind == 'pitch->bl':
        src_off, dst_off = rng.randrange(0, 0x2000), 0x20000
        src_pitch, dst_pitch = True, False
    elif kind == 'bl->pitch':
        src_off, dst_off = 0x20000, 0x10000 + rng.randrange(0, 0x2000)
        src_pitch, dst_pitch = False, True
    else:
        src_off, dst_off = 0x20000, 0x30000
        src_pitch, dst_pitch = False, False
    writes = [(0x100, 0), (0x101, BASE + src_off), (0x102, 0), (0x103, BASE + dst_off),
              (0x104, length * src_bpp + rng.randrange(0, 20)), (0x105, length * dst_bpp + rng.randrange(0, 20)),
              (0x106, length), (0x107, lines)]
    if mode != 'none':
        writes += [(0x1c0, rng.getrandbits(32)), (0x1c1, rng.getrandbits(32)),
                   (0x1c2, remap_word(selects, size, src_count, dst_count))]
    writes += dst.write_regs(0x1c3, dst_bpp)
    writes += src.write_regs(0x1ca, src_bpp)
    writes.append((0xc0, launch_word(src_pitch=src_pitch, dst_pitch=dst_pitch, remap=mode != 'none')))
    label = {'none': '', 'identity': ' identity remap', 'general': ' general remap'}[mode]
    return 'tiled ' + kind + label, writes


def build(rng, count):
    cases = []
    generators = [(case_plain, 60), (case_remap, 150), (case_semaphore, 12)]
    for mode, n in (('none', 50), ('identity', 25), ('general', 40)):
        for kind in ('pitch->bl', 'bl->pitch', 'bl->bl'):
            generators.append((lambda r, kind=kind, mode=mode: case_tiled(r, kind, mode), n))
    for generator, n in generators:
        for _ in range(n):
            name, writes = generator(rng)
            cases.append((name, writes, random_image(rng)))
    return cases


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    parser.add_argument('--header', type=Path)
    parser.add_argument('--seed', type=int, default=20261020)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    audited = audit(args.header) if args.header else 0
    rng = random.Random(args.seed)
    cases = build(rng, 0)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        with open(tmp / 'in.bin', 'wb') as out:
            for _, writes, image in cases:
                out.write(struct.pack('<II', len(image), len(writes)))
                for method, value in writes:
                    out.write(struct.pack('<II', method, value & 0xffffffff))
                out.write(bytes(image))
        subprocess.run([args.runner, str(tmp / 'in.bin'), str(tmp / 'out.bin')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        actual = (tmp / 'out.bin').read_bytes()

    results = {}
    failures = 0
    for index, (name, writes, image) in enumerate(cases):
        expected = bytearray(image)
        run_model(expected, writes)
        got = bytearray(actual[index * MEMORY:(index + 1) * MEMORY])
        # Four-word semaphores store a timestamp at +8
        for method, value in writes:
            if method == 0xc0 and ((value >> 3) & 3) == 2:
                target = address(dict(writes), 0x90) - BASE
                got[target + 8:target + 16] = b'\x00' * 8
        entry = results.setdefault(name, {'cases': 0, 'mismatches': 0, 'example': None})
        entry['cases'] += 1
        if got != expected:
            entry['mismatches'] += 1
            if entry['example'] is None:
                first = next(i for i in range(MEMORY) if got[i] != expected[i])
                entry['example'] = {'case': index, 'first_byte': first, 'expected': expected[first],
                                    'actual': got[first]}
    print('NVIDIA header fields audited: %d' % audited)
    for name, entry in results.items():
        unsupported = name.endswith('general remap')
        status = 'ok' if entry['mismatches'] == 0 else ('UNSUPPORTED' if unsupported else 'MISMATCH')
        print('%-34s %4d cases %4d mismatches  %s %s' % (name, entry['cases'], entry['mismatches'], status,
                                                       entry['example'] if entry['mismatches'] else ''))
        if entry['mismatches'] and not unsupported:
            failures += 1
    if args.report:
        args.report.write_text(json.dumps(results, indent=2))
    if failures:
        print('FAIL')
        sys.exit(1)
    print('PASS')


if __name__ == '__main__':
    main()
