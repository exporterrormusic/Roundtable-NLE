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
#include "project/Project.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/SpineClip.h"
#include "timeline/SequenceClip.h"
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

// ── Nested-sequence coverage (§4.6 slice 2a gap closure) ────────────────────
//
// A SequenceClip serializes only its reference, so an edit INSIDE the nested
// sequence is invisible to writeClip.  hashCompositeConfigAt must recurse into
// the referenced sequence (given the Project) so such edits invalidate the
// parent's hash — otherwise the segment-render cache would serve stale frames.

namespace {

// Build a project whose parent sequence (index 0) nests the child (index 1)
// via a SequenceClip spanning [0, dur).  Outputs the SequenceClip, the child
// sequence's video track (for adding inner clips), and the first inner clip.
void setupNested(Project& proj, SequenceClip*& seqClipOut, Track*& childTrackOut,
                 Clip*& innerOut, int64_t dur = 48000)
{
    Timeline* parent = proj.sequence(0);
    Timeline* child  = proj.addSequence();          // index 1

    childTrackOut = child->addVideoTrack("V1");
    innerOut      = addClip(childTrackOut, 0, dur);  // inner clip

    auto* parentV1 = parent->addVideoTrack("V1");
    auto seq = std::make_unique<SequenceClip>();
    seq->setTimelineIn(0);
    seq->setDuration(dur);
    seq->setSequenceIndex(1);
    seqClipOut = static_cast<SequenceClip*>(parentV1->addClip(std::move(seq)));
}

} // namespace

TEST(RenderStateHash, NestedInnerEditInvalidatesParent)
{
    Project proj;
    SequenceClip* seqClip = nullptr;
    Track* childTrack = nullptr;
    Clip* inner = nullptr;
    setupNested(proj, seqClip, childTrack, inner);
    Timeline* parent = proj.sequence(0);

    const uint64_t before = hashCompositeConfigAt(*parent, 1000, &proj);

    // Edit a clip INSIDE the nested sequence.
    inner->setBlendMode(2);

    EXPECT_NE(before, hashCompositeConfigAt(*parent, 1000, &proj));
}

TEST(RenderStateHash, NestedEditInvisibleWithoutProject)
{
    // Proves the recursion (not some incidental field) is what catches the
    // inner edit: with no Project the parent hash can't see inside the nest.
    Project proj;
    SequenceClip* seqClip = nullptr;
    Track* childTrack = nullptr;
    Clip* inner = nullptr;
    setupNested(proj, seqClip, childTrack, inner);
    Timeline* parent = proj.sequence(0);

    const uint64_t before = hashCompositeConfigAt(*parent, 1000, /*project=*/nullptr);
    inner->setBlendMode(2);
    EXPECT_EQ(before, hashCompositeConfigAt(*parent, 1000, /*project=*/nullptr));
}

TEST(RenderStateHash, NestedSettingsChangeInvalidatesParent)
{
    // The nested sequence renders at ITS OWN resolution — changing it must
    // invalidate the parent composite.
    Project proj;
    SequenceClip* seqClip = nullptr;
    Track* childTrack = nullptr;
    Clip* inner = nullptr;
    setupNested(proj, seqClip, childTrack, inner);
    Timeline* parent = proj.sequence(0);
    Timeline* child  = proj.sequence(1);

    const uint64_t before = hashCompositeConfigAt(*parent, 1000, &proj);
    child->settings().setResolution(640, 480);
    EXPECT_NE(before, hashCompositeConfigAt(*parent, 1000, &proj));
}

TEST(RenderStateHash, NestedEditOutsideMappedTickDoesNotInvalidate)
{
    // The SequenceClip is trimmed (sourceIn) so the parent tick maps to a
    // specific inner region; editing an inner clip active only ELSEWHERE in
    // the child must not invalidate this parent tick (no needless re-render).
    Project proj;
    SequenceClip* seqClip = nullptr;
    Track* childTrack = nullptr;
    Clip* inner = nullptr;
    setupNested(proj, seqClip, childTrack, inner, /*dur=*/48000);

    // Second inner clip occupies a later inner region [48000, 96000).
    Clip* innerB = addClip(childTrack, 48000, 48000);
    Timeline* parent = proj.sequence(0);

    // Parent tick 1000 → innerTick 1000 (sourceIn 0), where only innerA lives.
    const uint64_t before = hashCompositeConfigAt(*parent, 1000, &proj);
    innerB->setBlendMode(3);                         // inactive at innerTick 1000
    EXPECT_EQ(before, hashCompositeConfigAt(*parent, 1000, &proj));
}

TEST(RenderStateHash, NestedSourceInRemapsInnerContent)
{
    // Sliding the nested clip's sourceIn changes which inner content shows,
    // so the parent hash at a fixed tick must change.
    Project proj;
    SequenceClip* seqClip = nullptr;
    Track* childTrack = nullptr;
    Clip* inner = nullptr;
    setupNested(proj, seqClip, childTrack, inner);
    Clip* innerB = addClip(childTrack, 48000, 48000);
    innerB->setBlendMode(3);                         // make region B distinguishable
    Timeline* parent = proj.sequence(0);

    const uint64_t before = hashCompositeConfigAt(*parent, 1000, &proj);
    seqClip->setSourceIn(48000);                     // now tick 1000 → innerTick 49000 (region B)
    EXPECT_NE(before, hashCompositeConfigAt(*parent, 1000, &proj));
}

TEST(RenderStateHash, SelfReferencingNestDoesNotInfiniteLoop)
{
    // A sequence that nests itself must be cycle-guarded, not recurse forever.
    Project proj;
    Timeline* parent = proj.sequence(0);
    auto* v1 = parent->addVideoTrack("V1");
    auto seq = std::make_unique<SequenceClip>();
    seq->setTimelineIn(0);
    seq->setDuration(48000);
    seq->setSequenceIndex(0);                        // references its own sequence
    v1->addClip(std::move(seq));

    // Must terminate (cycle guard) and be deterministic.
    const uint64_t h = hashCompositeConfigAt(*parent, 1000, &proj);
    EXPECT_EQ(h, hashCompositeConfigAt(*parent, 1000, &proj));
}

} // namespace
} // namespace rt
