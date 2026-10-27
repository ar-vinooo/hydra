#!/usr/bin/env python3
"""MOV32I bit-transfer reference vs guest-to-Metal shader cache execution.

No physical NVIDIA execution. NVIDIA's headers audit cache method encodings;
Python computes the immediate bit patterns independently. Maxwell SASS fixture
encodings exercise Hydra's decoder and are not certified by the PTX manual.
"""
import argparse
import collections
import json
import os
from pathlib import Path
import random
import re
import struct
import subprocess
import tempfile


def audit(path, prefix, no_wfi):
    text = path.read_text()
    checks = {'INVALIDATE_SHADER_CACHES': 0x21c,
              'INVALIDATE_SHADER_CACHES_NO_WFI': no_wfi,
              'INVALIDATE_SHADER_CACHES_INSTRUCTION_TRUE': 1,
              'INVALIDATE_SHADER_CACHES_NO_WFI_INSTRUCTION_TRUE': 1}
    for suffix, expected in checks.items():
        symbol = prefix + suffix
        match = re.search(r'^#define\s+' + symbol + r'\s+(0x[0-9A-Fa-f]+|\d+)\s*$', text, re.M)
        if not match or int(match[1], 0) != expected:
            raise ValueError('NVIDIA header mismatch: ' + symbol)
    return len(checks)


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--nvidia-3d', type=Path, required=True)
    parser.add_argument('--nvidia-compute', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--group', help='Run one named action group')
    args = parser.parse_args()
    audited = audit(args.nvidia_3d, 'NVB197_', 0xda4)
    audited += audit(args.nvidia_compute, 'NVB1C0_', 0x1698)
    rng = random.Random(197)
    colors = [0, 0.125, 0.25, 0.5, 1, 2, -0.5, 65504]
    actions = ('cpu_write', '3d_invalidate', '3d_no_wfi', 'compute_invalidate',
               'compute_no_wfi', 'host_remap', 'scatter_code', 'switch_as',
               'unmap_restore', 'truncated_restore', 'non_instruction_flags',
               'gpu_code_write_3d', 'gpu_code_write_compute_no_wfi')
    cases = []
    for padding in (0, 3, 48, 384):
        for action in range(11):
            for _ in range(4):
                rgba = tuple(bits(rng.choice(colors)) for _ in range(4))
                cases.append((action, padding, rgba))
    for action in (11, 12):
        for _ in range(4):
            cases.append((action, 0, tuple(bits(rng.choice(colors)) for _ in range(4))))
    if args.group:
        cases = [case for case in cases if actions[case[0]] == args.group]
        if not cases:
            parser.error('Unknown or empty action group')
    with tempfile.TemporaryDirectory(prefix='hydra-shader-cache-') as directory:
        inputs, outputs = Path(directory) / 'input', Path(directory) / 'output'
        inputs.write_text(''.join(' '.join(map(str, (a, pad, *rgba))) + '\n'
                                  for a, pad, rgba in cases))
        env = dict(os.environ, MTL_DEBUG_LAYER='1', HYDRA_HOT_RELOAD='0',
                   HYDRA_SHADER_OVERRIDE_DIR=str(Path(directory) / 'override'))
        completed = subprocess.run([str(args.binary.resolve()), str(inputs), str(outputs)], env=env)
        rows = [tuple(map(int, row.split())) for row in outputs.read_text().splitlines()]
    if len(rows) != len(cases):
        raise ValueError('Incomplete Metal readback')
    failures = []
    for index, ((action, padding, expected), got) in enumerate(zip(cases, rows)):
        if len(got) != 8:
            raise ValueError('Invalid result row')
        old = cases[index - 1][2] if index else (0, 0, 0, 0)
        if got[:4] != expected or got[4:] != old:
            failures.append({'case': index, 'action': actions[action], 'padding_nops': padding,
                             'expected_bits': expected, 'metal_bits': got[:4],
                             'previous_expected_bits': old, 'previous_metal_bits': got[4:]})
    report = {'cases': len(cases), 'passed': len(cases) - len(failures),
              'runner_exit_code': completed.returncode,
              'nvidia_header_values_audited': audited,
              'groups': dict(collections.Counter(actions[a] for a, _, _ in cases)),
              'failures': failures[:12], 'comparison': 'exact RGBA32Float bit patterns',
              'limits': ['No physical NVIDIA GPU execution.',
                         'MOV32I/EXIT fixtures are not a complete SASS conformance test.',
                         'Numerical output uses fragment shaders; compute cache methods are exercised.',
                         'Concurrent unsynchronized guest code writes and shader sizes above 64 KiB are not covered.']}
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return bool(failures) or completed.returncode != 0


if __name__ == '__main__':
    raise SystemExit(main())
