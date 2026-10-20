/*
 * ClipPlayback.h -- a sync clip's audio as AudioSync plays it: trimmed to
 * the clip, its deleted regions cut out, resampled to the engine's rate,
 * with a map from frames of that buffer back to time in the source file
 * (for the playhead and for seeking).
 */

#pragma once

#include "audiosync/SyncClip.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace rt::audiosync {

struct ClipPlayback
{
    /// Mono samples at `rate`.
    std::vector<float> samples;
    /// One entry per kept stretch: the buffer frame it starts at and the
    /// source time of that frame, in order.
    std::vector<std::pair<int64_t, double>> timeMap;
    double rate{0.0};

    /// Source time of buffer frame `frame`; `fallback` when the frame is
    /// past the last stretch.
    [[nodiscard]] double sourceTimeAt(int64_t frame, double fallback) const;
    /// Buffer frame of source time `time`. A time in a cut lands on the
    /// start of the next kept stretch; past the end, on the end.
    [[nodiscard]] int64_t frameAt(double time) const;
};

/// The playback buffer for [start, end) of `audio` without
/// `deletedRegions`, at `engineRate` (linear resampling when the rates
/// differ by more than 1 Hz). Empty when start is past the audio.
[[nodiscard]] ClipPlayback buildClipPlayback(const AudioSampleData& audio, double start,
                                             double end,
                                             std::vector<std::pair<double, double>> deletedRegions,
                                             double engineRate);

} // namespace rt::audiosync
