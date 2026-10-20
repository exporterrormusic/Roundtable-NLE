/*
 * SyncClip.h -- the AudioSync data model shared by the panel and the core
 * sync algorithms (audiosync/AutoSync.h): one recorded clip and the decoded
 * samples of an imported file.
 */

#pragma once

#include "audio/SampleBuffer.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rt {

// ─── Audio clip data model ──────────────────────────────────────────────────

struct SyncClip
{
    int         id{0};
    std::string sourceFile;
    std::string character;
    double      start{0.0};     // seconds
    double      end{0.0};       // seconds
    std::string transcript;
    std::string editedText;
    int         matchState{0};  // 0=unmatched, 1=tentative, 2=confirmed
    float       confidence{0.0f};
    int         scriptLineNumber{-1};
    std::string scriptSegment;
    std::vector<std::pair<double,double>> deletedRegions;

    bool operator==(const SyncClip&) const = default;
};

/// Loaded audio sample data (mono float, keyed by file path). The samples
/// are normally a memory mapping of the decoded-audio cache file; copies
/// (waveforms, dialogs) share them.
struct AudioSampleData {
    SampleBuffer samples;  // mono float -1..1
    uint32_t sampleRate{44100};
};

} // namespace rt
