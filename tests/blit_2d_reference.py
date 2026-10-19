#!/usr/bin/env python3
"""2D engine (class 902D) PIXELS_FROM_MEMORY blits: Python model vs Hydra.

Each case is a random memory image plus a register script. The model applies
the documented fields of cl902d.h with plain loops (point sampling at pixel
centres, clip rectangle, destination clamping, pattern independent ROPs) and
the result is compared byte for byte with the guest memory Hydra leaves after
its engine ran and the texture cache flushed the GPU result back. Registers are
audited against the NVIDIA header first. Both of Hydra's paths are exercised:
the Metal blit (SRCCOPY, one slice) and the CPU path (ROP, volumes).

The header names the fields but has no prose about sampling. Convention used
here, not NVIDIA text: destination pixel i takes the source texel
floor(SRC_X0 + (i + 0.5) * DU_DX) with ORIGIN_CENTER and point filtering, edges
clamp (CPU path only; the cases for the Metal path stay inside the source).
ORIGIN_CORNER and bilinear filtering are not asserted.

Usage: blit_2d_reference.py <hydra-blit-2d-reference-tests> [--header cl902d.h]
           [--seed N] [--report out.json]
"""
import argparse
import json
import math
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

BASE = 0x200000
MEMORY = 0x80000
SRC_AT, DST_AT = 0x0, 0x30000
FORMATS = {'RGBA8': (0xD5, 4), 'R8': (0xF3, 1), 'RGBA16F': (0xCA, 8)}

# Header name -> method number (byte offset / 4)
REGISTERS = {
    'SET_DST_FORMAT': 0x80, 'SET_DST_MEMORY_LAYOUT': 0x81, 'SET_DST_BLOCK_SIZE': 0x82,
    'SET_DST_DEPTH': 0x83, 'SET_DST_LAYER': 0x84, 'SET_DST_PITCH': 0x85, 'SET_DST_WIDTH': 0x86,
    'SET_DST_HEIGHT': 0x87, 'SET_DST_OFFSET_UPPER': 0x88, 'SET_DST_OFFSET_LOWER': 0x89,
    'SET_SRC_FORMAT': 0x8c, 'SET_SRC_MEMORY_LAYOUT': 0x8d, 'SET_SRC_BLOCK_SIZE': 0x8e,
    'SET_SRC_DEPTH': 0x8f, 'SET_SRC_PITCH': 0x91, 'SET_SRC_WIDTH': 0x92,
    'SET_SRC_HEIGHT': 0x93, 'SET_SRC_OFFSET_UPPER': 0x94, 'SET_SRC_OFFSET_LOWER': 0x95,
    'SET_CLIP_X0': 0xa0, 'SET_CLIP_Y0': 0xa1, 'SET_CLIP_WIDTH': 0xa2, 'SET_CLIP_HEIGHT': 0xa3,
    'SET_CLIP_ENABLE': 0xa4, 'SET_COLOR_KEY_ENABLE': 0xa7, 'SET_ROP': 0xa8, 'SET_OPERATION': 0xab,
    'SET_PIXELS_FROM_MEMORY_SAMPLE_MODE': 0x223, 'SET_PIXELS_FROM_MEMORY_DST_X0': 0x22c,
    'SET_PIXELS_FROM_MEMORY_DST_Y0': 0x22d, 'SET_PIXELS_FROM_MEMORY_DST_WIDTH': 0x22e,
    'SET_PIXELS_FROM_MEMORY_DST_HEIGHT': 0x22f, 'SET_PIXELS_FROM_MEMORY_DU_DX_FRAC': 0x230,
    'SET_PIXELS_FROM_MEMORY_DU_DX_INT': 0x231, 'SET_PIXELS_FROM_MEMORY_DV_DY_FRAC': 0x232,
    'SET_PIXELS_FROM_MEMORY_DV_DY_INT': 0x233, 'SET_PIXELS_FROM_MEMORY_SRC_X0_FRAC': 0x234,
    'SET_PIXELS_FROM_MEMORY_SRC_X0_INT': 0x235, 'SET_PIXELS_FROM_MEMORY_SRC_Y0_FRAC': 0x236,
    'PIXELS_FROM_MEMORY_SRC_Y0_INT': 0x237,
}
PITCH_LAYOUT, BLOCKLINEAR_LAYOUT = 1, 0


def audit(header):
    text = header.read_text()
    for name, method in REGISTERS.items():
        # Header spells the destination/source pitch and offset registers differently
        header_name = name.replace('_PITCH', '_PITCH').replace('SET_DST_OFFSET', 'SET_DST_OFFSET')
        match = re.search(r'^#define\s+NV902D_%s\s+(0x[0-9A-Fa-f]+)' % re.escape(header_name), text, re.M)
        if not match or int(match[1], 16) // 4 != method:
            raise SystemExit('NVIDIA header mismatch for %s (%s)' % (name, match and match[1]))
    for name, value in (('SET_OPERATION_V_SRCCOPY', 3), ('SET_OPERATION_V_ROP', 4),
                        ('SET_DST_MEMORY_LAYOUT_V_PITCH', 1), ('SET_DST_MEMORY_LAYOUT_V_BLOCKLINEAR', 0),
                        ('SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_ORIGIN_CENTER', 0),
                        ('SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_FILTER_POINT', 0)):
        match = re.search(r'^#define\s+NV902D_%s\s+(0x[0-9A-Fa-f]+)' % name, text, re.M)
        if not match or int(match[1], 16) != value:
            raise SystemExit('NVIDIA header mismatch for ' + name)
    for name, bits in (('SET_DST_BLOCK_SIZE_HEIGHT', (4, 6)), ('SET_DST_BLOCK_SIZE_DEPTH', (8, 10)),
                       ('SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_ORIGIN', (0, 0)),
                       ('SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_FILTER', (4, 4))):
        match = re.search(r'^#define\s+NV902D_%s\s+(\d+):(\d+)' % name, text, re.M)
        if not match or (int(match[2]), int(match[1])) != bits:
            raise SystemExit('NVIDIA header mismatch for field ' + name)
    return len(REGISTERS) + 10


# ---------------------------------------------------------------- surfaces

def gob_inner(x, y):
    return (((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 +
            (y % 2) * 16 + (x % 16))


class Surface2D:
    def __init__(self, fmt, layout, width, height, depth=1, layer=0, bh=0, bd=0, base=0):
        self.fmt, (self.fmt_id, self.bpp) = fmt, FORMATS[fmt]
        self.layout, self.width, self.height = layout, width, height
        self.depth, self.layer, self.bh, self.bd, self.base = depth, layer, bh, bd, base
        self.stride = width * self.bpp + (8 if layout == PITCH_LAYOUT else 0)

    def offset(self, x, y, z=0):
        if self.layout == PITCH_LAYOUT:
            return self.base + (z * self.height + y) * self.stride + x * self.bpp
        xb = x * self.bpp
        gobs_x = (self.width * self.bpp + 63) // 64
        bh, bd = 1 << self.bh, 1 << self.bd
        blocks_y = (self.height + 8 * bh - 1) // (8 * bh)
        block = ((z // bd) * blocks_y + (y // 8) // bh) * gobs_x + xb // 64
        gob = (z % bd) * bh + (y // 8) % bh
        return self.base + block * 512 * bh * bd + gob * 512 + gob_inner(xb, y)

    def size(self):
        if self.layout == PITCH_LAYOUT:
            return self.stride * self.height * self.depth
        gobs_x = (self.width * self.bpp + 63) // 64
        bh, bd = 1 << self.bh, 1 << self.bd
        return (((self.height + 8 * bh - 1) // (8 * bh)) * gobs_x * 512 * bh * bd *
                ((self.depth + bd - 1) // bd))

    def regs(self, first, has_layer=True):
        addr = BASE + self.base
        out = [(first, self.fmt_id), (first + 1, self.layout),
               (first + 2, (self.bd << 8) | (self.bh << 4)), (first + 3, self.depth)]
        if has_layer:  # the class has a destination layer only
            out.append((first + 4, self.layer))
        return out + [(first + 5, self.stride), (first + 6, self.width), (first + 7, self.height),
                      (first + 8, addr >> 32), (first + 9, addr & 0xffffffff)]

    def read(self, mem, x, y, z=0):
        o = self.offset(x, y, z)
        return bytes(mem[o:o + self.bpp])

    def write(self, mem, x, y, z, data):
        o = self.offset(x, y, z)
        mem[o:o + self.bpp] = data


def fixed(value):
    raw = int(round(value * (1 << 32))) & ((1 << 64) - 1)
    return raw & 0xffffffff, raw >> 32


def blit_writes(dst_x0, dst_y0, w, h, dudx, dvdy, src_x0, src_y0, mode=0):
    du, dv, sx, sy = fixed(dudx), fixed(dvdy), fixed(src_x0), fixed(src_y0)
    return [(0x223, mode), (0x22c, dst_x0), (0x22d, dst_y0), (0x22e, w), (0x22f, h),
            (0x230, du[0]), (0x231, du[1]), (0x232, dv[0]), (0x233, dv[1]),
            (0x234, sx[0]), (0x235, sx[1]), (0x236, sy[0]), (0x237, sy[1])]  # 0x237 launches


# ------------------------------------------------------------------- model

def binary_rop(rop, s, d):
    out = 0
    for bit in range(8):
        index = (((s >> bit) & 1) << 1) | ((d >> bit) & 1)
        out |= ((rop >> index) & 1) << bit
    return out


def run_model(mem, writes, src, dst):
    regs = {}
    for method, value in writes:
        regs[method] = value & 0xffffffff
        if method == 0x237:
            blit(mem, regs, src, dst)


def signed_fixed(frac, integer):
    raw = (integer << 32) | frac
    if raw >> 63:
        raw -= 1 << 64
    return raw / (1 << 32)


def blit(mem, regs, src, dst):
    dst_x0, dst_y0 = regs.get(0x22c, 0), regs.get(0x22d, 0)
    x1 = min(dst_x0 + regs.get(0x22e, 0), dst.width)
    y1 = min(dst_y0 + regs.get(0x22f, 0), dst.height)
    x0, y0 = dst_x0, dst_y0
    if regs.get(0xa4, 0) & 1:
        x0, y0 = max(x0, regs.get(0xa0, 0)), max(y0, regs.get(0xa1, 0))
        x1 = min(x1, regs.get(0xa0, 0) + regs.get(0xa2, 0))
        y1 = min(y1, regs.get(0xa1, 0) + regs.get(0xa3, 0))
    if x0 >= x1 or y0 >= y1:
        return
    dudx = signed_fixed(regs.get(0x230, 0), regs.get(0x231, 0))
    dvdy = signed_fixed(regs.get(0x232, 0), regs.get(0x233, 0))
    src_x0 = signed_fixed(regs.get(0x234, 0), regs.get(0x235, 0))
    src_y0 = signed_fixed(regs.get(0x236, 0), regs.get(0x237, 0))
    operation = regs.get(0xab, 0) & 7
    rop = regs.get(0xa8, 0)
    # Read every source texel first: a blit is atomic with respect to overlap
    pixels = []
    for y in range(y0, y1):
        sy = math.floor(src_y0 + (y - dst_y0 + 0.5) * dvdy)
        for x in range(x0, x1):
            sx = math.floor(src_x0 + (x - dst_x0 + 0.5) * dudx)
            sx, sy_c = min(max(sx, 0), src.width - 1), min(max(sy, 0), src.height - 1)
            pixels.append((x, y, src.read(mem, sx, sy_c)))
    for x, y, data in pixels:
        if operation == 4:
            old = dst.read(mem, x, y, dst.layer)
            data = bytes(binary_rop(rop & 0xff, s, d) for s, d in zip(data, old))
        dst.write(mem, x, y, dst.layer, data)


# ------------------------------------------------------------------- cases

def random_image(rng, formats=None):
    image = bytearray(rng.randrange(256) for _ in range(MEMORY))
    return image


def finite_halves(rng, image, surface, count_bytes):
    """Keep RGBA16F texels finite so the GPU cannot canonicalise NaNs."""
    for offset in range(surface.base, surface.base + count_bytes, 2):
        if (image[offset + 1] & 0x7c) == 0x7c:
            image[offset + 1] &= 0x3f


def pick_scale(rng):
    return rng.choice([1.0, 1.0, 1.0, 2.0, 0.5, -1.0, -2.0, 0.25, 1.5, 0.75])


def make_surface(rng, fmt, layout, width, height, base, depth=1, layer=0):
    # A block is never taller than the image needs: Hydra reduces larger block
    # heights (as for mip levels) and NVN does not emit them
    needed = (height + 7) // 8
    limit = (needed - 1).bit_length() if layout == BLOCKLINEAR_LAYOUT else 0
    bh = rng.randrange(0, min(limit, 2) + 1) if layout == BLOCKLINEAR_LAYOUT else 0
    bd = rng.randrange(0, 2) if layout == BLOCKLINEAR_LAYOUT and depth > 1 else 0
    return Surface2D(fmt, layout, width, height, depth, layer, bh, bd, base)


def case_gpu(rng):
    fmt = rng.choice(list(FORMATS))
    sl, dl = rng.choice([PITCH_LAYOUT, BLOCKLINEAR_LAYOUT]), rng.choice([PITCH_LAYOUT, BLOCKLINEAR_LAYOUT])
    dst_w, dst_h = rng.randrange(8, 48), rng.randrange(8, 40)
    dx, dy = pick_scale(rng), pick_scale(rng)
    w, h = rng.randrange(1, dst_w + 1), rng.randrange(1, dst_h + 1)
    dst_x0, dst_y0 = rng.randrange(0, dst_w - w + 1), rng.randrange(0, dst_h - h + 1)
    # Source rectangle spanned by the blit, then the surface that contains it
    span_x, span_y = abs(dx) * w, abs(dy) * h
    src_w, src_h = int(math.ceil(span_x)) + rng.randrange(1, 12), int(math.ceil(span_y)) + rng.randrange(1, 12)
    left = rng.randrange(0, src_w - int(math.ceil(span_x)) + 1)
    top = rng.randrange(0, src_h - int(math.ceil(span_y)) + 1)
    # With a negative step the first sample is at the far edge
    src_x0 = left + (span_x if dx < 0 else 0)
    src_y0 = top + (span_y if dy < 0 else 0)
    src = make_surface(rng, fmt, sl, src_w, src_h, SRC_AT)
    dst = make_surface(rng, fmt, dl, dst_w, dst_h, DST_AT)
    writes = []
    clip = rng.random() < 0.3
    if clip:
        cx, cy = rng.randrange(0, dst_w), rng.randrange(0, dst_h)
        writes += [(0xa0, cx), (0xa1, cy), (0xa2, rng.randrange(1, dst_w)), (0xa3, rng.randrange(1, dst_h)),
                   (0xa4, 1)]
    writes = dst.regs(0x80) + src.regs(0x8c, False) + [(0xab, 3)] + writes
    writes += blit_writes(dst_x0, dst_y0, w, h, dx, dy, src_x0, src_y0)
    return 'GPU blit %s %s->%s' % (fmt, 'pitch' if sl else 'tiled', 'pitch' if dl else 'tiled'), writes, src, dst


def case_raw(rng):
    fmt = rng.choice(list(FORMATS))
    sl, dl = rng.choice([PITCH_LAYOUT, BLOCKLINEAR_LAYOUT]), rng.choice([PITCH_LAYOUT, BLOCKLINEAR_LAYOUT])
    dst_w, dst_h, depth = rng.randrange(8, 32), rng.randrange(8, 24), rng.choice([1, 2, 3])
    layer = rng.randrange(depth)
    use_rop = rng.random() < 0.6
    src_w, src_h = rng.randrange(2, 30), rng.randrange(2, 24)
    src = make_surface(rng, fmt, sl, src_w, src_h, SRC_AT)
    dst = make_surface(rng, fmt, dl, dst_w, dst_h, DST_AT, depth, layer)
    if dst.size() > MEMORY - DST_AT:
        dst = make_surface(rng, fmt, PITCH_LAYOUT, dst_w, dst_h, DST_AT, depth, layer)
    dx, dy = pick_scale(rng), pick_scale(rng)
    w, h = rng.randrange(1, dst_w + 1), rng.randrange(1, dst_h + 1)
    dst_x0, dst_y0 = rng.randrange(0, dst_w - w + 1), rng.randrange(0, dst_h - h + 1)
    src_x0, src_y0 = rng.randrange(-4, src_w), rng.randrange(-4, src_h)  # edges clamp on the CPU path
    writes = dst.regs(0x80) + src.regs(0x8c, False)
    if use_rop:
        truth = rng.randrange(16)
        writes += [(0xab, 4), (0xa8, truth | (truth << 4))]
    else:
        writes += [(0xab, 3)]
    writes += blit_writes(dst_x0, dst_y0, w, h, dx, dy, src_x0, src_y0)
    name = 'CPU ROP blit' if use_rop else 'CPU volume blit'
    return '%s %s' % (name, fmt), writes, src, dst


def build(rng):
    cases = []
    for generator, n in ((case_gpu, 300), (case_raw, 200)):
        for _ in range(n):
            name, writes, src, dst = generator(rng)
            image = random_image(rng)
            if 'RGBA16F' in name:
                finite_halves(rng, image, src, MEMORY - src.base)
                finite_halves(rng, image, dst, MEMORY - dst.base)
            cases.append((name, writes, src, dst, image))
    return cases


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    parser.add_argument('--header', type=Path)
    parser.add_argument('--seed', type=int, default=20261020)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    audited = audit(args.header) if args.header else 0
    cases = build(random.Random(args.seed))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        with open(tmp / 'in.bin', 'wb') as out:
            for _, writes, _, _, image in cases:
                out.write(struct.pack('<II', len(image), len(writes)))
                for method, value in writes:
                    out.write(struct.pack('<II', method, value & 0xffffffff))
                out.write(bytes(image))
        subprocess.run([args.runner, str(tmp / 'in.bin'), str(tmp / 'out.bin')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        actual = (tmp / 'out.bin').read_bytes()

    results, failures = {}, 0
    for index, (name, writes, src, dst, image) in enumerate(cases):
        expected = bytearray(image)
        run_model(expected, writes, src, dst)
        got = actual[index * MEMORY:(index + 1) * MEMORY]
        entry = results.setdefault(name, {'cases': 0, 'mismatches': 0, 'example': None})
        entry['cases'] += 1
        if got != bytes(expected):
            entry['mismatches'] += 1
            if entry['example'] is None:
                first = next(i for i in range(MEMORY) if got[i] != expected[i])
                entry['example'] = {'case': index, 'byte': first, 'expected': expected[first], 'actual': got[first]}
    print('NVIDIA header fields audited: %d' % audited)
    for name, entry in sorted(results.items()):
        status = 'ok' if entry['mismatches'] == 0 else 'MISMATCH'
        print('%-34s %4d cases %4d mismatches  %s %s' % (name, entry['cases'], entry['mismatches'], status,
                                                       entry['example'] if entry['mismatches'] else ''))
        failures += entry['mismatches'] != 0
    if args.report:
        args.report.write_text(json.dumps(results, indent=2))
    if failures:
        print('FAIL')
        sys.exit(1)
    print('PASS')


if __name__ == '__main__':
    main()
