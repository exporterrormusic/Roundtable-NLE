#pragma once
/*
 * DecodeAheadPlan.h — which source frames playback will actually show next.
 *
 * Decode-ahead used to walk a file in fixed integer strides that only knew
 * the TRANSPORT speed (J/K/L) and the source/project fps ratio.  A clip sped
 * up in Speed/Duration (4x) played at 1x therefore decoded, converted and
 * cached every source frame while only every 4th was shown: 4x the convert
 * work, a read-ahead covering 1/4 of the time, and a cache full of frames
 * nobody asked for.  Non-integer rates (1.5x) were also wrong with any
 * integer stride.
 *
 * The planner works from the exact fractional source position the
 * compositor mapped (VideoFrameMapping) and steps it by the real
 * source-frames-per-displayed-frame rate, so it names exactly the frames
 * mapTickToSourceFrame will request: the nearest frame for Frame Sampling,
 * floor AND ceil for the temporal modes (Frame Blending / Optical Flow).
 *
 * Pure and header-only: unit-tested in tests/gpu/test_decoder.cpp.
 */

#include "decode/KeyframeIndex.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt {

struct DecodeAheadRate
{
    /// Source frames advanced per displayed (project) frame.  Below
    /// kStridedStepMin the plan is dense (every frame), as before.
    double  step{1.0};
    /// Exact source position the compositor last mapped for this file and
    /// the frame it requested there; anchorFrame < 0 = no anchor (phase 0).
    double  anchorPos{0.0};
    int64_t anchorFrame{-1};
    /// Temporal interpolation: each displayed frame needs floor and ceil.
    bool    bothNeighbours{false};

    /// Non-null: playback is fast enough to show keyframes only
    /// (KeyframeIndex.h), so each displayed frame needs just its keyframe.
    std::shared_ptr<const KeyframeIndex> keyframesOnly;

    static constexpr double kStridedStepMin = 1.1;
    [[nodiscard]] bool strided() const noexcept { return step >= kStridedStepMin; }
};

/// Source frames needed for the next `count` displayed frames, ascending and
/// de-duplicated, all >= fromFrame.  Dense (fromFrame, fromFrame+1, ...) when
/// the rate is not strided.  Frame-count clamping/looping is the caller's.
[[nodiscard]] inline std::vector<int64_t>
plannedDecodeAheadFrames(const DecodeAheadRate& rate, int64_t fromFrame, int count)
{
    std::vector<int64_t> out;
    if (count <= 0) return out;

    if (rate.keyframesOnly && rate.keyframesOnly->usable() &&
        std::isfinite(rate.step) && rate.step > 0.0) {
        // A keyframe is shown when some displayed position rounds into
        // [keyframe, next keyframe).  Walk the keyframes rather than the
        // positions: dozens of positions can share one keyframe.
        const auto& kf = rate.keyframesOnly->frames;
        const bool anchored = rate.anchorFrame >= 0;
        const double phase = anchored ? rate.anchorPos
                                      : static_cast<double>(fromFrame);
        // Displayed positions are phase + j * step; look `count` ahead.
        const double horizon = static_cast<double>(fromFrame) +
                               static_cast<double>(count) * rate.step;
        auto it = std::lower_bound(kf.begin(), kf.end(), fromFrame);
        for (; it != kf.end() && static_cast<int>(out.size()) < count; ++it) {
            const int64_t k = *it;
            if (static_cast<double>(k) - 0.5 > horizon) break;
            const int64_t nextK = (it + 1 != kf.end()) ? *(it + 1) : INT64_MAX;
            // First displayed position that rounds to k or later.
            const double j = std::ceil(
                (static_cast<double>(k) - 0.5 - phase) / rate.step);
            const int64_t shown = std::llround(phase + j * rate.step);
            if (shown < nextK) out.push_back(k);
        }
        return out;
    }

    if (!rate.strided() || !std::isfinite(rate.step)) {
        out.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) out.push_back(fromFrame + i);
        return out;
    }

    // Keep the anchor's phase when we are at or ahead of it; a request
    // behind the anchor (seek back, another clip of the same file) has no
    // usable phase and starts at the requested frame itself.
    double pos = static_cast<double>(fromFrame);
    if (rate.anchorFrame >= 0 && fromFrame >= rate.anchorFrame - 1) {
        const double k = std::floor(
            (static_cast<double>(fromFrame) - rate.anchorPos) / rate.step);
        pos = rate.anchorPos + (k > 0.0 ? k : 0.0) * rate.step;
    }

    // The last frame a displayed position at `p` needs.
    const auto lastFrameOf = [&](double p) -> int64_t {
        if (!rate.bothNeighbours) return std::llround(p);
        const double lo = std::floor(p);
        return static_cast<int64_t>(lo) + ((p - lo) > 1e-6 ? 1 : 0);
    };
    // Skip displayed positions entirely behind fromFrame (bounded: the k
    // estimate above lands within a step or two).
    for (int guard = 0; guard < 8 && lastFrameOf(pos) < fromFrame; ++guard)
        pos += rate.step;

    out.reserve(static_cast<size_t>(count) * (rate.bothNeighbours ? 2u : 1u));
    const auto pushFrame = [&](int64_t f) {
        if (f < fromFrame) return;
        if (!out.empty() && f <= out.back()) return;
        out.push_back(f);
    };
    for (int i = 0; i < count; ++i, pos += rate.step) {
        if (rate.bothNeighbours) {
            const double lo = std::floor(pos);
            pushFrame(static_cast<int64_t>(lo));
            if (pos - lo > 1e-6) pushFrame(static_cast<int64_t>(lo) + 1);
        } else {
            pushFrame(std::llround(pos));
        }
    }
    return out;
}

} // namespace rt
