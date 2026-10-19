#!/usr/bin/env python3
"""Audit Maxwell encodings and compare channel policy with real Metal work.

The header proves encodings, not cache side effects or Hydra error policy.
No physical NVIDIA device executes. MEMBAR is checked against actual GPU
writes and an ordered completion publisher, not a Python timing simulation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--nvidia-header', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--control', action='store_true')
    args = parser.parse_args()
    definitions = {
        'NVB06F_MEM_OP_D': 0x34,
        'NVB06F_MEM_OP_D_OPERATION_MEMBAR': 5,
        'NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE': 9,
        'NVB06F_MEM_OP_D_OPERATION_L2_PEERMEM_INVALIDATE': 13,
        'NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE': 14,
        'NVB06F_MEM_OP_D_OPERATION_L2_CLEAN_COMPTAGS': 15,
        'NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY': 16,
        'NVB06F_ILLEGAL': 4,
        'NVB06F_SET_OBJECT': 0,
    }
    source = args.nvidia_header.read_text()
    for name, expected in definitions.items():
        match = re.search(r'^#define\s+' + name + r'\s+\(?(0x[\da-fA-F]+|\d+)\)?\s*$', source, re.M)
        if not match or int(match[1], 0) != expected:
            raise ValueError('NVIDIA header mismatch: ' + name)
    for name, field in (('NVB06F_MEM_OP_D_OPERATION', '31:27'),
                        ('NVB06F_MEM_OP_D_OPERAND_HIGH', '7:0')):
        if not re.search(r'^#define\s+' + name + r'\s+' + field + r'\s*$', source, re.M):
            raise ValueError('NVIDIA bit field mismatch: ' + name)
    rng = random.Random(0xb06f34)
    cases = [(0xd, (op << 27) | operand, rng.randrange(1, 256))
             for op in range(32) for operand in (0, 1, 0x7f, 0x80, 0xff)]
    # Unsupported puller methods, explicit ILLEGAL, and unbound engine access.
    unsupported = (1, 3, *range(0xf, 0x14), *range(0x15, 0x1a),
                   *range(0x1f, 0x40), 0x40, 0xd00)
    cases += [(method, rng.getrandbits(32), rng.randrange(1, 256))
              for method in unsupported for _ in range(4)]
    # GPFIFO is a channel class, not one of Hydra's bindable engine classes.
    cases += [(0, 0xb06f, fill) for fill in (1, 0x55, 0xaa, 0xff)]
    with tempfile.TemporaryDirectory(prefix='hydra-channel-') as directory:
        inputs, outputs, log = (Path(directory) / name for name in ('cases', 'results', 'runner.log'))
        inputs.write_text(''.join(' '.join(map(str, case)) + '\n' for case in cases))
        with log.open('w') as stream:
            run = subprocess.run([str(args.binary.resolve()), '--channel-method-cases', str(inputs), str(outputs)],
                                 env=dict(os.environ, MTL_DEBUG_LAYER='1', HYDRA_HOT_RELOAD='0'),
                                 stdout=stream, stderr=subprocess.STDOUT, timeout=120)
        rows = [tuple(map(int, line.split())) for line in outputs.read_text().splitlines()] if outputs.exists() else []
        runner_log = log.read_text()
    failures = []
    for index, (method, argument, fill) in enumerate(cases):
        supported = method == 0xd and argument >> 27 in (5, 9, 13, 14, 15, 16)
        barrier = supported and argument >> 27 == 5
        expected = (0 if supported or args.control else 25,
                    int(barrier), int(barrier), fill * 0x01010101 if barrier else 0)
        observed = rows[index] if index < len(rows) else None
        if observed != expected:
            failures.append({'case': index, 'input': cases[index], 'expected': expected, 'observed': observed})
    report = {
        'cases': len(cases), 'passed': len(cases) - len(failures),
        'runner_exit_code': run.returncode, 'observed_rows': len(rows),
        'header_sha256': hashlib.sha256(args.nvidia_header.read_bytes()).hexdigest(),
        'header_values_audited': len(definitions), 'header_fields_audited': 2,
        'control': args.control, 'failures': failures[:12],
        'limits': ['NVIDIA header encoding audit; no physical NVIDIA execution.',
                   'Fault 25 for unsupported commands is conservative Hydra policy.',
                   'Actual Metal blit writes plus an ordered publisher test MEMBAR visibility.',
                   'Accepted TLB/L2 opcodes are existing host policy; cache side effects and operands are not certified.'],
    }
    if run.returncode:
        report['runner_log_tail'] = runner_log[-4000:]
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return bool(failures) or run.returncode != 0 or len(rows) != len(cases)


if __name__ == '__main__':
    raise SystemExit(main())
