cmake_minimum_required(VERSION 3.28)

# ─── Tests ────────────────────────────────────────────────────────────────────
# Uses Google Test (fetched via FetchContent in third_party/)

include(GoogleTest)

# Qt6::Test for signal spy support in UI tests
find_package(Qt6 QUIET COMPONENTS Test)

# Build Qt/FFmpeg DLL path list for test discovery (gtest_discover_tests runs exes at build time)
set(_QT_BIN_DIR "${CMAKE_SOURCE_DIR}/third_party/qt/6.8.3/msvc2022_64/bin")
set(_FFMPEG_BIN_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg/bin")
set(_WINDEPLOYQT "${_QT_BIN_DIR}/windeployqt.exe")
cmake_path(NATIVE_PATH _QT_BIN_DIR _QT_BIN_DIR_NATIVE)
cmake_path(NATIVE_PATH _FFMPEG_BIN_DIR _FFMPEG_BIN_DIR_NATIVE)
set(_TEST_DLL_PATH "${_QT_BIN_DIR_NATIVE}\;${_FFMPEG_BIN_DIR_NATIVE}\;$ENV{PATH}")
set(_TEST_DISCOVERY_PROPS DISCOVERY_MODE PRE_TEST)

function(configure_roundtable_test target)
    set(options USES_QT)
    set(oneValue GROUP)
    cmake_parse_arguments(CRT "${options}" "${oneValue}" "" ${ARGN})
    if (NOT CRT_GROUP)
        set(CRT_GROUP core)
    endif()

    if (CRT_USES_QT)
        get_property(_qt_deploy_target GLOBAL PROPERTY ROUNDTABLE_QT_TEST_DEPLOY_TARGET)

        if (NOT _qt_deploy_target)
            set(_qt_deploy_target roundtable_qt_test_runtime)
            set_property(GLOBAL PROPERTY ROUNDTABLE_QT_TEST_DEPLOY_TARGET ${_qt_deploy_target})

            add_custom_target(${_qt_deploy_target} ALL
                DEPENDS ${target}
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/platforms"
                COMMAND "${_WINDEPLOYQT}"
                    --dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
                    --no-compiler-runtime
                    --no-system-d3d-compiler
                    --no-translations
                    "$<TARGET_FILE:${target}>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_QT_BIN_DIR}/Qt6Test.dll"
                    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/"
            )

            if (TARGET copy_ffmpeg_dlls)
                add_dependencies(${_qt_deploy_target} copy_ffmpeg_dlls)
            endif()
        else()
            add_dependencies(${_qt_deploy_target} ${target})
        endif()
    endif()

    # LABELS lets CI run only the deterministic, headless-safe "core" group
    # (ctest -L core) while still BUILDING every test for compile coverage.
    gtest_discover_tests(${target} ${_TEST_DISCOVERY_PROPS}
        PROPERTIES LABELS ${CRT_GROUP})
endfunction()

# ─── Helper: add_roundtable_test(name LIBS lib1 lib2 ... [USES_QT]) ──────────
# Creates an executable, links libraries + GTest, and registers with CTest.
function(add_roundtable_test name)
    set(options USES_QT)
    set(oneValue GROUP)
    cmake_parse_arguments(ART "${options}" "${oneValue}" "LIBS" ${ARGN})
    if (NOT ART_GROUP)
        set(ART_GROUP core)
    endif()

    # Test sources live in a subfolder named for their run-group, which is
    # also the source library they exercise: core/ gpu/ export/ ui/
    # (mirrors src/).  The group thus selects BOTH the ctest LABEL and the
    # source path — adding a test = drop test_X.cpp into tests/<group>/ and
    # call add_roundtable_test(test_X LIBS ... [GROUP <group>]).
    add_executable(${name} ${ART_GROUP}/${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ART_LIBS} GTest::gtest_main)

    if(ART_USES_QT)
        target_link_libraries(${name} PRIVATE Qt6::Test)
        configure_roundtable_test(${name} USES_QT GROUP ${ART_GROUP})
    else()
        configure_roundtable_test(${name} GROUP ${ART_GROUP})
    endif()
endfunction()

# ─── Test targets ─────────────────────────────────────────────────────────────

# Core-only tests
add_roundtable_test(test_timeline      LIBS roundtable_core)
add_roundtable_test(test_keyframe      LIBS roundtable_core)
add_roundtable_test(test_clips         LIBS roundtable_core)
add_roundtable_test(test_color_conversion LIBS roundtable_core)
add_roundtable_test(test_render_complexity LIBS roundtable_core)
add_roundtable_test(test_command       LIBS roundtable_core)
add_roundtable_test(test_project       LIBS roundtable_core)
add_roundtable_test(test_audio         LIBS roundtable_core)
add_roundtable_test(test_spine         LIBS roundtable_core)
add_roundtable_test(test_timeline_ui   LIBS roundtable_core)
add_roundtable_test(test_timeline_edit LIBS roundtable_core)
add_roundtable_test(test_transport     LIBS roundtable_core)
add_roundtable_test(test_effects       LIBS roundtable_core)
add_roundtable_test(test_audiofx       LIBS roundtable_core)
add_roundtable_test(test_audio_sync    LIBS roundtable_core)
add_roundtable_test(test_autosave      LIBS roundtable_core)
add_roundtable_test(test_integration   LIBS roundtable_core)
add_roundtable_test(test_animation_cache LIBS roundtable_core)
# Both pull in roundtable_core TUs that reference roundtable_gpu symbols
# (MediaPool prefetch -> Nv12Converter/GpuScheduler; FrameProducer ->
# GpuContext::get()), so they need roundtable_gpu at link time. Neither
# creates a device at run time, so they stay headless-safe ("core" group).
add_roundtable_test(test_media_source_service LIBS roundtable_gpu roundtable_core)
add_roundtable_test(test_playback_scheduler   LIBS roundtable_gpu roundtable_core)

# GPU tests — need a real Vulkan device, so NOT in the headless CI "core" run.
add_roundtable_test(test_decoder       LIBS roundtable_gpu roundtable_core GROUP gpu)
add_roundtable_test(test_gpu_spine     LIBS roundtable_gpu roundtable_core GROUP gpu)
add_roundtable_test(test_compositor    LIBS roundtable_gpu roundtable_core GROUP gpu)
add_roundtable_test(test_viewport      LIBS roundtable_gpu roundtable_core GROUP gpu)
add_roundtable_test(test_text          LIBS roundtable_gpu roundtable_core GROUP gpu)
add_roundtable_test(test_gpu_resource_manager LIBS roundtable_gpu roundtable_core GROUP gpu)

# Export tests
add_roundtable_test(test_export        LIBS roundtable_export roundtable_core GROUP export)

# UI tests (Qt required)
add_roundtable_test(test_monitors        LIBS roundtable_ui roundtable_core USES_QT GROUP ui)
add_roundtable_test(test_project_bin     LIBS roundtable_ui roundtable_core USES_QT GROUP ui)
add_roundtable_test(test_properties      LIBS roundtable_ui roundtable_core USES_QT GROUP ui)
add_roundtable_test(test_shot_composer   LIBS roundtable_ui roundtable_core USES_QT GROUP ui)
add_roundtable_test(test_waveform        LIBS roundtable_ui roundtable_gpu roundtable_core USES_QT GROUP ui)
add_roundtable_test(test_keyframe_editor LIBS roundtable_ui roundtable_core USES_QT GROUP ui)
add_roundtable_test(test_main_window     LIBS roundtable_ui roundtable_core USES_QT GROUP ui)
add_roundtable_test(test_history_panel   LIBS roundtable_ui roundtable_core USES_QT GROUP ui)
