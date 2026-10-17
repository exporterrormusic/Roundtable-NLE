/*
 * test_edit_session.cpp — undo recorded from before/after copies.
 *
 * EditSession brackets an edit, diffs the sequence, and records one step that
 * undoes/redoes in place. The contract under test:
 *   - undo of any edit restores the sequence exactly (hashTimelineState), and
 *     redo restores the edited state — for every EditOperations edit;
 *   - surviving clips, effects, graphic layers and keyframe tracks keep their
 *     addresses across undo/redo (panels hold raw pointers to them);
 *   - cancel() restores; nested gestures join; empty gestures record nothing;
 *     legacy commands inside a gesture are absorbed, not double-recorded;
 *   - a step stores only the clips that changed;
 *   - copies used for snapshots and undo keep effect and layer ids.
 */

#include <gtest/gtest.h>

#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/TrackCommands.h"
#include "edit/EditSession.h"
#include "edit/ModelDelta.h"
#include "effects/Effect.h"
#include "project/RenderStateHash.h"
#include "timeline/AudioClip.h"
#include "timeline/EditOperations.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/ImageClip.h"
#include "timeline/SpineClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>

namespace rt {
namespace {

constexpr int64_t kSec = 48000;

struct Fixture
{
    Timeline tl;
    CommandStack stack;
    EditSession session{stack};
    size_t v1 = 0, v2 = 0, a1 = 0;
    uint64_t videoId = 0, imageId = 0, graphicId = 0, spineId = 0, audioId = 0;

    Fixture()
    {
        session.setSequenceResolver([this](uint64_t id) -> Timeline* {
            return id == tl.id() ? &tl : nullptr;
        });
        Track* tv1 = tl.addVideoTrack("V1");
        Track* tv2 = tl.addVideoTrack("V2");
        Track* ta1 = tl.addAudioTrack("A1");
        for (size_t i = 0; i < tl.trackCount(); ++i) {
            if (tl.track(i) == tv1) v1 = i;
            if (tl.track(i) == tv2) v2 = i;
            if (tl.track(i) == ta1) a1 = i;
        }

        auto video = std::make_unique<VideoClip>();
        video->setDuration(kSec);
        videoId = tv1->addClip(std::move(video))->id();

        auto image = std::make_unique<ImageClip>();
        image->setTimelineIn(kSec);
        image->setDuration(kSec);
        auto blur = createEffect(EffectType::Blur);
        blur->addMask(OpacityMask{});
        image->effects().addEffect(std::move(blur));
        image->addMask(OpacityMask{});
        imageId = tv1->addClip(std::move(image))->id();

        auto graphic = std::make_unique<GraphicClip>();
        graphic->setTimelineIn(2 * kSec);
        graphic->setDuration(kSec);
        graphic->addTextLayer("Hello");
        graphic->addShapeLayer();
        graphicId = tv1->addClip(std::move(graphic))->id();

        auto spine = std::make_unique<SpineClip>();
        spine->setDuration(2 * kSec);
        spineId = tv2->addClip(std::move(spine))->id();

        auto audio = std::make_unique<AudioClip>();
        audio->setDuration(3 * kSec);
        audioId = ta1->addClip(std::move(audio))->id();

        Transition t;
        t.duration = kSec / 4;
        t.leftClipId = videoId;
        t.rightClipId = imageId;
        t.editPointTick = kSec;
        tv1->addTransition(t);
    }

    Clip* clip(uint64_t id)
    {
        for (size_t ti = 0; ti < tl.trackCount(); ++ti) {
            Track* t = tl.track(ti);
            const size_t ci = t->findClipIndexById(id);
            if (ci < t->clipCount()) return t->clip(ci);
        }
        return nullptr;
    }

    uint64_t hash() const { return hashTimelineState(tl); }
};

SelectionSet selectionOf(size_t track, uint64_t id)
{
    SelectionSet s;
    s.selectClip(ClipRef{track, id});
    return s;
}

// ── Copies keep nested ids ─────────────────────────────────────────────────

TEST(EditSession, SnapshotCopyKeepsEffectAndLayerIds)
{
    Fixture f;
    const Clip* image = f.clip(f.imageId);
    const auto copy = image->cloneForSnapshot();
    ASSERT_EQ(copy->effects().effectCount(), 1u);
    EXPECT_EQ(copy->effects().effect(0).id(), image->effects().effect(0).id());
    EXPECT_EQ(copy->id(), image->id());

    const auto* graphic = static_cast<const GraphicClip*>(f.clip(f.graphicId));
    const auto gcopy = graphic->cloneForSnapshot();
    const auto& a = graphic->layers();
    const auto& b = static_cast<const GraphicClip&>(*gcopy).layers();
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i)
        EXPECT_EQ(a[i]->layerId(), b[i]->layerId());

    // Duplicate semantics are unchanged: fresh ids everywhere.
    const auto dup = image->clone();
    EXPECT_NE(dup->id(), image->id());
    EXPECT_NE(dup->effects().effect(0).id(), image->effects().effect(0).id());
}

// ── Every edit operation undoes and redoes exactly ─────────────────────────

TEST(EditSession, EveryEditOperationUndoesAndRedoesExactly)
{
    using Op = std::function<std::unique_ptr<Command>(Fixture&)>;
    const std::vector<std::pair<const char*, Op>> ops = {
        {"split", [](Fixture& f) { return EditOperations::splitClip(f.tl, f.v1, f.videoId, kSec / 2); }},
        {"trim", [](Fixture& f) { return EditOperations::trimClip(f.tl, f.v1, f.imageId, ClipEdge::Tail, kSec + kSec / 2); }},
        {"roll", [](Fixture& f) { return EditOperations::rollingEdit(f.tl, f.v1, f.videoId, f.imageId, kSec - kSec / 10); }},
        {"ripple trim", [](Fixture& f) { return EditOperations::rippleTrim(f.tl, f.v1, f.imageId, ClipEdge::Tail, kSec + kSec / 3); }},
        {"ripple delete", [](Fixture& f) { return EditOperations::rippleDelete(f.tl, selectionOf(f.v1, f.imageId)); }},
        {"delete", [](Fixture& f) { return EditOperations::deleteSelection(f.tl, selectionOf(f.v1, f.graphicId)); }},
        {"move", [](Fixture& f) { return EditOperations::moveClip(f.tl, f.v1, f.graphicId, 6 * kSec); }},
        {"move to track", [](Fixture& f) { return EditOperations::moveClipToTrack(f.tl, f.v1, f.v2, f.graphicId, 7 * kSec); }},
        {"slip", [](Fixture& f) { return EditOperations::slipClip(f.tl, f.v1, f.videoId, kSec / 10); }},
        {"slide", [](Fixture& f) { return EditOperations::slideClip(f.tl, f.v1, f.imageId, kSec / 10); }},
        {"close all gaps", [](Fixture& f) {
            (void)EditOperations::moveClip(f.tl, f.v1, f.graphicId, 9 * kSec)->execute();
            return EditOperations::closeAllGaps(f.tl); }},
        {"split all", [](Fixture& f) { return EditOperations::splitAllAtPlayhead(f.tl, kSec / 2); }},
    };

    for (const auto& [name, op] : ops) {
        SCOPED_TRACE(name);
        Fixture f;
        auto cmd = op(f);
        ASSERT_NE(cmd, nullptr);
        const uint64_t before = f.hash();
        ASSERT_TRUE(f.session.execute(f.tl, std::move(cmd)));
        const uint64_t after = f.hash();
        ASSERT_NE(after, before) << "the edit changed nothing";
        ASSERT_EQ(f.stack.undoCount(), 1u);

        ASSERT_TRUE(f.stack.undo());
        EXPECT_EQ(f.hash(), before);
        ASSERT_TRUE(f.stack.redo());
        EXPECT_EQ(f.hash(), after);
        ASSERT_TRUE(f.stack.undo());
        EXPECT_EQ(f.hash(), before);
    }
}

// ── Pointers survive ───────────────────────────────────────────────────────

TEST(EditSession, PointersSurviveUndoAndRedo)
{
    Fixture f;
    Clip* image = f.clip(f.imageId);
    Effect* blur = &image->effects().effect(0);
    KeyframeTrack<float>* opacity = &image->opacity();
    KeyframeTrack<float>* blurRadius = &blur->param(0).track;
    auto* graphic = static_cast<GraphicClip*>(f.clip(f.graphicId));
    GraphicLayer* layer = graphic->layers()[0].get();
    Track* track = f.tl.track(f.v1);

    const uint64_t before = f.hash();
    ASSERT_TRUE(f.session.edit("Tweak", f.tl, [&] {
        opacity->addKeyframe(0, 0.3f);
        blurRadius->addKeyframe(0, 42.0f);
        blur->setEnabled(false);
        static_cast<TextLayer*>(layer)->setText("Changed");
        image->setTimelineIn(image->timelineIn() + kSec / 10);
    }));
    const uint64_t after = f.hash();

    for (int round = 0; round < 2; ++round) {
        ASSERT_TRUE(f.stack.undo());
        EXPECT_EQ(f.hash(), before);
        EXPECT_EQ(f.clip(f.imageId), image);
        EXPECT_EQ(&image->effects().effect(0), blur);
        EXPECT_EQ(&image->opacity(), opacity);
        EXPECT_EQ(&blur->param(0).track, blurRadius);
        EXPECT_EQ(graphic->layers()[0].get(), layer);
        EXPECT_EQ(f.tl.track(f.v1), track);
        EXPECT_TRUE(blur->isEnabled());
        EXPECT_EQ(static_cast<TextLayer*>(layer)->text(), "Hello");

        ASSERT_TRUE(f.stack.redo());
        EXPECT_EQ(f.hash(), after);
        EXPECT_EQ(f.clip(f.imageId), image);
        EXPECT_FALSE(blur->isEnabled());
        EXPECT_EQ(static_cast<TextLayer*>(layer)->text(), "Changed");
    }
}

// ── Structure changes ──────────────────────────────────────────────────────

TEST(EditSession, TrackAddRemoveAndCrossTrackMoveUndo)
{
    Fixture f;
    const uint64_t before = f.hash();
    const size_t tracksBefore = f.tl.trackCount();
    ASSERT_TRUE(f.session.edit("Restructure", f.tl, [&] {
        // Move the spine clip from V2 to V1's end, then remove V2 and add V3.
        auto moved = f.tl.track(f.v2)->removeClipById(f.spineId);
        moved->setTimelineIn(10 * kSec);
        f.tl.track(f.v1)->addClip(std::move(moved));
        f.tl.removeTrack(f.v2);
        f.tl.addVideoTrack("V3");
    }));
    EXPECT_EQ(f.tl.trackCount(), tracksBefore);
    ASSERT_TRUE(f.stack.undo());
    EXPECT_EQ(f.hash(), before);
    EXPECT_EQ(f.tl.track(f.v2)->name(), "V2");
    EXPECT_LT(f.tl.track(f.v2)->findClipIndexById(f.spineId), f.tl.track(f.v2)->clipCount());
}

TEST(EditSession, StepStoresOnlyChangedClips)
{
    Fixture f;
    Clip* audio = f.clip(f.audioId);
    ModelDelta delta;
    {
        const auto baseline = f.tl.cloneForSnapshot();
        audio->setBlendMode(4);
        delta = diffSequence(*baseline, f.tl);
    }
    EXPECT_EQ(delta.changedClipCount(), 1u);
    EXPECT_EQ(delta.clipsBefore.count(f.audioId), 1u);
    EXPECT_EQ(delta.clipsAfter.count(f.audioId), 1u);
}

// ── Gesture semantics ──────────────────────────────────────────────────────

TEST(EditSession, CancelRestoresAndRecordsNothing)
{
    Fixture f;
    const uint64_t before = f.hash();
    Clip* video = f.clip(f.videoId);
    f.session.beginGesture("Drag", f.tl);
    for (int i = 1; i <= 20; ++i) video->setTimelineIn(i * 100);
    f.tl.track(f.v1)->removeClipById(f.graphicId);
    f.session.cancel();
    EXPECT_EQ(f.hash(), before);
    EXPECT_EQ(f.clip(f.videoId), video) << "cancel keeps the live object";
    EXPECT_EQ(f.stack.undoCount(), 0u);
}

TEST(EditSession, NestedGesturesJoinAndEmptyRecordsNothing)
{
    Fixture f;
    EXPECT_TRUE(f.session.beginGesture("Outer", f.tl));
    EXPECT_FALSE(f.session.beginGesture("Inner", f.tl));
    f.clip(f.videoId)->setBlendMode(2);
    EXPECT_FALSE(f.session.commit()) << "inner commit only closes the inner level";
    EXPECT_TRUE(f.session.isOpen());
    EXPECT_TRUE(f.session.commit());
    EXPECT_EQ(f.stack.undoCount(), 1u);
    EXPECT_EQ(f.stack.undoDescription(), "Outer");

    EXPECT_FALSE(f.session.edit("Nothing", f.tl, [] {}));
    EXPECT_EQ(f.stack.undoCount(), 1u);
}

TEST(EditSession, LegacyCommandInsideGestureIsAbsorbed)
{
    Fixture f;
    const uint64_t before = f.hash();
    Clip* video = f.clip(f.videoId);
    f.session.beginGesture("Gesture", f.tl);
    f.stack.execute(std::make_unique<LambdaCommand>(
        "Legacy", [video] { video->setBlendMode(9); }, [video] { video->setBlendMode(0); }));
    f.stack.pushWithoutExecute(std::make_unique<LambdaCommand>("Legacy push", [] {}, [] {}));
    EXPECT_EQ(video->blendMode(), 9) << "the legacy command still ran";
    EXPECT_TRUE(f.session.commit());
    EXPECT_EQ(f.stack.undoCount(), 1u);
    ASSERT_TRUE(f.stack.undo());
    EXPECT_EQ(f.hash(), before);
}

TEST(EditSession, UndoIsIgnoredWhileGestureOpen)
{
    Fixture f;
    ASSERT_TRUE(f.session.edit("First", f.tl, [&] { f.clip(f.videoId)->setBlendMode(1); }));
    f.session.beginGesture("Second", f.tl);
    EXPECT_FALSE(f.stack.undo());
    f.session.cancel();
    EXPECT_TRUE(f.stack.undo());
}

TEST(EditSession, RaiiGestureCancelsWhenAbandoned)
{
    Fixture f;
    const uint64_t before = f.hash();
    {
        EditGesture g(f.session, "Scoped", f.tl);
        f.clip(f.videoId)->setBlendMode(3);
    }
    EXPECT_EQ(f.hash(), before);
    EXPECT_FALSE(f.session.isOpen());
}

TEST(EditSession, UndoAfterSequenceIsGoneDoesNothing)
{
    CommandStack stack;
    EditSession session(stack);
    auto tl = std::make_unique<Timeline>();
    tl->addVideoTrack("V1");
    Timeline* raw = tl.get();
    session.setSequenceResolver([&](uint64_t id) -> Timeline* {
        return tl && id == tl->id() ? tl.get() : nullptr;
    });
    ASSERT_TRUE(session.edit("Add", *raw, [&] { raw->addVideoTrack("V2"); }));
    tl.reset();
    EXPECT_TRUE(stack.undo()) << "the step is consumed without a sequence";
}

TEST(EditSession, CommitListenerHearsStepsAndUndo)
{
    Fixture f;
    int heard = 0;
    f.session.setCommitListener([&](uint64_t id) { EXPECT_EQ(id, f.tl.id()); ++heard; });
    ASSERT_TRUE(f.session.edit("Edit", f.tl, [&] { f.clip(f.videoId)->setBlendMode(1); }));
    ASSERT_TRUE(f.stack.undo());
    ASSERT_TRUE(f.stack.redo());
    EXPECT_EQ(heard, 3);
}

// A gesture costs one copy at begin and one diff at commit; both run on the
// UI thread once per user gesture.
// ── Object lifetime: removed clips/tracks stay alive and come back ─────────

TEST(EditSession, DeletedClipComesBackAsTheSameObject)
{
    Fixture f;
    Clip* image = f.clip(f.imageId);
    ASSERT_TRUE(f.session.execute(
        f.tl, EditOperations::deleteSelection(f.tl, selectionOf(f.v1, f.imageId))));
    EXPECT_EQ(f.clip(f.imageId), nullptr);
    // Still allocated (held by the step): reading it must be safe.
    EXPECT_EQ(image->id(), f.imageId);

    for (int round = 0; round < 3; ++round) {
        ASSERT_TRUE(f.stack.undo());
        EXPECT_EQ(f.clip(f.imageId), image) << "round " << round;
        ASSERT_TRUE(f.stack.redo());
        EXPECT_EQ(f.clip(f.imageId), nullptr);
    }
}

TEST(EditSession, UndoneAddKeepsTheObjectForRedo)
{
    Fixture f;
    Clip* added = nullptr;
    ASSERT_TRUE(f.session.edit("Add", f.tl, [&] {
        auto clip = std::make_unique<ImageClip>();
        clip->setTimelineIn(20 * kSec);
        clip->setDuration(kSec);
        added = f.tl.track(f.v2)->addClip(std::move(clip));
    }));
    const uint64_t id = added->id();
    ASSERT_TRUE(f.stack.undo());
    EXPECT_EQ(f.clip(id), nullptr);
    EXPECT_EQ(added->id(), id) << "parked, not freed";
    ASSERT_TRUE(f.stack.redo());
    EXPECT_EQ(f.clip(id), added);
}

TEST(EditSession, RemovedTrackComesBackAsTheSameObject)
{
    Fixture f;
    Track* v2 = f.tl.track(f.v2);
    const size_t before = f.tl.trackCount();
    ASSERT_TRUE(f.session.execute(
        f.tl, std::make_unique<RemoveTrackCommand>(&f.tl, f.v2)));
    EXPECT_EQ(f.tl.trackCount(), before - 1);
    ASSERT_TRUE(f.stack.undo());
    ASSERT_EQ(f.tl.trackCount(), before);
    EXPECT_EQ(f.tl.track(f.v2), v2);
    EXPECT_EQ(f.clip(f.spineId) != nullptr, true) << "its clip came back too";
}

TEST(EditSession, CancelBringsBackTheSameDeletedObject)
{
    Fixture f;
    Clip* image = f.clip(f.imageId);
    f.session.beginGesture("Drag", f.tl);
    f.session.execute(f.tl,
        EditOperations::deleteSelection(f.tl, selectionOf(f.v1, f.imageId)));
    EXPECT_EQ(f.clip(f.imageId), nullptr);
    f.session.cancel();
    EXPECT_EQ(f.clip(f.imageId), image);
}

TEST(EditSession, ExecuteTimelineEditFallsBackToActiveSequence)
{
    Fixture f;
    const uint64_t before = f.hash();
    // No active sequence: the plain stack runs it (legacy undo).
    executeTimelineEdit(&f.stack, nullptr,
        EditOperations::moveClip(f.tl, f.v1, f.graphicId, 6 * kSec));
    ASSERT_TRUE(f.stack.undo());
    EXPECT_EQ(f.hash(), before);
    f.stack.clear();

    // With one: recorded as a delta step, undone in place.
    f.session.setActiveSequence(&f.tl);
    Clip* graphic = f.clip(f.graphicId);
    executeTimelineEdit(&f.stack, nullptr,
        EditOperations::moveClip(f.tl, f.v1, f.graphicId, 6 * kSec));
    ASSERT_EQ(f.stack.undoCount(), 1u);
    ASSERT_TRUE(f.stack.undo());
    EXPECT_EQ(f.hash(), before);
    EXPECT_EQ(f.clip(f.graphicId), graphic);
}

TEST(EditSession, GestureCostFor1000Clips)
{
    Timeline tl;
    std::vector<Track*> tracks;
    for (int i = 0; i < 8; ++i) tracks.push_back(tl.addVideoTrack());
    for (int i = 0; i < 1000; ++i) {
        auto clip = std::make_unique<VideoClip>();
        clip->setTimelineIn(static_cast<int64_t>(i / 8) * kSec);
        clip->setDuration(kSec);
        clip->addMask(OpacityMask{});
        clip->positionX().addKeyframe(0, 0.0f);
        clip->positionX().addKeyframe(kSec, 100.0f);
        tracks[static_cast<size_t>(i) % tracks.size()]->addClip(std::move(clip));
    }
    CommandStack stack;
    EditSession session(stack);
    session.setSequenceResolver([&](uint64_t) { return &tl; });

    const auto t0 = std::chrono::steady_clock::now();
    session.beginGesture("Nudge", tl);
    tracks[3]->clip(10)->setTimelineIn(tracks[3]->clip(10)->timelineIn() + 1);
    ASSERT_TRUE(session.commit());
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[EditSession] gesture on 1000 clips: %.2f ms\n", ms);
    EXPECT_LT(ms, 100.0);
}

} // namespace
} // namespace rt
