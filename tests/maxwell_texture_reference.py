#!/usr/bin/env python3
"""Texture / depth-compare sampling reference vs Hydra TEXS -> MSL -> Metal.

The reference implements the standard graphics sampling rules (texel centres at
+0.5, bilinear weights from the fractional position, clamp/repeat/mirror/zero
addressing, depth compare before filtering) with plain Python floats. Hydra runs
guest TEXS 2D.LZ (colour) and TEXS 2D.LL.DC (depth compare) through its decoder,
MSL emitter and Metal. Bilinear weights are compared with a tolerance because
GPUs quantise the fractional weight (<= 1/256 of the texel range here).
No physical NVIDIA execution.

Usage: maxwell_texture_reference.py <hydra-maxwell-alu-reference-tests> [--report f.json]
"""
import argparse
import collections
import json
import math
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

KNOWN_GAPS = {}
CMP = {'never': 0, 'less': 1, 'equal': 2, 'lequal': 3, 'greater': 4, 'notequal': 5,
       'gequal': 6, 'always': 7}
CMP_FN = {
    'never': lambda r, d: False, 'less': lambda r, d: r < d, 'equal': lambda r, d: r == d,
    'lequal': lambda r, d: r <= d, 'greater': lambda r, d: r > d,
    'notequal': lambda r, d: r != d, 'gequal': lambda r, d: r >= d, 'always': lambda r, d: True,
}


def fbits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def bitsf(b):
    return struct.unpack('<f', struct.pack('<I', b))[0]


def address(i, n, mode):
    """Map integer texel index to a valid index, or None for the zero border."""
    if mode == 'edge':
        return min(max(i, 0), n - 1)
    if mode == 'repeat':
        return i % n
    if mode == 'mirror':
        m = i % (2 * n)
        return m if m < n else 2 * n - 1 - m
    if mode == 'zero':
        return i if 0 <= i < n else None
    raise ValueError(mode)


def fetch(texels, w, h, x, y, ms, mt, channels):
    xi, yi = address(x, w, ms), address(y, h, mt)
    if xi is None or yi is None:
        return [0.0] * channels
    return texels[yi * w + xi]


def sample(texels, w, h, u, v, linear, ms, mt, channels, combine=None):
    px, py = u * w, v * h
    if not linear:
        t = fetch(texels, w, h, math.floor(px), math.floor(py), ms, mt, channels)
        return t if combine is None else [combine(t[0])] * 1
    fx, fy = px - 0.5, py - 0.5
    x0, y0 = math.floor(fx), math.floor(fy)
    ax, ay = fx - x0, fy - y0
    out = [0.0] * channels
    for dy, wy in ((0, 1 - ay), (1, ay)):
        for dx, wx in ((0, 1 - ax), (1, ax)):
            t = fetch(texels, w, h, x0 + dx, y0 + dy, ms, mt, channels)
            for c in range(channels):
                out[c] += wx * wy * (combine(t[0]) if combine else t[c])
    return out


def coords(rng, n):
    out = [(rng.uniform(-1.6, 2.6), rng.uniform(-1.6, 2.6)) for _ in range(n)]
    out += [(rng.uniform(0.02, 0.98), rng.uniform(0.02, 0.98)) for _ in range(n // 2)]
    return [(bitsf(fbits(u)), bitsf(fbits(v))) for u, v in out]  # exactly what the GPU receives


def build(rng):
    """Returns configs: dict(kind,w,h,min,mag,ms,mt,cmp, texels, cases[(u,v,ref)])"""
    configs = []
    for w, h in ((4, 4), (5, 3), (2, 7)):
        texels = [[bitsf(fbits(rng.random())) for _ in range(4)] for _ in range(w * h)]
        for filt in ('n', 'l'):
            for ms in ('edge', 'repeat', 'mirror', 'zero'):
                mt = rng.choice(('edge', 'repeat', 'mirror', 'zero'))
                configs.append(dict(kind='color', w=w, h=h, filt=filt, ms=ms, mt=mt, cmp='always',
                                    texels=texels, cases=[(u, v, 0.0) for u, v in coords(rng, 24)]))
    for w, h in ((4, 4), (3, 5)):
        depth = [[bitsf(fbits(rng.random()))] for _ in range(w * h)]
        for filt in ('n', 'l'):
            for cmp in ('less', 'lequal', 'greater', 'gequal', 'notequal'):
                configs.append(dict(kind='depth', w=w, h=h, filt=filt,
                                    ms=rng.choice(('edge', 'repeat', 'mirror')),
                                    mt=rng.choice(('edge', 'repeat', 'mirror')), cmp=cmp,
                                    texels=depth,
                                    cases=[(u, v, bitsf(fbits(rng.random()))) for u, v in coords(rng, 24)]))
    return configs


def expected(cfg, case):
    u, v, ref = case
    linear = cfg['filt'] == 'l'
    if cfg['kind'] == 'color':
        return sample(cfg['texels'], cfg['w'], cfg['h'], u, v, linear, cfg['ms'], cfg['mt'], 4)
    fn = CMP_FN[cfg['cmp']]
    r = sample(cfg['texels'], cfg['w'], cfg['h'], u, v, linear, cfg['ms'], cfg['mt'], 1,
               combine=lambda d: 1.0 if fn(ref, d) else 0.0)
    return [r[0], r[0], r[0], 1.0]  # depth compare returns (r, r, r, 1)


def group_name(cfg):
    return '%s_%s_%s%s' % (cfg['kind'], 'linear' if cfg['filt'] == 'l' else 'nearest',
                           cfg['ms'], '_' + cfg['cmp'] if cfg['kind'] == 'depth' else '')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--seed', type=int, default=11)
    parser.add_argument('--show', type=int, default=3)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    configs = build(rng)
    with tempfile.TemporaryDirectory() as tmp:
        cases_path, out_path = Path(tmp) / 'cases.txt', Path(tmp) / 'out.txt'
        with cases_path.open('w') as f:
            for c in configs:
                f.write('CONFIG %s %d %d %s %s %s %s %d\n' % (c['kind'], c['w'], c['h'], c['filt'],
                        c['filt'], c['ms'], c['mt'], CMP[c['cmp']]))
                f.write('DATA ' + ' '.join('%x' % fbits(x) for t in c['texels'] for x in t) + '\n')
                for u, v, ref in c['cases']:
                    f.write('CASE %x %x %x\n' % (fbits(u), fbits(v), fbits(ref)))
        completed = subprocess.run([str(args.binary), '--texture', str(cases_path), str(out_path)])
        if completed.returncode != 0:
            return completed.returncode
        rows = [[bitsf(int(x, 16)) for x in line.split()] for line in out_path.read_text().splitlines()]
    expected_count = sum(len(c['cases']) for c in configs)
    if len(rows) != expected_count:
        print('incomplete readback: %d of %d' % (len(rows), expected_count))
        return 1
    stats = collections.OrderedDict()
    examples = collections.defaultdict(list)
    index = 0
    for cfg in configs:
        group = group_name(cfg)
        for case in cfg['cases']:
            exp, got = expected(cfg, case), rows[index]
            index += 1
            # Filtered values: 1/256 weight quantisation; nearest/point must be exact.
            tol = 1 / 128 if cfg['filt'] == 'l' else 1e-6
            ok = all(abs(a - b) <= tol for a, b in zip(exp, got))
            s = stats.setdefault(group, [0, 0])
            s[0] += ok
            s[1] += 1
            if not ok and len(examples[group]) < args.show:
                examples[group].append({'cfg': {k: cfg[k] for k in ('w', 'h', 'ms', 'mt', 'cmp')},
                                        'case': case, 'expected': exp, 'got': got})
    unexpected = 0
    for group, (passed, total) in stats.items():
        known = KNOWN_GAPS.get(group)
        status = 'ok ' if passed == total else ('GAP' if known else 'BAD')
        print('%s %-34s %4d/%-4d%s' % (status, group, passed, total, '  (' + known + ')' if known else ''))
        if passed != total and not known:
            unexpected += 1
            for e in examples[group]:
                print('      ', e)
        if passed == total and known:
            unexpected += 1
            print('       known gap now passes completely; remove it from KNOWN_GAPS')
    if args.report:
        args.report.write_text(json.dumps({'groups': stats, 'examples': examples}, indent=2, default=str))
    print('%d cases, %d groups with unexpected results' % (expected_count, unexpected))
    return 1 if unexpected else 0


if __name__ == '__main__':
    sys.exit(main())
