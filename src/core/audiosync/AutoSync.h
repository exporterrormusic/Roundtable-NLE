/*
 * AutoSync.h -- the AudioSync algorithms, free of any UI: turning
 * transcriptions into clips, matching clips to script lines (word-level
 * alignment first, then the heuristic merge / re-segment / global match /
 * speech trim), and silence trim + gap closing.
 *
 * Each function works on one script session's data (SyncSource) and edits a
 * clip list in place. The AudioSync panel reads its controls into
 * AutoSyncOptions, calls these, and shows the results.
 *
 *   autoSyncClips            the whole Auto-Sync (AutoSync.cpp)
 *   alignClipsToScript       word-level alignment (AutoSyncAlign.cpp)
 *   alignShotTakes           every take of a shot-table line (AutoSyncAlign.cpp)
 *   mergeSegmentsToMatchScript, resegmentByScript
 *                            heuristic pre-passes (AutoSyncResegment.cpp)
 *   clipsFromTranscriptions, appendClipsFromTranscriptions,
 *   closeInterClipGaps       (AutoSync.cpp)
 */

#pragma once

#include "audiosync/SyncClip.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

struct Script;
struct TranscriptionResult;

namespace audiosync {

/// One script session's data the algorithms read.
struct SyncSource
{
    const Script* script;                                          ///< may be null
    const std::vector<std::string>& audioPaths;                    ///< imported files
    const std::vector<TranscriptionResult>& transcriptions;        ///< per file, index-aligned
    const std::unordered_map<std::string, AudioSampleData>& samples;  ///< by file path
};

struct AutoSyncOptions
{
    /// Sync only this character's clips and lines (empty: everyone).
    std::string characterFilter;
    double frontPadSec{0.0};
    double endPadSec{0.2};
    /// Let several clips match the same line (retakes).
    bool retakes{false};
    /// Progress in percent with a short description.
    std::function<void(int, const std::string&)> progress;
};

struct AutoSyncResult
{
    enum class Path { NothingToSync, WordAligned, Heuristic };
    Path path{Path::NothingToSync};
    int matchCount{0};   ///< heuristic path: clips matched (in scope)
    int scopeTotal{0};   ///< heuristic path: clips in scope
};

struct GapCloseResult
{
    int trimmed{0};
    int gapsClosed{0};
    bool tooFewToClose{false};   ///< fewer than two clips: trimmed only
};

/// Match unconfirmed clips to script lines. Confirmed clips are kept.
[[nodiscard]] AutoSyncResult autoSyncClips(const SyncSource& source,
                                           std::vector<SyncClip>& clips,
                                           const AutoSyncOptions& options);

/// Word-level forced alignment: rebuilds the clips so each spans exactly one
/// script line's words. False (clips untouched) without word timestamps or
/// when no file's character is in the script.
bool alignClipsToScript(const SyncSource& source, std::vector<SyncClip>& clips,
                        double frontPadSec, double endPadSec);

/// Shot-table scripts: one candidate clip per take of each line; confirmed
/// clips are kept. Clip ids are renumbered.
bool alignShotTakes(const SyncSource& source, std::vector<SyncClip>& clips,
                    double frontPadSec, double endPadSec);

/// Merge adjacent short segments that match a script line better together.
void mergeSegmentsToMatchScript(const SyncSource& source, std::vector<SyncClip>& clips);

/// Re-cut clips so their boundaries follow script lines.
void resegmentByScript(const SyncSource& source, std::vector<SyncClip>& clips);

/// One clip per transcribed segment of every file.
[[nodiscard]] std::vector<SyncClip> clipsFromTranscriptions(const SyncSource& source);

/// Add clips for files that have none yet (existing clips keep their state).
void appendClipsFromTranscriptions(const SyncSource& source, std::vector<SyncClip>& clips);

/// Trim silence from unconfirmed clips, then close small gaps between
/// neighbouring clips of one file.
GapCloseResult closeInterClipGaps(const SyncSource& source, std::vector<SyncClip>& clips);

} // namespace audiosync
} // namespace rt
