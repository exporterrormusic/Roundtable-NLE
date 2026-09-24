#pragma once

#include <filesystem>

namespace rt::AppPaths {

/// The application root: the folder holding assets/ (and logs/).
///
///  - Development: the exe runs from build/bin/<Config>/, so the root is the
///    nearest parent containing both CMakeLists.txt and assets/ (repo root).
///  - Installed: assets/ sits beside the exe, so the root is the exe folder.
///  - ROUNDTABLE_ROOT overrides both.
///
/// Falls back to the working directory at first use. Resolved once.
[[nodiscard]] const std::filesystem::path& root();

/// root() / "assets"
[[nodiscard]] std::filesystem::path assets();

/// Make root() the process working directory.  Projects and presets store
/// asset paths relative to the root ("assets/backgrounds/x.png"), so they
/// must resolve identically however the app was started: launcher script,
/// installer shortcut (no working directory set), or a double-clicked exe.
/// Returns false (leaving the directory unchanged) if the switch failed.
bool makeRootCurrentDirectory();

} // namespace rt::AppPaths
