#!/usr/bin/env python3
"""Independent scalar reference for Hydra depth/stencil output.

NVIDIA clb197.h supplies the OGL/D3D encodings; OpenGL 4.6 core sections
17.3.3/17.3.4 supply the stencil/depth rules. Execution uses Hydra's renderer,
not a second CPU implementation of its cache. This is not a GM20B trace.
"""

import argparse
from collections import Counter
from dataclasses import astuple, dataclass, replace
import itertools
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

COMPARES = ("NEVER", "LESS", "EQUAL", "LEQUAL", "GREATER", "NOTEQUAL", "GEQUAL", "ALWAYS")
OPS = ("KEEP", "ZERO", "REPLACE", "INCRSAT", "DECRSAT", "INVERT", "INCR", "DECR")
OGL_OPS = (0x1E00, 0, 0x1E01, 0x1E02, 0x1E03, 0x150A, 0x8507, 0x8508)


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def compare(raw, a, b):
    index = raw - 0x200 if 0x200 <= raw <= 0x207 else raw - 1
    return (False, a < b, a == b, a <= b, a > b, a != b, a >= b, True)[index]


def stencil_op(raw, value, ref):
    index = OGL_OPS.index(raw) if raw in OGL_OPS else raw - 1
    return (value, 0, ref & 255, min(value + 1, 255), max(value - 1, 0),
            value ^ 255, (value + 1) & 255, (value - 1) & 255)[index]


@dataclass(frozen=True)
class Draw:
    z: float = 0.5
    depth_test: bool = True
    depth_write: bool = True
    depth_func: int = 8
    stencil_test: bool = False
    stencil_func: int = 8
    ref: int = 0
    read_mask: int = 255
    write_mask: int = 255
    fail: int = 1
    depth_fail: int = 1
    passed: int = 1


@dataclass(frozen=True)
class Case:
    group: str
    depth: float
    stencil: int
    split_encoder: bool
    draws: tuple

    def line(self):
        values = (self.depth, self.stencil, self.split_encoder,
                  *(v for draw in self.draws for v in astuple(draw)))
        return " ".join(str(int(v)) if isinstance(v, bool) else str(v) for v in values)

    def expected(self):
        depth, stencil, color = f32(self.depth), self.stencil & 255, (0, 0, 0, 0)
        for index, draw in enumerate(self.draws):
            stencil_pass = not draw.stencil_test or compare(
                draw.stencil_func, draw.ref & draw.read_mask & 255,
                stencil & draw.read_mask & 255)
            depth_pass = not draw.depth_test or compare(draw.depth_func, f32(draw.z), depth)
            if draw.stencil_test:
                op = draw.fail if not stencil_pass else draw.depth_fail if not depth_pass else draw.passed
                value = stencil_op(op, stencil, draw.ref)
                mask = draw.write_mask & 255
                stencil = (stencil & (255 ^ mask)) | (value & mask)
            if stencil_pass and depth_pass:
                color = (255, 0, 0, 255) if index == 0 else (0, 255, 0, 255)
                if draw.depth_test and draw.depth_write:
                    depth = f32(draw.z)
        return (*color, depth, stencil)


def cases():
    pairs = ((0.25, 0.75), (0.75, 0.25), (0.5, 0.5), (0, 1),
             (1, 0), (0.5, 0.25), (0.5, 0.75))
    codes = (*range(1, 9), *range(0x200, 0x208))
    for func, test, write, pair, split in itertools.product(
            codes, (False, True), (False, True), pairs, (False, True)):
        yield Case("depth", 0.5, 0, split,
                   tuple(Draw(z, test, write, func) for z in pair))

    for encoding, op, branch, old, mask, split in itertools.product(
            ("OGL", "D3D"), range(8), ("stencil_fail", "depth_fail", "pass"),
            (0, 1, 127, 128, 254, 255), (0, 15, 240, 255, 511), (False, True)):
        encode = lambda n: OGL_OPS[n % 8] if encoding == "OGL" else n % 8 + 1
        selected = encode(op)
        draw = Draw(stencil_test=True,
                    stencil_func=1 if branch == "stencil_fail" else 8,
                    depth_func=1 if branch == "depth_fail" else 8,
                    write_mask=mask, fail=selected if branch == "stencil_fail" else encode(op + 1),
                    depth_fail=selected if branch == "depth_fail" else encode(op + 2),
                    passed=selected if branch == "pass" else encode(op + 3))
        yield Case("stencil_operations", 0.5, old, split,
                   (replace(draw, ref=37), replace(draw, ref=222)))

    for func, (old, ref), read_mask, write_mask, split in itertools.product(
            codes, ((0, 0), (255, 255), (37, 84), (84, 37), (171, 171), (1, 0)),
            (0, 15, 240, 255, 511), (0, 0x55, 255), (False, True)):
        first = Draw(z=0.25, depth_func=2, stencil_test=True, stencil_func=func,
                     ref=ref, read_mask=read_mask, write_mask=write_mask,
                     fail=6, depth_fail=7, passed=3)
        second = replace(first, z=0.75, ref=ref ^ 255)
        yield Case("stencil_compare_masks", 0.5, old, split, (first, second))


def audit_header(path):
    definitions = dict(re.findall(r"^#define\s+(\S+)\s+(\S+)", path.read_text(), re.MULTILINE))
    expected = {}
    for i, name in enumerate(COMPARES):
        expected[f"NVB197_SET_DEPTH_FUNC_V_OGL_{name}"] = 0x200 + i
        d3d_name = {"LEQUAL": "LESSEQUAL", "GEQUAL": "GREATEREQUAL"}.get(name, name)
        expected[f"NVB197_SET_DEPTH_FUNC_V_D3D_{d3d_name}"] = i + 1
    for i, name in enumerate(OPS):
        expected[f"NVB197_SET_STENCIL_OP_FAIL_V_OGL_{name}"] = OGL_OPS[i]
        expected[f"NVB197_SET_STENCIL_OP_FAIL_V_D3D_{name}"] = i + 1
    for name, value in expected.items():
        actual = definitions.get(name)
        if actual is None or int(actual, 0) != value:
            raise ValueError(f"NVIDIA header mismatch: {name}={actual}, expected {value}")
    print(f"NVIDIA header: {len(expected)} depth/stencil enum values agree", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--nvidia-header", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.nvidia_header:
        audit_header(args.nvidia_header)
    checks = list(cases())
    with tempfile.TemporaryDirectory(prefix="hydra-depth-stencil-") as directory:
        source, result = Path(directory) / "cases.txt", Path(directory) / "results.txt"
        source.write_text("\n".join(case.line() for case in checks) + "\n")
        environment = {**os.environ, "MTL_DEBUG_LAYER": "1"}
        environment.pop("HYDRA_DEBUG_DEPTH_ALWAYS", None)
        subprocess.run([str(args.binary.resolve()), str(source), str(result)],
                       env=environment, check=True)
        lines = result.read_text().splitlines()
    if len(lines) != len(checks):
        raise ValueError(f"Expected {len(checks)} results, got {len(lines)}")
    failures, examples = Counter(), []
    for case, line in zip(checks, lines):
        r, g, b, a, depth_bits, stencil = map(int, line.split())
        depth = struct.unpack("<f", struct.pack("<I", depth_bits))[0]
        actual = (r, g, b, a, depth, stencil)
        expected = case.expected()
        if actual != expected:
            failures[case.group] += 1
            if len(examples) < 16:
                examples.append({"group": case.group, "case": case.line(),
                                 "expected": expected, "actual": actual})
    counts = Counter(case.group for case in checks)
    report = {"total": len(checks), "passed": len(checks) - sum(failures.values()),
              "groups": {name: {"total": count, "failed": failures[name]}
                         for name, count in counts.items()}, "examples": examples,
              "scope": "D32Float+S8, two draws per case, equal front/back stencil; not a Maxwell hardware trace"}
    for example in examples:
        print("MISMATCH", json.dumps(example))
    for name, count in counts.items():
        print(f"{name}: {count - failures[name]}/{count} match")
    print(f"{report['passed']}/{report['total']} depth/stencil cases match Python reference")
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
