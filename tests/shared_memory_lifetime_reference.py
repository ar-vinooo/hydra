#!/usr/bin/env python3
"""Independent shared-memory ownership model vs production kernel + real Metal.

This tests host memory lifetime, not a NVIDIA ISA instruction or physical GPU.
Object references follow handles/mappings; backing retirement until process
cleanup is Hydra's conservative policy. Every program includes a gated Metal
read through a real GMMU mapping, with no-copy source memory.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile


def model(program, control=False):
    handles = [[True, True], [True, True]]
    mappings = [[None, None], [None, None]]
    pinned = [set(), set()]
    alive = [True, True]
    pending = False
    rows = []
    for token in program:
        success = False
        kind = token[0]
        if kind == 'B':
            pending = success = True
        elif kind == 'F':
            success, pending = pending, False
        else:
            p = int(token[1])
            if alive[p]:
                if kind == 'D':
                    assert not pending
                    alive[p] = False
                    handles[p] = [False, False]
                    mappings[p] = [None, None]
                    pinned[p].clear()
                    success = True
                else:
                    obj = int(token[-1])
                    if kind == 'H':
                        success = not handles[p][obj] and handles[1-p][obj]
                        if success:
                            handles[p][obj] = True
                    elif handles[p][obj]:
                        if kind == 'C':
                            handles[p][obj] = False
                            success = True
                        elif kind == 'M':
                            slot = int(token[2])
                            success = mappings[p][slot] is None
                            if success:
                                mappings[p][slot] = obj
                                pinned[p].add(obj)
                        elif kind == 'U':
                            slot = int(token[2])
                            success = mappings[p][slot] == obj
                            if success:
                                mappings[p][slot] = None
                        else:
                            assert kind in 'VNA'
        destroyed = [not any(h[obj] for h in handles) and
                     not any(obj in maps for maps in mappings) for obj in range(2)]
        backing = [not destroyed[obj] or (not control and any(obj in pins for pins in pinned))
                   for obj in range(2)]
        mask = lambda flags: sum(int(flag) << index for index, flag in enumerate(flags))
        flat_maps = [obj for maps in mappings for obj in maps]
        rows.append((int(success), mask(destroyed), mask(backing),
                     mask(obj is not None for obj in flat_maps), mask(alive),
                     *(0 if obj is None else (0x35353535 if obj == 0 else 0x7a7a7a7a)
                       for obj in flat_maps)))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--control', action='store_true')
    args = parser.parse_args()
    programs = [
        'M000 B C00 C10 F D0 D1'.split(),
        'M000 B U000 C00 C10 F D0 D1'.split(),
        'M000 B M100 C00 C10 F D0 D1'.split(),
        'M000 B M010 U000 C00 C10 F D0 D1'.split(),
        'M000 B V000 U001 M001 U000 M001 F D0 D1'.split(),
        'M000 B U100 C00 H00 C10 C00 F D1 D0'.split(),
    ]
    for order in itertools.permutations(('U000', 'C00', 'C10')):
        programs.append(['M000', 'B', *order, 'F', 'D0', 'D1'])
    rng = random.Random(0x53484d)
    operations = [f'{kind}{p}{slot}{obj}' for kind in 'MUVNA'
                  for p, slot, obj in itertools.product(range(2), repeat=3)]
    operations += [f'{kind}{p}{obj}' for kind in 'CH'
                   for p, obj in itertools.product(range(2), repeat=2)]
    for _ in range(188):
        program = ['M000', 'B', *rng.choices(operations, k=36), 'F']
        program += rng.choices(operations + ['D0', 'D1'], k=12)
        program += ['D0', 'D1']
        programs.append(program)
    expected = [row for program in programs for row in model(program, args.control)]
    with tempfile.TemporaryDirectory(prefix='hydra-shared-memory-') as directory:
        inputs, outputs, log = (Path(directory) / name for name in ('programs', 'results', 'runner.log'))
        inputs.write_text(''.join(' '.join(program) + '\n' for program in programs))
        with log.open('w') as stream:
            run = subprocess.run([str(args.binary.resolve()), '--shared-memory-cases', str(inputs), str(outputs)],
                                 env=dict(os.environ, MTL_DEBUG_LAYER='1', HYDRA_HOT_RELOAD='0'),
                                 stdout=stream, stderr=subprocess.STDOUT, timeout=180)
        rows = [tuple(map(int, line.split())) for line in outputs.read_text().splitlines()] if outputs.exists() else []
        runner_log = log.read_text()
    failures = [{'step': index, 'expected': row, 'observed': rows[index] if index < len(rows) else None}
                for index, row in enumerate(expected) if index >= len(rows) or rows[index] != row]
    report = {
        'programs': len(programs), 'steps': len(expected), 'passed_steps': len(expected) - len(failures),
        'observed_steps': len(rows), 'runner_exit_code': run.returncode,
        'programs_sha256': hashlib.sha256('\n'.join(' '.join(p) for p in programs).encode()).hexdigest(),
        'control': args.control, 'failures': failures[:12],
        'limits': ['Host lifetime model, not NVIDIA ISA or physical NVIDIA execution.',
                   'Every program uses actual gated Metal blit readback of 16 KiB through a GMMU-resolved no-copy buffer.',
                   'Fixture safety guard is subtracted from observed ownership counts; GPU bytes alone do not prove lifetime.',
                   'Backing retention until process cleanup is conservative Hydra policy; not immediate unmap reclamation.',
                   'Concurrent handle-table operations and complete Horizon permission/error semantics are outside this fixture.'],
    }
    if run.returncode:
        report['runner_log_tail'] = runner_log[-6000:]
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return bool(failures) or run.returncode != 0 or len(rows) != len(expected)


if __name__ == '__main__':
    raise SystemExit(main())
