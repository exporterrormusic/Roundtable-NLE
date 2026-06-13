/*
 * RenderComplexity — static analysis of a timeline's visual compositing
 * cost, for the Premiere-style render bar (fable_cleanup.txt §4.6, slice 1).
 *
 * This is the READ-OUT half of the render-bar feature: it classifies each
 * stretch of the timeline as cheap (plays live) or heavy (would benefit from
 * a pre-render), WITHOUT rendering or caching anything.  It is a pure
 * function of the timeline's clip layout + effects, so it is deterministic
 * and unit-tested (tests/core/test_render_complexity.cpp), and the UI just
 * maps the result onto TimelineRuler::RenderBarSegment.
 *
 * v1 heuristic (deliberately conservative so the bar doesn't cry wolf):
 *   - Empty       : no visual clip active here (a gap).
 *   - NeedsRender : at least one active visual clip carries active effects —
 *                   the strong "extra GPU passes per frame" signal, and the
 *                   one users hit hardest (glitch/beat macros).
 *   - RealTime    : visual content with no effects.
 *
 * Intentionally NOT yet factored in (they need real frame-time measurement,
 * which is slice 2's segment-render cache, not a static guess): transitions,
 * the number of stacked layers, and source decode cost (ProRes etc.).  The
 * compositor blits/blends layers cheaply on the GPU, so flagging every
 * dissolve or multi-layer comp red would just make the bar noise.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace rt {

class Timeline;

/// Estimated compositing cost of a timeline stretch.
enum class RenderComplexity : uint8_t
{
    Empty,        ///< No visual clip active (gap).
    RealTime,     ///< Visual content, cheap enough to composite live.
    NeedsRender,  ///< Heavy enough to warrant a pre-render (has effects).
};

/// A maximal run of timeline with one complexity, half-open [startTick, endTick).
struct RenderComplexitySegment
{
    int64_t          startTick{0};
    int64_t          endTick{0};
    RenderComplexity complexity{RenderComplexity::Empty};
};

/// Partition [0, timelineEnd) into coalesced complexity segments.  Adjacent
/// runs of equal complexity are merged.  Returns empty when the timeline has
/// no visual content / zero duration.  Only video tracks count (audio,
/// dividers, and caption tracks are ignored — captions are cheap text burn-in
/// and don't drive the render bar).
[[nodiscard]] std::vector<RenderComplexitySegment>
analyzeRenderComplexity(const Timeline& timeline);

} // namespace rt
