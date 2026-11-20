#pragma once
/*
 * KeyframeIndex.h — where a long-GOP file's keyframes are, in frame numbers.
 *
 * Built once when a file is opened (VideoDecoderInit) from the container's
 * index.  Two users:
 *   - Very fast playback (a clip sped up 20x, or 8x shuttle) shows only
 *     keyframes: reaching an exact frame of P-frame-only H.264 means decoding
 *     every frame since the last keyframe, which no decoder can do at
 *     hundreds of frames a second.  A keyframe is one seek + one decode.
 *     Live playback only; paused, scrub and export stay frame-exact.
 *   - Jumping forward across a keyframe, the decoder seeks there instead of
 *     decoding every frame in between.
 *
 * Pure and header-only: unit-tested in tests/gpu/test_decoder.cpp.
 */

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rt {

struct KeyframeIndex
{
    std::vector<int64_t> frames;   ///< ascending keyframe frame numbers
    int64_t maxGap{0};             ///< largest distance between neighbours

    [[nodiscard]] bool usable() const noexcept { return frames.size() >= 2; }

    /// The keyframe at or before `frame`; `frame` itself when none is.
    [[nodiscard]] int64_t atOrBefore(int64_t frame) const noexcept
    {
        const auto it = std::upper_bound(frames.begin(), frames.end(), frame);
        return it == frames.begin() ? frame : *(it - 1);
    }

    /// The first keyframe after `frame`, or -1.
    [[nodiscard]] int64_t after(int64_t frame) const noexcept
    {
        const auto it = std::upper_bound(frames.begin(), frames.end(), frame);
        return it == frames.end() ? -1 : *it;
    }

    /// Going from the last decoded frame to `target` (a forward jump), is
    /// seeking to the keyframe before `target` cheaper than decoding on?
    /// Decoding on costs `target - lastDecoded` pictures; the seek costs the
    /// pictures from that keyframe to `target` plus about one for the seek.
    [[nodiscard]] bool seekBeatsDecodingOn(int64_t lastDecoded,
                                           int64_t target) const noexcept
    {
        if (lastDecoded < 0 || target <= lastDecoded + 1) return false;
        const int64_t k = atOrBefore(target);
        if (k <= lastDecoded || k > target) return false;
        return (target - k) + 2 < (target - lastDecoded);
    }

    /// From keyframe frame numbers in any order (duplicates allowed).
    [[nodiscard]] static KeyframeIndex fromFrames(std::vector<int64_t> f)
    {
        KeyframeIndex index;
        std::sort(f.begin(), f.end());
        f.erase(std::unique(f.begin(), f.end()), f.end());
        for (size_t i = 1; i < f.size(); ++i)
            index.maxGap = std::max(index.maxGap, f[i] - f[i - 1]);
        index.frames = std::move(f);
        return index;
    }
};

/// Source seconds per timeline second at or above which live playback shows
/// keyframes only (clip speed x shuttle).
inline constexpr double kKeyframeOnlyPlaybackSpeed = 8.0;

} // namespace rt
