"""Run saved guest GPU work with replay delays and live scheduler stress.

This runs native executables only when explicitly invoked. It records failures
and timeouts instead of treating an incomplete run as successful validation.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from compare_gpu_replays import compare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="Complete exported .hge capture")
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--scheduler", required=True, type=Path, help="hydra-shutdown-tests executable")
    parser.add_argument("--reference", type=Path, help="Optional separately collected native/hardware summary")
    parser.add_argument("--iterations", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--max-delay-us", type=int, default=500)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    parser.add_argument("--output-dir", type=Path, default=Path("build/correctness/ordering-stress"))
    args = parser.parse_args()
    summary = {"schema": 1, "status": "failed", "runs": [], "scheduler": "not_run"}
    try:
        if not 1 <= args.iterations <= 100000 or not 1 <= args.seed < (1 << 64) - args.iterations:
            raise ValueError("invalid iteration count or seed")
        if not 0 <= args.max_delay_us <= 1000000 or args.timeout_seconds <= 0:
            raise ValueError("invalid delay or timeout")
        # A fresh directory prevents an earlier successful summary from being
        # mistaken for output from a failed process in this run.
        args.output_dir.mkdir(parents=True, exist_ok=False)
        environment = dict(os.environ, MTL_DEBUG_LAYER="1")
        def run(name, command):
            result = subprocess.run(command, capture_output=True, text=True,
                                    timeout=args.timeout_seconds, env=environment)
            (args.output_dir / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise RuntimeError(f"{name} failed with exit {result.returncode}")
        baseline_path = args.output_dir / "baseline.json"
        run("baseline", [str(args.replay.resolve()), str(args.capture.resolve()), "--output", str(baseline_path.resolve())])
        baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
        compare(baseline, baseline)  # Also requires nonempty output evidence.
        if args.reference:
            result = compare(json.loads(args.reference.read_text(encoding="utf-8")), baseline)
            summary["reference_comparison"] = result
            if result["status"] != "equal":
                raise RuntimeError("baseline differs from the supplied reference")
        for iteration in range(args.iterations):
            seed = args.seed + iteration
            target = args.output_dir / f"replay-{seed}.json"
            run(f"replay-{seed}", [str(args.replay.resolve()), str(args.capture.resolve()),
                "--delay-seed", str(seed), "--max-delay-us", str(args.max_delay_us),
                "--output", str(target.resolve())])
            result = compare(baseline, json.loads(target.read_text(encoding="utf-8")))
            summary["runs"].append({"seed": seed, **result})
            if result["status"] != "equal":
                raise RuntimeError(f"replay diverged at delay seed {seed}")
        run("scheduler", [str(args.scheduler.resolve()), "--check-gpu-ordering-stress",
                          str(args.iterations), str(args.seed)])
        summary["scheduler"] = "passed"
        summary["status"] = "passed"
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        summary["error"] = str(error)
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        # Never overwrite an unrelated prior run when directory creation failed.
        if 'baseline_path' in locals():
            (args.output_dir / "stress.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
