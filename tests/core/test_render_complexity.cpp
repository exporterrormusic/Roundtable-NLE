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

} // namespace
} // namespace rt
