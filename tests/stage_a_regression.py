#!/usr/bin/env python3
"""Run instruction-stage regressions; passing does not certify stage closure."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build/swiftui-release'))
    parser.add_argument('--nvidia-header', type=Path, required=True)
    parser.add_argument('--nvidia-manual', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    binaries = args.build.resolve() / 'bin'
    shutdown = binaries / 'hydra-shutdown-tests.app/Contents/MacOS/hydra-shutdown-tests'
    engine = binaries / 'hydra-gpu-engine-tests'
    env = dict(os.environ, MTL_DEBUG_LAYER='1', HYDRA_HOT_RELOAD='0')
    cases = []
    for selector in ('stage-a-memory', 'shared-memory-lifetime', 'gpfifo-input',
                     'gpu-address-space', 'gpu-as-binding', 'gpu-as-lifecycle',
                     'gpu-heap-resize', 'video-events', 'pfifo-decode',
                     'channel-scheduling', 'channel-faults', 'gpu-process-cancel',
                     'semaphore-shutdown'):
        cases.append((selector, [str(shutdown), '--check-' + selector], 0, None))
    for name, command in (
        ('system-teardown', [str(shutdown)]),
        ('gpu-engine', [str(engine)]),
        ('texture-cache', [str(binaries / 'hydra-texture-cache-tests')]),
        ('raster-state', [str(binaries / 'hydra-raster-state-tests')]),
        ('mmu-lookup', [str(binaries / 'hydra-xci-tests'), '--check-mmu-lookup']),
    ):
        cases.append((name, command, 0, None))
    scripts = (
        ('device-shared', 'device_shared_reference.py', shutdown, []),
        ('shared-memory', 'shared_memory_lifetime_reference.py', shutdown, []),
        ('semaphore', 'semaphore_reduction_reference.py', engine,
         ['--nvidia-header', str(args.nvidia_header), '--nvidia-manual', str(args.nvidia_manual)]),
        ('channel-method', 'channel_method_reference.py', engine,
         ['--nvidia-header', str(args.nvidia_header)]),
    )
    for name, script, binary, extra in scripts:
        for control in (False, True):
            label = name + ('-control' if control else '')
            report = args.output / (label + '.json')
            command = [sys.executable, str(Path(__file__).parent / script), str(binary),
                       '--report', str(report), *extra, *(['--control'] if control else [])]
            cases.append((label, command, int(control), report))
    results = []
    for name, command, expected, report in cases:
        started = time.monotonic()
        with (args.output / (name + '.log')).open('w') as log:
            try:
                run = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=180)
                code = run.returncode
            except (subprocess.TimeoutExpired, OSError) as error:
                log.write(str(error))
                code = -1
        passed = code == expected
        if report:
            data = json.loads(report.read_text()) if report.exists() else {}
            passed = passed and data.get('runner_exit_code') == 0
            if expected:
                passed = passed and bool(data.get('failures'))
            else:
                passed = passed and not data.get('failures')
        results.append({'name': name, 'passed': passed, 'exit_code': code,
                        'expected_exit': expected, 'seconds': round(time.monotonic() - started, 3)})
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)
    summary = {'checks': len(results), 'passed': sum(r['passed'] for r in results),
               'metal_validation': True, 'hot_reload': False, 'results': results,
               'closure': 'Regression results do not close stage A; see the stage A exit audit.'}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    return not all(r['passed'] for r in results)


if __name__ == '__main__':
    raise SystemExit(main())
