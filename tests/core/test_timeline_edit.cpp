/*
 * test_timeline_edit.cpp — Tests for Step 13: Timeline Editing Tools.
 *
 * Tests the EditOperations (pure logic, no Qt):
 *   1. SelectionSet (select, deselect, toggle, marquee, select-all)
 *   2. SnapEngine (build targets, snap to nearest, snap pair)
 *   3. Razor / split clip
 *   4. Clip trim (head, tail, clamping)
 *   5. Rolling edit
 *   6. Ripple trim & ripple delete
 *   7. Slip tool
 *   8. Slide tool
 *   9. Clip move (same track, cross-track)
 *  10. Delete selection
 *  11. Clipboard (copy, cut, paste, duplicate)
 *  12. In/Out points
 *  13. Edit point navigation (next/prev)
 *  14. Helper functions (clipAtTime, findEditPoint)
 */

#include <gtest/gtest.h>

#include "timeline/EditOperations.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/NestTransitionTransfer.h"
#include "timeline/SequenceClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/TierListClip.h"
#include "command/CommandStack.h"

#include <memory>

// ═════════════════════════════════════════════════════════════════════════════
//  Helpers
// ═════════════════════════════════════════════════════════════════════════════

static constexpr int64_t TPS = 48000; // ticks per second

/// Create a video clip at the given position with the given duration (in seconds).
static std::unique_ptr<rt::VideoClip> makeClip(double startSec, double durSec)
{
    auto clip = std::make_unique<rt::VideoClip>();
    clip->setTimelineIn(static_cast<int64_t>(startSec * TPS));
    clip->setDuration(static_cast<int64_t>(durSec * TPS));
    clip->setSourceIn(0);
    return clip;
}

/// Helper: add a clip to a track and return its ID.
static uint64_t addClip(rt::Track* track, double startSec, double durSec)
{
    auto clip = makeClip(startSec, durSec);
    uint64_t id = clip->id();
    track->addClip(std::move(clip));
    return id;
}

/// Helper: set up a timeline with one video track containing 3 adjacent clips.
/// [0–2s] [2–5s] [5–8s]
struct TestTimeline
{
    rt::Timeline timeline;
    rt::Track*   vTrack{nullptr};
    uint64_t     clipA{0}, clipB{0}, clipC{0};
    rt::CommandStack stack;

    TestTimeline()
    {
        vTrack = timeline.addVideoTrack("V1");
        clipA = addClip(vTrack, 0.0, 2.0);
        clipB = addClip(vTrack, 2.0, 3.0);
        clipC = addClip(vTrack, 5.0, 3.0);
    }
};

// ═════════════════════════════════════════════════════════════════════════════
//  SelectionSet
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, SelectionEmpty)
{
    rt::SelectionSet sel;
    EXPECT_TRUE(sel.empty());
    EXPECT_EQ(sel.count(), 0u);
    EXPECT_FALSE(sel.singleSelection().has_value());
}

TEST(EditOperations, SelectSingle)
{
    rt::SelectionSet sel;
    rt::ClipRef ref{0, 100};
    sel.selectClip(ref);

    EXPECT_EQ(sel.count(), 1u);
    EXPECT_TRUE(sel.isSelected(ref));
    EXPECT_TRUE(sel.singleSelection().has_value());
    EXPECT_EQ(sel.singleSelection()->clipId, 100u);
}

TEST(EditOperations, SelectClipClearsPrevious)
{
    rt::SelectionSet sel;
    sel.selectClip({0, 100});
    sel.selectClip({0, 200}); // Not addToSelection → clears

    EXPECT_EQ(sel.count(), 1u);
    EXPECT_FALSE(sel.isSelected({0, 100}));
    EXPECT_TRUE(sel.isSelected({0, 200}));
}

TEST(EditOperations, SelectClipAddToSelection)
{
    rt::SelectionSet sel;
    sel.selectClip({0, 100});
    sel.selectClip({0, 200}, true); // Shift-click

    EXPECT_EQ(sel.count(), 2u);
    EXPECT_TRUE(sel.isSelected({0, 100}));
    EXPECT_TRUE(sel.isSelected({0, 200}));
    EXPECT_FALSE(sel.singleSelection().has_value());
}

TEST(EditOperations, SelectClipNoDuplicates)
{
    rt::SelectionSet sel;
    sel.selectClip({0, 100});
    sel.selectClip({0, 100}, true);

    EXPECT_EQ(sel.count(), 1u);
}

TEST(EditOperations, DeselectClip)
{
    rt::SelectionSet sel;
    sel.selectClip({0, 100});
    sel.selectClip({0, 200}, true);
    sel.deselectClip({0, 100});

    EXPECT_EQ(sel.count(), 1u);
    EXPECT_FALSE(sel.isSelected({0, 100}));
    EXPECT_TRUE(sel.isSelected({0, 200}));
}

TEST(EditOperations, ToggleClip)
{
    rt::SelectionSet sel;
    sel.selectClip({0, 100});

    sel.toggleClip({0, 100}); // Toggle off
    EXPECT_EQ(sel.count(), 0u);

    sel.toggleClip({0, 100}); // Toggle on
    EXPECT_EQ(sel.count(), 1u);
}

TEST(EditOperations, Clear)
{
    rt::SelectionSet sel;
    sel.selectClip({0, 100});
    sel.selectClip({1, 200}, true);
    sel.clear();

    EXPECT_TRUE(sel.empty());
}

TEST(EditOperations, IsSelectedById)
{
    rt::SelectionSet sel;
    sel.selectClip({0, 42});
    sel.selectClip({1, 99}, true);

    EXPECT_TRUE(sel.isSelectedById(42));
    EXPECT_TRUE(sel.isSelectedById(99));
    EXPECT_FALSE(sel.isSelectedById(1));
}

TEST(EditOperations, SelectRectangle)
{
    TestTimeline tt;

    rt::SelectionSet sel;
    // Select region 1s–4s on track 0 → should get clipA and clipB
    rt::TimelineRect rect{1 * TPS, 4 * TPS, 0, 0};
    sel.selectRect(tt.timeline, rect);

    EXPECT_EQ(sel.count(), 2u);
    EXPECT_TRUE(sel.isSelectedById(tt.clipA));
    EXPECT_TRUE(sel.isSelectedById(tt.clipB));
    EXPECT_FALSE(sel.isSelectedById(tt.clipC));
}

TEST(EditOperations, SelectAll)
{
    TestTimeline tt;

    rt::SelectionSet sel;
    sel.selectAll(tt.timeline);

    EXPECT_EQ(sel.count(), 3u);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SnapEngine
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, SnapEngineDefaults)
{
    rt::SnapEngine snap;
    EXPECT_TRUE(snap.isEnabled());
    EXPECT_DOUBLE_EQ(snap.thresholdPixels(), rt::SnapEngine::kDefaultThresholdPx);
}

TEST(EditOperations, SnapDisabled)
{
    rt::SnapEngine snap;
    snap.setEnabled(false);

    snap.addTarget({5 * TPS, rt::SnapTarget::Type::Playhead});
    auto result = snap.snap(5 * TPS + 100);

    EXPECT_FALSE(result.didSnap);
    EXPECT_EQ(result.snappedTick, 5 * TPS + 100);
}

TEST(EditOperations, SnapToPlayhead)
{
    rt::SnapEngine snap;
    snap.setPixelsPerSecond(100.0);
    snap.addTarget({5 * TPS, rt::SnapTarget::Type::Playhead});

    // A tick within threshold
    auto result = snap.snap(5 * TPS + 200);
    EXPECT_TRUE(result.didSnap);
    EXPECT_EQ(result.snappedTick, 5 * TPS);
    EXPECT_EQ(result.snapType, rt::SnapTarget::Type::Playhead);
}

TEST(EditOperations, SnapTooFar)
{
    rt::SnapEngine snap;
    snap.setPixelsPerSecond(100.0);
    snap.setThresholdPixels(5.0); // 5px threshold → very tight

    snap.addTarget({5 * TPS, rt::SnapTarget::Type::Playhead});

    // 1 second away at 100pps = 100px → far beyond threshold
    auto result = snap.snap(6 * TPS);
    EXPECT_FALSE(result.didSnap);
}

TEST(EditOperations, SnapBuildTargets)
{
    TestTimeline tt;
    tt.timeline.setPlayheadPosition(3 * TPS);

    rt::SnapEngine snap;
    snap.setPixelsPerSecond(100.0);
    snap.buildTargets(tt.timeline, 3 * TPS, 24.0);

    // Should have: playhead + 6 clip edges (3 clips × 2 edges each) = 7 targets
    EXPECT_GE(snap.targets().size(), 7u);
}

TEST(EditOperations, SnapBuildTargetsWithExclude)
{
    TestTimeline tt;

    rt::SnapEngine snap;
    snap.setPixelsPerSecond(100.0);
    snap.buildTargets(tt.timeline, 0, 24.0, {tt.clipA});

    // clipA edges should be excluded
    bool foundClipAStart = false;
    for (const auto& t : snap.targets())
    {
        if (t.tick == 0 && t.type == rt::SnapTarget::Type::ClipEdge)
            foundClipAStart = true;
    }
    EXPECT_FALSE(foundClipAStart);
}

TEST(EditOperations, SnapPair)
{
    rt::SnapEngine snap;
    snap.setPixelsPerSecond(100.0);
    snap.addTarget({5 * TPS, rt::SnapTarget::Type::ClipEdge});

    // Snap a pair where tickB is closer to the target
    auto result = snap.snapPair(4 * TPS, 5 * TPS + 100);
    EXPECT_TRUE(result.didSnap);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Razor / Split
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, SplitClip)
{
    TestTimeline tt;

    // Split clipB (2–5s) at 3.5s
    int64_t splitTime = static_cast<int64_t>(3.5 * TPS);
    auto cmd = rt::EditOperations::splitClip(tt.timeline, 0, tt.clipB, splitTime);
    ASSERT_NE(cmd, nullptr);

    tt.stack.execute(std::move(cmd));

    // Track should now have 4 clips
    EXPECT_EQ(tt.vTrack->clipCount(), 4u);

    // Original clipB should be trimmed to 2–3.5s
    size_t bIdx = tt.vTrack->findClipIndexById(tt.clipB);
    ASSERT_LT(bIdx, tt.vTrack->clipCount());
    EXPECT_EQ(tt.vTrack->clip(bIdx)->timelineIn(), 2 * TPS);
    EXPECT_EQ(tt.vTrack->clip(bIdx)->duration(), static_cast<int64_t>(1.5 * TPS));
}

TEST(EditOperations, SplitClipUndo)
{
    TestTimeline tt;

    int64_t splitTime = static_cast<int64_t>(3.5 * TPS);
    auto cmd = rt::EditOperations::splitClip(tt.timeline, 0, tt.clipB, splitTime);
    tt.stack.execute(std::move(cmd));

    EXPECT_EQ(tt.vTrack->clipCount(), 4u);

    tt.stack.undo();

    // Should be back to 3 clips
    EXPECT_EQ(tt.vTrack->clipCount(), 3u);
}

TEST(EditOperations, SplitTierListPreservesOneEventClock)
{
    rt::Timeline timeline;
    auto* track = timeline.addVideoTrack("V1");
    auto tier = std::make_unique<rt::TierListClip>();
    tier->setTimelineIn(2 * TPS);
    tier->setDuration(8 * TPS);
    const uint64_t leftId = tier->id();
    ASSERT_NE(track->addClip(std::move(tier)), nullptr);

    auto cmd = rt::EditOperations::splitClip(timeline, 0, leftId, 6 * TPS);
    ASSERT_NE(cmd, nullptr);
    cmd->execute();
    ASSERT_EQ(track->clipCount(), 2u);

    const auto* left = dynamic_cast<const rt::TierListClip*>(track->clip(0));
    const auto* right = dynamic_cast<const rt::TierListClip*>(track->clip(1));
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(left->eventTickAt(6 * TPS - 1), 4 * TPS - 1);
    EXPECT_EQ(right->eventTickAt(6 * TPS), 4 * TPS);
    EXPECT_EQ(right->eventTickAt(8 * TPS), 6 * TPS);
}

TEST(EditOperations, SplitOutsideClipReturnsNull)
{
    TestTimeline tt;

    // Split at 0 (before clipB)
    auto cmd = rt::EditOperations::splitClip(tt.timeline, 0, tt.clipB, 0);
    EXPECT_EQ(cmd, nullptr);

    // Split at 6s (after clipB)
    cmd = rt::EditOperations::splitClip(tt.timeline, 0, tt.clipB, 6 * TPS);
    EXPECT_EQ(cmd, nullptr);
}

TEST(EditOperations, SplitAllAtPlayhead)
{
    TestTimeline tt;
    // Add clip to a second track too
    rt::Track* aTrack = tt.timeline.addAudioTrack("A1");
    addClip(aTrack, 1.0, 4.0); // 1–5s

    int64_t playhead = static_cast<int64_t>(3.0 * TPS);
    auto cmd = rt::EditOperations::splitAllAtPlayhead(tt.timeline, playhead);
    ASSERT_NE(cmd, nullptr);

    tt.stack.execute(std::move(cmd));

    // V1: clipA(0–2) unchanged, clipB split at 3s, clipC unchanged → 4 clips
    EXPECT_EQ(tt.vTrack->clipCount(), 4u);
    // A1: clip split at 3s → 2 clips
    EXPECT_EQ(aTrack->clipCount(), 2u);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Trim
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, TrimClipHead)
{
    TestTimeline tt;

    // Trim clipB (2–5s) head to 3s
    auto cmd = rt::EditOperations::trimClip(tt.timeline, 0, tt.clipB,
                                             rt::ClipEdge::Head, 3 * TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    size_t idx = tt.vTrack->findClipIndexById(tt.clipB);
    EXPECT_EQ(tt.vTrack->clip(idx)->timelineIn(), 3 * TPS);
    EXPECT_EQ(tt.vTrack->clip(idx)->duration(), 2 * TPS); // 3–5s
}

TEST(EditOperations, TrimClipTail)
{
    TestTimeline tt;

    // Trim clipB (2–5s) tail to 4s
    auto cmd = rt::EditOperations::trimClip(tt.timeline, 0, tt.clipB,
                                             rt::ClipEdge::Tail, 4 * TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    size_t idx = tt.vTrack->findClipIndexById(tt.clipB);
    EXPECT_EQ(tt.vTrack->clip(idx)->duration(), 2 * TPS); // 2–4s
}

TEST(EditOperations, TrimClipMinDuration)
{
    TestTimeline tt;

    // Try to trim clipB head past its tail → should clamp
    auto cmd = rt::EditOperations::trimClip(tt.timeline, 0, tt.clipB,
                                             rt::ClipEdge::Head, 10 * TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    size_t idx = tt.vTrack->findClipIndexById(tt.clipB);
    EXPECT_GE(tt.vTrack->clip(idx)->duration(), 2000); // kMinClipDuration
}

TEST(EditOperations, TrimClipUndo)
{
    TestTimeline tt;

    int64_t origIn = tt.vTrack->clip(tt.vTrack->findClipIndexById(tt.clipB))->timelineIn();
    int64_t origDur = tt.vTrack->clip(tt.vTrack->findClipIndexById(tt.clipB))->duration();

    auto cmd = rt::EditOperations::trimClip(tt.timeline, 0, tt.clipB,
                                             rt::ClipEdge::Head, 3 * TPS);
    tt.stack.execute(std::move(cmd));
    tt.stack.undo();

    size_t idx = tt.vTrack->findClipIndexById(tt.clipB);
    EXPECT_EQ(tt.vTrack->clip(idx)->timelineIn(), origIn);
    EXPECT_EQ(tt.vTrack->clip(idx)->duration(), origDur);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Rolling Edit
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, RollingEdit)
{
    TestTimeline tt;

    // Rolling edit between clipA(0–2s) and clipB(2–5s): move edit to 3s
    auto cmd = rt::EditOperations::rollingEdit(tt.timeline, 0,
                                                tt.clipA, tt.clipB, 3 * TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    size_t ai = tt.vTrack->findClipIndexById(tt.clipA);
    size_t bi = tt.vTrack->findClipIndexById(tt.clipB);

    // clipA: 0–3s
    EXPECT_EQ(tt.vTrack->clip(ai)->timelineIn(), 0);
    EXPECT_EQ(tt.vTrack->clip(ai)->duration(), 3 * TPS);

    // clipB: 3–5s
    EXPECT_EQ(tt.vTrack->clip(bi)->timelineIn(), 3 * TPS);
    EXPECT_EQ(tt.vTrack->clip(bi)->duration(), 2 * TPS);
}

TEST(EditOperations, RollingEditUndo)
{
    TestTimeline tt;

    auto cmd = rt::EditOperations::rollingEdit(tt.timeline, 0,
                                                tt.clipA, tt.clipB, 3 * TPS);
    tt.stack.execute(std::move(cmd));
    tt.stack.undo();

    size_t ai = tt.vTrack->findClipIndexById(tt.clipA);
    size_t bi = tt.vTrack->findClipIndexById(tt.clipB);

    EXPECT_EQ(tt.vTrack->clip(ai)->duration(), 2 * TPS); // Back to 0–2s
    EXPECT_EQ(tt.vTrack->clip(bi)->timelineIn(), 2 * TPS);
    EXPECT_EQ(tt.vTrack->clip(bi)->duration(), 3 * TPS); // Back to 2–5s
}

// ═════════════════════════════════════════════════════════════════════════════
//  Ripple
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, RippleTrimTail)
{
    TestTimeline tt;

    // Ripple trim clipB tail from 5s to 4s → clipC should shift left by 1s
    int64_t clipCOrigIn = tt.vTrack->clip(
        tt.vTrack->findClipIndexById(tt.clipC))->timelineIn();

    auto cmd = rt::EditOperations::rippleTrim(tt.timeline, 0, tt.clipB,
                                               rt::ClipEdge::Tail, 4 * TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    size_t bi = tt.vTrack->findClipIndexById(tt.clipB);
    EXPECT_EQ(tt.vTrack->clip(bi)->duration(), 2 * TPS); // 2–4s

    size_t ci = tt.vTrack->findClipIndexById(tt.clipC);
    // clipC should have shifted left by 1s (from 5s to 4s)
    EXPECT_EQ(tt.vTrack->clip(ci)->timelineIn(), clipCOrigIn - 1 * TPS);
}

TEST(EditOperations, RippleDelete)
{
    TestTimeline tt;

    // Select clipB and ripple delete it
    rt::SelectionSet sel;
    sel.selectClip({0, tt.clipB});

    auto cmd = rt::EditOperations::rippleDelete(tt.timeline, sel);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    // clipB removed, clipC shifted left
    EXPECT_EQ(tt.vTrack->clipCount(), 2u);
    EXPECT_EQ(tt.vTrack->findClipIndexById(tt.clipB), tt.vTrack->clipCount());
}

// ═════════════════════════════════════════════════════════════════════════════
//  Close Gap — keep tracks in sync, never overlap clips on other tracks
// ═════════════════════════════════════════════════════════════════════════════

static size_t trackIndexOf(rt::Timeline& tl, rt::Track* t)
{
    for (size_t i = 0; i < tl.trackCount(); ++i)
        if (tl.track(i) == t) return i;
    return SIZE_MAX;
}

// Closing a gap on V1 must not shove a clip on another sync-locked track past
// the clip in front of it. The ripple is clamped to the room available on the
// tightest track, and every track shifts by that same amount (stays in sync).
TEST(EditOperations, CloseGapClampsToKeepTracksInSync)
{
    rt::Timeline timeline;
    rt::Track* v1 = timeline.addVideoTrack("V1");
    rt::Track* v2 = timeline.addVideoTrack("V2");

    // V1: clipA [0–2s]  GAP [2–4s]  clipB [4–6s]   (gap is 2s wide)
    addClip(v1, 0.0, 2.0);
    uint64_t b = addClip(v1, 4.0, 2.0);

    // V2: clipC [0–3s]  clipD [4–6s]   (only 1s of room before clipD)
    uint64_t c = addClip(v2, 0.0, 3.0);
    uint64_t d = addClip(v2, 4.0, 2.0);

    size_t v1Idx = trackIndexOf(timeline, v1);
    ASSERT_NE(v1Idx, SIZE_MAX);

    rt::CommandStack stack;
    auto cmd = rt::EditOperations::closeGap(timeline, v1Idx, 2 * TPS, 4 * TPS);
    ASSERT_NE(cmd, nullptr);
    stack.execute(std::move(cmd));

    // Gap (2s) is wider than V2's room (1s) → ripple clamped to 1s on BOTH
    // tracks. clipB and clipD each shift left by exactly 1s.
    EXPECT_EQ(v1->clip(v1->findClipIndexById(b))->timelineIn(), 3 * TPS);
    EXPECT_EQ(v2->clip(v2->findClipIndexById(d))->timelineIn(), 3 * TPS);

    // clipD lands flush against clipC's tail (3s) — no overlap.
    EXPECT_GE(v2->clip(v2->findClipIndexById(d))->timelineIn(),
              v2->clip(v2->findClipIndexById(c))->timelineOut());
}

// With nothing else to constrain it, the gap closes fully and clips butt up.
TEST(EditOperations, CloseGapFullyClosesWhenUnobstructed)
{
    rt::Timeline timeline;
    rt::Track* v1 = timeline.addVideoTrack("V1");

    addClip(v1, 0.0, 2.0);
    uint64_t b = addClip(v1, 4.0, 2.0);   // gap [2–4s]

    size_t v1Idx = trackIndexOf(timeline, v1);
    ASSERT_NE(v1Idx, SIZE_MAX);

    rt::CommandStack stack;
    auto cmd = rt::EditOperations::closeGap(timeline, v1Idx, 2 * TPS, 4 * TPS);
    ASSERT_NE(cmd, nullptr);
    stack.execute(std::move(cmd));

    EXPECT_EQ(v1->clip(v1->findClipIndexById(b))->timelineIn(), 2 * TPS); // butts clipA
}

// A (track-)locked track must never move, even when sync-locked.
TEST(EditOperations, CloseGapSkipsLockedTracks)
{
    rt::Timeline timeline;
    rt::Track* v1 = timeline.addVideoTrack("V1");
    rt::Track* v2 = timeline.addVideoTrack("V2");

    addClip(v1, 0.0, 2.0);
    uint64_t b = addClip(v1, 4.0, 2.0);   // gap [2–4s]
    uint64_t d = addClip(v2, 4.0, 2.0);   // would move if V2 weren't locked
    v2->setLocked(true);

    size_t v1Idx = trackIndexOf(timeline, v1);
    rt::CommandStack stack;
    auto cmd = rt::EditOperations::closeGap(timeline, v1Idx, 2 * TPS, 4 * TPS);
    ASSERT_NE(cmd, nullptr);
    stack.execute(std::move(cmd));

    // V1 closes fully (locked V2 doesn't constrain it); V2's clip stays put.
    EXPECT_EQ(v1->clip(v1->findClipIndexById(b))->timelineIn(), 2 * TPS);
    EXPECT_EQ(v2->clip(v2->findClipIndexById(d))->timelineIn(), 4 * TPS);
}

// A gap on a locked track must not become the origin of a ripple edit. The
// locked track stays put and other sync-locked tracks must not move either.
TEST(EditOperations, CloseGapOnLockedEditedTrackDoesNothing)
{
    rt::Timeline timeline;
    rt::Track* v1 = timeline.addVideoTrack("V1");
    rt::Track* v2 = timeline.addVideoTrack("V2");

    addClip(v1, 0.0, 2.0);
    uint64_t b = addClip(v1, 4.0, 2.0);   // selected gap would be [2-4s]
    uint64_t d = addClip(v2, 4.0, 2.0);   // sync-locked by default
    v1->setLocked(true);

    const size_t v1Idx = trackIndexOf(timeline, v1);
    auto cmd = rt::EditOperations::closeGap(
        timeline, v1Idx, 2 * TPS, 4 * TPS);

    EXPECT_EQ(cmd, nullptr);
    EXPECT_EQ(v1->clip(v1->findClipIndexById(b))->timelineIn(), 4 * TPS);
    EXPECT_EQ(v2->clip(v2->findClipIndexById(d))->timelineIn(), 4 * TPS);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Slip
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, SlipClip)
{
    TestTimeline tt;

    size_t bi = tt.vTrack->findClipIndexById(tt.clipB);
    int64_t origSourceIn = tt.vTrack->clip(bi)->sourceIn();
    int64_t origTimelineIn = tt.vTrack->clip(bi)->timelineIn();
    int64_t origDuration = tt.vTrack->clip(bi)->duration();

    // Slip clipB by +1s of source
    auto cmd = rt::EditOperations::slipClip(tt.timeline, 0, tt.clipB, TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    // Source in should change, timeline position should NOT change
    EXPECT_EQ(tt.vTrack->clip(bi)->sourceIn(), origSourceIn + TPS);
    EXPECT_EQ(tt.vTrack->clip(bi)->timelineIn(), origTimelineIn);
    EXPECT_EQ(tt.vTrack->clip(bi)->duration(), origDuration);
}

TEST(EditOperations, SlipClipClampNegative)
{
    TestTimeline tt;

    // Slip clipB by -10s → should clamp to sourceIn=0
    auto cmd = rt::EditOperations::slipClip(tt.timeline, 0, tt.clipB, -10 * TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    size_t bi = tt.vTrack->findClipIndexById(tt.clipB);
    EXPECT_EQ(tt.vTrack->clip(bi)->sourceIn(), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Slide
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, SlideClip)
{
    TestTimeline tt;

    // Slide clipB (2–5s) by +1s
    auto cmd = rt::EditOperations::slideClip(tt.timeline, 0, tt.clipB, TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    size_t bi = tt.vTrack->findClipIndexById(tt.clipB);
    EXPECT_EQ(tt.vTrack->clip(bi)->timelineIn(), 3 * TPS); // Shifted from 2s to 3s
}

TEST(EditOperations, SlideZeroDelta)
{
    TestTimeline tt;

    auto cmd = rt::EditOperations::slideClip(tt.timeline, 0, tt.clipB, 0);
    EXPECT_EQ(cmd, nullptr); // No-op
}

// ═════════════════════════════════════════════════════════════════════════════
//  Move
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, MoveClipSameTrack)
{
    TestTimeline tt;

    auto cmd = rt::EditOperations::moveClip(tt.timeline, 0, tt.clipA, 10 * TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    size_t ai = tt.vTrack->findClipIndexById(tt.clipA);
    EXPECT_EQ(tt.vTrack->clip(ai)->timelineIn(), 10 * TPS);
}

TEST(EditOperations, MoveClipClampNegative)
{
    TestTimeline tt;

    auto cmd = rt::EditOperations::moveClip(tt.timeline, 0, tt.clipA, -5 * TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    size_t ai = tt.vTrack->findClipIndexById(tt.clipA);
    EXPECT_EQ(tt.vTrack->clip(ai)->timelineIn(), 0); // Clamped to 0
}

TEST(EditOperations, MoveClipToTrack)
{
    TestTimeline tt;
    rt::Track* aTrack = tt.timeline.addAudioTrack("A1");

    auto cmd = rt::EditOperations::moveClipToTrack(tt.timeline, 0, 1,
                                                    tt.clipA, 5 * TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    // clipA should be removed from V1
    EXPECT_EQ(tt.vTrack->findClipIndexById(tt.clipA), tt.vTrack->clipCount());

    // A new clip should exist on A1 at 5s
    EXPECT_GE(aTrack->clipCount(), 1u);
    EXPECT_EQ(aTrack->clip(0)->timelineIn(), 5 * TPS);
}

TEST(EditOperations, MoveClipSameTrackSameIndex)
{
    TestTimeline tt;

    // If fromTrack == toTrack, should use simple move
    auto cmd = rt::EditOperations::moveClipToTrack(tt.timeline, 0, 0,
                                                    tt.clipA, 10 * TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    size_t ai = tt.vTrack->findClipIndexById(tt.clipA);
    EXPECT_EQ(tt.vTrack->clip(ai)->timelineIn(), 10 * TPS);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Delete
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, DeleteSelection)
{
    TestTimeline tt;

    rt::SelectionSet sel;
    sel.selectClip({0, tt.clipA});
    sel.selectClip({0, tt.clipB}, true);

    auto cmd = rt::EditOperations::deleteSelection(tt.timeline, sel);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    EXPECT_EQ(tt.vTrack->clipCount(), 1u);
    EXPECT_EQ(tt.vTrack->findClipIndexById(tt.clipC), 0u);
}

TEST(EditOperations, DeleteEmptySelection)
{
    TestTimeline tt;

    rt::SelectionSet sel;
    auto cmd = rt::EditOperations::deleteSelection(tt.timeline, sel);
    EXPECT_EQ(cmd, nullptr);
}

TEST(EditOperations, DeleteSelectionUndo)
{
    TestTimeline tt;

    rt::SelectionSet sel;
    sel.selectClip({0, tt.clipB});

    auto cmd = rt::EditOperations::deleteSelection(tt.timeline, sel);
    tt.stack.execute(std::move(cmd));
    EXPECT_EQ(tt.vTrack->clipCount(), 2u);

    tt.stack.undo();
    EXPECT_EQ(tt.vTrack->clipCount(), 3u);
    EXPECT_NE(tt.vTrack->findClipIndexById(tt.clipB), tt.vTrack->clipCount());
}

// ═════════════════════════════════════════════════════════════════════════════
//  Clipboard
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, CopySelection)
{
    TestTimeline tt;

    rt::SelectionSet sel;
    sel.selectClip({0, tt.clipA});
    sel.selectClip({0, tt.clipB}, true);

    rt::ClipboardContents clipboard;
    rt::EditOperations::copySelection(tt.timeline, sel, clipboard);

    EXPECT_EQ(clipboard.entries.size(), 2u);
    EXPECT_FALSE(clipboard.empty());

    // Relative times: clipA at 0, clipB at 2s relative to clipA
    bool foundZero = false, foundOffset = false;
    for (const auto& entry : clipboard.entries)
    {
        if (entry.relativeTime == 0) foundZero = true;
        if (entry.relativeTime == 2 * TPS) foundOffset = true;
    }
    EXPECT_TRUE(foundZero);
    EXPECT_TRUE(foundOffset);
}

TEST(EditOperations, CopyEmptySelection)
{
    TestTimeline tt;
    rt::SelectionSet sel;
    rt::ClipboardContents clipboard;
    rt::EditOperations::copySelection(tt.timeline, sel, clipboard);
    EXPECT_TRUE(clipboard.empty());
}

TEST(EditOperations, Paste)
{
    TestTimeline tt;

    // Copy clipA
    rt::SelectionSet sel;
    sel.selectClip({0, tt.clipA});
    rt::ClipboardContents clipboard;
    rt::EditOperations::copySelection(tt.timeline, sel, clipboard);

    // Paste at 10s
    auto cmd = rt::EditOperations::paste(tt.timeline, clipboard, 10 * TPS);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    EXPECT_EQ(tt.vTrack->clipCount(), 4u);
}

TEST(EditOperations, PasteUndo)
{
    TestTimeline tt;

    rt::SelectionSet sel;
    sel.selectClip({0, tt.clipA});
    rt::ClipboardContents clipboard;
    rt::EditOperations::copySelection(tt.timeline, sel, clipboard);

    auto cmd = rt::EditOperations::paste(tt.timeline, clipboard, 10 * TPS);
    tt.stack.execute(std::move(cmd));
    EXPECT_EQ(tt.vTrack->clipCount(), 4u);

    tt.stack.undo();
    EXPECT_EQ(tt.vTrack->clipCount(), 3u);
}

TEST(EditOperations, FitTransitionShortensToSpaceLeftByOppositeEdge)
{
    rt::Timeline timeline;
    rt::Track* track = timeline.addVideoTrack("V1");
    const uint64_t a = addClip(track, 0.0, 1.0);
    const uint64_t b = addClip(track, 1.0, 0.5);
    const uint64_t c = addClip(track, 1.5, 1.0);

    // The first transition occupies 18,000 ticks of B from its head, leaving
    // only 6,000 ticks at B's tail. A centered transition at that tail can
    // therefore be 12,000 ticks total instead of the 24,000-tick default.
    rt::Transition existing;
    existing.duration = 36000;
    existing.leftClipId = a;
    existing.rightClipId = b;
    existing.editPointTick = TPS;
    track->addTransition(existing);

    rt::Transition requested;
    requested.duration = rt::kDefaultTransitionDuration;
    requested.leftClipId = b;
    requested.rightClipId = c;
    requested.editPointTick = 3 * TPS / 2;

    ASSERT_TRUE(rt::EditOperations::fitTransitionToAvailableDuration(
        *track, requested));
    EXPECT_EQ(requested.duration, 12000);

    int64_t existingStart = 0, existingEnd = 0;
    int64_t requestedStart = 0, requestedEnd = 0;
    existing.getRange(existingStart, existingEnd);
    requested.getRange(requestedStart, requestedEnd);
    EXPECT_EQ(requestedStart, existingEnd);
    EXPECT_LT(requestedStart, requestedEnd);
}

TEST(EditOperations, FitTransitionRejectsOnlyWhenNoDurationRemains)
{
    rt::Timeline timeline;
    rt::Track* track = timeline.addVideoTrack("V1");
    const uint64_t a = addClip(track, 0.0, 1.0);
    const uint64_t b = addClip(track, 1.0, 0.5);
    const uint64_t c = addClip(track, 1.5, 1.0);

    rt::Transition existing;
    existing.duration = TPS; // Its right half consumes all 0.5 seconds of B.
    existing.leftClipId = a;
    existing.rightClipId = b;
    existing.editPointTick = TPS;
    track->addTransition(existing);

    rt::Transition requested;
    requested.duration = rt::kDefaultTransitionDuration;
    requested.leftClipId = b;
    requested.rightClipId = c;
    requested.editPointTick = 3 * TPS / 2;

    EXPECT_FALSE(rt::EditOperations::fitTransitionToAvailableDuration(
        *track, requested));
}

TEST(EditOperations, CopyAndPasteStandaloneTransitionAtAnotherCut)
{
    TestTimeline tt;

    rt::Transition source;
    source.type = rt::TransitionType::WipeLeft;
    source.duration = TPS;
    source.offset = -TPS / 3;
    source.leftClipId = tt.clipA;
    source.rightClipId = tt.clipB;
    source.editPointTick = 2 * TPS;
    source.param1 = 0.42f;
    source.param2 = 0.17f;
    ASSERT_EQ(tt.vTrack->addTransition(source), 0u);

    rt::ClipboardContents clipboard;
    rt::EditOperations::copyTransition(tt.timeline, 0, 0, clipboard);
    EXPECT_FALSE(clipboard.empty());
    EXPECT_FALSE(clipboard.hasClips());
    ASSERT_TRUE(clipboard.hasStandaloneTransition());

    auto paste = rt::EditOperations::pasteTransitionAtEdge(
        tt.timeline, clipboard, 0, tt.clipB, rt::ClipEdge::Tail);
    ASSERT_NE(paste, nullptr);
    tt.stack.execute(std::move(paste));

    ASSERT_EQ(tt.vTrack->transitionCount(), 2u);
    const size_t pastedIndex = tt.vTrack->findTransition(tt.clipB, tt.clipC);
    ASSERT_NE(pastedIndex, rt::Track::kNoTransition);
    const rt::Transition* pasted = tt.vTrack->transition(pastedIndex);
    ASSERT_NE(pasted, nullptr);
    EXPECT_EQ(pasted->type, source.type);
    EXPECT_EQ(pasted->duration, source.duration);
    EXPECT_EQ(pasted->offset, source.offset);
    EXPECT_EQ(pasted->editPointTick, 5 * TPS);
    EXPECT_FLOAT_EQ(pasted->param1, source.param1);
    EXPECT_FLOAT_EQ(pasted->param2, source.param2);

    tt.stack.undo();
    EXPECT_EQ(tt.vTrack->transitionCount(), 1u);
    EXPECT_EQ(tt.vTrack->findTransition(tt.clipB, tt.clipC),
              rt::Track::kNoTransition);

    tt.stack.redo();
    EXPECT_NE(tt.vTrack->findTransition(tt.clipB, tt.clipC),
              rt::Track::kNoTransition);
}

TEST(EditOperations, PasteStandaloneTransitionReplacesTargetAndUndoRestoresIt)
{
    TestTimeline tt;

    rt::Transition copied;
    copied.type = rt::TransitionType::DipToWhite;
    copied.duration = TPS / 2;
    copied.leftClipId = tt.clipA;
    copied.rightClipId = tt.clipB;
    copied.editPointTick = 2 * TPS;
    tt.vTrack->addTransition(copied);

    rt::Transition oldTarget;
    oldTarget.type = rt::TransitionType::CrossDissolve;
    oldTarget.duration = TPS / 4;
    oldTarget.leftClipId = tt.clipB;
    oldTarget.rightClipId = tt.clipC;
    oldTarget.editPointTick = 5 * TPS;
    tt.vTrack->addTransition(oldTarget);

    rt::ClipboardContents clipboard;
    rt::EditOperations::copyTransition(tt.timeline, 0, 0, clipboard);
    auto paste = rt::EditOperations::pasteTransitionAtEdge(
        tt.timeline, clipboard, 0, tt.clipC, rt::ClipEdge::Head);
    ASSERT_NE(paste, nullptr);
    tt.stack.execute(std::move(paste));

    ASSERT_EQ(tt.vTrack->transitionCount(), 2u);
    size_t targetIndex = tt.vTrack->findTransition(tt.clipB, tt.clipC);
    ASSERT_NE(targetIndex, rt::Track::kNoTransition);
    EXPECT_EQ(tt.vTrack->transition(targetIndex)->type,
              rt::TransitionType::DipToWhite);
    EXPECT_EQ(tt.vTrack->transition(targetIndex)->duration, TPS / 2);

    tt.stack.undo();
    targetIndex = tt.vTrack->findTransition(tt.clipB, tt.clipC);
    ASSERT_NE(targetIndex, rt::Track::kNoTransition);
    EXPECT_EQ(tt.vTrack->transition(targetIndex)->type,
              rt::TransitionType::CrossDissolve);
    EXPECT_EQ(tt.vTrack->transition(targetIndex)->duration, TPS / 4);
}

// Pasting from a 3-video-track sequence into one with MORE tracks must
// anchor the clips at V1 (bottom = highest index) and fill upward — NOT
// land them on the top tracks.
TEST(EditOperations, PasteCrossSequenceAnchorsAtV1)
{
    rt::Timeline src;
    rt::Track* s0 = src.addVideoTrack("");  // idx0 = top    = V3
    rt::Track* s1 = src.addVideoTrack("");  // idx1          = V2
    rt::Track* s2 = src.addVideoTrack("");  // idx2 = bottom = V1
    addClip(s0, 0.0, 1.0);
    addClip(s1, 0.0, 1.0);
    addClip(s2, 0.0, 1.0);

    rt::SelectionSet sel;
    sel.selectClip({0, s0->clip(0)->id()}, true);
    sel.selectClip({1, s1->clip(0)->id()}, true);
    sel.selectClip({2, s2->clip(0)->id()}, true);
    rt::ClipboardContents clipboard;
    rt::EditOperations::copySelection(src, sel, clipboard);

    rt::Timeline dst;
    for (int i = 0; i < 5; ++i) dst.addVideoTrack("");  // idx0..4, idx4 = V1
    rt::CommandStack stack;
    auto cmd = rt::EditOperations::paste(dst, clipboard, 10 * TPS);
    ASSERT_NE(cmd, nullptr);
    stack.execute(std::move(cmd));

    // Bottom three tracks (V1,V2,V3 = idx 4,3,2) get the clips; top two empty.
    EXPECT_EQ(dst.track(4)->clipCount(), 1u);
    EXPECT_EQ(dst.track(3)->clipCount(), 1u);
    EXPECT_EQ(dst.track(2)->clipCount(), 1u);
    EXPECT_EQ(dst.track(1)->clipCount(), 0u);
    EXPECT_EQ(dst.track(0)->clipCount(), 0u);
}

// Pasting from a sequence with MORE video tracks than the destination must
// create the missing tracks so no clip is dropped, and undo must remove them.
TEST(EditOperations, PasteCrossSequenceAddsOverflowTracks)
{
    rt::Timeline src;
    std::vector<rt::Track*> st;
    for (int i = 0; i < 4; ++i) st.push_back(src.addVideoTrack(""));
    rt::SelectionSet sel;
    for (int i = 0; i < 4; ++i) {
        addClip(st[static_cast<size_t>(i)], 0.0, 1.0);
        sel.selectClip({static_cast<size_t>(i), st[static_cast<size_t>(i)]->clip(0)->id()}, true);
    }
    rt::ClipboardContents clipboard;
    rt::EditOperations::copySelection(src, sel, clipboard);

    rt::Timeline dst;
    dst.addVideoTrack("");
    dst.addVideoTrack("");
    const size_t before = dst.trackCount();

    rt::CommandStack stack;
    auto cmd = rt::EditOperations::paste(dst, clipboard, 0);
    ASSERT_NE(cmd, nullptr);
    stack.execute(std::move(cmd));

    EXPECT_EQ(dst.trackCount(), before + 2);  // two overflow video tracks
    size_t total = 0;
    for (size_t i = 0; i < dst.trackCount(); ++i) total += dst.track(i)->clipCount();
    EXPECT_EQ(total, 4u);                       // all clips placed, none dropped

    stack.undo();
    EXPECT_EQ(dst.trackCount(), before);        // overflow tracks removed
    size_t after = 0;
    for (size_t i = 0; i < dst.trackCount(); ++i) after += dst.track(i)->clipCount();
    EXPECT_EQ(after, 0u);
}

// Insert-paste must use the same V1/A1 anchoring as overwrite paste: clips
// from a 3-track source land on the destination's BOTTOM three video tracks.
TEST(EditOperations, PasteInsertCrossSequenceAnchorsAtV1)
{
    rt::Timeline src;
    rt::Track* s0 = src.addVideoTrack("");  // idx0 = top    = V3
    rt::Track* s1 = src.addVideoTrack("");  // idx1          = V2
    rt::Track* s2 = src.addVideoTrack("");  // idx2 = bottom = V1
    addClip(s0, 0.0, 1.0);
    addClip(s1, 0.0, 1.0);
    addClip(s2, 0.0, 1.0);

    rt::SelectionSet sel;
    sel.selectClip({0, s0->clip(0)->id()}, true);
    sel.selectClip({1, s1->clip(0)->id()}, true);
    sel.selectClip({2, s2->clip(0)->id()}, true);
    rt::ClipboardContents clipboard;
    rt::EditOperations::copySelection(src, sel, clipboard);

    rt::Timeline dst;
    for (int i = 0; i < 5; ++i) dst.addVideoTrack("");  // idx0..4, idx4 = V1
    rt::CommandStack stack;
    auto cmd = rt::EditOperations::pasteInsert(dst, clipboard, 10 * TPS);
    ASSERT_NE(cmd, nullptr);
    stack.execute(std::move(cmd));

    // Bottom three tracks (V1,V2,V3 = idx 4,3,2) get the clips; top two empty.
    EXPECT_EQ(dst.track(4)->clipCount(), 1u);
    EXPECT_EQ(dst.track(3)->clipCount(), 1u);
    EXPECT_EQ(dst.track(2)->clipCount(), 1u);
    EXPECT_EQ(dst.track(1)->clipCount(), 0u);
    EXPECT_EQ(dst.track(0)->clipCount(), 0u);
}

// Insert-paste from a wider source must create overflow tracks so no clip
// is dropped, shift existing content right by the inserted span, and undo
// must restore both the tracks and the shifted positions.
TEST(EditOperations, PasteInsertCrossSequenceAddsOverflowTracksAndShifts)
{
    rt::Timeline src;
    std::vector<rt::Track*> st;
    for (int i = 0; i < 4; ++i) st.push_back(src.addVideoTrack(""));
    rt::SelectionSet sel;
    for (int i = 0; i < 4; ++i) {
        addClip(st[static_cast<size_t>(i)], 0.0, 1.0);
        sel.selectClip({static_cast<size_t>(i), st[static_cast<size_t>(i)]->clip(0)->id()}, true);
    }
    rt::ClipboardContents clipboard;
    rt::EditOperations::copySelection(src, sel, clipboard);

    rt::Timeline dst;
    rt::Track* d0 = dst.addVideoTrack("");
    dst.addVideoTrack("");
    // Existing clip at t=0 on the top track — must shift right by the span.
    addClip(d0, 0.0, 2.0);
    const int64_t oldIn = d0->clip(0)->timelineIn();
    const size_t before = dst.trackCount();

    rt::CommandStack stack;
    auto cmd = rt::EditOperations::pasteInsert(dst, clipboard, 0);
    ASSERT_NE(cmd, nullptr);
    stack.execute(std::move(cmd));

    EXPECT_EQ(dst.trackCount(), before + 2);  // two overflow video tracks
    size_t total = 0;
    for (size_t i = 0; i < dst.trackCount(); ++i) total += dst.track(i)->clipCount();
    EXPECT_EQ(total, 5u);                     // 4 pasted + 1 pre-existing

    // The pre-existing clip must have rippled right by the inserted span (1s).
    bool foundShifted = false;
    for (size_t i = 0; i < dst.trackCount(); ++i) {
        const rt::Track* t = dst.track(i);
        for (size_t c = 0; c < t->clipCount(); ++c) {
            if (t->clip(c)->duration() == 2 * TPS &&
                t->clip(c)->timelineIn() == oldIn + 1 * TPS)
                foundShifted = true;
        }
    }
    EXPECT_TRUE(foundShifted);

    stack.undo();
    EXPECT_EQ(dst.trackCount(), before);      // overflow tracks removed
    size_t after = 0;
    for (size_t i = 0; i < dst.trackCount(); ++i) after += dst.track(i)->clipCount();
    EXPECT_EQ(after, 1u);                     // only the pre-existing clip
}

TEST(EditOperations, CutSelection)
{
    TestTimeline tt;

    rt::SelectionSet sel;
    sel.selectClip({0, tt.clipA});
    rt::ClipboardContents clipboard;

    auto cmd = rt::EditOperations::cutSelection(tt.timeline, sel, clipboard);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    EXPECT_EQ(tt.vTrack->clipCount(), 2u);
    EXPECT_FALSE(clipboard.empty());
}

TEST(EditOperations, DuplicateSelection)
{
    TestTimeline tt;

    rt::SelectionSet sel;
    sel.selectClip({0, tt.clipA});

    auto cmd = rt::EditOperations::duplicateSelection(tt.timeline, sel);
    ASSERT_NE(cmd, nullptr);
    tt.stack.execute(std::move(cmd));

    EXPECT_EQ(tt.vTrack->clipCount(), 4u);
}

// ═════════════════════════════════════════════════════════════════════════════
//  In/Out Points
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, SetInPoint)
{
    TestTimeline tt;

    rt::EditOperations::setInPoint(tt.timeline, 3 * TPS);
    EXPECT_EQ(tt.timeline.inPoint(), 3 * TPS);
}

TEST(EditOperations, SetOutPoint)
{
    TestTimeline tt;

    rt::EditOperations::setOutPoint(tt.timeline, 7 * TPS);
    EXPECT_EQ(tt.timeline.outPoint(), 7 * TPS);
}

TEST(EditOperations, InPointClearsInvalidOut)
{
    TestTimeline tt;

    rt::EditOperations::setOutPoint(tt.timeline, 5 * TPS);
    rt::EditOperations::setInPoint(tt.timeline, 6 * TPS); // Past out point

    EXPECT_EQ(tt.timeline.inPoint(), 6 * TPS);
    EXPECT_EQ(tt.timeline.outPoint(), -1); // Should be cleared
}

TEST(EditOperations, OutPointClearsInvalidIn)
{
    TestTimeline tt;

    rt::EditOperations::setInPoint(tt.timeline, 5 * TPS);
    rt::EditOperations::setOutPoint(tt.timeline, 4 * TPS); // Before in point

    EXPECT_EQ(tt.timeline.outPoint(), 4 * TPS);
    EXPECT_EQ(tt.timeline.inPoint(), -1); // Should be cleared
}

TEST(EditOperations, ClearInOutPoints)
{
    TestTimeline tt;

    rt::EditOperations::setInPoint(tt.timeline, 1 * TPS);
    rt::EditOperations::setOutPoint(tt.timeline, 5 * TPS);
    rt::EditOperations::clearInOutPoints(tt.timeline);

    EXPECT_EQ(tt.timeline.inPoint(), -1);
    EXPECT_EQ(tt.timeline.outPoint(), -1);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Edit point navigation
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, NextEditPoint)
{
    TestTimeline tt;
    // Clips: [0–2s] [2–5s] [5–8s]
    // Edit points: 0, 2s, 5s, 8s (in, out, in, out, in, out)

    // From 0 → next is 2s
    EXPECT_EQ(rt::EditOperations::nextEditPoint(tt.timeline, 0), 2 * TPS);

    // From 2s → next is 5s
    EXPECT_EQ(rt::EditOperations::nextEditPoint(tt.timeline, 2 * TPS), 5 * TPS);

    // From 5s → next is 8s
    EXPECT_EQ(rt::EditOperations::nextEditPoint(tt.timeline, 5 * TPS), 8 * TPS);

    // From 8s → no more → stays at 8s
    EXPECT_EQ(rt::EditOperations::nextEditPoint(tt.timeline, 8 * TPS), 8 * TPS);
}

TEST(EditOperations, PrevEditPoint)
{
    TestTimeline tt;
    // Edit points: 0, 2s, 5s, 8s

    // From 8s → prev is 5s
    EXPECT_EQ(rt::EditOperations::prevEditPoint(tt.timeline, 8 * TPS), 5 * TPS);

    // From 5s → prev is 2s  (clipA out=2s is < 5s)
    EXPECT_EQ(rt::EditOperations::prevEditPoint(tt.timeline, 5 * TPS), 2 * TPS);

    // From 1s → prev is 0
    EXPECT_EQ(rt::EditOperations::prevEditPoint(tt.timeline, TPS), 0);

    // From 0 → stays at 0
    EXPECT_EQ(rt::EditOperations::prevEditPoint(tt.timeline, 0), 0);
}

TEST(EditOperations, NextEditPointEmptyTimeline)
{
    rt::Timeline timeline;
    timeline.addVideoTrack("V1"); // No clips

    EXPECT_EQ(rt::EditOperations::nextEditPoint(timeline, 0), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Helpers
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, ClipAtTime)
{
    TestTimeline tt;

    // At 1s → clipA
    auto* found = rt::EditOperations::clipAtTime(*tt.vTrack, TPS);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id(), tt.clipA);

    // At 3s → clipB
    found = rt::EditOperations::clipAtTime(*tt.vTrack, 3 * TPS);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id(), tt.clipB);

    // At 10s → nothing
    found = rt::EditOperations::clipAtTime(*tt.vTrack, 10 * TPS);
    EXPECT_EQ(found, nullptr);
}

TEST(EditOperations, FindEditPoint)
{
    TestTimeline tt;

    // Near 2s → should find edit between clipA and clipB
    auto ep = rt::EditOperations::findEditPoint(*tt.vTrack, 2 * TPS);
    EXPECT_NE(ep.leftClip, nullptr);
    EXPECT_NE(ep.rightClip, nullptr);
    EXPECT_EQ(ep.editTime, 2 * TPS);
}

TEST(EditOperations, FindEditPointEmpty)
{
    rt::Timeline timeline;
    rt::Track* track = timeline.addVideoTrack("V1");

    auto ep = rt::EditOperations::findEditPoint(*track, TPS);
    EXPECT_EQ(ep.leftClip, nullptr);
    EXPECT_EQ(ep.rightClip, nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
//  EditTool enum
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, EditToolValues)
{
    EXPECT_NE(static_cast<int>(rt::EditTool::Selection),
              static_cast<int>(rt::EditTool::Razor));
    EXPECT_NE(static_cast<int>(rt::EditTool::Rolling),
              static_cast<int>(rt::EditTool::Ripple));
    EXPECT_NE(static_cast<int>(rt::EditTool::Slip),
              static_cast<int>(rt::EditTool::Slide));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Edge cases
// ═════════════════════════════════════════════════════════════════════════════

TEST(EditOperations, SplitInvalidTrack)
{
    TestTimeline tt;
    auto cmd = rt::EditOperations::splitClip(tt.timeline, 99, tt.clipA, TPS);
    EXPECT_EQ(cmd, nullptr);
}

TEST(EditOperations, TrimInvalidClip)
{
    TestTimeline tt;
    auto cmd = rt::EditOperations::trimClip(tt.timeline, 0, 99999,
                                             rt::ClipEdge::Head, TPS);
    EXPECT_EQ(cmd, nullptr);
}

TEST(EditOperations, MoveInvalidTrack)
{
    TestTimeline tt;
    auto cmd = rt::EditOperations::moveClip(tt.timeline, 99, tt.clipA, 0);
    EXPECT_EQ(cmd, nullptr);
}

TEST(EditOperations, ClipRefEquality)
{
    rt::ClipRef a{0, 100};
    rt::ClipRef b{0, 100};
    rt::ClipRef c{1, 100};

    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a != c);
}

TEST(EditOperations, ClipboardClear)
{
    rt::ClipboardContents clipboard;
    EXPECT_TRUE(clipboard.empty());

    TestTimeline tt;
    rt::SelectionSet sel;
    sel.selectClip({0, tt.clipA});
    rt::EditOperations::copySelection(tt.timeline, sel, clipboard);
    EXPECT_FALSE(clipboard.empty());

    clipboard.clear();
    EXPECT_TRUE(clipboard.empty());
}

TEST(EditOperations, OverwriteMiddleKeepsKeyframesAtOriginalTimelinePositions)
{
    rt::Timeline timeline;
    rt::Track* track = timeline.addVideoTrack("V1");
    rt::CommandStack stack;

    auto original = makeClip(0.0, 10.0);
    original->opacity().addKeyframe(1 * TPS, 0.1f);
    original->opacity().addKeyframe(4 * TPS, 0.4f);
    original->opacity().addKeyframe(5 * TPS, 0.5f);
    original->opacity().addKeyframe(6 * TPS, 0.6f);
    original->opacity().addKeyframe(9 * TPS, 0.9f);
    original->positionX().addKeyframe(1 * TPS, 10.0f);
    original->positionX().addKeyframe(8 * TPS, 80.0f);
    original->shutterAngle().addKeyframe(1 * TPS, 90.0f);
    original->shutterAngle().addKeyframe(8 * TPS, 270.0f);
    const uint64_t originalId = original->id();
    ASSERT_NE(track->addClip(std::move(original)), nullptr);

    auto incoming = makeClip(4.0, 2.0);
    const uint64_t incomingId = incoming->id();
    ASSERT_NE(track->addClip(std::move(incoming)), nullptr);

    auto overwrite = rt::EditOperations::resolveOverlaps(
        timeline, 0, incomingId);
    ASSERT_NE(overwrite, nullptr);
    stack.execute(std::move(overwrite));

    ASSERT_EQ(track->clipCount(), 3u);
    auto* left = track->clip(track->findClipIndexById(originalId));
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->timelineIn(), 0);
    EXPECT_EQ(left->timelineOut(), 4 * TPS);
    ASSERT_EQ(left->opacity().keyframeCount(), 1u);
    EXPECT_EQ(left->timelineIn() + left->opacity().keyframe(0).time, 1 * TPS);
    ASSERT_EQ(left->positionX().keyframeCount(), 1u);
    EXPECT_EQ(left->timelineIn() + left->positionX().keyframe(0).time, 1 * TPS);
    ASSERT_EQ(left->shutterAngle().keyframeCount(), 1u);
    EXPECT_FLOAT_EQ(left->shutterAngle().keyframe(0).value, 90.0f);

    rt::Clip* right = nullptr;
    for (size_t i = 0; i < track->clipCount(); ++i) {
        auto* clip = track->clip(i);
        if (clip->id() != originalId && clip->id() != incomingId)
            right = clip;
    }
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(right->timelineIn(), 6 * TPS);
    EXPECT_EQ(right->timelineOut(), 10 * TPS);
    ASSERT_EQ(right->opacity().keyframeCount(), 2u);
    EXPECT_EQ(right->timelineIn() + right->opacity().keyframe(0).time, 6 * TPS);
    EXPECT_EQ(right->timelineIn() + right->opacity().keyframe(1).time, 9 * TPS);
    ASSERT_EQ(right->positionX().keyframeCount(), 1u);
    EXPECT_EQ(right->timelineIn() + right->positionX().keyframe(0).time, 8 * TPS);
    ASSERT_EQ(right->shutterAngle().keyframeCount(), 1u);
    EXPECT_FLOAT_EQ(right->shutterAngle().keyframe(0).value, 270.0f);

    stack.undo();
    ASSERT_EQ(track->clipCount(), 2u);
    auto* restored = track->clip(track->findClipIndexById(originalId));
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->duration(), 10 * TPS);
    ASSERT_EQ(restored->opacity().keyframeCount(), 5u);
    EXPECT_EQ(restored->opacity().keyframe(0).time, 1 * TPS);
    EXPECT_EQ(restored->opacity().keyframe(4).time, 9 * TPS);

    stack.redo();
    ASSERT_EQ(track->clipCount(), 3u);
    auto* redoneLeft = track->clip(track->findClipIndexById(originalId));
    ASSERT_NE(redoneLeft, nullptr);
    EXPECT_EQ(redoneLeft->opacity().keyframeCount(), 1u);
}

TEST(EditOperations, PasteGraphicAtSameStartOverwritesExistingGraphic)
{
    rt::Timeline timeline;
    rt::Track* track = timeline.addVideoTrack("V1");
    rt::CommandStack stack;

    auto source = std::make_unique<rt::GraphicClip>();
    source->setTimelineIn(0);
    source->setDuration(2 * TPS);
    source->setLabel("Copied GFX");
    source->addTextLayer("Replacement");
    const uint64_t sourceId = source->id();
    ASSERT_NE(track->addClip(std::move(source)), nullptr);

    auto existing = std::make_unique<rt::GraphicClip>();
    existing->setTimelineIn(5 * TPS);
    existing->setDuration(2 * TPS);
    existing->setLabel("Old GFX");
    existing->addTextLayer("Old");
    const uint64_t existingId = existing->id();
    ASSERT_NE(track->addClip(std::move(existing)), nullptr);

    rt::SelectionSet selection;
    selection.selectClip({0, sourceId});
    rt::ClipboardContents clipboard;
    rt::EditOperations::copySelection(timeline, selection, clipboard);

    auto paste = rt::EditOperations::paste(timeline, clipboard, 5 * TPS);
    ASSERT_NE(paste, nullptr);
    stack.execute(std::move(paste));

    ASSERT_EQ(track->clipCount(), 2u);
    EXPECT_EQ(track->findClipIndexById(existingId), track->clipCount());
    const auto* pasted = rt::EditOperations::clipAtTime(*track, 5 * TPS);
    ASSERT_NE(pasted, nullptr);
    EXPECT_NE(pasted->id(), existingId);
    EXPECT_EQ(pasted->clipType(), rt::ClipType::Graphic);
    EXPECT_EQ(pasted->label(), "Copied GFX");

    stack.undo();
    ASSERT_EQ(track->clipCount(), 2u);
    EXPECT_LT(track->findClipIndexById(existingId), track->clipCount());
    EXPECT_EQ(rt::EditOperations::clipAtTime(*track, 5 * TPS)->id(), existingId);

    stack.redo();
    ASSERT_EQ(track->clipCount(), 2u);
    EXPECT_EQ(track->findClipIndexById(existingId), track->clipCount());
    EXPECT_EQ(rt::EditOperations::clipAtTime(*track, 5 * TPS)->label(), "Copied GFX");
}

TEST(EditOperations, GraphicInsertDurationStopsAtNextClip)
{
    rt::Timeline timeline;
    rt::Track* track = timeline.addVideoTrack("Graphics");

    auto first = std::make_unique<rt::GraphicClip>();
    first->setTimelineIn(0);
    first->setDuration(2 * TPS);
    ASSERT_NE(track->addClip(std::move(first)), nullptr);

    auto next = std::make_unique<rt::GraphicClip>();
    next->setTimelineIn(4 * TPS);
    next->setDuration(2 * TPS);
    ASSERT_NE(track->addClip(std::move(next)), nullptr);

    EXPECT_EQ(rt::EditOperations::nonOverlappingInsertDuration(
                  *track, 2 * TPS, 5 * TPS),
              2 * TPS);
    EXPECT_EQ(rt::EditOperations::nonOverlappingInsertDuration(
                  *track, 1 * TPS, 5 * TPS),
              0);
}

TEST(EditOperations, ExcludedMovedGraphicsStillCannotOverlap)
{
    rt::Timeline timeline;
    rt::Track* track = timeline.addVideoTrack("Graphics");

    auto left = std::make_unique<rt::GraphicClip>();
    left->setTimelineIn(0);
    left->setDuration(5 * TPS);
    const uint64_t leftId = left->id();
    ASSERT_NE(track->addClip(std::move(left)), nullptr);

    auto moved = std::make_unique<rt::GraphicClip>();
    moved->setTimelineIn(2 * TPS);
    moved->setDuration(5 * TPS);
    const uint64_t movedId = moved->id();
    ASSERT_NE(track->addClip(std::move(moved)), nullptr);

    auto resolve = rt::EditOperations::resolveOverlaps(
        timeline, 0, movedId, {leftId, movedId});
    ASSERT_NE(resolve, nullptr);
    resolve->execute();

    ASSERT_EQ(track->clipCount(), 2u);
    EXPECT_EQ(track->clip(0)->timelineOut(), track->clip(1)->timelineIn());
}

TEST(NestTransitionTransfer, PreservesInteriorBoundaryAndUndoTransitions)
{
    rt::Timeline parent;
    rt::Track* parentTrack = parent.addVideoTrack("V1");
    const uint64_t a = addClip(parentTrack, 10.0, 2.0);
    const uint64_t b = addClip(parentTrack, 12.0, 2.0);
    const uint64_t c = addClip(parentTrack, 14.0, 2.0);

    rt::Transition fadeIn;
    fadeIn.type = rt::TransitionType::DipToWhite;
    fadeIn.duration = TPS / 2;
    fadeIn.leftClipId = 0;
    fadeIn.rightClipId = a;
    fadeIn.editPointTick = 10 * TPS;
    fadeIn.param1 = 0.35f;
    parentTrack->addTransition(fadeIn);

    rt::Transition dissolve;
    dissolve.type = rt::TransitionType::CrossDissolve;
    dissolve.duration = TPS;
    dissolve.leftClipId = a;
    dissolve.rightClipId = b;
    dissolve.editPointTick = 12 * TPS;
    dissolve.offset = TPS / 10;
    parentTrack->addTransition(dissolve);

    rt::Transition boundary;
    boundary.type = rt::TransitionType::WipeLeft;
    boundary.duration = TPS / 2;
    boundary.leftClipId = b;
    boundary.rightClipId = c;
    boundary.editPointTick = 14 * TPS;
    boundary.param2 = 0.7f;
    parentTrack->addTransition(boundary);

    const std::unordered_set<uint64_t> selected{a, b};
    const auto snapshots = rt::captureTransitionsForNest(parent, selected);
    ASSERT_EQ(snapshots.size(), 3u);

    rt::Timeline nested;
    rt::Track* nestedTrack = nested.addVideoTrack("V1");
    const uint64_t nestedA = addClip(nestedTrack, 0.0, 2.0);
    const uint64_t nestedB = addClip(nestedTrack, 2.0, 2.0);
    const std::unordered_map<size_t, size_t> trackMap{{0, 0}};
    const std::unordered_map<uint64_t, uint64_t> nestedIds{
        {a, nestedA}, {b, nestedB}};

    EXPECT_EQ(rt::addTransitionsInsideNest(
                  nested, snapshots, selected, trackMap, nestedIds, 10 * TPS),
              2u);
    ASSERT_EQ(nestedTrack->transitionCount(), 2u);
    const size_t nestedFadeIndex = nestedTrack->findTransition(0, nestedA);
    const size_t nestedDissolveIndex =
        nestedTrack->findTransition(nestedA, nestedB);
    ASSERT_NE(nestedFadeIndex, rt::Track::kNoTransition);
    ASSERT_NE(nestedDissolveIndex, rt::Track::kNoTransition);
    EXPECT_EQ(nestedTrack->transition(nestedFadeIndex)->editPointTick, 0);
    EXPECT_EQ(nestedTrack->transition(nestedFadeIndex)->type, fadeIn.type);
    EXPECT_FLOAT_EQ(nestedTrack->transition(nestedFadeIndex)->param1,
                    fadeIn.param1);
    EXPECT_EQ(nestedTrack->transition(nestedDissolveIndex)->editPointTick,
              2 * TPS);
    EXPECT_EQ(nestedTrack->transition(nestedDissolveIndex)->duration,
              dissolve.duration);
    EXPECT_EQ(nestedTrack->transition(nestedDissolveIndex)->offset,
              dissolve.offset);

    // Simulate execute: removing selected clips drops their track-owned
    // transitions, then the new SequenceClip receives the external boundary.
    parentTrack->removeClipById(a);
    parentTrack->removeClipById(b);
    EXPECT_EQ(parentTrack->transitionCount(), 0u);
    auto sequenceClip = std::make_unique<rt::SequenceClip>();
    sequenceClip->setTimelineIn(10 * TPS);
    sequenceClip->setDuration(4 * TPS);
    const uint64_t sequenceClipId = sequenceClip->id();
    ASSERT_NE(parentTrack->addClip(std::move(sequenceClip)), nullptr);

    EXPECT_EQ(rt::addBoundaryTransitionsToNestClip(
                  parent, snapshots, selected, 0, sequenceClipId),
              1u);
    const size_t boundaryIndex =
        parentTrack->findTransition(sequenceClipId, c);
    ASSERT_NE(boundaryIndex, rt::Track::kNoTransition);
    EXPECT_EQ(parentTrack->transition(boundaryIndex)->type, boundary.type);
    EXPECT_FLOAT_EQ(parentTrack->transition(boundaryIndex)->param2,
                    boundary.param2);

    // Simulate undo: removing the nest drops its boundary transition; fresh
    // clones get new IDs, and every original transition is restored/remapped.
    parentTrack->removeClipById(sequenceClipId);
    const uint64_t restoredA = addClip(parentTrack, 10.0, 2.0);
    const uint64_t restoredB = addClip(parentTrack, 12.0, 2.0);
    const std::unordered_map<uint64_t, uint64_t> restoredIds{
        {a, restoredA}, {b, restoredB}};
    EXPECT_EQ(rt::restoreTransitionsAfterNestUndo(
                  parent, snapshots, restoredIds),
              3u);
    EXPECT_NE(parentTrack->findTransition(0, restoredA),
              rt::Track::kNoTransition);
    EXPECT_NE(parentTrack->findTransition(restoredA, restoredB),
              rt::Track::kNoTransition);
    const size_t restoredBoundary =
        parentTrack->findTransition(restoredB, c);
    ASSERT_NE(restoredBoundary, rt::Track::kNoTransition);
    EXPECT_EQ(parentTrack->transition(restoredBoundary)->editPointTick,
              boundary.editPointTick);
}
