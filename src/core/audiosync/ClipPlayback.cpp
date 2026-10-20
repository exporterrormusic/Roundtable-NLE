/*
 * ClipPlayback.cpp -- see ClipPlayback.h. Moved from AudioSync::playClip,
 * its playhead timer and seekPlayingClip unchanged.
 */

#include "audiosync/ClipPlayback.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rt::audiosync {

double ClipPlayback::sourceTimeAt(int64_t frame, double fallback) const
{
    double currentTime = fallback;
    for (size_t i = 0; i < timeMap.size(); ++i) {
        auto [segStart, segTime] = timeMap[i];
        int64_t segEnd = (i + 1 < timeMap.size())
            ? timeMap[i + 1].first
            : static_cast<int64_t>(samples.size());
        if (frame >= segStart && frame < segEnd) {
            currentTime = segTime + static_cast<double>(frame - segStart) / rate;
            break;
        }
    }
    return currentTime;
}

int64_t ClipPlayback::frameAt(double time) const
{
    int64_t targetFrame = 0;
    for (size_t i = 0; i < timeMap.size(); ++i) {
        auto [segStart, segTime] = timeMap[i];
        int64_t segEnd = (i + 1 < timeMap.size())
            ? timeMap[i + 1].first
            : static_cast<int64_t>(samples.size());
        double segDuration = static_cast<double>(segEnd - segStart) / rate;
        if (time >= segTime && time < segTime + segDuration) {
            targetFrame = segStart + static_cast<int64_t>((time - segTime) * rate);
            break;
        }
        if (time < segTime) {
            targetFrame = segStart;
            break;
        }
        targetFrame = segEnd;
    }
    return targetFrame;
}

ClipPlayback buildClipPlayback(const AudioSampleData& audioData, double start, double end,
                               std::vector<std::pair<double, double>> deletedRegions,
                               double engineRate)
{
    ClipPlayback playback;
    auto& buffer = playback.samples;
    auto& timeMap = playback.timeMap;

    // Build buffer from trimIn..trimOut, excluding deletedRegions
    auto startSample = static_cast<size_t>(start * audioData.sampleRate);
    auto endSample   = static_cast<size_t>(end * audioData.sampleRate);
    if (startSample >= audioData.samples.size()) return playback;
    if (endSample > audioData.samples.size()) endSample = audioData.samples.size();

    // Sort deleted regions by start time for sequential processing
    auto sortedDeleted = std::move(deletedRegions);
    std::sort(sortedDeleted.begin(), sortedDeleted.end());

    // Build buffer skipping deleted regions, and build a time-mapping
    double sr = audioData.sampleRate;

    double cursor = start;
    for (const auto& [delStart, delEnd] : sortedDeleted) {
        if (delStart >= end) break;
        double regionStart = std::max(delStart, start);
        double regionEnd   = std::min(delEnd, end);
        if (regionStart <= cursor && regionEnd > cursor) {
            // Cursor is inside this deleted region — skip ahead
            cursor = regionEnd;
            continue;
        }
        if (regionStart > cursor) {
            // Copy samples from cursor to regionStart
            auto from = static_cast<size_t>(cursor * sr);
            auto to   = static_cast<size_t>(regionStart * sr);
            from = std::min(from, audioData.samples.size());
            to   = std::min(to, audioData.samples.size());
            if (to > from) {
                timeMap.push_back({static_cast<int64_t>(buffer.size()), cursor});
                buffer.insert(buffer.end(),
                    audioData.samples.begin() + static_cast<ptrdiff_t>(from),
                    audioData.samples.begin() + static_cast<ptrdiff_t>(to));
            }
        }
        cursor = regionEnd;
    }
    // Copy remaining samples from cursor to clip end
    if (cursor < end) {
        auto from = static_cast<size_t>(cursor * sr);
        auto to   = std::min(endSample, audioData.samples.size());
        from = std::min(from, audioData.samples.size());
        if (to > from) {
            timeMap.push_back({static_cast<int64_t>(buffer.size()), cursor});
            buffer.insert(buffer.end(),
                audioData.samples.begin() + static_cast<ptrdiff_t>(from),
                audioData.samples.begin() + static_cast<ptrdiff_t>(to));
        }
    }

    // ── Resample buffer to engine output rate if rates differ ─────────
    // The AudioEngine callback advances its play position at the engine
    // sample rate and reads source samples by index. If the buffer is at
    // a different rate, playback speed and duration will be wrong.
    if (std::abs(sr - engineRate) > 1.0 && !buffer.empty()) {
        double ratio = engineRate / sr;
        auto newSize = static_cast<size_t>(
            static_cast<double>(buffer.size()) * ratio) + 1;
        std::vector<float> resampled(newSize);
        for (size_t j = 0; j < newSize; ++j) {
            double srcPos = static_cast<double>(j) / ratio;
            auto idx = static_cast<size_t>(srcPos);
            float frac = static_cast<float>(srcPos - static_cast<double>(idx));
            if (idx + 1 < buffer.size()) {
                resampled[j] = buffer[idx] * (1.0f - frac)
                             + buffer[idx + 1] * frac;
            } else if (idx < buffer.size()) {
                resampled[j] = buffer[idx];
            }
        }
        // Scale time-map offsets from source-rate frames to engine-rate frames
        for (auto& [offset, time] : timeMap) {
            offset = static_cast<int64_t>(static_cast<double>(offset) * ratio);
        }
        buffer = std::move(resampled);
        sr = engineRate;
    }
    playback.rate = sr;   // the effective rate, for the playhead and seeks
    return playback;
}

} // namespace rt::audiosync
