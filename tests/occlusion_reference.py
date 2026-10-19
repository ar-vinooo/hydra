#!/usr/bin/env python3
"""Occlusion report model vs the asynchronous reports of Hydra on Metal.

Random programs of draws, reports, counter resets, submissions and encoder
switches run on the Metal renderer (hydra-gpu-engine-tests --occlusion-cases).
Python models the ZPASS counter of NVIDIA's SET_ZPASS_PIXEL_COUNT /
CLEAR_REPORT_VALUE / SET_REPORT_SEMAPHORE methods independently: the value a
report holds is the number of samples of the draws since the last reset, the
last report to a slot wins, and reports reach memory in stream order. This is
a reference model against Apple GPU execution, not a run on NVIDIA hardware;
the in-memory layout of the report (value, then timestamp) is the one tested
in tests/gpu_engine_tests.cpp.
"""
import argparse
import json
from pathlib import Path
import random
import subprocess
import tempfile

SLOTS = 48
COVERAGE = {0: 16, 1: 0, 2: 4}


def make_programs(seed, count):
    rng = random.Random(seed)
    programs = [
        # Rollover of the visibility pages inside one encoder
        'D5000 R0',
        'D3000 E D3000 R1 Z D10 R2',
        'D1 R0 Z R1 D2 R2',
        'X0 D5 R0 X1 D1 R1',
        'D1 S R0 D1 S S R1',
        'C1 D7 R0 C2 D3 R1 C0 D1 R2 Z C2 D2 r3',
    ]
    weights = [('D', 40), ('C', 8), ('R', 14), ('r', 5), ('Z', 6), ('S', 8),
               ('E', 8), ('F', 3), ('X', 4)]
    kinds = [kind for kind, weight in weights for _ in range(weight)]
    while len(programs) < count:
        ops = []
        for _ in range(rng.randint(8, 60)):
            kind = rng.choice(kinds)
            if kind == 'D':
                ops.append('D%d' % rng.choice([1, 1, 1, 2, 3, 8, 40]))
            elif kind == 'C':
                ops.append('C%d' % rng.randrange(3))
            elif kind in 'Rr':
                ops.append('%s%d' % (kind, rng.randrange(SLOTS)))
            elif kind == 'X':
                ops.append('X%d' % rng.randrange(2))
            else:
                ops.append(kind)
        programs.append(' '.join(ops))
    return programs


def model(program, control):
    """Expected slot contents: (value, has timestamp, order of its writer)."""
    counter = 0
    coverage = 16
    counting = True
    slots = [[0, False, 0] for _ in range(SLOTS)]
    order = 0
    for op in program.split():
        kind, number = op[0], int(op[1:] or 0)
        if kind == 'D' and counting:
            # The control miscounts scissored draws to show a mismatch
            sample = coverage - 1 if control and coverage == 4 else coverage
            counter += sample * max(number, 1)
        elif kind == 'C':
            coverage = COVERAGE[number]
        elif kind == 'X':
            counting = bool(number)
        elif kind == 'Z':
            counter = 0
        elif kind == 'R':
            slots[number] = [counter, True, order]
            order += 1
        elif kind == 'r':
            # A short report replaces the low word only, the timestamp stays
            # the one of the last long report
            slots[number][0] = (slots[number][0] & ~0xffffffff) | (counter & 0xffffffff)
    return slots


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--programs', type=int, default=300)
    parser.add_argument('--seed', type=int, default=2026)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--control', action='store_true',
                        help='break the model on purpose: the run must fail')
    args = parser.parse_args()

    programs = make_programs(args.seed, args.programs)
    with tempfile.TemporaryDirectory(prefix='hydra-occlusion-') as directory:
        cases, results = Path(directory) / 'cases', Path(directory) / 'results'
        cases.write_text('\n'.join(programs) + '\n')
        run = subprocess.run([str(args.binary), '--occlusion-cases', str(cases),
                              str(results)], capture_output=True, text=True)
        text = results.read_text() if results.exists() else ''

    observed, current = [], None
    for line in text.splitlines():
        fields = line.split()
        if fields[0] == 'P':
            current = []
            observed.append(current)
        else:
            current.append((int(fields[2]), int(fields[3])))

    failures = []
    for index, program in enumerate(programs):
        expected = model(program, args.control)
        if index >= len(observed):
            failures.append({'program': index, 'text': program, 'error': 'missing'})
            continue
        for slot, (value, timestamp) in enumerate(observed[index]):
            want = expected[slot][0]
            if value != want:
                failures.append({'program': index, 'text': program, 'slot': slot,
                                 'expected': want, 'metal': value})
                break
        else:
            # Reports reach memory in stream order: the timestamps of the
            # slots whose last writer was a long report never go back
            long_slots = sorted((s for s in range(SLOTS) if expected[s][1]),
                                key=lambda s: expected[s][2])
            stamps = [observed[index][s][1] for s in long_slots]
            if any(stamp == 0 for stamp in stamps) or stamps != sorted(stamps):
                failures.append({'program': index, 'text': program,
                                 'error': 'timestamps', 'stamps': stamps})

    summary = {
        'programs': len(programs),
        'passed': len(programs) - len({f['program'] for f in failures}),
        'runner_exit_code': run.returncode,
        'seed': args.seed,
        'control': args.control,
        'comparison': 'exact 64-bit counter values per report slot, last report '
                      'wins, stream-ordered timestamps',
        'failures': failures[:10],
        'limits': [
            'Apple GPU execution against a Python model; not NVIDIA hardware.',
            'The model is the counter semantics the tests encode (sum of the '
            'samples of draws since the last reset), not a hardware trace.',
            'Sample counts come from three coverage setups (16, 0 and 4 samples).',
        ],
    }
    if args.report:
        args.report.write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({k: summary[k] for k in ('programs', 'passed',
                                              'runner_exit_code', 'control')}))
    ok = run.returncode == 0 and not failures
    if args.control:
        return 0 if failures else 1
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
