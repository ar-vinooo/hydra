#!/usr/bin/env python3
"""NVIDIA semaphore reduction equations vs production puller + actual Metal I/O.

The puller arithmetic is emulated on the CPU. Metal kernels initialize guest
words and read the final result, so this tests real GPU visibility and WFI too.
No NVIDIA device executes. Detailed channel equations are from NVIDIA's Turing
PBDMA manual; matching Maxwell opcodes do not prove all GM20B side effects.
"""
import argparse
from collections import Counter
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile

MASK = (1 << 32) - 1


def signed(value):
    return value if value < 1 << 31 else value - (1 << 32)


def model(case, control=False):
    op, sign, value, payload, count, no_wfi = case
    valid = op < 8 and not (op >= 6 and sign)
    if valid:
        for _ in range(count):
            if op in (0, 1):
                convert = signed if sign else int
                choose = min if op == 0 else max
                value = choose(convert(value), convert(payload)) & MASK
            elif op == 2:
                value ^= payload
            elif op == 3:
                value &= payload
            elif op == 4:
                value |= payload
            elif op == 5:
                value = (value + payload) % (1 << 32)
            elif op == 6:
                value = 0 if value + 1 > payload else value + 1
                if control:
                    value = (value + 1) & MASK
            else:
                value = payload if value == 0 or value > payload else value - 1
    # Error 25 is Hydra's conservative fault mapping, not a NVIDIA error enum.
    return value, 0 if valid else 25, int(valid and not no_wfi)


def audit(header, manual):
    definitions = {
        'NVB06F_SEMAPHORED': 0x1c,
        'NVB06F_SEMAPHORED_OPERATION_REDUCTION': 0x10,
        'NVB06F_SEMAPHORED_REDUCTION_MIN': 0,
        'NVB06F_SEMAPHORED_REDUCTION_MAX': 1,
        'NVB06F_SEMAPHORED_REDUCTION_XOR': 2,
        'NVB06F_SEMAPHORED_REDUCTION_AND': 3,
        'NVB06F_SEMAPHORED_REDUCTION_OR': 4,
        'NVB06F_SEMAPHORED_REDUCTION_ADD': 5,
        'NVB06F_SEMAPHORED_REDUCTION_INC': 6,
        'NVB06F_SEMAPHORED_REDUCTION_DEC': 7,
        'NVB06F_SEMAPHORED_FORMAT_SIGNED': 0,
        'NVB06F_SEMAPHORED_FORMAT_UNSIGNED': 1,
    }
    text = header.read_text()
    for key, expected in definitions.items():
        match = re.search(r'^#define\s+' + key + r'\s+\(?(0x[0-9a-fA-F]+|\d+)\)?\s*$', text, re.M)
        if not match or int(match[1], 0) != expected:
            raise ValueError('NVIDIA header mismatch: ' + key)
    text = ' '.join(manual.read_text().split())
    for equation in ('INC U inv v = (v >= p) ? 0 : v + 1',
                     'DEC U inv v = (v == 0 || v > p) ? p : v - 1'):
        if equation not in text:
            raise ValueError('NVIDIA manual equation missing: ' + equation)
    return len(definitions)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--nvidia-header', type=Path, required=True)
    parser.add_argument('--nvidia-manual', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--control', action='store_true')
    args = parser.parse_args()
    audited = audit(args.nvidia_header, args.nvidia_manual)
    edges = (0, 1, 2, 3, 7, 0x7fffffff, 0x80000000, MASK - 1, MASK)
    cases = [(op, sign, value, payload, 1, no_wfi)
             for op, sign, value, payload, no_wfi in itertools.product(
                 range(8), range(2), edges, edges, range(2))]
    rng = random.Random(0xb06f)
    for op in range(8):
        for _ in range(80):
            sign = rng.randrange(2) if op < 6 else 0
            cases.append((op, sign, rng.getrandbits(32), rng.getrandbits(32),
                          rng.choice((1, 2, 17)), rng.randrange(2)))
    for op, payload, no_wfi in itertools.product((6, 7), range(8), range(2)):
        cases.append((op, 0, payload + 1, payload, 33, no_wfi))
    for op, sign, no_wfi in itertools.product((8, 15), range(2), range(2)):
        cases.append((op, sign, MASK, 7, 1, no_wfi))
    with tempfile.TemporaryDirectory(prefix='hydra-semaphore-') as directory:
        inputs, outputs = Path(directory) / 'cases', Path(directory) / 'results'
        inputs.write_text(''.join(' '.join(map(str, case)) + '\n' for case in cases))
        with (Path(directory) / 'runner.log').open('w+') as log:
            run = subprocess.run([str(args.binary.resolve()), '--semaphore-cases',
                                  str(inputs), str(outputs)],
                                 env=dict(os.environ, MTL_DEBUG_LAYER='1', HYDRA_HOT_RELOAD='0'),
                                 stdout=log, stderr=subprocess.STDOUT)
            log.seek(0)
            runner_log = log.read()
        rows = [tuple(map(int, row.split())) for row in outputs.read_text().splitlines()] if outputs.exists() else []
    failures = []
    for index, case in enumerate(cases):
        expected = model(case, args.control)
        got = rows[index] if index < len(rows) else None
        if expected != got:
            failures.append({'case': index, 'input': case, 'expected': expected, 'metal_readback_fault_checkpoints': got})
    report = {
        'cases': len(cases), 'passed': len(cases) - len(failures),
        'runner_exit_code': run.returncode, 'observed_rows': len(rows),
        'nvidia_header_values_audited': audited,
        'groups': dict(Counter('op%d_%s' % (c[0], 'signed' if c[1] else 'unsigned') for c in cases)),
        'sources_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in (args.nvidia_header, args.nvidia_manual)},
        'control': args.control, 'failures': failures[:12],
        'limits': ['No physical NVIDIA execution.',
                   'Detailed channel equations/signedness table come from Turing; Maxwell header confirms opcodes, not complete GM20B semantics.',
                   'Production puller reductions use CPU atomics; real Metal kernels initialize/read the shared memory and test WFI ordering.',
                   'Error 25 is Hydra fault policy; concurrent CPU/GPU atomics, reduction timestamps and all semaphore modifiers are not certified.'],
    }
    if run.returncode:
        report['runner_log_tail'] = runner_log[-4000:]
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return bool(failures) or run.returncode != 0 or len(rows) != len(cases)


if __name__ == '__main__':
    raise SystemExit(main())
