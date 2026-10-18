cmake_minimum_required(VERSION 3.15...3.31)
set(CMAKE_POLICY_VERSION_MINIMUM 3.15)

include(FetchContent)

# Use LLVM clang instead of the system default one on Apple
# TODO: test with Xcode
if (APPLE AND NOT CMAKE_GENERATOR STREQUAL "Xcode")
    # Find LLVM
    execute_process(
        COMMAND brew --prefix llvm
        OUTPUT_VARIABLE LLVM_ROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if (LLVM_ROOT)
        message(STATUS "Using Homebrew LLVM: ${LLVM_ROOT}")
        set(CMAKE_C_COMPILER "${LLVM_ROOT}/bin/clang")
        set(CMAKE_CXX_COMPILER "${LLVM_ROOT}/bin/clang++")

        # Manually set sysroot
        execute_process(
            COMMAND xcrun --show-sdk-path
            OUTPUT_VARIABLE SDK_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(CMAKE_OSX_SYSROOT "${SDK_PATH}" CACHE PATH "Sysroot")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -isysroot ${SDK_PATH}" CACHE STRING "Flags")
    else ()
        message(WARNING "LLVM not found via brew. Falling back to default system compiler.")
    endif ()
endif ()

project(hydra VERSION 0.4.2 LANGUAGES CXX)

set(IOS_DEPLOYMENT_TARGET 17.4.0)

if (CMAKE_OSX_ARCHITECTURES)
    set(TARGET_ARCHS "${CMAKE_OSX_ARCHITECTURES}")
else ()
    set(TARGET_ARCHS "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif ()

option(CLANG_TIDY_ENABLED "Enable clang-tidy" ON)
option(HYDRA_HOT_RELOAD "Export symbols and build the hydra-hot module (HYDRA_HOT_MODULE)" OFF)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(HYDRA_LINT_TARGET_DEFAULT ON)
else ()
    set(HYDRA_LINT_TARGET_DEFAULT OFF)
endif ()
option(HYDRA_LINT_TARGET "Add the hydra-lint target" ${HYDRA_LINT_TARGET_DEFAULT})
set(HYDRA_LINT_EXTRA_BUILD_DIRS "" CACHE STRING
    "Extra build directories to include in hydra-lint (e.g. a second frontend)")
option(CUBEB_ENABLED "Enable the cubeb audio backend" ON)
option(HYDRA_FFMPEG_VIDEO "Build the FFmpeg video decoder backend" OFF)
option(SDL_ENABLED "Enable building SDL files" ON)
if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if (TARGET_ARCHS MATCHES "arm64")
        option(HYPERVISOR_ENABLED "Enable the Hypervisor CPU backend" ON)
    else ()
        set(HYPERVISOR_ENABLED OFF)
    endif ()
    set(FRONTEND "SDL3" CACHE STRING "Choose which frontend to use.")
elseif (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(HYPERVISOR_ENABLED OFF)
    set(FRONTEND "SwiftUI")
else ()
    set(HYPERVISOR_ENABLED OFF)
    set(FRONTEND "SDL3")
endif ()

if (FRONTEND STREQUAL "SDL3")
    set(SDL_ENABLED ON)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    option(MACOS_BUNDLE "Build a macOS app bundle" OFF)
elseif (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(MACOS_BUNDLE ON)
else ()
    set(MACOS_BUNDLE OFF)
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(GIT_HASH_SHORT "")
else ()
    # Set up Git hash (only if git is available and we're in a git repo)
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    # Get the first 7 characters
    string(SUBSTRING ${GIT_HASH} 0 7 GIT_HASH_SHORT)
    if(NOT GIT_HASH_SHORT)
        set(GIT_HASH_SHORT "0000000")
    endif()

    if(GIT_DIRTY)
        set(GIT_HASH_SHORT "${GIT_HASH_SHORT}+dirty")
    endif()
endif ()

if (NOT CMAKE_BUILD_TYPE)
    message("Build Type not set, defaulting to Debug...")
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Enable Objective-C and Swift on Apple platforms
if (APPLE)
    enable_language(OBJC OBJCXX Swift)
endif()

# Set output directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# HACK: link C++ standard library
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    link_libraries(c++)
endif ()

# Packages
find_package(fmt CONFIG)
if (NOT fmt_FOUND)
    message("fmt not found, downloading from GitHub")

    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 10.2.1
    )
    FetchContent_MakeAvailable(fmt)
endif ()

# zstd (NSZ/XCZ). Built from source and linked statically so it matches the
# deployment target and the app does not depend on Homebrew.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)

# Externals

# elf
include_directories(SYSTEM externals/elf/include)

# toml11
#add_subdirectory(externals/toml11 SYSTEM) # Hatch already uses toml11

# dynarmic
add_subdirectory(externals/dynarmic SYSTEM)

# cubeb
if (CUBEB_ENABLED)
    set(BUILD_TESTS OFF)
    set(BUILD_TOOLS OFF)
    add_subdirectory(externals/cubeb SYSTEM)
endif ()

# luft
#add_subdirectory(externals/luft SYSTEM)

# hatch
add_subdirectory(externals/hatch SYSTEM)

# libyaz0
add_subdirectory(externals/libyaz0 SYSTEM)

# nx2elf
add_subdirectory(externals/nx2elf SYSTEM)

# date
set(BUILD_TZ_LIB ON)
set(USE_SYSTEM_TZ_DB ON)
add_subdirectory(externals/date SYSTEM)

set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
add_library(hydra_compile_options INTERFACE)
target_compile_options(hydra_compile_options INTERFACE
    # TODO: uncomment
    #-fno-exceptions
    #-fno-asynchronous-unwind-tables
    -fno-rtti
    # TODO: uncomment
    #-fno-threadsafe-statics
    # Hot reloadable modules call into the executable
    $<$<NOT:$<BOOL:${HYDRA_HOT_RELOAD}>>:-fvisibility=hidden>

    # Warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wimplicit-int-float-conversion
    -Wshadow
    -Wdouble-promotion
    -Wundef
    -Wnon-virtual-dtor
    -Wold-style-cast

    # Disabled warnings
    -Wno-missing-designated-field-initializers
    -Wno-unused-template

    # Extensions
    -Wno-c99-extensions
    -Wno-gnu-anonymous-struct
    -Wno-nested-anon-types
    -Wno-zero-length-array
    -Wno-vla-extension
    -Wno-gnu-case-range
    -Wno-gnu-zero-variadic-macro-arguments
)

include_directories(SYSTEM externals/metal-cpp externals/dynarmic externals/stb)

if (HYPERVISOR_ENABLED)
    add_compile_definitions(HYDRA_HYPERVISOR_ENABLED)
endif ()

if (CUBEB_ENABLED)
    add_compile_definitions(HYDRA_CUBEB_ENABLED)
endif ()

if (SDL_ENABLED)
    add_compile_definitions(HYDRA_SDL_ENABLED)
endif ()

if (APPLE)
    set(CMAKE_XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC NO)
endif ()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(HYDRA_DEBUG)
endif ()

add_subdirectory(src)

if (HYDRA_HOT_RELOAD)
    # Hash of the headers, a module built against other class layouts is
    # refused instead of crashing
    add_custom_target(hydra-hot-abi
        COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${CMAKE_BINARY_DIR}/generated/hot_abi.hpp -P ${CMAKE_SOURCE_DIR}/cmake/hot_abi.cmake
        BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/hot_abi.hpp)
    add_dependencies(hydra-core hydra-hot-abi)
    target_compile_definitions(hydra-core PRIVATE HYDRA_HOT_ABI_HEADER)
    target_include_directories(hydra-core PRIVATE ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(hydra-core PRIVATE
        HYDRA_DEFAULT_HOT_MODULE="${CMAKE_BINARY_DIR}/libhydra-hot${CMAKE_SHARED_MODULE_SUFFIX}"
        HYDRA_DEFAULT_HOT_DIR="${CMAKE_BINARY_DIR}/hot-debug")

    # The module resolves hydra-core symbols from the running executable
    target_link_options(hydra PRIVATE "LINKER:-export_dynamic")

    # Same flags as hydra-core (class layouts must match), but not linked
    file(GLOB_RECURSE HYDRA_HOT_GPU_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/core/hw/tegra_x1/gpu/engines/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/core/hw/tegra_x1/gpu/renderer/*.cpp")
    # Process-wide globals, platform implementation singletons and the null
    # backend remain owned by the executable. Never duplicate their state.
    list(FILTER HYDRA_HOT_GPU_SOURCES EXCLUDE REGEX "/(null/|metal/impl\\.cpp$)")
    list(APPEND HYDRA_HOT_GPU_SOURCES
        "${CMAKE_SOURCE_DIR}/src/core/hot/table.cpp"
        "${CMAKE_SOURCE_DIR}/src/core/hot/texture_cache_hot.cpp"
        "${CMAKE_SOURCE_DIR}/src/core/hw/tegra_x1/gpu/memory_util.cpp")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/hot_sources.txt"
        CONTENT "$<JOIN:${HYDRA_HOT_GPU_SOURCES},\n>\n")
    add_library(hydra-hot MODULE ${HYDRA_HOT_GPU_SOURCES})
    target_link_libraries(hydra-hot PRIVATE hydra_compile_options $<COMPILE_ONLY:hydra-core> $<COMPILE_ONLY:hydra-common>)
    target_include_directories(hydra-hot PRIVATE $<TARGET_PROPERTY:hydra-core,INCLUDE_DIRECTORIES>)
    target_compile_definitions(hydra-hot PRIVATE $<TARGET_PROPERTY:hydra-core,COMPILE_DEFINITIONS>)
    target_compile_options(hydra-hot PRIVATE $<TARGET_PROPERTY:hydra-core,COMPILE_OPTIONS>)
    # fmt is built with hidden symbols, it is stateless
    target_link_libraries(hydra-hot PRIVATE fmt::fmt)
    target_link_options(hydra-hot PRIVATE "LINKER:-undefined,dynamic_lookup")
    add_dependencies(hydra-hot hydra-hot-abi)

    option(HYDRA_HOT_TESTS "Test live modules and Metal output in one process" OFF)
    if (HYDRA_HOT_TESTS)
        add_executable(hydra-hot-reload-tests tests/hot_reload_tests.cpp)
        target_include_directories(hydra-hot-reload-tests PRIVATE src src/ztd/src)
        target_link_libraries(hydra-hot-reload-tests PRIVATE hydra-core hydra-common hydra_compile_options)
        target_link_options(hydra-hot-reload-tests PRIVATE "LINKER:-export_dynamic")
        foreach (VARIANT IN ITEMS a b rejected)
            add_library(hydra-hot-fixture-${VARIANT} MODULE tests/hot_module_fixture.cpp)
            target_include_directories(hydra-hot-fixture-${VARIANT} PRIVATE src src/ztd/src)
            target_link_libraries(hydra-hot-fixture-${VARIANT} PRIVATE
                hydra_compile_options $<COMPILE_ONLY:hydra-core> $<COMPILE_ONLY:hydra-common>)
            target_link_options(hydra-hot-fixture-${VARIANT} PRIVATE "LINKER:-undefined,dynamic_lookup")
            if (VARIANT STREQUAL "b")
                target_compile_definitions(hydra-hot-fixture-${VARIANT} PRIVATE HYDRA_FIXTURE_COLOR="0.75")
            else ()
                target_compile_definitions(hydra-hot-fixture-${VARIANT} PRIVATE HYDRA_FIXTURE_COLOR="0.25")
            endif ()
            if (VARIANT STREQUAL "rejected")
                target_compile_definitions(hydra-hot-fixture-${VARIANT} PRIVATE HYDRA_FIXTURE_REJECT)
            endif ()
        endforeach ()
    endif ()
endif ()

option(HYDRA_XCI_TESTS "Build XCI loader regression tests" OFF)
option(HYDRA_VISUAL_DIAGNOSTIC_TESTS "Build standalone visual diagnostic controller tests" OFF)
if (HYDRA_VISUAL_DIAGNOSTIC_TESTS)
    add_executable(hydra-visual-diagnostic-tests
        tests/visual_diagnostics_tests.cpp
        src/core/hw/tegra_x1/gpu/visual_diagnostics.cpp)
    target_include_directories(hydra-visual-diagnostic-tests PRIVATE src)
    target_link_libraries(hydra-visual-diagnostic-tests PRIVATE hydra_compile_options)
endif ()
option(HYDRA_FRAME_TRACE_TESTS "Build frame capture regression tests" OFF)
if (HYDRA_FRAME_TRACE_TESTS)
    add_executable(hydra-frame-trace-tests tests/frame_tracer_tests.cpp)
    target_include_directories(hydra-frame-trace-tests PRIVATE src)
    target_link_libraries(hydra-frame-trace-tests PRIVATE fmt::fmt hydra_compile_options)
endif ()
if (HYDRA_XCI_TESTS)
    add_executable(hydra-xci-tests tests/xci_tests.cpp)
    target_include_directories(hydra-xci-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-xci-tests PRIVATE hydra-core hydra-common hydra_compile_options)
endif ()

option(HYDRA_METAL_CACHE_TESTS "Build Metal binary archive integration tests" OFF)
if (HYDRA_METAL_CACHE_TESTS)
    add_executable(hydra-shutdown-tests MACOSX_BUNDLE tests/shutdown_tests.cpp res/images/default_avatar_image.png)
    set_source_files_properties(res/images/default_avatar_image.png PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_include_directories(hydra-shutdown-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-shutdown-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    if (HYDRA_FFMPEG_VIDEO)
        target_compile_definitions(hydra-shutdown-tests PRIVATE
            HYDRA_FFMPEG_VIDEO=1
            HYDRA_VIDEO_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/video")
    endif ()
    add_custom_command(TARGET hydra-shutdown-tests POST_BUILD
        COMMAND codesign --force --sign - --entitlements "${CMAKE_SOURCE_DIR}/hydra.macos.entitlements" "$<TARGET_FILE:hydra-shutdown-tests>")
    add_executable(hydra-decompile-tool tests/decompile_tool.cpp)
    target_include_directories(hydra-decompile-tool PRIVATE src src/ztd/src)
    target_link_libraries(hydra-decompile-tool PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-shader-library-tests tests/shader_library_tests.cpp
        tests/shader_coverage_tests.cpp)
    target_include_directories(hydra-shader-library-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-shader-library-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-maxwell-alu-reference-tests tests/maxwell_alu_reference_tests.cpp
        tests/shader_coverage_tests.cpp)
    target_include_directories(hydra-maxwell-alu-reference-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-maxwell-alu-reference-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-color-target-format-tests tests/color_target_format_tests.cpp)
    target_include_directories(hydra-color-target-format-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-color-target-format-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-timezone-tests tests/timezone_tests.cpp)
    target_include_directories(hydra-timezone-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-timezone-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-shader-corpus-tests tests/shader_corpus_tests.cpp)
    target_include_directories(hydra-shader-corpus-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-shader-corpus-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-metal-cache-tests tests/metal_cache_tests.cpp)
    target_include_directories(hydra-metal-cache-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-metal-cache-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-texture-cache-tests tests/texture_cache_tests.cpp
                   tests/texture_format_tests.cpp)
    target_include_directories(hydra-texture-cache-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-texture-cache-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-raster-state-tests tests/raster_state_tests.cpp)
    target_include_directories(hydra-raster-state-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-raster-state-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-blend-reference-tests tests/blend_reference_tests.cpp)
    target_include_directories(hydra-blend-reference-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-blend-reference-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-vertex-reference-tests tests/vertex_reference_tests.cpp)
    target_include_directories(hydra-vertex-reference-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-vertex-reference-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-depth-stencil-reference-tests tests/depth_stencil_reference_tests.cpp)
    target_include_directories(hydra-depth-stencil-reference-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-depth-stencil-reference-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    foreach (SUITE dma-slice shader-video buffer-queue)
        string(REPLACE "-" "_" TEST_SOURCE ${SUITE})
        add_executable(hydra-${SUITE}-tests tests/${TEST_SOURCE}_tests.cpp)
        target_include_directories(hydra-${SUITE}-tests PRIVATE src src/ztd/src)
        target_link_libraries(hydra-${SUITE}-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    endforeach ()
    add_executable(hydra-gpu-engine-tests tests/gpu_engine_tests.cpp tests/blit_2d_tests.cpp)
    target_include_directories(hydra-gpu-engine-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-gpu-engine-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-render-hash-tests tests/render_hash_tests.cpp)
    target_include_directories(hydra-render-hash-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-render-hash-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-shader-cache-reference-tests tests/shader_cache_reference_tests.cpp)
    target_include_directories(hydra-shader-cache-reference-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-shader-cache-reference-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-buffer-snapshot-tests tests/buffer_snapshot_tests.cpp)
    target_include_directories(hydra-buffer-snapshot-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-buffer-snapshot-tests PRIVATE hydra-core hydra-common hydra_compile_options)
    add_executable(hydra-register-write-tests tests/register_write_tests.cpp)
    target_include_directories(hydra-register-write-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-register-write-tests PRIVATE hydra-core hydra-common hydra_compile_options "-framework Hypervisor")
    add_custom_command(TARGET hydra-register-write-tests POST_BUILD
        COMMAND codesign --force --sign - --entitlements "${CMAKE_SOURCE_DIR}/hydra.macos.entitlements" "$<TARGET_FILE:hydra-register-write-tests>")

    add_executable(hydra-memory-tracking-tests tests/memory_tracking_tests.cpp)
    target_include_directories(hydra-memory-tracking-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-memory-tracking-tests PRIVATE hydra-core hydra-common hydra_compile_options "-framework Hypervisor")
    add_custom_command(TARGET hydra-memory-tracking-tests POST_BUILD
        COMMAND codesign --force --sign - --entitlements "${CMAKE_SOURCE_DIR}/hydra.macos.entitlements" "$<TARGET_FILE:hydra-memory-tracking-tests>")
endif ()

option(HYDRA_AUDIO_TESTS "Build audio decoder and PCM queue regression tests" OFF)
option(HYDRA_VIDEO_TESTS "Build FFmpeg video decoder regression tests" OFF)
if (HYDRA_VIDEO_TESTS)
    if (NOT HYDRA_FFMPEG_VIDEO)
        message(FATAL_ERROR "HYDRA_VIDEO_TESTS requires HYDRA_FFMPEG_VIDEO")
    endif ()
    add_executable(hydra-video-tests tests/video_tests.cpp)
    target_link_libraries(hydra-video-tests PRIVATE hydra-video hydra_compile_options)
    target_compile_definitions(hydra-video-tests PRIVATE
        HYDRA_VIDEO_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/video")
endif ()
if (HYDRA_AUDIO_TESTS)
    add_executable(hydra-audio-tests tests/audio_tests.cpp)
    target_include_directories(hydra-audio-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-audio-tests PRIVATE hydra-common hydra_compile_options)
endif ()

option(HYDRA_INPUT_TESTS "Build player assignment and profile persistence tests" OFF)
if (HYDRA_INPUT_TESTS)
    add_executable(hydra-input-tests tests/input_tests.cpp)
    target_include_directories(hydra-input-tests PRIVATE src src/ztd/src)
    target_link_libraries(hydra-input-tests PRIVATE hydra-core hydra-common hydra_compile_options)
endif ()
