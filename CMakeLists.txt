cmake_minimum_required(VERSION 3.28)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Set before project()/target creation so MSVC runtime selection is honored
# by all targets unless explicitly overridden.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "MSVC runtime" FORCE)

# Read version from src/version.h (written by publish-gui.ps1 at release time).
# This is checked into git so every tag has an immutable version.
file(STRINGS "${CMAKE_SOURCE_DIR}/src/version.h" _version_line
     REGEX "#define ROUNDTABLE_VERSION")
if(_version_line MATCHES "\"([0-9.]+)\"")
    set(PROJECT_VERSION "${CMAKE_MATCH_1}")
else()
    set(PROJECT_VERSION "0.0.0")
endif()
message(STATUS "Version: ${PROJECT_VERSION} (from src/version.h)")
# Reconfigure when version.h changes (pulled a new tag)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/version.h")

project(Roundtable VERSION ${PROJECT_VERSION} LANGUAGES CXX)

# ─── C++20 Standard ─────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── CRT linkage ────────────────────────────────────────────────────────────
# Force all targets (including FetchContent deps) to use the dynamic CRT
# (/MD in Release, /MDd in Debug).

# ─── Output directories ─────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ─── Build type defaults ────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# ─── Unity (jumbo) build support ────────────────────────────────────────────
# Groups multiple .cpp files into single TUs to reduce header re-parsing.
# Enable with -DCMAKE_UNITY_BUILD=ON for faster full rebuilds.
# Default OFF — incremental builds are faster without it.
option(CMAKE_UNITY_BUILD "Enable unity (jumbo) build" ON)
if(CMAKE_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 8)
endif()

# ─── Compiler flags ─────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:/W4>             # High warning level
        $<$<COMPILE_LANGUAGE:C,CXX>:/MP>             # Multi-processor compilation (use all Ryzen cores)
        $<$<COMPILE_LANGUAGE:C,CXX>:/permissive->    # Standards conformance
        $<$<COMPILE_LANGUAGE:C,CXX>:/Zc:__cplusplus> # Report correct __cplusplus value
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>          # Source and execution charset
    )
    # Prevent Windows.h from defining min/max macros (safe for third-party libs)
    add_compile_definitions(NOMINMAX)
    # WIN32_LEAN_AND_MEAN applied per-target to avoid breaking PortAudio WASAPI
    # Release optimizations (generator expression for multi-config generators)
    add_compile_options(
        $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/O2>
        $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Oi>
        $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Gy>
        # /Zi: emit debug info into a PDB.  Does NOT disable optimizations.
        # Needed so the in-process SEH crash handler can resolve
        # roundtable.exe frames to function names — without it every
        # frame in crash_log.txt is a raw "+0xNNNN" offset.
        $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Zi>
    )
    add_link_options(
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
        # /DEBUG (implicitly /DEBUG:FULL with /LTCG) writes the matching
        # PDB next to the EXE.  Compatible with /OPT:REF + /OPT:ICF;
        # those strip unreferenced code from the EXE but keep the PDB
        # complete.  PDB ships with the build but is not redistributed.
        $<$<CONFIG:Release>:/DEBUG>
    )
    # AddressSanitizer (MSVC /fsanitize=address).  Opt-in: catches the
    # heap-corruption / use-after-free class of bugs (0xC0000374 crashes)
    # at the faulting instruction instead of a deferred ntdll fast-fail.
    # Intended for the test suite (ctest -L core) and local repro sessions,
    # not for shipping builds.  Incompatible with /GL+/LTCG and incremental
    # linking, so it force-disables LTO below.
    option(ROUNDTABLE_ASAN "Build with AddressSanitizer (disables LTO)" OFF)
    if(ROUNDTABLE_ASAN)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address>)
        add_link_options(/INCREMENTAL:NO)
        message(STATUS "ROUNDTABLE_ASAN=ON — AddressSanitizer enabled, LTO disabled")
    endif()

    # Link-Time Code Generation (very slow — only for final distribution builds)
    option(ROUNDTABLE_LTO "Enable /GL + /LTCG for whole-program optimization (slow)" ON)
    if(ROUNDTABLE_LTO AND NOT ROUNDTABLE_ASAN)
        add_compile_options($<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/GL>)
        add_link_options($<$<CONFIG:Release>:/LTCG>)
    endif()
else()
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wpedantic>
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(
            $<$<COMPILE_LANGUAGE:C,CXX>:-O3>
            $<$<COMPILE_LANGUAGE:C,CXX>:-march=native>
            $<$<COMPILE_LANGUAGE:C,CXX>:-flto>
        )
    endif()
endif()

# ─── Portable dependencies (no system install required) ──────────────────────
# All deps fetched via FetchContent or shipped in third_party/. End-user
# machines do not need a CUDA Toolkit: Windows CUDA builds copy the exact
# redistributable runtime closure into the application directory below.
add_subdirectory(third_party)

# ─── Windows CUDA runtime deployment ────────────────────────────────────────
# whisper.cpp/ggml links CUDA statically into Roundtable but imports the
# versioned CUDA runtime and cuBLAS DLLs. Keep those DLLs beside the executable
# so transcription never succeeds only because the build machine has CUDA on
# PATH. DeployCudaRuntime also follows cuBLAS' PE imports (currently cublasLt)
# and writes the manifest consumed by the launch diagnostics and installer.
if(WIN32 AND ROUNDTABLE_WHISPER_CUDA_FOUND)
    if(NOT ROUNDTABLE_CUDA_RUNTIME_SEEDS OR
       NOT ROUNDTABLE_CUDA_RUNTIME_SOURCE_DIR)
        message(FATAL_ERROR
            "whisper.cpp CUDA is enabled, but its runtime packaging metadata is missing")
    endif()

    set(_roundtable_cuda_generated_dir "${CMAKE_BINARY_DIR}/generated/cuda-runtime")
    file(MAKE_DIRECTORY "${_roundtable_cuda_generated_dir}")
    set(ROUNDTABLE_CUDA_RUNTIME_SEED_ENTRIES "")
    foreach(_cuda_seed IN LISTS ROUNDTABLE_CUDA_RUNTIME_SEEDS)
        string(APPEND ROUNDTABLE_CUDA_RUNTIME_SEED_ENTRIES
            "    [==[${_cuda_seed}]==]\n")
    endforeach()
    set(ROUNDTABLE_CUDA_SEED_MANIFEST
        "${_roundtable_cuda_generated_dir}/CudaRuntimeSeeds.cmake")
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/CudaRuntimeSeeds.cmake.in"
        "${ROUNDTABLE_CUDA_SEED_MANIFEST}"
        @ONLY)

    add_custom_target(copy_cuda_runtime_dlls ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DROUNDTABLE_CUDA_SEED_MANIFEST=${ROUNDTABLE_CUDA_SEED_MANIFEST}"
            "-DROUNDTABLE_CUDA_DESTINATION=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
            "-DROUNDTABLE_CUDA_MODE=DEPLOY"
            -P "${CMAKE_SOURCE_DIR}/cmake/DeployCudaRuntime.cmake"
        DEPENDS
            ${ROUNDTABLE_CUDA_RUNTIME_SEEDS}
            "${ROUNDTABLE_CUDA_SEED_MANIFEST}"
            "${CMAKE_SOURCE_DIR}/cmake/DeployCudaRuntime.cmake"
        COMMENT "Bundling CUDA runtime DLL dependency closure"
        VERBATIM)
    set_property(TARGET copy_cuda_runtime_dlls PROPERTY FOLDER "Deployment")
endif()

# ─── Dev-build isolation ────────────────────────────────────────────────────
# When ON, QSettings uses ("ROUNDTABLE", "NLE-Dev") so dev builds don't
# contaminate the installed version's settings (RecentFiles, workspaces, etc.).
option(ROUNDTABLE_DEV_BUILD "Isolate dev build settings from installed version" OFF)

# CrisperWhisper 2 uses non-commercial model weights.  The adapter is enabled for
# the owner's personal builds for now; turn this option OFF for the public
# distribution when requested.  The Python package and model weights are never
# downloaded or redistributed by the build.
option(ROUNDTABLE_ENABLE_CRISPERWHISPER_PERSONAL
       "Enable the personal-use CrisperWhisper adapter" ON)


# Native .shotanim runtime — ABANDONED; spine-cpp is the permanent production
# renderer (the parked tools/native_engine/ sources were removed 2026-07-01;
# recover from git history if ever needed). The owner holds a Spine Editor
# license so distribution is covered by Section 2 of the Spine Editor License
# Agreement (see docs/THIRD_PARTY_LICENSES.md).

# ─── Global definitions ─────────────────────────────────────────────────────
add_compile_definitions(
    ROUNDTABLE_VERSION="${PROJECT_VERSION}"
    VK_NO_PROTOTYPES          # Required: volk provides Vulkan function pointers at runtime.
                              # Without this, vulkan.h declares direct prototypes that link
                              # to volk's pointer variables instead of calling through them.
    $<$<CONFIG:Debug>:ROUNDTABLE_DEBUG>
    $<$<BOOL:${ROUNDTABLE_DEV_BUILD}>:ROUNDTABLE_DEV_BUILD>
    $<$<BOOL:${ROUNDTABLE_ENABLE_CRISPERWHISPER_PERSONAL}>:ROUNDTABLE_HAS_CRISPERWHISPER>
    ROUNDTABLE_CRISPERWHISPER_WORKER_PATH="${CMAKE_SOURCE_DIR}/src/core/ai/crisperwhisper_worker.py"
    ROUNDTABLE_CRISPERWHISPER_PYTHON_PATH="${CMAKE_SOURCE_DIR}/.venv/Scripts/python.exe"
    $<$<BOOL:${CUDAToolkit_FOUND}>:ROUNDTABLE_HAS_CUDA>
    $<$<BOOL:${FFMPEG_FOUND}>:ROUNDTABLE_HAS_FFMPEG>
)

# ─── Shader compilation ─────────────────────────────────────────────────────
# Compile GLSL → SPIR-V at build time (requires glslc from Vulkan SDK or shaderc)
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SPIRV_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SPIRV_OUTPUT_DIR})

file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
    ${SHADER_DIR}/*.vert
    ${SHADER_DIR}/*.frag
    ${SHADER_DIR}/*.comp
)

# Try to find glslc (from tools/shaderc/, Vulkan SDK, or system PATH)
find_program(GLSLC_EXECUTABLE glslc
    HINTS ${CMAKE_SOURCE_DIR}/tools/shaderc
          $ENV{VULKAN_SDK}/Bin
          $ENV{VULKAN_SDK}/bin
)

if(GLSLC_EXECUTABLE AND SHADER_SOURCES)
    foreach(SHADER ${SHADER_SOURCES})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPIRV_OUTPUT ${SPIRV_OUTPUT_DIR}/${SHADER_NAME}.spv)
        add_custom_command(
            OUTPUT ${SPIRV_OUTPUT}
            COMMAND ${GLSLC_EXECUTABLE} ${SHADER} -o ${SPIRV_OUTPUT}
            DEPENDS ${SHADER}
            COMMENT "Compiling shader: ${SHADER_NAME}"
        )
        list(APPEND SPIRV_OUTPUTS ${SPIRV_OUTPUT})
    endforeach()
    add_custom_target(shaders ALL DEPENDS ${SPIRV_OUTPUTS})
else()
    if(NOT GLSLC_EXECUTABLE)
        message(STATUS "glslc not found — shaders will not be compiled (add Vulkan SDK to PATH or install shaderc)")
    endif()
    add_custom_target(shaders) # Empty target so dependencies don't break
endif()

# ─── Subdirectories ─────────────────────────────────────────────────────────
add_subdirectory(src)

enable_testing()
option(ROUNDTABLE_BUILD_TESTS "Build Roundtable unit/integration tests" OFF)
if(ROUNDTABLE_BUILD_TESTS)
    find_package(Qt6 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        add_subdirectory(tests)
    else()
        message(STATUS "Qt6::Test not found — skipping test targets")
    endif()
else()
    message(STATUS "ROUNDTABLE_BUILD_TESTS=OFF — skipping tests (pass -DROUNDTABLE_BUILD_TESTS=ON to enable)")
endif()

if(MSVC)
    # Normalize CRT linkage on every local, non-imported target to avoid
    # mixed-runtime link warnings (LNK4098).
    get_property(_allTargets GLOBAL PROPERTY TARGETS)
    foreach(_t IN LISTS _allTargets)
        get_target_property(_isImported ${_t} IMPORTED)
        if(_isImported)
            continue()
        endif()
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()
        set_property(TARGET ${_t} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endforeach()
endif()

# ─── Install ─────────────────────────────────────────────────────────────────
if(TARGET roundtable)
    install(TARGETS roundtable RUNTIME DESTINATION bin)
endif()

if(WIN32 AND ROUNDTABLE_WHISPER_CUDA_FOUND)
    set(_roundtable_cuda_install_script
        "${_roundtable_cuda_generated_dir}/InstallCudaRuntime.cmake")
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/InstallCudaRuntime.cmake.in"
        "${_roundtable_cuda_install_script}"
        @ONLY)
    # Registered after install(TARGETS roundtable) so the install-time helper
    # can validate the installed executable's actual PE dependency closure.
    install(SCRIPT "${_roundtable_cuda_install_script}")
endif()

install(DIRECTORY ${SPIRV_OUTPUT_DIR}/ DESTINATION shaders)

# Copy FFmpeg DLLs to output directory for runtime
if(FFMPEG_FOUND)
    file(GLOB FFMPEG_DLLS "${FFMPEG_BIN_DIR}/*.dll")
    # Copy DLLs alongside test executables (bin/ output dir)
    # Use a custom target instead of POST_BUILD on a specific target
    add_custom_target(copy_ffmpeg_dlls ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/
        COMMENT "Copying FFmpeg DLLs to output directory"
    )
    foreach(DLL ${FFMPEG_DLLS})
        get_filename_component(DLL_NAME ${DLL} NAME)
        add_custom_command(TARGET copy_ffmpeg_dlls POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLL}
                    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/
            COMMENT "Copying ${DLL_NAME}"
        )
    endforeach()
    # Keep deployment inside the app's build graph.  Passing both targets to
    # `cmake --build` makes the Visual Studio generator launch back-to-back
    # parallel MSBuild sessions, whose reusable nodes can race over .tlog files.
    if(TARGET roundtable)
        add_dependencies(roundtable copy_ffmpeg_dlls)
    endif()
endif()
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/ DESTINATION assets)
