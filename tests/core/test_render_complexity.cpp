/*
 * test_render_complexity.cpp — render-bar read-out analysis (§4.6 slice 1).
 *
 * analyzeRenderComplexity is a pure function of the timeline's visual clip
 * layout + effects, so it is fully testable without a GPU/decoder:
 *   - gaps are Empty, plain clips RealTime, effected clips NeedsRender;
 *   - segments tile [0, end) and coalesce equal-complexity runs;
 *   - only visual (video, non-caption) tracks count.
 */

#include <gtest/gtest.h>

#include "timeline/RenderComplexity.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/SpineClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/Transition.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"

namespace rt {
namespace {

Clip* addVisualClip(Track* tr, int64_t in, int64_t dur, bool withEffect)
{
    auto c = std::make_unique<SpineClip>();
    c->setTimelineIn(in);
    c->setDuration(dur);
    if (withEffect)
        c->effects().addEffect(createEffect(EffectType::ColorCorrect));
    return tr->addClip(std::move(c));
}

Clip* addNestedClip(Track* tr, int64_t in, int64_t dur)
{
    auto c = std::make_unique<SequenceClip>();
    c->setTimelineIn(in);
    c->setDuration(dur);
    return tr->addClip(std::move(c));
}

// Add a cross-dissolve covering [editPoint - dur/2, editPoint + dur/2).
void addDissolve(Track* tr, int64_t editPoint, int64_t dur)
{
    Transition t;
    t.type          = TransitionType::CrossDissolve;
    t.editPointTick = editPoint;
    t.duration      = dur;
    t.leftClipId    = 1;   // both non-zero → cross-dissolve range
    t.rightClipId   = 2;
    tr->addTransition(t);
}

// Find the segment covering a tick (segments are half-open [start, end)).
RenderComplexity complexityAt(const std::vector<RenderComplexitySegment>& segs,
                              int64_t tick)
{
    for (const auto& s : segs)
        if (tick >= s.startTick && tick < s.endTick) return s.complexity;
    return RenderComplexity::Empty;
}

TEST(RenderComplexity, EmptyTimelineYieldsNoSegments)
{
    Timeline tl;
    tl.addVideoTrack("V1");
    EXPECT_TRUE(analyzeRenderComplexity(tl).empty());
}

TEST(RenderComplexity, PlainClipIsRealTime)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addVisualClip(v1, 0, 48000, /*withEffect=*/false);

    const auto segs = analyzeRenderComplexity(tl);
    ASSERT_FALSE(segs.empty());
    EXPECT_EQ(complexityAt(segs, 1000), RenderComplexity::RealTime);
}

TEST(RenderComplexity, EffectedClipNeedsRender)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addVisualClip(v1, 0, 48000, /*withEffect=*/true);

    const auto segs = analyzeRenderComplexity(tl);
    EXPECT_EQ(complexityAt(segs, 1000), RenderComplexity::NeedsRender);
}

TEST(RenderComplexity, GapBetweenClipsIsEmpty)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addVisualClip(v1, 0,     48000, false);   // [0, 48000)
    addVisualClip(v1, 96000, 48000, false);   // [96000, 144000)

    const auto segs = analyzeRenderComplexity(tl);
    EXPECT_EQ(complexityAt(segs, 1000),  RenderComplexity::RealTime);
    EXPECT_EQ(complexityAt(segs, 60000), RenderComplexity::Empty);   // the gap
    EXPECT_EQ(complexityAt(segs, 100000), RenderComplexity::RealTime);
}

TEST(RenderComplexity, BoundariesTrackEffectChange)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addVisualClip(v1, 0,     48000, false);   // plain   [0, 48000)
    addVisualClip(v1, 48000, 48000, true);    // effected [48000, 96000)

    const auto segs = analyzeRenderComplexity(tl);
    EXPECT_EQ(complexityAt(segs, 47999), RenderComplexity::RealTime);
    EXPECT_EQ(complexityAt(segs, 48000), RenderComplexity::NeedsRender);
}

TEST(RenderComplexity, AdjacentEqualRunsCoalesce)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addVisualClip(v1, 0,     48000, false);
    addVisualClip(v1, 48000, 48000, false);   // touching, both plain

    const auto segs = analyzeRenderComplexity(tl);
    // The two touching plain clips collapse into a single RealTime segment.
    int realtimeRuns = 0;
    for (const auto& s : segs)
        if (s.complexity == RenderComplexity::RealTime) ++realtimeRuns;
    EXPECT_EQ(realtimeRuns, 1);
}

TEST(RenderComplexity, EffectOnUpperLayerMarksOverlap)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* v2 = tl.addVideoTrack("V2");
    addVisualClip(v1, 0, 96000, false);        // base, plain
    addVisualClip(v2, 24000, 24000, true);     // overlay with effect [24000,48000)

    const auto segs = analyzeRenderComplexity(tl);
    EXPECT_EQ(complexityAt(segs, 10000), RenderComplexity::RealTime);    // base only
    EXPECT_EQ(complexityAt(segs, 30000), RenderComplexity::NeedsRender); // overlay effect
    EXPECT_EQ(complexityAt(segs, 60000), RenderComplexity::RealTime);    // base only again
}

TEST(RenderComplexity, AudioTrackIgnored)
{
    Timeline tl;
    tl.addVideoTrack("V1");
    tl.addAudioTrack("A1");
    // No visual clips anywhere → nothing to render-bar even if audio existed.
    EXPECT_TRUE(analyzeRenderComplexity(tl).empty());
}

// ── Smarter heuristic: nested sequences, transitions, tall stacks ───────────

TEST(RenderComplexity, NestedSequenceNeedsRender)
{
    // A nested SequenceClip recursively composites a whole inner timeline —
    // always worth pre-rendering, even with no effects of its own.
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addNestedClip(v1, 0, 48000);

    const auto segs = analyzeRenderComplexity(tl);
    EXPECT_EQ(complexityAt(segs, 1000), RenderComplexity::NeedsRender);
}

TEST(RenderComplexity, LoneDissolveStaysRealTime)
{
    // A plain cross-dissolve between two single clips is cheap — only one
    // layer is live across the dissolve, so it must NOT trip NeedsRender.
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addVisualClip(v1, 0,     48000, false);   // [0, 48000)
    addVisualClip(v1, 48000, 48000, false);   // [48000, 96000)
    addDissolve(v1, 48000, 12000);            // [42000, 54000)

    const auto segs = analyzeRenderComplexity(tl);
    EXPECT_EQ(complexityAt(segs, 48000), RenderComplexity::RealTime);
}

TEST(RenderComplexity, TransitionOverBusyStackNeedsRender)
{
    // Three stacked layers are fine live, but a transition blending atop them
    // adds a pass over real work → NeedsRender only inside the transition span.
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* v2 = tl.addVideoTrack("V2");
    auto* v3 = tl.addVideoTrack("V3");
    addVisualClip(v1, 0, 48000, false);
    addVisualClip(v2, 0, 48000, false);
    addVisualClip(v3, 0, 48000, false);
    addDissolve(v1, 18000, 12000);            // [12000, 24000)

    const auto segs = analyzeRenderComplexity(tl);
    EXPECT_EQ(complexityAt(segs, 18000), RenderComplexity::NeedsRender); // in transition
    EXPECT_EQ(complexityAt(segs, 30000), RenderComplexity::RealTime);    // 3 layers, no transition
}

TEST(RenderComplexity, ThreeLayersWithoutTransitionStayRealTime)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* v2 = tl.addVideoTrack("V2");
    auto* v3 = tl.addVideoTrack("V3");
    addVisualClip(v1, 0, 48000, false);
    addVisualClip(v2, 0, 48000, false);
    addVisualClip(v3, 0, 48000, false);

    const auto segs = analyzeRenderComplexity(tl);
    EXPECT_EQ(complexityAt(segs, 1000), RenderComplexity::RealTime);
}

TEST(RenderComplexity, TallStackNeedsRender)
{
    // Six simultaneous layers crosses the decode/upload/blend-volume bar.
    Timeline tl;
    std::vector<Track*> tracks;
    for (int i = 0; i < 6; ++i)
        tracks.push_back(tl.addVideoTrack("V" + std::to_string(i + 1)));
    for (auto* tr : tracks)
        addVisualClip(tr, 0, 48000, false);

    const auto segs = analyzeRenderComplexity(tl);
    EXPECT_EQ(complexityAt(segs, 1000), RenderComplexity::NeedsRender);
}

TEST(RenderComplexity, FiveLayersStayRealTime)
{
    // One below the tall-stack bar — still real-time.
    Timeline tl;
    std::vector<Track*> tracks;
    for (int i = 0; i < 5; ++i)
        tracks.push_back(tl.addVideoTrack("V" + std::to_string(i + 1)));
    for (auto* tr : tracks)
        addVisualClip(tr, 0, 48000, false);

    const auto segs = analyzeRenderComplexity(tl);
    EXPECT_EQ(complexityAt(segs, 1000), RenderComplexity::RealTime);
}

} // namespace
} // namespace rt
