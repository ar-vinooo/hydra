"""Build/run the real portable C++ checks; optionally require native Metal checks.

Usage: uv run --no-project python tools/validate_gpu_correctness.py
macOS gate: python3 tools/validate_gpu_correctness.py --require-metal --build-dir build/swiftui-release
"""
import argparse
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import os
from gpu_optimization_gate import check_build, source_fingerprint, write_receipt

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="Path to a C++23 clang++/g++ executable")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/swiftui-release")
    parser.add_argument("--require-metal", action="store_true", help="Fail unless native Metal checks also pass")
    parser.add_argument("--portable-only", action="store_true", help="Run only portable checks, even on macOS")
    parser.add_argument("--timeout-seconds", type=int, default=900, help="Maximum duration of each build/test process")
    parser.add_argument("--enable-optimizations", action="store_true", help="Run baseline and optimized native checks, then issue a source-bound activation receipt")
    args = parser.parse_args()
    if args.require_metal and args.portable_only:
        parser.error("--require-metal and --portable-only are mutually exclusive")
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    if args.enable_optimizations and (args.portable_only or sys.platform != "darwin"):
        parser.error("--enable-optimizations requires native macOS checks")
    out = ROOT / "build/correctness"
    out.mkdir(parents=True, exist_ok=True)
    summary = {"schema": 1, "platform": platform.system(), "status": "running", "checks": []}
    trial_receipt = out / "gpu-optimization-trial.receipt"
    published_receipt = out / "gpu-optimization.receipt"

    def run(name, command, env=None):
        print(f"Running {name}", flush=True)
        log = out / f"{name}.log"
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT,
                                    env=env, timeout=args.timeout_seconds)
        summary["checks"].append({"name": name, "returncode": result.returncode, "log": str(log)})
        if result.returncode:
            print(log.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)
            raise RuntimeError(f"{name} failed with exit code {result.returncode}")

    try:
        initial_fingerprint = source_fingerprint(ROOT)
        if args.enable_optimizations:
            published_receipt.unlink(missing_ok=True)
        if args.compiler:
            compiler = [args.compiler]
        elif native := shutil.which("clang++") or shutil.which("g++"):
            compiler = [native]
        elif uv := shutil.which("uv"):
            compiler = [uv, "run", "--no-project", "--with", "ziglang==0.16.0", "python", "-m", "ziglang", "c++"]
        else:
            raise RuntimeError("C++23 compiler unavailable; install clang++/g++ or uv, or pass --compiler")
        suffix = ".exe" if sys.platform == "win32" else ""
        common = ["-std=c++23", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion",
                  "-Wsign-conversion", "-Wshadow", "-Wdouble-promotion", "-Wundef",
                  "-Wnon-virtual-dtor", "-Wold-style-cast", "-Werror", "-Isrc", "-Isrc/ztd/src"]
        if "ziglang" in compiler:
            common.append("-Wno-nullability-completeness")
        versions = out / f"content-version-tests{suffix}"
        run("compile-content-versions", compiler + common + ["-include", "tests/content_version_test_types.hpp",
            "tests/content_version_tests.cpp", "src/core/hw/tegra_x1/gpu/resource/content_version.cpp", "-o", str(versions)])
        run("content-versions", [str(versions)])
        ranges = out / f"range-set-tests{suffix}"
        run("compile-range-set", compiler + common + ["-include", "tests/content_version_test_types.hpp",
            "tests/range_set_tests.cpp", "src/core/hw/tegra_x1/gpu/resource/content_version.cpp", "-o", str(ranges)])
        run("range-set", [str(ranges)])
        policy = out / f"optimization-policy-tests{suffix}"
        policy_fixture = "portable-policy-fixture"
        run("compile-optimization-policy", compiler + common + [
            '-DHYDRA_GPU_OPTIMIZATION_FINGERPRINT="' + policy_fixture + '"',
            "-DHYDRA_GPU_OPTIMIZATION_BUILD_ALLOWED=1", "tests/optimization_policy_tests.cpp",
            "src/core/hw/tegra_x1/gpu/optimization_policy.cpp", "-o", str(policy)])
        run("optimization-policy", [sys.executable, "-B", str(ROOT / "tests/optimization_policy_reference.py"),
            str(policy), policy_fixture])
        limits = out / f"compute-limits-tests{suffix}"
        run("compile-compute-limits", compiler + common + ["tests/compute_limits_tests.cpp", "-o", str(limits)])
        run("compute-limits", [sys.executable, str(ROOT / "tests/compute_limits_reference.py"), str(limits)])
        run("trace-checker", [sys.executable, "-B", str(ROOT / "tests/gpu_trace_tests.py")])
        run("guest-events", [sys.executable, "-B", str(ROOT / "tests/guest_event_tests.py")])
        run("diff-check", ["git", "diff", "--check"])
        if sys.platform == "darwin" and not args.portable_only:
            build = args.build_dir.resolve()
            if not (build / "CMakeCache.txt").is_file():
                raise RuntimeError(f"Configure the macOS app build first: {build}")
            targets = ["hydra-content-version-tests", "hydra-resource-registry-tests",
                       "hydra-gpu-engine-tests", "hydra-shutdown-tests"]
            run("metal-build", ["cmake", "--build", str(build), "--target", *targets, "hydra-gpu-replay", "hydra-guest-replay", "-j", "6"])
            environment = dict(os.environ, MTL_DEBUG_LAYER="1", HYDRA_GPU_OPTIMIZATIONS="0")
            run("baseline-optimization-policy", [str(build / "bin/hydra-gpu-engine-tests"),
                "--check-optimization-policy"], environment)
            for target in targets[:-1]:
                run(target, [str(build / "bin" / target)], environment)
            run("compute-ir-profile", [str(build / "bin/hydra-gpu-engine-tests"), "--check-compute-ir-profile"], environment)
            run("gpu-ordering-stress", [str(build / "bin/hydra-shutdown-tests"),
                "--check-gpu-ordering-stress", "32", "1"], environment)
            for selector in ["--check-pfifo-decode", "--check-channel-scheduling", "--check-gpu-process-cancel", "--check-command-buffer-batching"]:
                run(selector.removeprefix("--"), [str(build / "bin/hydra-shutdown-tests"), selector], environment)
            if args.enable_optimizations:
                fingerprint = check_build(ROOT, build)
                if fingerprint != initial_fingerprint:
                    raise RuntimeError("Sources changed during baseline validation")
                write_receipt(trial_receipt, fingerprint)
                optimized = dict(environment, HYDRA_GPU_OPTIMIZATIONS="1",
                                 HYDRA_GPU_OPTIMIZATION_RECEIPT=str(trial_receipt))
                run("optimized-policy", [str(build / "bin/hydra-gpu-engine-tests"), "--check-optimization-policy"], optimized)
                run("optimized-gpu-engines", [str(build / "bin/hydra-gpu-engine-tests")], optimized)
                run("optimized-compute-profile", [str(build / "bin/hydra-gpu-engine-tests"), "--check-compute-ir-profile"], optimized)
                for selector in ["--check-pfifo-decode", "--check-channel-scheduling", "--check-gpu-process-cancel", "--check-command-buffer-batching"]:
                    run("optimized-" + selector.removeprefix("--"), [str(build / "bin/hydra-shutdown-tests"), selector], optimized)
                run("optimized-ordering-stress", [str(build / "bin/hydra-shutdown-tests"),
                    "--check-gpu-ordering-stress", "1000", "1"], optimized)
                if check_build(ROOT, build) != initial_fingerprint:
                    raise RuntimeError("Sources changed during optimized validation")
                write_receipt(published_receipt, fingerprint)
                summary["optimization_receipt"] = str(published_receipt)
            summary["status"] = "passed"
        elif args.require_metal:
            raise RuntimeError("Native Metal validation requires macOS; portable success cannot satisfy this gate")
        else:
            summary["status"] = "portable_passed_metal_unverified"
        print(summary["status"], flush=True)
        return 0
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        summary["status"] = "failed"
        summary["error"] = str(error)
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        trial_receipt.unlink(missing_ok=True)
        if args.enable_optimizations and summary["status"] != "passed":
            published_receipt.unlink(missing_ok=True)
        (out / "validation.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
