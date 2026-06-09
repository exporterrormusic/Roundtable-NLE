/*
 * BeatDetector — lightweight onset/beat detection for the beat-reactive
 * effects. Given mono PCM, it produces a list of onset times (seconds).
 *
 * Pipeline: optional bass low-pass (focus on kick/bass) → short-time energy →
 * positive energy flux → adaptive-threshold peak picking with a minimum
 * inter-onset interval. Cheap and dependency-free; good enough to drive a
 * zoom/flash on the beat without a full tempo tracker.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace rt {

struct BeatDetectorParams
{
    bool  bassOnly{true};        ///< low-pass first so kicks/bass dominate
    float bassCutoffHz{180.0f};  ///< low-pass cutoff when bassOnly
    float minIntervalSec{0.12f}; ///< reject onsets closer than this (~500 BPM cap)
    float sensitivity{1.2f};     ///< threshold = local-mean flux * sensitivity (lower = more beats)
};

/// Detect onsets in mono float PCM. Returns onset times in seconds, ascending.
[[nodiscard]] std::vector<float> detectBeats(const float* mono, size_t numSamples,
                                             uint32_t sampleRate,
                                             const BeatDetectorParams& params = {});

/// Convenience: down-mix interleaved audio to mono then detect.
[[nodiscard]] std::vector<float> detectBeatsInterleaved(const float* interleaved,
                                                        int64_t frames, uint16_t channels,
                                                        uint32_t sampleRate,
                                                        const BeatDetectorParams& params = {});

} // namespace rt
