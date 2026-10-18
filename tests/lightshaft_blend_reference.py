#!/usr/bin/env python3
"""Captured NVIDIA blend-register equation vs actual Metal readback.

The Maxwell header supplies register encodings, not a specification of
SEPARATE_FOR_ALPHA side effects. This checks preservation of explicitly
programmed ADD RGB / REVERSE_SUBTRACT alpha, as used by the captured occlusion
pass. It is a mathematical reference, not execution on an NVIDIA GPU.
"""
import argparse
import itertools
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def storage(fmt, x):
    if fmt == 'RGBA8Unorm':
        return round(max(0, min(1, x)) * 255) / 255
    return struct.unpack('<e', struct.pack('<e', x))[0]


def audit_header(path):
    text = path.read_text()
    checked = 0
    for prefix in ('NVB197_SET_BLEND_', 'NVB197_SET_BLEND_PER_TARGET_'):
        for name, expected in {
            'COLOR_OP_V_D3D_ADD': 1, 'ALPHA_OP_V_D3D_REVSUBTRACT': 3,
            'COLOR_SOURCE_COEFF_V_D3D_ONE': 2,
            'COLOR_DEST_COEFF_V_D3D_INVSRCALPHA': 6,
            'ALPHA_SOURCE_COEFF_V_D3D_ONE': 2, 'ALPHA_DEST_COEFF_V_D3D_ONE': 2,
            'COLOR_OP_V_OGL_FUNC_ADD': 0x8006,
            'ALPHA_OP_V_OGL_FUNC_REVERSE_SUBTRACT': 0x800B,
            'COLOR_SOURCE_COEFF_V_OGL_ONE': 0x4001,
            'COLOR_DEST_COEFF_V_OGL_ONE_MINUS_SRC_ALPHA': 0x4303,
            'ALPHA_SOURCE_COEFF_V_OGL_ONE': 0x4001,
            'ALPHA_DEST_COEFF_V_OGL_ONE': 0x4001,
        }.items():
            symbol = prefix + name
            match = re.search(r'^#define\s+' + symbol + r'\s+(0x[0-9a-fA-F]+|\d+)\s*$', text, re.M)
            if not match or int(match[1], 0) != expected:
                raise ValueError('NVIDIA encoding mismatch: ' + symbol)
            checked += 1
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--nvidia-header', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    audited = audit_header(args.nvidia_header)
    cases = []
    for fmt, kind, flag, encoding, source_a, destination_a in itertools.product(
            ('RGBA8Unorm', 'RGBA16Float'), ('global', 'target'), (0, 1, 2),
            ('D3D', 'OGL'), range(0, 256, 17), (0, 0.25, 0.5, 1)):
        raw = (1, 2, 6, 3, 2, 2) if encoding == 'D3D' else (
            0x8006, 0x4001, 0x4303, 0x800B, 0x4001, 0x4001)
        dst = (0.173, 0.317, 0.491, destination_a)
        src = (0.619, 0.283, 0.137, source_a / 255)
        cases.append((fmt, kind, flag, raw, dst, src))
    with tempfile.TemporaryDirectory(prefix='hydra-lightshaft-') as directory:
        inputs, outputs = Path(directory) / 'cases', Path(directory) / 'results'
        inputs.write_text(''.join(
            'registers ' + ' '.join(map(str, (fmt, kind, flag, *raw, *dst, *src))) + '\n'
            for fmt, kind, flag, raw, dst, src in cases))
        env = dict(os.environ, MTL_DEBUG_LAYER='1', HYDRA_HOT_RELOAD='0')
        subprocess.run([str(args.binary.resolve()), str(inputs), str(outputs)], check=True, env=env)
        rows = outputs.read_text().splitlines()
    if len(rows) != len(cases):
        raise ValueError('Missing Metal results')
    failures = []
    for index, ((fmt, kind, flag, raw, dst, src), row) in enumerate(zip(cases, rows)):
        d = [storage(fmt, v) for v in dst]
        s = list(map(f32, src))
        # Independently expressed occlusion equations, not the C++ selector:
        # RGB = source + destination * (1 - source alpha)
        # A = destination alpha - source alpha; UNORM clamps at storage.
        rgb = [f32(s[c] + f32(d[c] * f32(1 - s[3]))) for c in range(3)]
        expected = [storage(fmt, v) for v in (*rgb, f32(d[3] - s[3]))]
        data = bytes.fromhex(row)
        got = ([v / 255 for v in data[:4]] if fmt == 'RGBA8Unorm'
               else list(struct.unpack('<4e', data[:8])))
        tolerance = 1.1 / 255 if fmt == 'RGBA8Unorm' else 0.001
        if any(abs(a - b) > tolerance for a, b in zip(expected, got)):
            failures.append({'case': index, 'format': fmt, 'registers': kind,
                             'flag': flag, 'source_alpha': src[3],
                             'destination_alpha': dst[3], 'expected': expected, 'metal': got})
    report = {'cases': len(cases), 'passed': len(cases) - len(failures),
              'nvidia_encodings_audited': audited, 'failures': failures[:12],
              'reference': 'NVIDIA encodings + explicit captured blend equation; scalar Python',
              'execution': 'Apple Metal GPU, production Hydra equation selector and enum decoders',
              'limits': ['No physical NVIDIA execution.',
                         'Does not establish SEPARATE_FOR_ALPHA register-write side effects.',
                         'Does not replay the entire guest shader or radial-blur chain.']}
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
