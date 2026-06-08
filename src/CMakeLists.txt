# ─── Source root ──────────────────────────────────────────────────────────────

add_subdirectory(core)
add_subdirectory(gpu)

# ─── Qt6-dependent targets (optional — skip if Qt6 not installed) ────────────
find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Network Test)
if(Qt6_FOUND)
    add_subdirectory(ui)
    add_subdirectory(export)

    # ─── Application icon (Windows) ────────────────────────────────────
    # Converts icon.png → icon.ico at build time, then embeds it into the
    # .exe via a Windows resource file.  Replacing icon.png and rebuilding
    # automatically picks up the new icon.
    set(ROUNDTABLE_ICON_PNG "${CMAKE_SOURCE_DIR}/icon.png")
    set(ROUNDTABLE_ICON_ICO "${CMAKE_BINARY_DIR}/icon.ico")

    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_FOUND AND EXISTS "${ROUNDTABLE_ICON_PNG}")
        add_custom_command(
            OUTPUT  "${ROUNDTABLE_ICON_ICO}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/png_to_ico.py"
                    "${ROUNDTABLE_ICON_PNG}" "${ROUNDTABLE_ICON_ICO}"
            DEPENDS "${ROUNDTABLE_ICON_PNG}" "${CMAKE_SOURCE_DIR}/tools/png_to_ico.py"
            COMMENT "Converting icon.png → icon.ico"
        )

        # Configure the .rc template with the absolute path to the .ico
        set(ROUNDTABLE_ICO_PATH "${ROUNDTABLE_ICON_ICO}")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/roundtable.rc.in"
            "${CMAKE_BINARY_DIR}/roundtable.rc"
            @ONLY
        )
        set(ROUNDTABLE_RC_FILE "${CMAKE_BINARY_DIR}/roundtable.rc")
        message(STATUS "Application icon: ${ROUNDTABLE_ICON_PNG}")
    else()
        set(ROUNDTABLE_RC_FILE "")
        if(NOT EXISTS "${ROUNDTABLE_ICON_PNG}")
            message(STATUS "icon.png not found — building without application icon")
        else()
            message(STATUS "Python3 not found — building without application icon")
        endif()
    endif()

    # ─── Main executable ─────────────────────────────────────────────────
    # roundtable.manifest forces the process Active Code Page to UTF-8 so
    # std::filesystem::path narrow conversions stop using CP-1252 (which
    # throws/mangles non-ANSI filenames). MSVC merges it with the default
    # linker-generated manifest via /MANIFESTINPUT.
    set(ROUNDTABLE_MANIFEST_FILE "")
    if(MSVC)
        set(ROUNDTABLE_MANIFEST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/roundtable.manifest")
    endif()
    add_executable(roundtable WIN32 main.cpp ${ROUNDTABLE_RC_FILE} ${ROUNDTABLE_MANIFEST_FILE})

    # Ensure icon.ico is built before roundtable (the .rc references it)
    if(ROUNDTABLE_RC_FILE)
        set_source_files_properties("${ROUNDTABLE_ICON_ICO}" PROPERTIES GENERATED TRUE)
        # The .rc file depends on the .ico — make sure it's generated first
        add_custom_target(generate_icon DEPENDS "${ROUNDTABLE_ICON_ICO}")
        add_dependencies(roundtable generate_icon)
    endif()

    target_link_libraries(roundtable PRIVATE
        roundtable_core
        roundtable_gpu
        roundtable_ui
        roundtable_export
        Qt6::Widgets
        $<$<PLATFORM_ID:Windows>:winmm>
    )

    if(TARGET shaders)
        add_dependencies(roundtable shaders)
    endif()

    # Copy qt.conf to build output so deployed Qt finds its plugins
    # (overrides hardcoded prefix path from portable Qt installation)
    add_custom_command(TARGET roundtable POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/qt.conf"
            "$<TARGET_FILE_DIR:roundtable>/qt.conf"
        COMMENT "Copying qt.conf to output directory"
    )

    # Copy icon.ico to project root so installer.iss picks up the
    # freshly-generated icon (SetupIconFile references root icon.ico)
    if(TARGET generate_icon)
        add_custom_command(TARGET roundtable POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ROUNDTABLE_ICON_ICO}"
                "${CMAKE_SOURCE_DIR}/icon.ico"
            COMMENT "Copying icon.ico to project root for installer"
        )
    endif()

    message(STATUS "Qt6 found — building full application")
else()
    message(STATUS "Qt6 not found — building core/gpu/tests only (UI skipped)")
endif()
