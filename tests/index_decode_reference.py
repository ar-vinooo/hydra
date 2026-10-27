#!/usr/bin/env python3
"""Guest primitive types and index buffers: Python model vs IndexCache::decode.

Metal cannot draw every guest primitive type (line loops, fans, polygons, quads,
quad strips) or 8-bit indices, and it restarts strips only at the maximum index.
Hydra therefore decodes such draws into plain triangle or line lists. This
script decomposes the same primitives with the standard rules (OpenGL / the
NVIDIA BEGIN_OP names, audited against clb197.h) and compares:

  * the decoded index list, primitive type and count of every decoded draw;
  * for draws that Hydra passes through unchanged, that Metal's native
    interpretation is equivalent (point, line and triangle lists without
    restart, strips without restart or with the maximum index as the restart).

Restart indices end the primitive being assembled, including incomplete list
primitives. Non-indexed draws use the vertex numbers 0..count-1. Adjacency and
patch primitives are not covered (geometry and tessellation are unsupported).

Usage: index_decode_reference.py <hydra-index-decode-reference-tests>
           [--header clb197.h] [--seed N]
"""
import argparse
import os
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

POINTS, LINES, LINE_LOOP, LINE_STRIP, TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN, QUADS, QUAD_STRIP, POLYGON = range(10)
NAMES = ['Points', 'Lines', 'LineLoop', 'LineStrip', 'Triangles', 'TriangleStrip', 'TriangleFan', 'Quads',
         'QuadStrip', 'Polygon']
U8, U16, U32, NONE = range(4)
MAX_INDEX = {U8: 0xff, U16: 0xffff, U32: 0xffffffff}


def audit(header):
    text = header.read_text()
    for value, name in enumerate(['POINTS', 'LINES', 'LINE_LOOP', 'LINE_STRIP', 'TRIANGLES', 'TRIANGLE_STRIP',
                                  'TRIANGLE_FAN', 'QUADS', 'QUAD_STRIP', 'POLYGON']):
        match = re.search(r'^#define\s+NVB197_BEGIN_OP_%s\s+(0x[0-9A-Fa-f]+)' % name, text, re.M)
        if not match or int(match[1], 16) != value:
            raise SystemExit('NVIDIA header mismatch for BEGIN_OP_' + name)
    for value, name in enumerate(['ONE_BYTE', 'TWO_BYTES', 'FOUR_BYTES']):
        match = re.search(r'^#define\s+NVB197_SET_INDEX_BUFFER_E_INDEX_SIZE_%s\s+(0x[0-9A-Fa-f]+)' % name, text, re.M)
        if not match or int(match[1], 16) != value:
            raise SystemExit('NVIDIA header mismatch for INDEX_SIZE_' + name)
    return 13


def decompose(prim, v):
    """Vertices of the triangles / lines the primitive assembles to."""
    n = len(v)
    out = []
    if prim == POINTS:
        out = list(v)
    elif prim == LINES:
        out = v[:n // 2 * 2]
    elif prim == TRIANGLES:
        out = v[:n // 3 * 3]
    elif prim in (LINE_STRIP, LINE_LOOP):
        for i in range(n - 1):
            out += [v[i], v[i + 1]]
        if prim == LINE_LOOP and n > 1:
            out += [v[n - 1], v[0]]
    elif prim == TRIANGLE_STRIP:
        for i in range(n - 2):
            out += [v[i + (i & 1)], v[i + 1 - (i & 1)], v[i + 2]]
    elif prim in (TRIANGLE_FAN, POLYGON):
        for i in range(1, n - 1):
            out += [v[0], v[i], v[i + 1]]
    elif prim == QUADS:
        for b in range(0, n - 3, 4):
            out += [v[b], v[b + 1], v[b + 2], v[b], v[b + 2], v[b + 3]]
    elif prim == QUAD_STRIP:
        for b in range(0, n - 3, 2):
            out += [v[b], v[b + 1], v[b + 3], v[b], v[b + 3], v[b + 2]]
    return out


def output_primitive(prim):
    if prim in (TRIANGLE_STRIP, TRIANGLE_FAN, POLYGON, QUADS, QUAD_STRIP):
        return TRIANGLES
    if prim in (LINE_LOOP, LINE_STRIP):
        return LINES
    return prim


def expected_draw(prim, itype, count, restart_enabled, restart_index, indices):
    vertices = list(range(count)) if itype == NONE else list(indices[:count])
    segments, current = [], []
    for value in vertices:
        if itype != NONE and restart_enabled and value == restart_index:
            segments.append(current)
            current = []
        else:
            current.append(value)
    segments.append(current)
    out = []
    for segment in segments:
        out += decompose(prim, segment)
    return out


def metal_native_ok(prim, itype, restart_enabled, restart_index, indices):
    """Whether Metal draws this primitive with the guest semantics unchanged
    (incomplete trailing primitives are dropped by both)."""
    if prim not in (POINTS, LINES, TRIANGLES, LINE_STRIP, TRIANGLE_STRIP) or itype == U8:
        return False
    if itype == NONE or not restart_enabled or restart_index not in indices:
        return True
    # Metal restarts strips at the maximum index only
    return prim in (LINE_STRIP, TRIANGLE_STRIP) and restart_index == MAX_INDEX[itype]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runner')
    parser.add_argument('--header', type=Path)
    parser.add_argument('--seed', type=int, default=20261020)
    args = parser.parse_args()
    audited = audit(args.header) if args.header else 0
    rng = random.Random(args.seed)
    cases = []
    for prim in range(10):
        for itype in (U8, U16, U32, NONE):
            for restart in ('off', 'value', 'max'):
                if itype == NONE and restart != 'off':
                    continue
                for _ in range(25):
                    count = rng.randrange(0, 30)
                    restart_index = 0
                    limit = MAX_INDEX[itype] if itype != NONE else 0
                    indices = []
                    if itype != NONE:
                        restart_index = rng.randrange(0, 8) if restart == 'value' else MAX_INDEX[itype]
                        top = 40 if itype != U8 else 40
                        indices = [rng.randrange(0, top) for _ in range(count)]
                        if restart != 'off':
                            for i in range(count):
                                if rng.random() < 0.15:
                                    indices[i] = restart_index
                        if restart == 'off' and rng.random() < 0.3 and limit > 0:
                            indices = [min(i * 1000, limit - 1) if itype == U32 else i for i in indices]
                    cases.append((prim, itype, count, restart != 'off', restart_index, indices))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        lines = ['%d %d %d %d %d %s' % (p, t, c, int(r), ri, ' '.join(map(str, ix))) for p, t, c, r, ri, ix in cases]
        (tmp / 'in.txt').write_text('\n'.join(lines) + '\n')
        subprocess.run([args.runner, str(tmp / 'in.txt'), str(tmp / 'out.txt')], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1'))
        results = (tmp / 'out.txt').read_text().split('\n')
    stats = {}
    for case, line in zip(cases, results):
        prim, itype, count, restart_enabled, restart_index, indices = case
        fields = list(map(int, line.split()))
        passthrough, out_type, out_prim, out_count = fields[:4]
        decoded = fields[4:]
        entry = stats.setdefault((NAMES[prim], 'index' if itype != NONE else 'no index'), [0, 0, None])
        entry[0] += 1
        want = expected_draw(prim, itype, count, restart_enabled, restart_index, indices)
        problem = None
        if passthrough:
            if not metal_native_ok(prim, itype, restart_enabled, restart_index, indices):
                problem = 'passed through but Metal cannot draw it unchanged'
        else:
            if out_prim != output_primitive(prim):
                problem = 'primitive type %d, expected %d' % (out_prim, output_primitive(prim))
            elif decoded != want or out_count != len(want):
                problem = 'indices differ: got %s expected %s' % (decoded[:12], want[:12])
        if problem:
            entry[1] += 1
            entry[2] = entry[2] or (problem, case)
    failed = False
    print('NVIDIA header values audited: %d' % audited)
    for key, (n, bad, example) in sorted(stats.items()):
        print('%-14s %-9s %4d cases %3d mismatches %s' % (key[0], key[1], n, bad, example if bad else ''))
        failed |= bad != 0
    if failed:
        print('FAIL')
        sys.exit(1)
    print('PASS')


if __name__ == '__main__':
    main()
