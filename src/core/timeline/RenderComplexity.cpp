/*
 * RenderComplexity.cpp — see RenderComplexity.h for the contract + heuristic.
 */

#include "timeline/RenderComplexity.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/Transition.h"

#include <algorithm>

namespace rt {

namespace {

/// A composite carrying this many simultaneous visual layers is flagged heavy
/// even with no effects: that many per-frame decodes + uploads + blends starts
/// to threaten real-time on ordinary hardware.  Kept high so a routine 2–3
/// layer comp (which the GPU blends trivially) stays RealTime.
constexpr int kHeavyLayerCount = 6;

/// True for tracks whose clips appear in the composited picture.
bool isVisualTrack(const Track& tr) noexcept
{
    return tr.type() == TrackType::Video && !tr.isDivider() && !tr.isCaptionTrack();
}

/// Classify the composition active at a single tick.
///
/// "NeedsRender" = worth pre-rendering because it likely won't sustain real-
/// time.  We escalate on the signals that actually cost the compositor extra
/// per-frame work, ordered strongest first, while staying conservative on the
/// cheap ones (a lone dissolve, a 2–3 layer stack) so the bar doesn't cry wolf:
///   - any active EFFECT         → extra GPU passes (glitch/beat macros, grades)
///   - a nested SEQUENCE         → recursively composites a whole inner timeline
///   - a TRANSITION over a busy stack (>=3 layers) → blend pass atop real work
///   - a very TALL layer stack (>=kHeavyLayerCount) → decode/upload/blend volume
RenderComplexity classifyAt(const Timeline& timeline, int64_t tick)
{
    int  visualLayers = 0;
    bool anyEffects   = false;
    bool anyNested    = false;
    int  transitions  = 0;

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* tr = timeline.track(ti);
        if (!tr || !isVisualTrack(*tr)) continue;

        for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
            const Clip* c = tr->clip(ci);
            if (!c || !c->isVisual()) continue;
            // Half-open: a clip ending exactly at `tick` is no longer active.
            if (tick < c->timelineIn() || tick >= c->timelineOut()) continue;
            ++visualLayers;
            if (c->effects().hasActiveEffects())        anyEffects = true;
            if (c->clipType() == ClipType::Sequence)    anyNested  = true;
        }

        for (const Transition& t : tr->transitions()) {
            int64_t ts = 0, te = 0;
            t.getRange(ts, te);
            if (tick >= ts && tick < te) ++transitions;
        }
    }

    if (visualLayers == 0) return RenderComplexity::Empty;

    // Strong, always-heavy signals.
    if (anyEffects) return RenderComplexity::NeedsRender;
    if (anyNested)  return RenderComplexity::NeedsRender;

    // Volume / combination signals (conservative bars — see the doc comment).
    if (transitions > 0 && visualLayers >= 3) return RenderComplexity::NeedsRender;
    if (visualLayers >= kHeavyLayerCount)     return RenderComplexity::NeedsRender;

    return RenderComplexity::RealTime;
}

} // namespace

std::vector<RenderComplexitySegment> analyzeRenderComplexity(const Timeline& timeline)
{
    // ── Collect the ticks where the composition can change: every visual
    //    clip's in/out edge.  Complexity is constant between consecutive
    //    edges, so we only need to classify one representative tick per gap.
    std::vector<int64_t> edges;
    edges.push_back(0);

    int64_t maxEnd = 0;
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* tr = timeline.track(ti);
        if (!tr || !isVisualTrack(*tr)) continue;
        for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
            const Clip* c = tr->clip(ci);
            if (!c || !c->isVisual()) continue;
            const int64_t in  = c->timelineIn();
            const int64_t out = c->timelineOut();
            edges.push_back(in);
            edges.push_back(out);
            maxEnd = std::max(maxEnd, out);
        }

        // Transition start/end are also complexity edges (a dissolve over a
        // busy stack flips a sub-span to NeedsRender), so the bar must be able
        // to break there even mid-clip.
        for (const Transition& t : tr->transitions()) {
            int64_t ts = 0, te = 0;
            t.getRange(ts, te);
            edges.push_back(ts);
            edges.push_back(te);
            maxEnd = std::max(maxEnd, te);
        }
    }

    const int64_t end = std::max<int64_t>(timeline.duration(), maxEnd);
    if (end <= 0) return {};   // no visual content
    edges.push_back(end);

    // Dedup + drop anything outside (0, end).
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    edges.erase(std::remove_if(edges.begin(), edges.end(),
                               [end](int64_t t) { return t < 0 || t > end; }),
                edges.end());

    // ── Walk consecutive [a, b) intervals, classify at `a`, coalesce. ───
    std::vector<RenderComplexitySegment> segments;
    for (size_t i = 0; i + 1 < edges.size(); ++i) {
        const int64_t a = edges[i];
        const int64_t b = edges[i + 1];
        if (b <= a) continue;
        const RenderComplexity cx = classifyAt(timeline, a);

        if (!segments.empty() && segments.back().complexity == cx &&
            segments.back().endTick == a) {
            segments.back().endTick = b;   // extend the run
        } else {
            segments.push_back({a, b, cx});
        }
    }

    return segments;
}

} // namespace rt
