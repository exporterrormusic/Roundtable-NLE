/*
 * SrtIO — Import / export SRT subtitle files.
 *
 * Import: parses SRT entries and creates CaptionClips on the caption track.
 * Export: writes the caption track's CaptionClips as SRT entries (falls back
 *         to legacy GraphicClip text clips when the project has no captions).
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rt {

class Timeline;

struct SrtEntry
{
    int         index{0};
    int64_t     startTick{0};
    int64_t     endTick{0};
    std::string text;
};

/// Parse an SRT file into subtitle entries.
std::vector<SrtEntry> parseSrt(const std::filesystem::path& path);

/// Import SRT entries as CaptionClips on the caption track (created if absent).
/// Returns the number of clips created.
int importSrt(Timeline& timeline, const std::vector<SrtEntry>& entries);

/// Export captions from the timeline to an SRT file ("Speaker: text" when a
/// speaker is set). Falls back to GraphicClip text clips for legacy projects.
/// Returns the number of entries written.
int exportSrt(const Timeline& timeline, const std::filesystem::path& path);

/// Export timeline markers as a YouTube-chapters text block ("MM:SS Title"
/// per line, 00:00 intro synthesized if the first marker starts later).
/// Returns the number of chapter lines written (0 = no markers / IO error).
int exportYouTubeChapters(const Timeline& timeline,
                          const std::filesystem::path& path);

} // namespace rt
