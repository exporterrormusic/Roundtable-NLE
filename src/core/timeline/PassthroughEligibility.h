/*
 * PassthroughEligibility — Phase 4.2 export 16F passthrough predicate.
 *
 * Decides whether the composite at a single timeline tick is exactly ONE
 * full-frame, fully-opaque video clip rendered 1:1 — no second visual layer,
 * no transition, no effect, no opacity<1, no transform/scale/rotation/crop, no
 * blend mode, no opacity mask, no retime, no caption, and no active adjustment
 * layer above it.  When so, the export path can skip the 8-bit compositor and
 * pack that single source frame straight to 10-bit (see the 16F passthrough in
 * CompositeService).
 *
 * This is STRICTLY STRONGER than RenderComplexity::RealTime and is a sibling of
 * RenderComplexity::classifyAt: a pure, unit-testable walk of the timeline at a
 * tick.  It deliberately computes eligibility from the raw Clip keyframe tracks
 * — NOT from a built LayerInfo, which folds the video-character 0.85× contain-
 * fit into the layer's scale and would mis-judge a 1:1 clip.
 *
 * SCOPE: this covers only the TIMELINE-level conditions.  The SOURCE-level gates
 * (source bit depth > 8, source dimensions == output dimensions, not alpha /
 * packed-alpha, not VFR, not display-rotated, BT.709-limited-SDR colour) need
 * the decoded VideoStreamInfo and are applied by the caller (CompositeService)
 * after resolving the media handle for the returned clip.
 */

#pragma once

#include <cstdint>

namespace rt {

class Timeline;
class VideoClip;

/// Result of evaluatePassthroughAt.  `clip`/`localTick` are only meaningful
/// when `eligible` is true.
struct PassthroughTarget {
    bool       eligible{false};
    VideoClip* clip{nullptr};    ///< The single source video clip.
    int64_t    localTick{0};     ///< tick - clip->timelineIn() (for keyframe eval / mapping).
};

/// Evaluate timeline-level passthrough eligibility at `tick`.  Takes a non-const
/// Timeline because the per-tick keyframe accessors (opacity/scale/…) are
/// non-const.  Pure: reads the timeline, mutates nothing.
[[nodiscard]] PassthroughTarget evaluatePassthroughAt(Timeline& timeline, int64_t tick);

} // namespace rt
