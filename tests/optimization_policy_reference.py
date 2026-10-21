"""Exercise the production policy in fresh processes for each receipt case."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1]).resolve())
    fingerprint = sys.argv[2]
    with tempfile.TemporaryDirectory() as directory:
        target = Path(directory) / "gate.receipt"
        for name, requested, contents, expected in (
                ("missing", "1", None, False),
                ("wrong-source", "1", "HYDRA_GPU_P0_V1\nwrong\n", False),
                ("wrong-schema", "1", "unverified\n" + fingerprint + "\n", False),
                ("truncated", "1", "HYDRA_GPU_P0_V1\n", False),
                ("extra-record", "1", "HYDRA_GPU_P0_V1\n" + fingerprint + "\nextra\n", False),
                ("disabled", "0", "HYDRA_GPU_P0_V1\n" + fingerprint + "\n", False),
                ("no-request", "", "HYDRA_GPU_P0_V1\n" + fingerprint + "\n", False),
                ("verified", "1", "HYDRA_GPU_P0_V1\n" + fingerprint + "\n", True)):
            target.unlink(missing_ok=True)
            if contents is not None:
                target.write_text(contents, encoding="utf-8", newline="\n")
            environment = dict(os.environ, HYDRA_GPU_OPTIMIZATIONS=requested,
                               HYDRA_GPU_OPTIMIZATION_RECEIPT=str(target))
            subprocess.run([binary, str(int(expected))], env=environment, check=True)
            print(f"PASS: {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
