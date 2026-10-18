#!/bin/bash
# Builds the reference-test binaries and runs every Python-vs-Metal reference check.
# Usage: tests/run_python_references.sh [build-dir]   (default build/swiftui-release)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build/swiftui-release}"
NV="${HYDRA_NVIDIA_HEADERS:-$ROOT/build/nvidia-headers}"
BASE=https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes

mkdir -p "$NV"
[ -s "$NV/clb197.h" ] || curl -fsSL -o "$NV/clb197.h" "$BASE/3d/clb197.h" || { echo "download clb197.h failed"; exit 2; }
[ -s "$NV/clb1c0.h" ] || curl -fsSL -o "$NV/clb1c0.h" "$BASE/compute/clb1c0.h" || { echo "download clb1c0.h failed"; exit 2; }

ninja -C "$BUILD" hydra-shader-cache-reference-tests hydra-maxwell-alu-reference-tests \
  hydra-color-target-format-tests hydra-blend-reference-tests hydra-vertex-reference-tests \
  hydra-depth-stencil-reference-tests >/dev/null || { echo "build failed: run ninja -C $BUILD"; exit 2; }

BIN="$BUILD/bin"; T="$ROOT/tests"; fail=0
run() { echo "== $1"; shift; "$@" 2>&1 | grep -v "Context leak" | tail -${TAIL:-3}; [ "${PIPESTATUS[0]}" -eq 0 ] || fail=1; }
run shader-cache python3 "$T/shader_cache_reference.py" "$BIN/hydra-shader-cache-reference-tests" --nvidia-3d "$NV/clb197.h" --nvidia-compute "$NV/clb1c0.h" --report /dev/null
TAIL=1 run maxwell-alu python3 "$T/maxwell_alu_reference.py" "$BIN/hydra-maxwell-alu-reference-tests"
TAIL=1 run maxwell-texture python3 "$T/maxwell_texture_reference.py" "$BIN/hydra-maxwell-alu-reference-tests"
run color-target-formats python3 "$T/color_target_format_reference.py" "$BIN/hydra-color-target-format-tests" --header "$NV/clb197.h"
TAIL=1 run msaa-registers python3 "$T/msaa_register_audit.py" "$NV/clb197.h"
TAIL=1 run blend python3 "$T/blend_reference.py" "$BIN/hydra-blend-reference-tests"
TAIL=1 run vertex python3 "$T/vertex_reference.py" "$BIN/hydra-vertex-reference-tests"
TAIL=1 run depth-stencil python3 "$T/depth_stencil_reference.py" "$BIN/hydra-depth-stencil-reference-tests"
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
