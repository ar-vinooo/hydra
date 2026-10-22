"""Build the production optimization policy and verify incremental invalidation."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from gpu_optimization_gate import source_fingerprint, write_receipt


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"),
                     "CMake and Ninja are required")
class IncrementalBuildTests(unittest.TestCase):
    def test_policy_edits_preserve_pch_and_reject_stale_receipts(self):
        with tempfile.TemporaryDirectory(prefix="hydra-incremental-") as directory:
            root = Path(directory)
            for name in ("cmake/GpuOptimizationGate.cmake",
                         "src/core/hw/tegra_x1/gpu/optimization_policy.cpp",
                         "src/core/hw/tegra_x1/gpu/optimization_policy.hpp",
                         "tests/optimization_policy_tests.cpp"):
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / name, destination)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.16)\n'
                'project(policy_fixture LANGUAGES CXX)\n'
                'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n'
                'option(HYDRA_HOT_RELOAD "Hot reload fixture" OFF)\n'
                'add_subdirectory(src/core)\n'
                'add_executable(policy-probe tests/optimization_policy_tests.cpp)\n'
                'target_link_libraries(policy-probe PRIVATE hydra-core)\n')
            (root / "src/core/CMakeLists.txt").write_text(
                'add_library(hydra-core STATIC\n'
                '  hw/tegra_x1/gpu/optimization_policy.cpp\n'
                '  hw/tegra_x1/gpu/unrelated.cpp)\n'
                'target_compile_features(hydra-core PUBLIC cxx_std_17)\n'
                'target_include_directories(hydra-core PUBLIC "${CMAKE_SOURCE_DIR}/src")\n'
                'target_precompile_headers(hydra-core PRIVATE <string>)\n'
                'include("${CMAKE_SOURCE_DIR}/cmake/GpuOptimizationGate.cmake")\n')
            source = root / "src/core/hw/tegra_x1/gpu/unrelated.cpp"
            source.write_text("int fixture_value() { return 1; }\n")
            build = root / "build"

            def run(*command, env=None):
                result = subprocess.run(command, cwd=root, env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout)
                return result.stdout

            run("cmake", "-S", str(root), "-B", str(build), "-G", "Ninja")
            run("cmake", "--build", str(build), "--target", "policy-probe", "-j", "2")
            pch_files = [path for path in build.rglob("cmake_pch.*")
                         if path.suffix in (".pch", ".gch")]
            self.assertEqual(len(pch_files), 1)
            pch_time = pch_files[0].stat().st_mtime_ns
            entries = json.loads((build / "compile_commands.json").read_text())
            for entry in entries:
                has_identity = "HYDRA_GPU_OPTIMIZATION_FINGERPRINT" in entry["command"]
                self.assertEqual(has_identity, entry["file"].endswith("optimization_policy.cpp"))

            receipt = root / "policy.receipt"
            fingerprint = source_fingerprint(root)
            self.assertEqual((build / "gpu-optimization-fingerprint.txt").read_text(),
                             fingerprint + "\n1\n")
            write_receipt(receipt, fingerprint)
            env = dict(os.environ, HYDRA_GPU_OPTIMIZATIONS="1",
                       HYDRA_GPU_OPTIMIZATION_RECEIPT=str(receipt))
            probe = str(build / ("policy-probe.exe" if os.name == "nt" else "policy-probe"))
            run(probe, "1", env=env)

            source.write_text("int fixture_value() { return 2; }\n")
            run("cmake", "--build", str(build), "--target", "policy-probe", "-j", "2")
            self.assertEqual(pch_files[0].stat().st_mtime_ns, pch_time,
                             "A covered source edit rebuilt the unrelated core PCH")
            run(probe, "0", env=env)
            write_receipt(receipt, source_fingerprint(root))
            run(probe, "1", env=env)

            run("cmake", "-S", str(root), "-B", str(build), "-DHYDRA_HOT_RELOAD=ON")
            run("cmake", "--build", str(build), "--target", "policy-probe", "-j", "2")
            self.assertEqual(pch_files[0].stat().st_mtime_ns, pch_time)
            run(probe, "0", env=env)


if __name__ == "__main__":
    unittest.main()
