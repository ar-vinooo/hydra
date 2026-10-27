#!/usr/bin/env python3
"""Independent vertex-fetch oracle; no third-party Python dependencies.

Encoding: NVIDIA clb197.h, SET_VERTEX_ATTRIBUTE_A. Numeric reference: OpenGL
4.6 core 2.3.5 and 10.2 (integer conversion and missing components). NVIDIA's
header names formats but does not itself specify every conversion equation;
agreement here is not a Maxwell hardware trace.

Run: python3 tests/vertex_reference.py <hydra-vertex-reference-tests>
     [--nvidia-header /path/to/clb197.h]
"""

import argparse
from collections import Counter
from dataclasses import dataclass
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile


# Values are taken from the primary header, not parsed from Hydra's enums.
SIZES = {
    (32, 1): 0x12, (32, 2): 0x04, (32, 3): 0x02, (32, 4): 0x01,
    (16, 1): 0x1B, (16, 2): 0x0F, (16, 3): 0x05, (16, 4): 0x03,
    (8, 1): 0x1D, (8, 2): 0x18, (8, 3): 0x13, (8, 4): 0x0A,
}
TYPES = {"snorm": 1, "unorm": 2, "sint": 3, "uint": 4,
         "uscaled": 5, "sscaled": 6, "float": 7}


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


@dataclass(frozen=True)
class Case:
    kind: str
    widths: tuple
    size: int
    swap: bool
    payload: bytes
    label: str

    @property
    def word(self):
        # Stream 3, byte offset 12, active source; exercise field extraction.
        return 3 | (12 << 7) | (self.size << 21) | (TYPES[self.kind] << 27) | (int(self.swap) << 31)

    def expected(self):
        if self.kind == "float":
            bits = self.widths[0]
            fmt = "e" if bits == 16 else "f"
            values = list(struct.unpack("<" + fmt * len(self.widths), self.payload))
        else:
            packed = int.from_bytes(self.payload, "little")
            values = []
            for bits in self.widths:
                value = packed & ((1 << bits) - 1)
                packed >>= bits
                if self.kind in ("snorm", "sint", "sscaled"):
                    value = signed(value, bits)
                if self.kind == "unorm":
                    value /= (1 << bits) - 1
                elif self.kind == "snorm":
                    value = max(-1.0, value / ((1 << (bits - 1)) - 1))
                values.append(f32(value))
        values += [0.0] * (4 - len(values))
        if len(self.widths) < 4:
            values[3] = 1.0
        if self.swap:
            values[0], values[2] = values[2], values[0]
        return [f32(value) for value in values]


def cases():
    for kind in TYPES:
        for (bits, count), size in SIZES.items():
            if (kind in ("snorm", "unorm") and bits == 32) or (kind == "float" and bits == 8):
                continue  # No corresponding Metal vertex format in Hydra.
            if kind == "float":
                patterns = [-3.0, -1.0, -0.0, 0.0, 0.001, 0.33325, 0.5, 1.0, 1.5, 65504.0]
            else:
                patterns = [0, 1, 2, (1 << (bits - 1)) - 1,
                            1 << (bits - 1), (1 << bits) - 2, (1 << bits) - 1]
            for swap in (False, True):
                for row in range(len(patterns)):
                    values = [patterns[(row + c) % len(patterns)] for c in range(count)]
                    fmt = "e" if bits == 16 else "f"
                    payload = (struct.pack("<" + fmt * count, *values) if kind == "float"
                               else b"".join(v.to_bytes(bits // 8, "little") for v in values))
                    yield Case(kind, (bits,) * count, size, swap, payload,
                               f"{kind}{bits}x{count} swap={swap} values={values}")

    # Exhaust every input byte, including exact zero, in the particle color
    # format. Distinct channels catch missing/double R/B swaps.
    for swap in (False, True):
        for value in range(256):
            payload = bytes((value, (value + 73) % 256, 255 - value, (value * 11) % 256))
            yield Case("unorm", (8,) * 4, 0x0A, swap, payload,
                       f"unorm8x4 exhaustive byte={value} swap={swap}")
    yield Case("unorm", (8,) * 4, 0x0A, False, bytes((93, 0, 255, 0)),
               "CTR trace color (93, 0, 255, 0)")

    for kind in ("snorm", "unorm"):
        for swap in (False, True):
            for row in range(8):
                values = [(0, 1, 511, 512, 1022, 1023, 93, 341)[(row + c) % 8]
                          for c in range(3)] + [row % 4]
                packed = sum(value << shift for value, shift in zip(values, (0, 10, 20, 30)))
                yield Case(kind, (10, 10, 10, 2), 0x30, swap, packed.to_bytes(4, "little"),
                           f"{kind}10_10_10_2 swap={swap} values={values}")


def audit_header(path):
    text = path.read_text()
    prefix = "NVB197_SET_VERTEX_ATTRIBUTE_A_"
    definitions = dict(re.findall(r"^#define\s+(\S+)\s+(\S+)", text, re.MULTILINE))
    expected = {"STREAM": "4:0", "SOURCE": "6:6", "OFFSET": "20:7",
                "COMPONENT_BIT_WIDTHS": "26:21", "NUMERICAL_TYPE": "29:27",
                "SWAP_R_AND_B": "31:31", "COMPONENT_BIT_WIDTHS_A2B10G10R10": 0x30}
    for (bits, count), size in SIZES.items():
        name = "_".join(f"{component}{bits}" for component in "RGBA"[:count])
        expected["COMPONENT_BIT_WIDTHS_" + name] = size
    for name, value in TYPES.items():
        expected["NUMERICAL_TYPE_NUM_" + name.upper()] = value
    for name, value in expected.items():
        actual = definitions.get(prefix + name)
        if isinstance(value, int):
            actual = int(actual, 0) if actual else None
        if actual != value:
            raise ValueError(f"NVIDIA header mismatch: {name}: {actual} != {value}")
    print(f"NVIDIA header: {len(expected)} fields/enum values agree")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--nvidia-header", type=Path)
    args = parser.parse_args()
    if args.nvidia_header:
        audit_header(args.nvidia_header)
    checks = list(cases())
    with tempfile.TemporaryDirectory(prefix="hydra-vertex-reference-") as directory:
        source = Path(directory) / "cases.txt"
        result = Path(directory) / "result.txt"
        source.write_text("".join(f"{case.word:08x} {case.payload.ljust(16, bytes([0xAD])).hex()}\n"
                                  for case in checks))
        environment = {**os.environ, "MTL_DEBUG_LAYER": "1"}
        # A diagnostic bisect switch would deliberately disable the behavior
        # this reference test measures.
        environment.pop("HYDRA_NO_VERTEX_SWAP", None)
        subprocess.run([str(args.binary.resolve()), str(source), str(result)],
                       env=environment, check=True)
        rows = result.read_text().splitlines()
    if len(rows) != len(checks):
        raise ValueError(f"Expected {len(checks)} results, got {len(rows)}")
    failures = Counter()
    for case, row in zip(checks, rows):
        actual = struct.unpack("<4f", bytes.fromhex(row))
        expected = case.expected()
        # A few float32 ULPs accommodate normalization and varying interpolation.
        if not all(math.isfinite(a) and abs(a - e) <= max(2e-7, abs(e) * 3e-7)
                   for a, e in zip(actual, expected)):
            failures[case.kind] += 1
            if sum(failures.values()) <= 16:
                print(f"MISMATCH {case.label}: expected={expected} actual={actual}")
        if case.label.startswith("CTR"):
            print(f"{case.label}: expected={expected} actual={actual}")
    bad = sum(failures.values())
    print(f"{len(checks) - bad}/{len(checks)} vertex cases match Python reference")
    if failures:
        print(f"Failures by type: {dict(failures)}")
    return int(bad != 0)


if __name__ == "__main__":
    raise SystemExit(main())
