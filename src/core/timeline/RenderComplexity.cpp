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

/// True for tracks whose clips appear in the composited picture.
bool isVisualTrack(const Track& tr) noexcept
{
    return tr.type() == TrackType::Video && !tr.isDivider() && !tr.isCaptionTrack();
}

/// Classify the composition active at a single tick.
RenderComplexity classifyAt(const Timeline& timeline, int64_t tick)
{
    int  visualLayers = 0;
    bool anyEffects   = false;

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* tr = timeline.track(ti);
        if (!tr || !isVisualTrack(*tr)) continue;

        for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
            const Clip* c = tr->clip(ci);
            if (!c || !c->isVisual()) continue;
            // Half-open: a clip ending exactly at `tick` is no longer active.
            if (tick < c->timelineIn() || tick >= c->timelineOut()) continue;
            ++visualLayers;
            if (c->effects().hasActiveEffects()) anyEffects = true;
        }
    }

    if (visualLayers == 0) return RenderComplexity::Empty;
    return anyEffects ? RenderComplexity::NeedsRender : RenderComplexity::RealTime;
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
