# Keep the source list/normalization aligned with tools/gpu_optimization_gate.py.
# Reconfigure when any covered source changes so an older receipt cannot enable
# newly built optimizations after a GPU, CPU-memory or test change.
file(GLOB_RECURSE HYDRA_GPU_GATE_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/core/hw/tegra_x1/gpu/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/hw/tegra_x1/gpu/*.hpp"
    "${CMAKE_SOURCE_DIR}/src/core/hw/tegra_x1/cpu/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/hw/tegra_x1/cpu/*.hpp"
    "${CMAKE_SOURCE_DIR}/src/core/hot/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/hot/*.hpp"
    "${CMAKE_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_SOURCE_DIR}/tests/*.hpp"
    "${CMAKE_SOURCE_DIR}/tests/*.py"
    "${CMAKE_SOURCE_DIR}/tools/*.py")
list(APPEND HYDRA_GPU_GATE_FILES
    "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/src/core/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/cmake/GpuOptimizationGate.cmake")
list(SORT HYDRA_GPU_GATE_FILES)
set(HYDRA_GPU_GATE_INPUT "")
foreach(HYDRA_GPU_GATE_FILE IN LISTS HYDRA_GPU_GATE_FILES)
    file(RELATIVE_PATH HYDRA_GPU_GATE_RELATIVE "${CMAKE_SOURCE_DIR}" "${HYDRA_GPU_GATE_FILE}")
    file(READ "${HYDRA_GPU_GATE_FILE}" HYDRA_GPU_GATE_CONTENT)
    string(REPLACE "\r\n" "\n" HYDRA_GPU_GATE_CONTENT "${HYDRA_GPU_GATE_CONTENT}")
    string(SHA256 HYDRA_GPU_GATE_HASH "${HYDRA_GPU_GATE_CONTENT}")
    string(APPEND HYDRA_GPU_GATE_INPUT "${HYDRA_GPU_GATE_RELATIVE}:${HYDRA_GPU_GATE_HASH}\n")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${HYDRA_GPU_GATE_FILES})
string(SHA256 HYDRA_GPU_GATE_FINGERPRINT "${HYDRA_GPU_GATE_INPUT}")
set(HYDRA_GPU_GATE_ALLOWED 1)
if(HYDRA_HOT_RELOAD)
    set(HYDRA_GPU_GATE_ALLOWED 0)
endif()
target_compile_definitions(hydra-core PRIVATE
    HYDRA_GPU_OPTIMIZATION_FINGERPRINT="${HYDRA_GPU_GATE_FINGERPRINT}"
    HYDRA_GPU_OPTIMIZATION_BUILD_ALLOWED=${HYDRA_GPU_GATE_ALLOWED})
file(WRITE "${CMAKE_BINARY_DIR}/gpu-optimization-fingerprint.txt"
    "${HYDRA_GPU_GATE_FINGERPRINT}\n${HYDRA_GPU_GATE_ALLOWED}\n")
