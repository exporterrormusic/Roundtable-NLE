/*
 * RenderComplexity — static analysis of a timeline's visual compositing
 * cost, for the Premiere-style render bar (cleanup audit §4.6, slice 1).
 *
 * This is the READ-OUT half of the render-bar feature: it classifies each
 * stretch of the timeline as cheap (plays live) or heavy (would benefit from
 * a pre-render), WITHOUT rendering or caching anything.  It is a pure
 * function of the timeline's clip layout + effects, so it is deterministic
 * and unit-tested (tests/core/test_render_complexity.cpp), and the UI just
 * maps the result onto TimelineRuler::RenderBarSegment.
 *
 * Heuristic (escalate on what actually costs the compositor per-frame work,
 * strongest signal first; stay conservative on the cheap stuff so the bar
 * doesn't cry wolf):
 *   - Empty       : no visual clip active here (a gap).
 *   - NeedsRender : any of —
 *       • an active clip carries active EFFECTS (extra GPU passes — glitch/beat
 *         macros, grades; the signal users hit hardest), OR
 *       • a nested SEQUENCE clip is active (recursively composites a whole
 *         inner timeline — genuinely expensive), OR
 *       • a TRANSITION overlaps a busy stack (>=3 visual layers), OR
 *       • the layer stack is very tall (>=6 visual layers — decode/upload/blend
 *         volume threatens real-time even without effects).
 *   - RealTime    : visual content below all those bars (a lone clip, a 2–3
 *                   layer comp, a plain dissolve — the GPU handles these live).
 *
 * Still a STATIC estimate (no decode-cost / measured frame-time input).  A
 * codec-aware or measured-cost upgrade could ride on the segment cache's real
 * composite timings later; until then a tall stack of cheap stills and a tall
 * stack of 4K ProRes read the same here.
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
