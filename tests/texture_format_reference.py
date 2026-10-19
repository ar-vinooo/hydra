#!/usr/bin/env python3
"""Guest TIC colour formats sampled through Hydra + Metal vs a Python decoder.

Expected texel values come from the NVIDIA format names in clb197tex.h
(NVB197_TEXHEAD_BL_COMPONENTS_SIZES_*): names with packed fields list the
components from the most significant bits down, so in little-endian memory
A8B8G8R8 is bytes R, G, B, A; names joined by underscores (R32_G32, R16_G16)
list the components in memory order. Decoding uses exact Python arithmetic.
tests/texture_format_reference_tests samples the raw guest bytes through
Hydra's texture cache, format table and swizzled Metal view. This is a model
from NVIDIA's names vs Apple's GPU, not execution on an NVIDIA GPU, and the
header does not say which component type combinations are legal.

Combinations Hydra does not map to a host format are reported as gaps; they are
not value mismatches. Compressed, depth/stencil and video formats are not
covered here.

Usage: texture_format_reference.py <hydra-texture-format-reference-tests>
           [--header clb197tex.h] [--report out.json] [--seed N]
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

UNORM, SNORM, SINT, UINT, FLOAT = 2, 1, 3, 4, 7
COMPONENT_NAMES = {UNORM: 'Unorm', SNORM: 'Snorm', SINT: 'Sint', UINT: 'Uint', FLOAT: 'Float'}
ZERO, SR, SG, SB, SA, ONE_INT, ONE_FLOAT = 0, 2, 3, 4, 5, 6, 7
R, G, B, A = 0, 1, 2, 3

# name, image format id, header name, bytes per texel, fields (channel, shift, bits)
FORMATS = [
    ('R8', 0x1D, 'R8', 1, [(R, 0, 8)]),
    ('R16', 0x1B, 'R16', 2, [(R, 0, 16)]),
    ('R32', 0x0F, 'R32', 4, [(R, 0, 32)]),
    ('GR8', 0x18, 'G8R8', 2, [(R, 0, 8), (G, 8, 8)]),
    ('RG16', 0x0C, 'R16_G16', 4, [(R, 0, 16), (G, 16, 16)]),
    ('R32G32', 0x04, 'R32_G32', 8, [(R, 0, 32), (G, 32, 32)]),
    ('RGB32', 0x02, 'R32_G32_B32', 12, [(R, 0, 32), (G, 32, 32), (B, 64, 32)]),
    ('RGBA16', 0x03, 'R16_G16_B16_A16', 8,
     [(R, 0, 16), (G, 16, 16), (B, 32, 16), (A, 48, 16)]),
    ('RGBA32', 0x01, 'R32_G32_B32_A32', 16,
     [(R, 0, 32), (G, 32, 32), (B, 64, 32), (A, 96, 32)]),
    ('ARGB8', 0x08, 'A8B8G8R8', 4, [(R, 0, 8), (G, 8, 8), (B, 16, 8), (A, 24, 8)]),
    ('XBGR8', 0x07, 'X8B8G8R8', 4, [(R, 0, 8), (G, 8, 8), (B, 16, 8)]),
    ('A2BGR10', 0x09, 'A2B10G10R10', 4, [(R, 0, 10), (G, 10, 10), (B, 20, 10), (A, 30, 2)]),
    ('B5G6R5', 0x15, 'B5G6R5', 2, [(R, 0, 5), (G, 5, 6), (B, 11, 5)]),
    ('ABGR4', 0x12, 'A4B4G4R4', 2, [(R, 0, 4), (G, 4, 4), (B, 8, 4), (A, 12, 4)]),
    ('A1BGR5', 0x14, 'A1B5G5R5', 2, [(R, 0, 5), (G, 5, 5), (B, 10, 5), (A, 15, 1)]),
    ('ABG5R1', 0x13, 'A5B5G5R1', 2, [(R, 0, 1), (G, 1, 5), (B, 6, 5), (A, 11, 5)]),
    ('GR4', 0x1E, 'G4R4', 1, [(R, 0, 4), (G, 4, 4)]),
]
SPECIAL = [  # fixed float formats: name, id, header name, bytes
    ('B10GR11Float', 0x21, 'BF10GF11RF11', 4),
    ('E5BGR9', 0x20, 'E5B9G9R9_SHAREDEXP', 4),
]


COMPRESSED = [  # name, id, header name, bytes per 4x4 block, kind
    ('DXT1', 0x24, 'DXT1', 8, 'bc1'),
    ('DXT23', 0x25, 'DXT23', 16, 'bc2'),
    ('DXT45', 0x26, 'DXT45', 16, 'bc3'),
    ('DXN1', 0x27, 'DXN1', 8, 'bc4'),
    ('DXN2', 0x28, 'DXN2', 16, 'bc5'),
    ('BC7U', 0x17, 'BC7U', 16, 'bc7'),
    ('BC6H_SF16', 0x10, 'BC6H_SF16', 16, 'bc6s'),
    ('BC6H_UF16', 0x11, 'BC6H_UF16', 16, 'bc6u'),
]
COMPRESSED_COMBOS = {  # name: (component types, sRGB variant)
    'DXT1': ([UNORM], True), 'DXT23': ([UNORM], True), 'DXT45': ([UNORM], True),
    'DXN1': ([UNORM, SNORM], False), 'DXN2': ([UNORM, SNORM], False),
    'BC7U': ([UNORM], True), 'BC6H_SF16': ([FLOAT], False), 'BC6H_UF16': ([FLOAT], False),
}
ORACLE_KINDS = ('bc7', 'bc6u', 'bc6s')
oracle_results = {}  # (kind, block hex) -> 16 x (r, g, b, a)


def is_compressed(fmt):
    return len(fmt) > 4 and isinstance(fmt[4], str)


def rgb565(value):
    r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
    return [(r << 3 | r >> 2) / 255, (g << 2 | g >> 4) / 255, (b << 3 | b >> 2) / 255]


def bc1_colors(block, four_color_always):
    c0, c1 = struct.unpack('<HH', block[:4])
    a, b = rgb565(c0), rgb565(c1)
    if c0 > c1 or four_color_always:
        palette = [a + [1.0], b + [1.0],
                   [(2 * x + y) / 3 for x, y in zip(a, b)] + [1.0],
                   [(x + 2 * y) / 3 for x, y in zip(a, b)] + [1.0]]
    else:
        palette = [a + [1.0], b + [1.0], [(x + y) / 2 for x, y in zip(a, b)] + [1.0], [0.0, 0.0, 0.0, 0.0]]
    indices = struct.unpack('<I', block[4:8])[0]
    return [palette[(indices >> (2 * i)) & 3] for i in range(16)]


def bc4_values(block, signed):
    """Sixteen single-channel values of a BC4 block, as floats."""
    if signed:
        a0, a1 = struct.unpack('<bb', block[:2])
        lo, hi = -127.0, 127.0
        a0f, a1f = float(max(a0, -127)), float(max(a1, -127))
        scale = 127.0
    else:
        a0, a1 = block[0], block[1]
        a0f, a1f = float(a0), float(a1)
        lo, hi, scale = 0.0, 255.0, 255.0
    if a0 > a1:
        palette = [a0f, a1f] + [((8 - i) * a0f + (i - 1) * a1f) / 7 for i in range(2, 8)]
    else:
        palette = [a0f, a1f] + [((6 - i) * a0f + (i - 1) * a1f) / 5 for i in range(2, 6)] + [lo, hi]
    bits = int.from_bytes(block[2:8], 'little')
    return [max(palette[(bits >> (3 * i)) & 7] / scale, -1.0) for i in range(16)]


def decode_block(fmt, data, ctype):
    kind = fmt[4]
    signed = ctype == SNORM
    if kind in ORACLE_KINDS:
        return oracle_results[(kind, data.hex())]
    if kind == 'bc1':
        return bc1_colors(data, False)
    if kind == 'bc2':
        alpha = int.from_bytes(data[:8], 'little')
        colors = bc1_colors(data[8:], True)
        return [c[:3] + [((alpha >> (4 * i)) & 15) / 15] for i, c in enumerate(colors)]
    if kind == 'bc3':
        alpha = bc4_values(data[:8], False)
        colors = bc1_colors(data[8:], True)
        return [c[:3] + [alpha[i]] for i, c in enumerate(colors)]
    if kind == 'bc4':
        return [[v, 0.0, 0.0, 1.0] for v in bc4_values(data, signed)]
    red, green = bc4_values(data[:8], signed), bc4_values(data[8:], signed)
    return [[red[i], green[i], 0.0, 1.0] for i in range(16)]


def component_types(name, fields):
    sizes = [bits for _, _, bits in fields]
    types = [UNORM, UINT]
    if min(sizes) >= 4:
        types += [SNORM, SINT]
    if all(bits in (16, 32) for bits in sizes):
        types += [FLOAT]
    if all(bits == 32 for bits in sizes):
        types = [UINT, SINT, FLOAT]  # no 32-bit normalized host formats
    return types


def audit(header):
    text = header.read_text()
    count = 0
    for name, ident, header_name, *_ in FORMATS + COMPRESSED + [(n, i, h, b) for n, i, h, b in SPECIAL]:
        match = re.search(r'^#define\s+NVB197_TEXHEAD_BL_COMPONENTS_SIZES_%s\s+(0x[0-9A-Fa-f]+)' %
                          re.escape(header_name), text, re.M)
        if not match or int(match[1], 16) != ident:
            raise SystemExit('NVIDIA header mismatch for ' + header_name)
        count += 1
    return count


def f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def bits_f32(word):
    return struct.unpack('<f', struct.pack('<I', word & 0xffffffff))[0]


def half(value):
    return struct.unpack('<e', struct.pack('<H', value))[0]


def small_float(value, exp_bits, man_bits):
    """Unsigned float with a 5-bit exponent (bias 15) as in R11G11B10."""
    exponent = value >> man_bits
    mantissa = value & ((1 << man_bits) - 1)
    if exponent == 0:
        return mantissa / (1 << man_bits) * 2.0 ** -14
    if exponent == 31:
        return math.inf if mantissa == 0 else math.nan
    return (1 + mantissa / (1 << man_bits)) * 2.0 ** (exponent - 15)


def srgb_to_linear(value):
    if value <= 0.04045:
        return value / 12.92
    return ((value + 0.055) / 1.055) ** 2.4


def field_value(word, shift, bits):
    return (word >> shift) & ((1 << bits) - 1)


def decode_field(raw, bits, ctype):
    if ctype == UNORM:
        return raw / ((1 << bits) - 1)
    if ctype == SNORM:
        signed = raw - (1 << bits) if raw >> (bits - 1) else raw
        return max(signed / ((1 << (bits - 1)) - 1), -1.0)
    if ctype in (SINT,):
        return raw - (1 << bits) if raw >> (bits - 1) else raw
    if ctype == UINT:
        return raw
    if ctype == FLOAT:
        if bits == 16:
            return half(raw)
        return bits_f32(raw)
    raise ValueError(ctype)


def decode_texel(fmt, data, ctype):
    """Return the four guest components (R, G, B, A) of one texel."""
    name, _, _, size, *rest = fmt
    word = int.from_bytes(data[:size], 'little')
    if name == 'B10GR11Float':
        return [small_float(field_value(word, 0, 11), 5, 6), small_float(field_value(word, 11, 11), 5, 6),
                small_float(field_value(word, 22, 10), 5, 5), 1.0]
    if name == 'E5BGR9':
        exponent = field_value(word, 27, 5)
        scale = 2.0 ** (exponent - 15) / 512
        return [field_value(word, 0, 9) * scale, field_value(word, 9, 9) * scale,
                field_value(word, 18, 9) * scale, 1.0]
    is_int = ctype in (SINT, UINT)
    components = [0, 0, 0, 1] if is_int else [0.0, 0.0, 0.0, 1.0]
    for channel, shift, bits in rest[0]:
        components[channel] = decode_field(field_value(word, shift, bits), bits, ctype)
    return components


def present_channels(fmt):
    if is_compressed(fmt):
        return {'bc4': {R}, 'bc5': {R, G}}.get(fmt[4], {R, G, B, A})
    if fmt[0] in ('B10GR11Float', 'E5BGR9'):
        return {R, G, B}
    return {channel for channel, _, _ in fmt[4]}


def select(components, source, is_int):
    if source == ZERO:
        return 0
    if source in (SR, SG, SB, SA):
        return components[source - SR]
    if source == ONE_INT:
        return 1
    return 1.0 if not is_int else 1


SWIZZLES = [
    (SR, SG, SB, SA),
    (SA, SB, SG, SR),
    (SR, SR, SR, 'one'),
    (ZERO, SB, 'one', SG),
]


def make_swizzle(swizzle, is_int):
    return tuple((ONE_INT if is_int else ONE_FLOAT) if s == 'one' else s for s in swizzle)


def random_texels(rng, size, width, compressed=False, kind=None):
    if compressed:
        # BC7 blocks starting with a zero byte use a reserved mode: D3D defines
        # them as all zero, Apple GPUs return alpha 1, so they are not compared
        blocks = [bytes([1]) + bytes(size - 1) if kind == 'bc7' else bytes(size),
                  bytes([0xff]) * size, bytes([0x80]) * size]
        blocks += [bytes(rng.randrange(256) for _ in range(size)) for _ in range(40)]
        if kind in ('bc6u', 'bc6s'):
            # Reserved modes decode to undefined values; use the 14 defined ones
            modes = [0, 1, 0x02, 0x06, 0x0a, 0x0e, 0x12, 0x16, 0x1a, 0x1e]
            fixed = []
            for block in blocks[3:]:
                mode = rng.choice(modes)
                keep = 0xfc if mode < 2 else 0xe0
                fixed.append(bytes([(block[0] & keep) | mode]) + block[1:])
            blocks = blocks[:3] + fixed
        return blocks
    patterns = [bytes(size * width), bytes([0xff]) * (size * width), bytes([0x80]) * (size * width),
                bytes([0x7f]) * (size * width)]
    patterns += [bytes(rng.randrange(256) for _ in range(size * width)) for _ in range(6)]
    return patterns


def words_equal(kind, actual_word, expected, srgb_channel=False, extra=0.0, rel=0.0):
    if kind == 'F':
        actual = bits_f32(actual_word)
        if math.isnan(expected):
            return math.isnan(actual)
        if math.isinf(expected):
            return actual == expected
        tolerance = max(2e-3 if srgb_channel else 1e-4, extra)
        tolerance = max(tolerance, abs(expected) * max(rel, 1e-6 if abs(expected) > 1.0 else 0.0))
        return abs(actual - expected) <= tolerance
    if kind == 'I':
        value = actual_word - (1 << 32) if actual_word >> 31 else actual_word
        return value == expected
    return actual_word == expected & 0xffffffff


def run_oracle(bcdec, tmp, blocks):
    if not bcdec.exists():
        bcdec.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['curl', '-fsSL', '-o', str(bcdec),
                        'https://raw.githubusercontent.com/iOrange/bcdec/main/bcdec.h'], check=True)
    binary = tmp / 'bcdec_oracle'
    subprocess.run(['cc', '-O1', '-I' + str(bcdec.parent), '-o', str(binary),
                    str(Path(__file__).with_name('bcdec_oracle.c'))], check=True)
    text = ''.join('%s %s\n' % block for block in blocks)
    output = subprocess.run([str(binary)], input=text, capture_output=True, text=True, check=True).stdout
    results = {}
    for block, line in zip(blocks, output.strip().split('\n')):
        words = [bits_f32(int(w, 16)) for w in line.split()]
        results[block] = [words[i * 4:i * 4 + 4] for i in range(16)]
    return results


def gob_inner(x_byte, y):
    """Byte offset inside a 64x8 byte, 512 byte GOB (Tegra X1 block linear)."""
    return (((x_byte % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x_byte % 32) // 16) * 32 +
            (y % 2) * 16 + (x_byte % 16))


def block_linear_swizzle(linear, width_e, height_e, element_bytes, block_height_log2):
    """Lay row-major elements out in GOBs grouped into blocks 2**log2 GOBs tall."""
    row_bytes = width_e * element_bytes
    gobs_x = (row_bytes + 63) // 64
    block_h = 1 << block_height_log2
    block_rows = (height_e + 8 * block_h - 1) // (8 * block_h)
    out = bytearray(block_rows * gobs_x * block_h * 512)
    for y in range(height_e):
        gob_y = y // 8
        block_y, gob_in_block = gob_y // block_h, gob_y % block_h
        for x in range(width_e):
            for byte in range(element_bytes):
                x_byte = x * element_bytes + byte
                gob = (block_y * gobs_x + x_byte // 64) * block_h + gob_in_block
                out[gob * 512 + gob_inner(x_byte, y)] = linear[(y * width_e + x) * element_bytes + byte]
    return bytes(out)


BLOCK_LINEAR_FORMATS = [  # format name, component type
    ('R8', UINT), ('GR8', UINT), ('ARGB8', UINT), ('RGBA16', UINT), ('RGBA32', UINT),
    ('DXT1', UNORM), ('DXT45', UNORM),
]
BLOCK_LINEAR_SIZES = [(8, 8), (16, 16), (64, 8), (100, 37), (33, 129), (200, 24), (7, 3), (128, 64)]


def run_block_linear(runner, rng):
    by_name = {f[0]: f for f in FORMATS + COMPRESSED}
    cases, lines = [], []
    for name, ctype in BLOCK_LINEAR_FORMATS:
        fmt = by_name[name]
        compressed = is_compressed(fmt)
        element_bytes = fmt[3]
        for width, height in BLOCK_LINEAR_SIZES:
            width_e = (width + 3) // 4 if compressed else width
            height_e = (height + 3) // 4 if compressed else height
            # A block is never taller than the image needs: Hydra reduces larger
            # block heights (as for mip levels) and NVN does not emit them
            needed_gobs = (height_e + 7) // 8
            for block_height_log2 in range(5):
                if (1 << block_height_log2) > 1 << (needed_gobs - 1).bit_length():
                    continue
                linear = bytes(rng.randrange(256) for _ in range(width_e * height_e * element_bytes))
                guest = block_linear_swizzle(linear, width_e, height_e, element_bytes, block_height_log2)
                cases.append((fmt, ctype, width, height, width_e, linear, block_height_log2))
                lines.append('S %x %x %x %x %x 0 2 3 4 5 %x %x 1 0 %x 1 1 %s' % (
                    fmt[1], ctype, ctype, ctype, ctype, width, height, block_height_log2, guest.hex()))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'bl.in').write_text('\n'.join(lines) + '\n')
        subprocess.run([runner, str(tmp / 'bl.in'), str(tmp / 'bl.out')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        results = (tmp / 'bl.out').read_text().strip().split('\n')
    assert len(results) == len(cases)
    mismatches, total = {}, 0
    for (fmt, ctype, width, height, width_e, linear, bh), line in zip(cases, results):
        parts = line.split()
        kind, words = parts[0], [int(w, 16) for w in parts[1:]]
        compressed = is_compressed(fmt)
        is_int = ctype in (SINT, UINT)
        blocks = {}
        for y in range(height):
            for x in range(width):
                if compressed:
                    index = (y // 4) * width_e + x // 4
                    if index not in blocks:
                        blocks[index] = decode_block(fmt, linear[index * fmt[3]:(index + 1) * fmt[3]], ctype)
                    components = blocks[index][(y % 4) * 4 + x % 4]
                else:
                    offset = (y * width + x) * fmt[3]
                    components = decode_texel(fmt, linear[offset:offset + fmt[3]], ctype)
                for channel in range(4):
                    total += 1
                    expected = components[channel]
                    tolerance = 2.5 / 255 if compressed else 0.0
                    if not words_equal(kind, words[(y * width + x) * 4 + channel], expected, False, tolerance):
                        key = '%s %dx%d block height %d' % (fmt[0], width, height, 1 << bh)
                        entry = mismatches.setdefault(key, {'count': 0, 'example': None})
                        entry['count'] += 1
                        entry['example'] = entry['example'] or {'x': x, 'y': y, 'channel': channel,
                                                                'expected': repr(expected),
                                                                'actual': '%08x' % words[(y * width + x) * 4 + channel]}
    return total, mismatches


def level_geometry(fmt, width, height, level, block_height_log2):
    """Texel size, element size, adjusted block height and byte size of a mip level."""
    compressed = is_compressed(fmt)
    w, h = max(width >> level, 1), max(height >> level, 1)
    w_e, h_e = ((w + 3) // 4, (h + 3) // 4) if compressed else (w, h)
    bh = block_height_log2
    while bh > 0 and h_e <= 8 << (bh - 1):  # blocks shrink for small levels
        bh -= 1
    gobs_x = (w_e * fmt[3] + 63) // 64
    size = gobs_x * ((h_e + (8 << bh) - 1) // (8 << bh)) * (512 << bh)
    return w, h, w_e, h_e, bh, size


MIP_CASES = [  # width, height, levels
    (64, 64, 7), (100, 37, 7), (33, 129, 8), (16, 16, 3), (128, 32, 6),
]


def run_mips(runner, rng):
    by_name = {f[0]: f for f in FORMATS + COMPRESSED}
    cases, lines = [], []
    for name, ctype in BLOCK_LINEAR_FORMATS:
        fmt = by_name[name]
        for width, height, levels in MIP_CASES:
            for layers in (1, 3):
                for bh0 in range(4):
                    geometry = [level_geometry(fmt, width, height, l, bh0) for l in range(levels)]
                    if (1 << bh0) > 1 << ((geometry[0][3] + 7) // 8 - 1).bit_length():
                        continue
                    total = sum(g[5] for g in geometry)
                    layer_stride = total if layers == 1 else -(-total // (512 << geometry[0][4])) * (512 << geometry[0][4])
                    guest = bytearray(layer_stride * layers)
                    expected = []  # per layer, level: linear bytes
                    for layer in range(layers):
                        offset = layer * layer_stride
                        per_level = []
                        for g in geometry:
                            w, h, w_e, h_e, bh, size = g
                            linear = bytes(rng.randrange(256) for _ in range(w_e * h_e * fmt[3]))
                            guest[offset:offset + size] = block_linear_swizzle(linear, w_e, h_e, fmt[3], bh).ljust(size, b'\0')
                            per_level.append(linear)
                            offset += size
                        expected.append(per_level)
                    cases.append((fmt, ctype, width, height, levels, layers, bh0, geometry, expected))
                    lines.append('S %x %x %x %x %x 0 2 3 4 5 %x %x 1 0 %x %x %x %s' % (
                        fmt[1], ctype, ctype, ctype, ctype, width, height, bh0, levels, layers, bytes(guest).hex()))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'mip.in').write_text('\n'.join(lines) + '\n')
        subprocess.run([runner, str(tmp / 'mip.in'), str(tmp / 'mip.out')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        results = (tmp / 'mip.out').read_text().strip().split('\n')
    assert len(results) == len(cases)
    mismatches, total = {}, 0
    for (fmt, ctype, width, height, levels, layers, bh0, geometry, expected), line in zip(cases, results):
        parts = line.split()
        kind, words = parts[0], [int(w, 16) for w in parts[1:]]
        compressed = is_compressed(fmt)
        cursor = 0
        for layer in range(layers):
            for level, (w, h, w_e, h_e, bh, size) in enumerate(geometry):
                linear = expected[layer][level]
                blocks = {}
                for y in range(h):
                    for x in range(w):
                        if compressed:
                            index = (y // 4) * w_e + x // 4
                            if index not in blocks:
                                blocks[index] = decode_block(fmt, linear[index * fmt[3]:(index + 1) * fmt[3]], ctype)
                            components = blocks[index][(y % 4) * 4 + x % 4]
                        else:
                            offset = (y * w + x) * fmt[3]
                            components = decode_texel(fmt, linear[offset:offset + fmt[3]], ctype)
                        for channel in range(4):
                            total += 1
                            word = words[(cursor + y * w + x) * 4 + channel]
                            if not words_equal(kind, word, components[channel], False, 2.5 / 255 if compressed else 0.0):
                                key = '%s %dx%d levels %d layers %d block height %d: layer %d level %d' % (
                                    fmt[0], width, height, levels, layers, 1 << bh0, layer, level)
                                entry = mismatches.setdefault(key, {'count': 0, 'example': None})
                                entry['count'] += 1
                                entry['example'] = entry['example'] or {
                                    'x': x, 'y': y, 'channel': channel, 'expected': repr(components[channel]),
                                    'actual': '%08x' % word}
                cursor += w * h
    return total, mismatches


ASTC_FORMATS = [  # name, image format id, block width, block height
    ('ASTC_2D_4X4', 0x40, 4, 4), ('ASTC_2D_5X4', 0x50, 5, 4), ('ASTC_2D_5X5', 0x41, 5, 5),
    ('ASTC_2D_6X5', 0x51, 6, 5), ('ASTC_2D_6X6', 0x42, 6, 6), ('ASTC_2D_8X5', 0x55, 8, 5),
    ('ASTC_2D_8X6', 0x52, 8, 6), ('ASTC_2D_8X8', 0x44, 8, 8), ('ASTC_2D_10X5', 0x56, 10, 5),
    ('ASTC_2D_10X6', 0x57, 10, 6), ('ASTC_2D_10X8', 0x53, 10, 8), ('ASTC_2D_10X10', 0x45, 10, 10),
    ('ASTC_2D_12X10', 0x54, 12, 10), ('ASTC_2D_12X12', 0x46, 12, 12),
]
EAC_R11_MODIFIERS = [-3, -6, -9, -10, 2, 5, 8, 11]  # table 0, index 0 is used


def astc_void_extent(rgba16):
    """A 16 byte LDR void-extent block: one constant colour, no extent."""
    low = 0xfffffffffffffdfc  # block mode 0x1fc, LDR, reserved bits set, extent all ones
    high = rgba16[0] | (rgba16[1] << 16) | (rgba16[2] << 32) | (rgba16[3] << 48)
    return struct.pack('<QQ', low, high)


def etc1_block(rng):
    """An ETC1-compatible block with every pixel index 0; returns bytes and per-pixel RGB."""
    diff = rng.random() < 0.5
    flip = rng.randrange(2)
    table1, table2 = rng.randrange(8), rng.randrange(8)
    tables = [[2, 8, -2, -8], [5, 17, -5, -17], [9, 29, -9, -29], [13, 42, -13, -42],
              [18, 60, -18, -60], [24, 80, -24, -80], [33, 106, -33, -106], [47, 183, -47, -183]]
    if diff:
        base = [rng.randrange(4, 28) for _ in range(3)]
        delta = [rng.randrange(-4, 4) for _ in range(3)]
        second = [b + d for b, d in zip(base, delta)]
        colors = [[(c << 3) | (c >> 2) for c in base], [(c << 3) | (c >> 2) for c in second]]
        head = bytes([base[0] << 3 | (delta[0] & 7), base[1] << 3 | (delta[1] & 7), base[2] << 3 | (delta[2] & 7)])
    else:
        a = [rng.randrange(16) for _ in range(3)]
        b = [rng.randrange(16) for _ in range(3)]
        colors = [[(c << 4) | c for c in a], [(c << 4) | c for c in b]]
        head = bytes([a[0] << 4 | b[0], a[1] << 4 | b[1], a[2] << 4 | b[2]])
    flags = (table1 << 5) | (table2 << 2) | (int(diff) << 1) | flip
    block = head + bytes([flags]) + bytes(4)
    texels = {}
    for y in range(4):
        for x in range(4):
            sub = (0 if y < 2 else 1) if flip else (0 if x < 2 else 1)
            modifier = tables[(table1, table2)[sub]][0]
            texels[(x, y)] = [min(max(c + modifier, 0), 255) / 255 for c in colors[sub]]
    return block, texels


def eac_block(rng):
    """An EAC R11 block with every index 0; returns bytes and the value."""
    base, multiplier = rng.randrange(256), rng.randrange(1, 16)
    block = bytes([base, (multiplier << 4) | 0]) + bytes(6)
    value = base * 8 + 4 + EAC_R11_MODIFIERS[0] * multiplier * 8
    return block, min(max(value, 0), 2047) / 2047


def run_compressed_extra(runner, rng):
    """ASTC void-extent blocks, ETC2 RGB / RGBA (ETC1 subset) and EAC R11 blocks.

    The blocks decode to a constant colour per block (or per ETC sub-block), which
    makes the model trivial while still checking the block size, the block order
    and the host format of every compressed texture format."""
    cases, lines = [], []
    for name, ident, bw, bh in ASTC_FORMATS:
        for srgb in (False, True):
            blocks_x, blocks_y = 3, 2
            blocks = [[rng.randrange(65536) for _ in range(4)] for _ in range(blocks_x * blocks_y)]
            data = b''.join(astc_void_extent(b) for b in blocks)
            lines.append('S %x %x %x %x %x %x 2 3 4 5 %x %x 0 0 0 1 1 %s' % (
                ident, UNORM, UNORM, UNORM, UNORM, int(srgb), blocks_x * bw, blocks_y * bh, data.hex()))
            expected = {}
            for y in range(blocks_y * bh):
                for x in range(blocks_x * bw):
                    c = blocks[(y // bh) * blocks_x + x // bw]
                    rgb = [srgb_to_linear((v >> 8) / 255) if srgb else v / 65535 for v in c[:3]]
                    expected[(x, y)] = rgb + [c[3] / 65535]
            cases.append((name + (' sRGB' if srgb else ''), blocks_x * bw, blocks_y * bh, expected, 4e-3 if srgb else 2e-3))
    for ident, name, kind, srgb in ((0x06, 'ETC2_RGB', 'rgb', False), (0x06, 'ETC2_RGB sRGB', 'rgb', True),
                                   (0x0b, 'ETC2_RGBA', 'rgba', False), (0x19, 'EAC R11', 'r11', False)):
        for _ in range(12):
            blocks_x, blocks_y = 3, 2
            data, expected = b'', {}
            for by in range(blocks_y):
                for bx in range(blocks_x):
                    if kind == 'r11':
                        block, value = eac_block(rng)
                        data += block
                        for y in range(4):
                            for x in range(4):
                                expected[(bx * 4 + x, by * 4 + y)] = [value, 0.0, 0.0, 1.0]
                    else:
                        alpha = None
                        if kind == 'rgba':
                            alpha_block, alpha = eac_block(rng)
                            data += alpha_block
                        block, texels = etc1_block(rng)
                        data += block
                        for (x, y), rgb in texels.items():
                            if srgb:
                                rgb = [srgb_to_linear(v) for v in rgb]
                            expected[(bx * 4 + x, by * 4 + y)] = rgb + [alpha if alpha is not None else 1.0]
            lines.append('S %x %x %x %x %x %x 2 3 4 5 %x %x 0 0 0 1 1 %s' % (
                ident, UNORM, UNORM, UNORM, UNORM, int(srgb), blocks_x * 4, blocks_y * 4, data.hex()))
            cases.append((name, blocks_x * 4, blocks_y * 4, expected, 4e-3 if srgb else 2e-3))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'extra.in').write_text('\n'.join(lines) + '\n')
        subprocess.run([runner, str(tmp / 'extra.in'), str(tmp / 'extra.out')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        results = (tmp / 'extra.out').read_text().strip().split('\n')
    assert len(results) == len(cases)
    mismatches, total = {}, 0
    for (name, width, height, expected, tolerance), line in zip(cases, results):
        parts = line.split()
        words = [int(w, 16) for w in parts[1:]]
        for y in range(height):
            for x in range(width):
                for channel in range(4):
                    total += 1
                    if not words_equal('F', words[(y * width + x) * 4 + channel], expected[(x, y)][channel], False, tolerance):
                        entry = mismatches.setdefault(name, {'count': 0, 'example': None})
                        entry['count'] += 1
                        entry['example'] = entry['example'] or {
                            'x': x, 'y': y, 'channel': channel, 'expected': expected[(x, y)][channel],
                            'actual': bits_f32(words[(y * width + x) * 4 + channel])}
    return total, mismatches


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    parser.add_argument('--header', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--seed', type=int, default=20261020)
    parser.add_argument('--bcdec', type=Path, default=Path('/tmp/hydra-bcdec/bcdec.h'),
                        help='bcdec.h (downloaded when missing) for the BC6H/BC7 oracle')
    args = parser.parse_args()
    audited = audit(args.header) if args.header else 0
    rng = random.Random(args.seed)

    combos = []  # (fmt, ctype, srgb)
    for fmt in FORMATS:
        for ctype in component_types(fmt[0], fmt[4]):
            combos.append((fmt, ctype, False))
        if fmt[0] in ('ARGB8', 'XBGR8'):
            combos.append((fmt, UNORM, True))
    for fmt in SPECIAL:
        combos.append((fmt, FLOAT, False))
    for fmt in COMPRESSED:
        types, has_srgb = COMPRESSED_COMBOS[fmt[0]]
        for ctype in types:
            combos.append((fmt, ctype, False))
        if has_srgb:
            combos.append((fmt, UNORM, True))

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        lines = ['M %x %x %x %x %x %x' % (c[0][1], c[1], c[1], c[1], c[1], int(c[2])) for c in combos]
        (tmp / 'map.in').write_text('\n'.join(lines) + '\n')
        subprocess.run([args.runner, str(tmp / 'map.in'), str(tmp / 'map.out')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        names = (tmp / 'map.out').read_text().split('\n')
        mapped = [(c, names[i]) for i, c in enumerate(combos) if names[i] != 'invalid']
        gaps = [(c, names[i]) for i, c in enumerate(combos) if names[i] == 'invalid']

        cases = []
        input_lines = []
        for (fmt, ctype, srgb), host in mapped:
            is_int = ctype in (SINT, UINT)
            size = fmt[3]
            compressed = is_compressed(fmt)
            for texels in random_texels(rng, size, 4, compressed, fmt[4] if compressed else None):
                for swizzle in SWIZZLES:
                    swz = make_swizzle(swizzle, is_int)
                    cases.append((fmt, ctype, srgb, host, texels, swz, is_int))
                    input_lines.append('S %x %x %x %x %x %x %x %x %x %x 4 %x 0 0 0 1 1 %s' % (
                        fmt[1], ctype, ctype, ctype, ctype, int(srgb), *swz, 4 if compressed else 1,
                        texels.hex()))
        needed = sorted({(fmt[4], texels.hex()) for fmt, _, _, _, texels, _, _ in cases
                         if is_compressed(fmt) and fmt[4] in ORACLE_KINDS})
        if needed:
            oracle_results.update(run_oracle(args.bcdec, tmp, needed))
        (tmp / 'sample.in').write_text('\n'.join(input_lines) + '\n')
        subprocess.run([args.runner, str(tmp / 'sample.in'), str(tmp / 'sample.out')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        results = (tmp / 'sample.out').read_text().strip().split('\n')

    assert len(results) == len(cases), (len(results), len(cases))
    mismatches = {}
    total = 0
    for (fmt, ctype, srgb, host, texels, swz, is_int), line in zip(cases, results):
        parts = line.split()
        kind, words = parts[0], [int(w, 16) for w in parts[1:]]
        size = fmt[3]
        compressed = is_compressed(fmt)
        block = decode_block(fmt, texels, ctype) if compressed else None
        tolerance = 0.0
        rel = 0.0
        if compressed:
            tolerance = 3.0 / 127 if ctype == SNORM else 2.5 / 255
            if fmt[4] in ('bc6u', 'bc6s'):
                tolerance, rel = 1e-4, 2e-3
        for texel in range(16 if compressed else 4):
            data = texels if compressed else texels[texel * size:(texel + 1) * size]
            components = block[texel] if compressed else decode_texel(fmt, data, ctype)
            if srgb:
                components = [srgb_to_linear(c) if i < 3 else c for i, c in enumerate(components)]
            for channel in range(4):
                source = swz[channel]
                if source in (SR, SG, SB, SA) and (source - SR) not in present_channels(fmt):
                    # The header does not define this; the convention of every
                    # graphics API (and Metal) is 0 for G/B and 1 for alpha
                    total += 1
                    expected = 1 if source == SA else 0
                    expected = float(expected) if not is_int else expected
                    word = words[texel * 4 + channel]
                    if not words_equal(kind, word, expected, False, tolerance):
                        key = '%s/%s%s reads missing %s' % (fmt[0], COMPONENT_NAMES[ctype],
                                                           '/sRGB' if srgb else '', 'RGBA'[source - SR])
                        entry = mismatches.setdefault(key, {'count': 0, 'example': None})
                        entry['count'] += 1
                        entry['example'] = entry['example'] or {
                            'bytes': data.hex(), 'texel': texel, 'expected': repr(expected),
                            'actual': '%08x' % word, 'kind': kind}
                    continue
                total += 1
                expected = select(components, source, is_int)
                ok = words_equal(kind, words[texel * 4 + channel], expected,
                                 srgb and swz[channel] in (SR, SG, SB),
                                 tolerance * (2 if srgb and swz[channel] in (SR, SG, SB) else 1), rel)
                if not ok:
                    key = '%s/%s%s -> %s' % (fmt[0], COMPONENT_NAMES[ctype], '/sRGB' if srgb else '', host)
                    entry = mismatches.setdefault(key, {'count': 0, 'example': None})
                    entry['count'] += 1
                    if entry['example'] is None:
                        entry['example'] = {'bytes': data.hex(), 'swizzle': list(swz), 'texel': texel,
                                            'channel': channel, 'expected': repr(expected),
                                            'actual': '%08x' % words[texel * 4 + channel], 'kind': kind}

    print('NVIDIA header names audited: %d' % audited)
    print('Format/type combinations: %d mapped, %d without host format' % (len(mapped), len(gaps)))
    for (fmt, ctype, srgb), _ in gaps:
        print('GAP  %s/%s%s' % (fmt[0], COMPONENT_NAMES[ctype], '/sRGB' if srgb else ''))
    print('Compared %d component values' % total)
    for key, entry in sorted(mismatches.items()):
        print('MISMATCH %s: %d, e.g. %s' % (key, entry['count'], entry['example']))
    if args.report:
        args.report.write_text(json.dumps({
            'seed': args.seed, 'mapped': len(mapped), 'gaps': [
                '%s/%s%s' % (c[0][0], COMPONENT_NAMES[c[1]], '/sRGB' if c[2] else '') for c, _ in gaps],
            'compared': total, 'mismatches': mismatches}, indent=2))
    bl_total, bl_mismatches = run_block_linear(args.runner, random.Random(args.seed + 1))
    print('Block-linear layout: compared %d component values' % bl_total)
    for key, entry in sorted(bl_mismatches.items()):
        print('MISMATCH block-linear %s: %d, e.g. %s' % (key, entry['count'], entry['example']))
    mismatches.update({'block-linear ' + k: v for k, v in bl_mismatches.items()})
    total += bl_total
    mip_total, mip_mismatches = run_mips(args.runner, random.Random(args.seed + 2))
    print('Mip chains and array layers: compared %d component values' % mip_total)
    for key, entry in sorted(mip_mismatches.items()):
        print('MISMATCH mips %s: %d, e.g. %s' % (key, entry['count'], entry['example']))
    mismatches.update({'mips ' + k: v for k, v in mip_mismatches.items()})
    total += mip_total
    extra_total, extra_mismatches = run_compressed_extra(args.runner, random.Random(args.seed + 3))
    print('ASTC / ETC2 / EAC blocks: compared %d component values' % extra_total)
    for key, entry in sorted(extra_mismatches.items()):
        print('MISMATCH %s: %d, e.g. %s' % (key, entry['count'], entry['example']))
    mismatches.update({'compressed ' + k: v for k, v in extra_mismatches.items()})
    total += extra_total
    if mismatches:
        print('FAIL')
        sys.exit(1)
    print('PASS: %d component values match' % total)


if __name__ == '__main__':
    main()
