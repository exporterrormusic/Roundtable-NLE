/*
 * test_model_snapshot.cpp — read-only render snapshots of the edited model.
 *
 * Render threads draw from copies published by SnapshotPublisher, never from
 * the live Timeline the UI edits. The contract under test:
 *   - a snapshot copy renders and caches exactly like the original
 *     (same clip ids, track order, flags; equal hashTimelineState);
 *   - every clip type's clone() keeps the shared base state (masks,
 *     markers, pitch, offline) — clones used to drop them per type;
 *   - a published snapshot never changes after later edits;
 *   - unchanged sequences are shared between snapshots, dirty ones re-copied;
 *   - an edit that skipped markDirty() is reported by verifySequences().
 */

#include <gtest/gtest.h>

#include "playback/SnapshotPublisher.h"
#include "project/Project.h"
#include "project/RenderStateHash.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/AudioClip.h"
#include "timeline/CaptionClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/ImageClip.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TierListClip.h"
#include "timeline/Timeline.h"
#include "timeline/TitleClip.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

namespace rt {
namespace {

constexpr int64_t kSec = 48000;

std::vector<std::function<std::unique_ptr<Clip>()>> everyClipType()
{
    return {
        [] { return std::make_unique<VideoClip>(); },
        [] { return std::make_unique<AudioClip>(); },
        [] { return std::make_unique<ImageClip>(); },
        [] { return std::make_unique<GraphicClip>(); },
        [] { return std::make_unique<AdjustmentClip>(); },
        [] { return std::make_unique<TitleClip>(); },
        [] { return std::make_unique<SequenceClip>(); },
        [] { return std::make_unique<CaptionClip>(); },
        [] { return std::make_unique<SpineClip>(); },
        [] { return std::make_unique<PngPuppetClip>(); },
        [] { return std::make_unique<TierListClip>(); },
    };
}

Clip* place(Track* track, std::unique_ptr<Clip> clip, int64_t in, int64_t dur)
{
    clip->setTimelineIn(in);
    clip->setDuration(dur);
    return track->addClip(std::move(clip));
}

// A sequence with every track kind, every clip type, masks, markers and a
// transition — the shapes a snapshot copy has to reproduce.
std::unique_ptr<Timeline> makeRichTimeline()
{
    auto tl = std::make_unique<Timeline>();
    tl->addCaptionTrack();
    Track* v1 = tl->addVideoTrack("V1");
    Track* v2 = tl->addVideoTrack("V2");
    tl->addDividerTrack(static_cast<size_t>(-1), /*permanent=*/true);
    Track* a1 = tl->addAudioTrack("A1");
    v2->setMuted(true);
    a1->setVolume(0.5f);

    int64_t t = 0;
    for (const auto& make : everyClipType()) {
        auto clip = make();
        Track* target = clip->isAudio() ? a1 : (clip->isCaption() ? tl->captionTrack() : v1);
        clip->addMask(OpacityMask{});
        clip->addMarker(Marker{kSec / 2, "beat", 0xFF00FF00});
        clip->setMaintainPitch(false);
        clip->setBlendMode(1);
        clip->opacity().addKeyframe(0, 0.25f);
        place(target, std::move(clip), t, kSec);
        t += kSec;
    }
    Clip* left = v1->clip(0);
    Clip* right = v1->clip(1);
    Transition tr;
    tr.duration = kSec / 4;
    tr.leftClipId = left->id();
    tr.rightClipId = right->id();
    tr.editPointTick = left->timelineOut();
    tr.param1 = 0.3f;
    v1->addTransition(tr);
    tl->addMarker(kSec, "sequence marker");
    return tl;
}

// ── Clone fidelity ──────────────────────────────────────────────────────────

TEST(ModelSnapshot, EveryClipTypeCloneKeepsBaseState)
{
    for (const auto& make : everyClipType()) {
        auto clip = make();
        clip->addMask(OpacityMask{});
        clip->addMarker(Marker{100, "m", 0xFFFFFFFF});
        clip->setMaintainPitch(false);
        clip->setOffline(true);
        clip->setBlendMode(3);
        clip->setLinkId(42);

        const auto copy = clip->clone();
        SCOPED_TRACE(static_cast<int>(clip->clipType()));
        EXPECT_EQ(copy->clipType(), clip->clipType());
        EXPECT_EQ(copy->masks().size(), 1u);
        EXPECT_EQ(copy->markers().size(), 1u);
        EXPECT_FALSE(copy->maintainPitch());
        EXPECT_TRUE(copy->isOffline());
        EXPECT_EQ(copy->blendMode(), 3);
        EXPECT_EQ(copy->linkId(), 42u);
        EXPECT_NE(copy->id(), clip->id()) << "clone() keeps duplicate semantics";
    }
}

TEST(ModelSnapshot, CloneForSnapshotIsExact)
{
    const auto tl = makeRichTimeline();
    const auto copy = tl->cloneForSnapshot();

    EXPECT_EQ(hashTimelineState(*tl), hashTimelineState(*copy));
    ASSERT_EQ(copy->trackCount(), tl->trackCount());
    for (size_t ti = 0; ti < tl->trackCount(); ++ti) {
        const Track* a = tl->track(ti);
        const Track* b = copy->track(ti);
        EXPECT_EQ(a->name(), b->name());
        EXPECT_EQ(a->isDivider(), b->isDivider());
        EXPECT_EQ(a->isPermanentDivider(), b->isPermanentDivider());
        EXPECT_EQ(a->isCaptionTrack(), b->isCaptionTrack());
        ASSERT_EQ(a->clipCount(), b->clipCount());
        for (size_t ci = 0; ci < a->clipCount(); ++ci)
            EXPECT_EQ(a->clip(ci)->id(), b->clip(ci)->id());
        ASSERT_EQ(a->transitionCount(), b->transitionCount());
    }
    EXPECT_EQ(copy->markers().size(), tl->markers().size());
}

TEST(ModelSnapshot, StateHashSeesEdits)
{
    const auto tl = makeRichTimeline();
    const uint64_t before = hashTimelineState(*tl);
    tl->track(1)->clip(0)->setTimelineIn(5);
    EXPECT_NE(before, hashTimelineState(*tl));
}

// ── Publisher ───────────────────────────────────────────────────────────────

TEST(ModelSnapshot, PublishedSnapshotIgnoresLaterEdits)
{
    auto tl = makeRichTimeline();
    SnapshotPublisher pub;
    pub.bind(nullptr, tl.get());
    ASSERT_TRUE(pub.publish());

    const auto snap = pub.latest();
    ASSERT_TRUE(snap && snap->timeline);
    const Clip* published = snap->timeline->track(1)->clip(0);
    const int64_t in = published->timelineIn();

    tl->track(1)->clip(0)->setTimelineIn(in + kSec * 10);
    tl->track(1)->clip(0)->setBlendMode(7);
    EXPECT_EQ(published->timelineIn(), in);
    EXPECT_EQ(published->blendMode(), 1);

    pub.markDirty(SnapshotPublisher::DirtyScope::ActiveSequence);
    ASSERT_TRUE(pub.publish());
    EXPECT_EQ(pub.latest()->timeline->track(1)->clip(0)->blendMode(), 7);
    // The held snapshot is still the old one.
    EXPECT_EQ(published->blendMode(), 1);
}

TEST(ModelSnapshot, CoalescesDirtyMarksIntoOnePublish)
{
    Timeline tl;
    tl.addVideoTrack("V1");
    SnapshotPublisher pub;
    pub.bind(nullptr, &tl);
    ASSERT_TRUE(pub.publish());
    const uint64_t v1 = pub.latest()->editVersion;

    EXPECT_FALSE(pub.publish()) << "nothing dirty";
    pub.markDirty(SnapshotPublisher::DirtyScope::ActiveSequence);
    pub.markDirty(SnapshotPublisher::DirtyScope::ActiveSequence);
    pub.markDirty(SnapshotPublisher::DirtyScope::ActiveSequence);
    EXPECT_TRUE(pub.publish());
    EXPECT_FALSE(pub.publish());
    EXPECT_EQ(pub.latest()->editVersion, v1 + 1);
}

TEST(ModelSnapshot, SharesUnchangedSequences)
{
    Project proj;
    proj.sequence(0)->addVideoTrack("V1");
    Timeline* other = proj.addSequence("Nested");
    place(other->addVideoTrack("V1"), std::make_unique<ImageClip>(), 0, kSec);

    SnapshotPublisher pub;
    pub.bind(&proj, proj.sequence(0));
    ASSERT_TRUE(pub.publish());
    const auto first = pub.latest();
    ASSERT_EQ(first->sequences.size(), 2u);
    EXPECT_EQ(first->sequenceIndex, 0u);
    EXPECT_EQ(first->timeline, first->sequences[0]);

    // Active-sequence edit: only sequence 0 is copied again.
    pub.markDirty(SnapshotPublisher::DirtyScope::ActiveSequence);
    ASSERT_TRUE(pub.publish());
    const auto second = pub.latest();
    EXPECT_NE(second->sequences[0], first->sequences[0]);
    EXPECT_EQ(second->sequences[1], first->sequences[1]);
    EXPECT_EQ(pub.lastPublishStats().sequencesShared, 1u);

    // Undo/redo-style edit: everything is copied again.
    pub.markDirty(SnapshotPublisher::DirtyScope::AllSequences);
    ASSERT_TRUE(pub.publish());
    EXPECT_NE(pub.latest()->sequences[1], second->sequences[1]);

    // Nested lookup resolves through the snapshot, by project index.
    const Timeline* nested = pub.latest()->sequenceAt(1);
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->name(), "Nested");
    EXPECT_EQ(hashTimelineState(*nested), hashTimelineState(*other));
    EXPECT_EQ(pub.latest()->sequenceAt(2), nullptr);
}

TEST(ModelSnapshot, SequenceListChangeRecopiesEverything)
{
    Project proj;
    SnapshotPublisher pub;
    pub.bind(&proj, proj.sequence(0));
    ASSERT_TRUE(pub.publish());

    proj.addSequence("Added");
    // Even a too-narrow signal can't reuse copies across a changed list.
    pub.markDirty(SnapshotPublisher::DirtyScope::ActiveSequence);
    ASSERT_TRUE(pub.publish());
    EXPECT_EQ(pub.latest()->sequences.size(), 2u);
    EXPECT_EQ(pub.lastPublishStats().sequencesShared, 0u);
}

TEST(ModelSnapshot, VerifyReportsEditThatSkippedMarkDirty)
{
    auto tl = makeRichTimeline();
    SnapshotPublisher pub;
    pub.bind(nullptr, tl.get());
    ASSERT_TRUE(pub.publish());

    std::string detail;
    EXPECT_TRUE(pub.verifySequences(&detail));

    Clip* clip = tl->track(1)->clip(2);
    clip->setBlendMode(5);  // no markDirty()
    EXPECT_FALSE(pub.verifySequences(&detail));
    EXPECT_NE(detail.find("id=" + std::to_string(clip->id())), std::string::npos) << detail;
    EXPECT_TRUE(pub.isDirty());

    ASSERT_TRUE(pub.publish());
    EXPECT_TRUE(pub.verifySequences(&detail));
}

TEST(ModelSnapshot, VerifyChecksInactiveSequencesInRotation)
{
    Project proj;
    proj.sequence(0)->addVideoTrack("V1");
    Timeline* other = proj.addSequence("Other");
    Clip* hidden = place(other->addVideoTrack("V1"), std::make_unique<ImageClip>(), 0, kSec);

    SnapshotPublisher pub;
    pub.bind(&proj, proj.sequence(0));
    ASSERT_TRUE(pub.publish());

    hidden->setBlendMode(4);  // edit in a non-active sequence, unsignaled
    bool caught = false;
    for (int i = 0; i < 3 && !caught; ++i)
        caught = !pub.verifySequences();
    EXPECT_TRUE(caught);
}

TEST(ModelSnapshot, DestroyedActiveTimelineUnbinds)
{
    auto tl = std::make_unique<Timeline>();
    tl->addVideoTrack("V1");
    SnapshotPublisher pub;
    pub.bind(nullptr, tl.get());
    ASSERT_TRUE(pub.publish());
    const auto held = pub.latest();

    tl.reset();
    EXPECT_TRUE(pub.isDirty());
    ASSERT_TRUE(pub.publish());
    EXPECT_EQ(pub.latest()->timeline, nullptr);
    // The snapshot a render thread still holds stays valid.
    EXPECT_EQ(held->timeline->trackCount(), 1u);
}

TEST(ModelSnapshot, SnapshotReleasedOnAnotherThread)
{
    auto tl = makeRichTimeline();
    SnapshotPublisher pub;
    pub.bind(nullptr, tl.get());
    ASSERT_TRUE(pub.publish());

    auto held = pub.latest();
    pub.markDirty(SnapshotPublisher::DirtyScope::ActiveSequence);
    ASSERT_TRUE(pub.publish());
    // The render thread drops the last reference; the copy is parked and
    // freed by the next publish on this thread.
    std::thread([h = std::move(held)]() mutable { h.reset(); }).join();
    pub.markDirty(SnapshotPublisher::DirtyScope::ActiveSequence);
    EXPECT_TRUE(pub.publish());
    pub.collectRetired();
}

TEST(ModelSnapshot, RenderThreadReadsWhileUiEdits)
{
    auto tl = makeRichTimeline();
    SnapshotPublisher pub;
    pub.bind(nullptr, tl.get());
    ASSERT_TRUE(pub.publish());

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};
    std::thread render([&] {
        while (!stop.load()) {
            const auto snap = pub.latest();
            uint64_t sum = 0;
            for (size_t ti = 0; ti < snap->timeline->trackCount(); ++ti) {
                const Track* t = snap->timeline->track(ti);
                for (size_t ci = 0; ci < t->clipCount(); ++ci)
                    sum += static_cast<uint64_t>(t->clip(ci)->timelineIn());
            }
            (void)sum;
            reads.fetch_add(1);
        }
    });
    for (int i = 0; i < 500; ++i) {
        tl->track(1)->clip(0)->setTimelineIn(i);
        pub.markDirty(SnapshotPublisher::DirtyScope::ActiveSequence);
        pub.publish();
    }
    stop = true;
    render.join();
    EXPECT_GT(reads.load(), 0u);
    EXPECT_EQ(pub.latest()->timeline->track(1)->clip(0)->timelineIn(), 499);
}

// Publishing happens on the UI thread after edits, so its cost bounds how
// heavy a drag feels. Generous ceiling; the printed time is what to watch.
TEST(ModelSnapshot, PublishCostFor1000Clips)
{
    Timeline tl;
    std::vector<Track*> tracks;
    for (int i = 0; i < 8; ++i) tracks.push_back(tl.addVideoTrack());
    const auto makers = everyClipType();
    for (int i = 0; i < 1000; ++i) {
        auto clip = makers[static_cast<size_t>(i) % makers.size()]();
        if (clip->isAudio() || clip->isCaption()) clip = std::make_unique<VideoClip>();
        clip->addMask(OpacityMask{});
        clip->positionX().addKeyframe(0, 0.0f);
        clip->positionX().addKeyframe(kSec, 100.0f);
        place(tracks[static_cast<size_t>(i) % tracks.size()], std::move(clip),
              static_cast<int64_t>(i / 8) * kSec, kSec);
    }

    SnapshotPublisher pub;
    pub.bind(nullptr, &tl);
    ASSERT_TRUE(pub.publish());
    double worst = 0.0;
    for (int i = 0; i < 10; ++i) {
        pub.markDirty(SnapshotPublisher::DirtyScope::ActiveSequence);
        ASSERT_TRUE(pub.publish());
        worst = std::max(worst, pub.lastPublishStats().cloneMs);
    }
    EXPECT_EQ(pub.lastPublishStats().clipsCloned, 1000u);
    std::printf("[ModelSnapshot] publish of 1000 clips: worst %.2f ms\n", worst);
    EXPECT_LT(worst, 100.0);
}

} // namespace
} // namespace rt
