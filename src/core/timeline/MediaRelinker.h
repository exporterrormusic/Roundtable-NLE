/*
 * MediaRelinker — walk a Timeline and update every clip whose media path
 * matches the given old path. Mirrors the pattern used by
 * AudioSync::relinkAudioFile but works across all sequences/tracks.
 */

#pragma once

#include <string>

namespace rt {

class Timeline;
class Project;
class ShotPresetManager;

namespace MediaRelinker {

/// Replace all occurrences of @p oldPath with @p newPath across every clip
/// in @p timeline (Video, Audio, Image). Returns the number of clip
/// references updated. Path comparison is exact (case-insensitive on
/// Windows is intentionally NOT applied — paths come from QFileDialog and
/// match what the clip stored).
int relinkPath(Timeline* timeline,
               const std::string& oldPath,
               const std::string& newPath);

/// Replace matching media paths across every sequence in @p project.
/// Returns the total number of Video, Audio, and Image clip references
/// updated. This is the project-bin relink behavior: inactive sequences are
/// kept in sync with the active one.
int relinkPath(Project* project,
               const std::string& oldPath,
               const std::string& newPath);

/// Walk every saved shot preset and rewrite background paths matching
/// @p oldPath to @p newPath. Modified presets are re-saved to disk.
/// Returns the number of background references updated.
int relinkPresetBackground(ShotPresetManager* mgr,
                           const std::string& oldPath,
                           const std::string& newPath);

} // namespace MediaRelinker
} // namespace rt
