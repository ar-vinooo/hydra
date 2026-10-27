#!/usr/bin/env python3
"""Python mapping/reference-count model vs production QueryMemory and Metal.

No physical NVIDIA device executes. This validates host-visible mapping
lifetime and GPU bytes, not undocumented Horizon error/permission semantics.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile


def model(program, control):
    spaces = [{}, {}]
    rows = []
    for token in program:
        kind = token[0]
        args = list(map(int, token[1:].split(':')))
        as_id = args[0]
        success = True
        if kind == 'D':
            spaces[as_id] = None
        elif kind == 'R':
            spaces[as_id] = {}
        elif spaces[as_id] is None:
            success = False
        else:
            gpu_page, *rest = args[1:]
            if kind == 'M':
                guest, count = rest
                for offset in range(count):
                    spaces[as_id][gpu_page + offset] = guest + offset
            else:
                count, = rest
                for page in range(gpu_page, gpu_page + count):
                    if kind == 'S':
                        spaces[as_id][page] = None
                    else:
                        spaces[as_id].pop(page, None)
        counts = [sum(guest == page for space in spaces if space is not None
                      for guest in space.values()) for page in range(8)]
        if control:
            counts = [0] * 8
        mask = sum((count != 0) << page for page, count in enumerate(counts))
        gpu_words = []
        for space in spaces:
            for page in range(8):
                guest = None if space is None else space.get(page)
                gpu_words.append(0 if guest is None else (0x20 + guest) * 0x01010101)
        rows.append((int(success), mask, *counts, *gpu_words))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--control', action='store_true')
    args = parser.parse_args()
    programs = [
        'M0:0:0:4 M1:0:0:4 U0:1:2 D1 U0:0:4'.split(),
        'M0:0:0:8 M0:2:0:4 S0:3:2 U0:0:8'.split(),
        'M0:0:0:4 M0:4:0:4 M1:0:2:4 D0 R0 M0:0:0:8 D1 D0'.split(),
        'M0:0:0:8 R0 M0:0:4:4 D0 M0:0:0:1 U0:0:1 R0'.split(),
    ]
    rng = random.Random(0xd3a1ce)
    for _ in range(124):
        program = []
        for _ in range(24):
            kind = rng.choice(('M', 'M', 'M', 'U', 'S', 'D', 'R'))
            space, gpu_page, guest = (rng.randrange(2), rng.randrange(8), rng.randrange(8))
            if kind in 'DR':
                program.append(f'{kind}{space}')
            elif kind == 'M':
                count = rng.randint(1, min(8 - gpu_page, 8 - guest))
                program.append(f'M{space}:{gpu_page}:{guest}:{count}')
            else:
                program.append(f'{kind}{space}:{gpu_page}:{rng.randint(1, 8 - gpu_page)}')
        programs.append(program)
    expected = [row for p in programs for row in model(p, args.control)]
    with tempfile.TemporaryDirectory(prefix='hydra-device-shared-') as directory:
        inputs, outputs, log = (Path(directory) / p for p in ('programs', 'results', 'runner.log'))
        inputs.write_text(''.join(' '.join(p) + '\n' for p in programs))
        with log.open('w') as stream:
            run = subprocess.run([str(args.binary.resolve()), '--device-shared-cases', str(inputs), str(outputs)],
                                 env=dict(os.environ, MTL_DEBUG_LAYER='1', HYDRA_HOT_RELOAD='0'),
                                 stdout=stream, stderr=subprocess.STDOUT, timeout=180)
        observed = [tuple(map(int, line.split())) for line in outputs.read_text().splitlines()] if outputs.exists() else []
        runner_log = log.read_text()
    failures = [{'step': i, 'expected': row, 'observed': observed[i] if i < len(observed) else None}
                for i, row in enumerate(expected) if i >= len(observed) or observed[i] != row]
    report = {'programs': len(programs), 'steps': len(expected), 'passed_steps': len(expected) - len(failures),
              'observed_steps': len(observed), 'runner_exit_code': run.returncode,
              'control': args.control, 'failures': failures[:12],
              'programs_sha256': hashlib.sha256(inputs.read_bytes() if inputs.exists() else
                  ''.join(' '.join(p) + '\n' for p in programs).encode()).hexdigest(),
              'limits': ['No physical NVIDIA execution; host ownership/reference-count model.',
                         'Actual Metal blits verify live guest-backed GMMU mappings after each operation.',
                         'Sparse/unmapped slots use a cleared result; this fixture does not read sparse zero blocks on Metal.',
                         'Complete Horizon error codes, permissions and concurrent CPU remapping are not certified.']}
    if run.returncode:
        report['runner_log_tail'] = runner_log[-6000:]
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return bool(failures) or run.returncode != 0 or len(observed) != len(expected)


if __name__ == '__main__':
    raise SystemExit(main())
