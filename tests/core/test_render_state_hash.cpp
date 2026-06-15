/*
 * test_render_state_hash.cpp — compositing-config hash (§4.6 slice 2a).
 *
 * hashCompositeConfigAt is the invalidation key for the future segment-render
 * cache, so the contract under test is exactly the safety property:
 *   - identical config (same active clips/state) hashes equal across ticks;
 *   - ANY pixel-affecting edit to an ACTIVE clip / track / settings changes
 *     the hash (no stale frames);
 *   - an edit to a clip NOT active at a tick leaves that tick's hash alone
 *     (so the cache isn't needlessly thrown away — the safe-but-not-wasteful
 *     direction).
 */

#include <gtest/gtest.h>

#include "project/RenderStateHash.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/SpineClip.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "project/Settings.h"

namespace rt {
namespace {

Clip* addClip(Track* tr, int64_t in, int64_t dur)
{
    auto c = std::make_unique<SpineClip>();
    c->setTimelineIn(in);
    c->setDuration(dur);
    return tr->addClip(std::move(c));
}

TEST(RenderStateHash, StableWhenNothingChanges)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addClip(v1, 0, 48000);
    EXPECT_EQ(hashCompositeConfigAt(tl, 1000), hashCompositeConfigAt(tl, 1000));
}

TEST(RenderStateHash, EqualAcrossTicksInSameConfig)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addClip(v1, 0, 48000);
    // Two ticks governed by the same single clip — same config, same hash.
    EXPECT_EQ(hashCompositeConfigAt(tl, 1000), hashCompositeConfigAt(tl, 2000));
}

TEST(RenderStateHash, DiffersBetweenActiveClipAndGap)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addClip(v1, 0, 48000);   // active at 1000, gap at 60000
    EXPECT_NE(hashCompositeConfigAt(tl, 1000), hashCompositeConfigAt(tl, 60000));
}

TEST(RenderStateHash, BlendModeChangeInvalidates)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    Clip* c = addClip(v1, 0, 48000);
    const uint64_t before = hashCompositeConfigAt(tl, 1000);
    c->setBlendMode(2);
    EXPECT_NE(before, hashCompositeConfigAt(tl, 1000));
}

TEST(RenderStateHash, AddingEffectInvalidates)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    Clip* c = addClip(v1, 0, 48000);
    const uint64_t before = hashCompositeConfigAt(tl, 1000);
    c->effects().addEffect(createEffect(EffectType::ColorCorrect));
    EXPECT_NE(before, hashCompositeConfigAt(tl, 1000));
}

TEST(RenderStateHash, MovingClipInvalidates)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    Clip* c = addClip(v1, 0, 48000);
    const uint64_t before = hashCompositeConfigAt(tl, 1000);
    c->setTimelineIn(500);   // shifted: tick 1000 now lands at a different source position
    EXPECT_NE(before, hashCompositeConfigAt(tl, 1000));
}

TEST(RenderStateHash, MutingTrackInvalidates)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addClip(v1, 0, 48000);
    const uint64_t before = hashCompositeConfigAt(tl, 1000);
    v1->setMuted(true);
    EXPECT_NE(before, hashCompositeConfigAt(tl, 1000));
}

TEST(RenderStateHash, SoloingAnotherTrackInvalidates)
{
    // Cross-track: soloing V2 hides V1, so a tick where only V1 is active
    // must change hash even though V2 has no clip there.
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* v2 = tl.addVideoTrack("V2");
    addClip(v1, 0, 48000);
    const uint64_t before = hashCompositeConfigAt(tl, 1000);
    v2->setSoloed(true);
    EXPECT_NE(before, hashCompositeConfigAt(tl, 1000));
}

TEST(RenderStateHash, SettingsChangeInvalidates)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addClip(v1, 0, 48000);
    const uint64_t before = hashCompositeConfigAt(tl, 1000);
    tl.settings().setFrameRate(tl.settings().frameRate() + 1.0);
    EXPECT_NE(before, hashCompositeConfigAt(tl, 1000));

    const uint64_t mid = hashCompositeConfigAt(tl, 1000);
    tl.settings().setResolution(1280, 720);
    EXPECT_NE(mid, hashCompositeConfigAt(tl, 1000));
}

TEST(RenderStateHash, EditingInactiveClipDoesNotInvalidate)
{
    // A on [0,48000), B on [48000,96000) (same track, disjoint times).
    // Editing B must NOT change the hash at tick 1000 where only A is active.
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    addClip(v1, 0, 48000);
    Clip* b = addClip(v1, 48000, 48000);

    const uint64_t aOnly = hashCompositeConfigAt(tl, 1000);
    b->setBlendMode(3);
    EXPECT_EQ(aOnly, hashCompositeConfigAt(tl, 1000));   // unchanged — B inactive here
    // ...but B's own region DID change.
    // (sanity: the two regions hash differently regardless)
    EXPECT_NE(hashCompositeConfigAt(tl, 1000), hashCompositeConfigAt(tl, 60000));
}

} // namespace
} // namespace rt
