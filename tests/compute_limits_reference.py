"""Check production C++ compute dimension guards against Python integers."""
import itertools
from pathlib import Path
import random
import subprocess
import sys
import tempfile


def main():
    binary = Path(sys.argv[1]).resolve()
    maximum_u64 = (1 << 64) - 1
    edges = [0, 1, 2, 16, 32, 1024, 1 << 32, (1 << 63), maximum_u64]
    cases = list(itertools.product(edges, edges, edges, [0, 1, 1024, maximum_u64]))
    rng = random.Random(0xB1C0)
    cases.extend(
        (rng.getrandbits(64), rng.getrandbits(64), rng.getrandbits(64), rng.getrandbits(64))
        for _ in range(20000)
    )
    cases.extend((rng.randrange(1, 65), rng.randrange(1, 65), rng.randrange(1, 65), 1024)
                 for _ in range(4000))
    with tempfile.TemporaryDirectory(prefix="hydra-compute-limits-") as directory:
        source = Path(directory) / "cases.txt"
        result = Path(directory) / "actual.txt"
        source.write_text("".join(" ".join(map(str, case)) + "\n" for case in cases), encoding="utf-8")
        subprocess.run([str(binary), str(source), str(result)], check=True)
        rows = result.read_text(encoding="utf-8").splitlines()
        if len(rows) != len(cases):
            raise AssertionError("The C++ runner did not execute every case")
        for case, actual in zip(cases, rows, strict=True):
            x, y, z, limit = case
            product = x * y * z  # Arbitrary precision: independent overflow oracle.
            expected = f"{int(x > 0 and y > 0 and z > 0 and product <= limit)} " \
                       f"{int(x > 0 and y > 0 and z > 0 and product <= maximum_u64)} {int(product == 0)}"
            if actual != expected:
                raise AssertionError(f"{case}: C++ {actual!r}, expected {expected!r}")
    print(f"PASS: {len(cases)} compute dimension cases against Python arbitrary-precision integers")


if __name__ == "__main__":
    main()
