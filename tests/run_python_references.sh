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
[ -s "$NV/clb197tex.h" ] || curl -fsSL -o "$NV/clb197tex.h" "$BASE/3d/clb197tex.h" || { echo "download clb197tex.h failed"; exit 2; }
[ -s "$NV/clb1c0.h" ] || curl -fsSL -o "$NV/clb1c0.h" "$BASE/compute/clb1c0.h" || { echo "download clb1c0.h failed"; exit 2; }

ninja -C "$BUILD" hydra-shader-cache-reference-tests hydra-maxwell-alu-reference-tests \
  hydra-color-target-format-tests hydra-blend-reference-tests hydra-vertex-reference-tests \
  hydra-depth-stencil-reference-tests hydra-timezone-tests hydra-texture-format-reference-tests hydra-copy-engine-reference-tests hydra-blit-2d-reference-tests hydra-index-decode-reference-tests hydra-viewport-reference-tests >/dev/null || { echo "build failed: run ninja -C $BUILD"; exit 2; }

BIN="$BUILD/bin"; T="$ROOT/tests"; fail=0
run() { echo "== $1"; shift; "$@" 2>&1 | grep -v "Context leak" | tail -${TAIL:-3}; [ "${PIPESTATUS[0]}" -eq 0 ] || fail=1; }
run shader-cache python3 "$T/shader_cache_reference.py" "$BIN/hydra-shader-cache-reference-tests" --nvidia-3d "$NV/clb197.h" --nvidia-compute "$NV/clb1c0.h" --report /dev/null
TAIL=1 run maxwell-alu python3 "$T/maxwell_alu_reference.py" "$BIN/hydra-maxwell-alu-reference-tests"
TAIL=1 run maxwell-texture python3 "$T/maxwell_texture_reference.py" "$BIN/hydra-maxwell-alu-reference-tests"
run color-target-formats python3 "$T/color_target_format_reference.py" "$BIN/hydra-color-target-format-tests" --header "$NV/clb197.h"
TAIL=1 run msaa-registers python3 "$T/msaa_register_audit.py" "$NV/clb197.h"
TAIL=1 run timezone python3 "$T/timezone_reference.py" "$BIN/hydra-timezone-tests"
TAIL=1 run blend python3 "$T/blend_reference.py" "$BIN/hydra-blend-reference-tests"
TAIL=1 run vertex python3 "$T/vertex_reference.py" "$BIN/hydra-vertex-reference-tests"
TAIL=1 run depth-stencil python3 "$T/depth_stencil_reference.py" "$BIN/hydra-depth-stencil-reference-tests"
TAIL=2 run texture-formats python3 "$T/texture_format_reference.py" "$BIN/hydra-texture-format-reference-tests" --header "$NV/clb197tex.h"
[ -s "$NV/clb0b5.h" ] || curl -fsSL -o "$NV/clb0b5.h" "$BASE/dma-copy/clb0b5.h" || { echo "download clb0b5.h failed"; exit 2; }
TAIL=3 run copy-engine python3 "$T/copy_engine_reference.py" "$BIN/hydra-copy-engine-reference-tests" --header "$NV/clb0b5.h"
[ -s "$NV/cl902d.h" ] || curl -fsSL -o "$NV/cl902d.h" "$BASE/twod/cl902d.h" || { echo "download cl902d.h failed"; exit 2; }
[ -s "$NV/cla140.h" ] || curl -fsSL -o "$NV/cla140.h" "$BASE/inline-to-memory/cla140.h" || { echo "download cla140.h failed"; exit 2; }
TAIL=2 run blit-2d python3 "$T/blit_2d_reference.py" "$BIN/hydra-blit-2d-reference-tests" --header "$NV/cl902d.h"
TAIL=2 run inline-to-memory python3 "$T/inline_to_memory_reference.py" "$BIN/hydra-copy-engine-reference-tests" --header "$NV/cla140.h"
[ -s "$NV/clb197tex.h" ] || curl -fsSL -o "$NV/clb197tex.h" "$BASE/3d/clb197tex.h"
TAIL=3 run sampler python3 "$T/sampler_reference.py" "$BIN/hydra-maxwell-alu-reference-tests" --header "$NV/clb197tex.h"
TAIL=2 run texture-types python3 "$T/texture_type_reference.py" "$BIN/hydra-maxwell-alu-reference-tests"
TAIL=2 run index-decode python3 "$T/index_decode_reference.py" "$BIN/hydra-index-decode-reference-tests" --header "$NV/clb197.h"
TAIL=3 run viewport python3 "$T/viewport_reference.py" "$BIN/hydra-viewport-reference-tests" --header "$NV/clb197.h"
TAIL=2 run texel-fetch python3 "$T/texel_fetch_reference.py" "$BIN/hydra-maxwell-alu-reference-tests"
TAIL=2 run gather python3 "$T/gather_reference.py" "$BIN/hydra-maxwell-alu-reference-tests"
echo "== unsupported-feature reporting"
if grep -rn "LOG_NOT_IMPLEMENTED(ShaderDecompiler" "$ROOT/src/core/hw/tegra_x1/gpu/renderer/shader_decompiler" | grep -v "unsupported.hpp"; then
  echo "use SHADER_UNSUPPORTED so the shader records the feature"; fail=1
fi
[ $fail -eq 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
