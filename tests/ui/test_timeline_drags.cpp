/*
 * test_timeline_drags.cpp — timeline drags driven by real mouse events.
 *
 * Characterizes what each TimelinePanel drag does today, before its code is
 * reorganized into drag tools (rewrite Stage 9): where the clips end up, that
 * a finished drag is exactly one undo step which undoes and redoes exactly,
 * and that Esc mid-drag restores everything and records nothing.
 *
 * Geometry: 30 fps, 300 px per second (10 px per frame), scroll at 0.
 */

#include <gtest/gtest.h>

#include "command/CommandStack.h"
#include "edit/EditSession.h"
#include "panels/timeline/TimelinePanel.h"
#include "project/RenderStateHash.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"
#include "Constants.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>

#include <memory>

using namespace rt;

namespace {

constexpr int64_t kSecond = kTicksPerSecond;
constexpr int64_t kFrame  = kTicksPerSecond / 30;
constexpr double  kPxPerSecond = 300.0;

void ensureApp()
{
    static int argc = 1;
    static const char* argv[] = {"test_timeline_drags"};
    if (!QApplication::instance())
        new QApplication(argc, const_cast<char**>(argv));
}

/// A timeline, an undo stack recording through an EditSession, and a shown
/// TimelinePanel that mouse events are sent to.
struct DragRig
{
    Timeline timeline;
    CommandStack stack;
    EditSession edits{stack};
    std::unique_ptr<TimelinePanel> panel;

    DragRig()
    {
        ensureApp();
        timeline.settings().setFrameRate(30.0);
        edits.setSequenceResolver([this](uint64_t id) -> Timeline* {
            return id == timeline.id() ? &timeline : nullptr;
        });
    }

    ~DragRig() { panel.reset(); }

    Track* videoTrack(const char* name) { return timeline.addVideoTrack(name); }

    /// A clip at [in, in + duration) whose source starts at `sourceIn` and
    /// has ten seconds of media, so trims have room either way.
    Clip* addClip(Track* track, int64_t in, int64_t duration, int64_t sourceIn = kSecond)
    {
        auto clip = std::make_unique<VideoClip>();
        clip->setTimelineIn(in);
        clip->setDuration(duration);
        clip->setSourceIn(sourceIn);
        clip->setSourceDuration(10 * kSecond);
        clip->setSourceFps(30.0);
        return track->addClip(std::move(clip));
    }

    size_t indexOf(const Track* track) const
    {
        for (size_t i = 0; i < timeline.trackCount(); ++i)
            if (timeline.track(i) == track) return i;
        return SIZE_MAX;
    }

    /// Builds the panel once the timeline is set up.
    void show()
    {
        panel = std::make_unique<TimelinePanel>();
        panel->setCommandStack(&stack);
        panel->setTimeline(&timeline);
        panel->setFrameRate(30.0);   // the panel's own grid (it defaults to 60)
        panel->resize(1600, 700);
        panel->show();
        QApplication::processEvents();
        panel->layoutEngine().setPixelsPerSecond(kPxPerSecond);
        panel->layoutEngine().setScrollX(0.0);
        QApplication::processEvents();
    }

    QPointF pointAt(int64_t tick, size_t track) const
    {
        return {panel->headerWidth() + panel->layoutEngine().timeToPixelX(tick),
                static_cast<double>(panel->trackRowRect(track).center().y())};
    }

    void mouse(QEvent::Type type, QPointF pos, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QMouseEvent event(type, pos, panel->mapToGlobal(pos), Qt::LeftButton,
                          buttons, modifiers);
        QApplication::sendEvent(panel.get(), &event);
    }

    /// Press at `from`, move through intermediate points to `to`, release.
    void drag(QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
              bool releaseAtEnd = true)
    {
        mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, modifiers);
        constexpr int kSteps = 8;
        for (int i = 1; i <= kSteps; ++i) {
            const QPointF p = from + (to - from) * (static_cast<double>(i) / kSteps);
            mouse(QEvent::MouseMove, p, Qt::LeftButton, modifiers);
        }
        if (releaseAtEnd)
            mouse(QEvent::MouseButtonRelease, to, Qt::NoButton, modifiers);
        QApplication::processEvents();
    }

    void click(QPointF at, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, modifiers);
        mouse(QEvent::MouseButtonRelease, at, Qt::NoButton, modifiers);
        QApplication::processEvents();
    }

    void escape()
    {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(panel.get(), &press);
        QApplication::processEvents();
    }

    uint64_t state() const { return hashTimelineState(timeline); }

    /// A finished drag is one undo step that undoes to `before` and redoes
    /// to what the drag left.
    void expectOneExactUndoStep(uint64_t before, size_t stepsBefore = 0)
    {
        const uint64_t after = state();
        ASSERT_NE(after, before) << "the drag changed nothing";
        ASSERT_EQ(stack.undoCount(), stepsBefore + 1) << "a drag must record exactly one step";
        stack.undo();
        EXPECT_EQ(state(), before) << "undo did not restore the timeline exactly";
        stack.redo();
        EXPECT_EQ(state(), after) << "redo did not reapply the drag exactly";
    }
};

const Clip* clipById(const Timeline& timeline, uint64_t id, size_t* trackOut = nullptr)
{
    for (size_t t = 0; t < timeline.trackCount(); ++t) {
        const Track* track = timeline.track(t);
        const size_t i = track->findClipIndexById(id);
        if (i < track->clipCount()) {
            if (trackOut) *trackOut = t;
            return track->clip(i);
        }
    }
    return nullptr;
}

} // namespace

// ─── Click ───────────────────────────────────────────────────────────────────

TEST(TimelineDrags, ClickSelectsAndChangesNothing)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const Clip* clip = rig.addClip(v1, kSecond, kSecond);
    const uint64_t id = clip->id();
    rig.show();
    const uint64_t before = rig.state();

    rig.click(rig.pointAt(kSecond + kSecond / 2, rig.indexOf(v1)));

    EXPECT_EQ(rig.state(), before);
    EXPECT_EQ(rig.stack.undoCount(), 0u);
    EXPECT_TRUE(rig.panel->selection().isSelected({rig.indexOf(v1), id}));
    EXPECT_FALSE(rig.panel->isDragInProgress());
}

// ─── Clip move ───────────────────────────────────────────────────────────────

TEST(TimelineDrags, MoveAlongTheTrack)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t id = rig.addClip(v1, kSecond, kSecond)->id();
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(kSecond + kSecond / 2, t),
             rig.pointAt(kSecond + kSecond / 2 + 12 * kFrame, t));

    const Clip* moved = clipById(rig.timeline, id);
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(moved->timelineIn(), kSecond + 12 * kFrame);
    EXPECT_EQ(moved->duration(), kSecond);
    EXPECT_FALSE(rig.panel->isDragInProgress());
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, MoveToAnotherTrack)
{
    DragRig rig;
    Track* v2 = rig.videoTrack("V2");
    Track* v1 = rig.videoTrack("V1");
    const uint64_t id = rig.addClip(v1, kSecond, kSecond)->id();
    rig.show();
    const uint64_t before = rig.state();

    rig.drag(rig.pointAt(kSecond + kSecond / 2, rig.indexOf(v1)),
             rig.pointAt(kSecond + kSecond / 2, rig.indexOf(v2)));

    size_t track = SIZE_MAX;
    const Clip* moved = clipById(rig.timeline, id, &track);
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(track, rig.indexOf(v2));
    EXPECT_EQ(moved->timelineIn(), kSecond);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, GroupMoveClampsAtTimeZero)
{
    // Two selected clips dragged left past tick 0: the GROUP stops when the
    // leftmost reaches 0, so their spacing is kept (feedback-group-move-clamp).
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, kSecond, kSecond / 2)->id();
    const uint64_t b = rig.addClip(v1, 2 * kSecond, kSecond / 2)->id();
    rig.show();
    const size_t t = rig.indexOf(v1);
    rig.panel->selection().selectClip({t, a}, false);
    rig.panel->selection().selectClip({t, b}, true);
    const uint64_t before = rig.state();

    rig.drag(rig.pointAt(2 * kSecond + kSecond / 4, t),
             rig.pointAt(kSecond / 4, t));   // two seconds left

    EXPECT_EQ(clipById(rig.timeline, a)->timelineIn(), 0);
    EXPECT_EQ(clipById(rig.timeline, b)->timelineIn(), kSecond);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, EscapeDuringMoveRestoresAndRecordsNothing)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    rig.addClip(v1, kSecond, kSecond);
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(kSecond + kSecond / 2, t),
             rig.pointAt(2 * kSecond + kSecond / 2, t), Qt::NoModifier,
             /*releaseAtEnd=*/false);
    ASSERT_TRUE(rig.panel->isDragInProgress());
    ASSERT_NE(rig.state(), before) << "the move should apply live";
    rig.escape();

    EXPECT_FALSE(rig.panel->isDragInProgress());
    EXPECT_EQ(rig.state(), before);
    EXPECT_EQ(rig.stack.undoCount(), 0u);
}

// ─── Trims ───────────────────────────────────────────────────────────────────

TEST(TimelineDrags, TrimTail)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t id = rig.addClip(v1, kSecond, kSecond)->id();
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    // Grab just inside the tail, drag 9 frames right.
    rig.drag(rig.pointAt(2 * kSecond - kFrame / 4, t),
             rig.pointAt(2 * kSecond - kFrame / 4 + 9 * kFrame, t));

    const Clip* clip = clipById(rig.timeline, id);
    EXPECT_EQ(clip->timelineIn(), kSecond);
    EXPECT_EQ(clip->duration(), kSecond + 9 * kFrame);
    EXPECT_EQ(clip->sourceIn(), kSecond);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, TrimHead)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t id = rig.addClip(v1, kSecond, kSecond)->id();
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    // Grab just inside the head, drag 6 frames right (shorter clip).
    rig.drag(rig.pointAt(kSecond + kFrame / 4, t),
             rig.pointAt(kSecond + kFrame / 4 + 6 * kFrame, t));

    const Clip* clip = clipById(rig.timeline, id);
    EXPECT_EQ(clip->timelineIn(), kSecond + 6 * kFrame);
    EXPECT_EQ(clip->duration(), kSecond - 6 * kFrame);
    EXPECT_EQ(clip->sourceIn(), kSecond + 6 * kFrame);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, EscapeDuringTrimRestoresAndRecordsNothing)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    rig.addClip(v1, kSecond, kSecond);
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(2 * kSecond - kFrame / 4, t),
             rig.pointAt(2 * kSecond + 9 * kFrame, t), Qt::NoModifier,
             /*releaseAtEnd=*/false);
    ASSERT_TRUE(rig.panel->isDragInProgress());
    rig.escape();

    EXPECT_FALSE(rig.panel->isDragInProgress());
    EXPECT_EQ(rig.state(), before);
    EXPECT_EQ(rig.stack.undoCount(), 0u);
}

TEST(TimelineDrags, TrimSnapsToTheNeighboursEdge)
{
    // A tail dragged to within a few pixels of the next clip's head lands
    // exactly on it.
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, kSecond, kSecond)->id();
    rig.addClip(v1, 3 * kSecond, kSecond);
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    QPointF to = rig.pointAt(3 * kSecond, t);
    to.rx() -= 3.0;   // 0.3 frame short of the head
    rig.drag(rig.pointAt(2 * kSecond - kFrame / 4, t), to);

    EXPECT_EQ(clipById(rig.timeline, a)->timelineOut(), 3 * kSecond);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, DropAboveTheTopTrackCreatesAVideoTrack)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t id = rig.addClip(v1, kSecond, kSecond)->id();
    rig.show();
    const size_t tracksBefore = rig.timeline.trackCount();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    QPointF to = rig.pointAt(kSecond + kSecond / 2, t);
    to.setY(rig.panel->trackRowRect(t).top() + 2);   // the new-track band
    rig.drag(rig.pointAt(kSecond + kSecond / 2, t), to);

    ASSERT_EQ(rig.timeline.trackCount(), tracksBefore + 1);
    size_t track = SIZE_MAX;
    ASSERT_NE(clipById(rig.timeline, id, &track), nullptr);
    EXPECT_NE(track, rig.indexOf(v1)) << "the clip should be on the new track";
    EXPECT_EQ(rig.timeline.track(track)->type(), TrackType::Video);
    rig.expectOneExactUndoStep(before);
}

// ─── Rolling edit ────────────────────────────────────────────────────────────

TEST(TimelineDrags, RollingEditMovesTheCut)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, kSecond, kSecond)->id();
    const uint64_t b = rig.addClip(v1, 2 * kSecond, kSecond)->id();
    rig.show();
    rig.panel->setActiveTool(EditTool::Rolling);
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(2 * kSecond, t), rig.pointAt(2 * kSecond + 6 * kFrame, t));

    const Clip* left = clipById(rig.timeline, a);
    const Clip* right = clipById(rig.timeline, b);
    EXPECT_EQ(left->timelineIn(), kSecond);
    EXPECT_EQ(left->duration(), kSecond + 6 * kFrame);
    EXPECT_EQ(right->timelineIn(), 2 * kSecond + 6 * kFrame);
    EXPECT_EQ(right->duration(), kSecond - 6 * kFrame);
    EXPECT_EQ(right->sourceIn(), kSecond + 6 * kFrame);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, RollingEditRollsSelectedCutsOnOtherTracks)
{
    DragRig rig;
    Track* v2 = rig.videoTrack("V2");
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a1 = rig.addClip(v1, kSecond, kSecond)->id();
    rig.addClip(v1, 2 * kSecond, kSecond);
    const uint64_t a2 = rig.addClip(v2, kSecond, kSecond)->id();
    rig.addClip(v2, 2 * kSecond, kSecond);
    rig.show();
    const size_t t1 = rig.indexOf(v1);
    const size_t t2 = rig.indexOf(v2);
    rig.panel->selection().selectClip({t1, a1}, false);
    rig.panel->selection().selectClip({t2, a2}, true);
    rig.panel->setActiveTool(EditTool::Rolling);
    const uint64_t before = rig.state();

    rig.drag(rig.pointAt(2 * kSecond, t1), rig.pointAt(2 * kSecond - 4 * kFrame, t1));

    EXPECT_EQ(clipById(rig.timeline, a1)->timelineOut(), 2 * kSecond - 4 * kFrame);
    EXPECT_EQ(clipById(rig.timeline, a2)->timelineOut(), 2 * kSecond - 4 * kFrame);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, EscapeDuringRollRestoresAndRecordsNothing)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    rig.addClip(v1, kSecond, kSecond);
    rig.addClip(v1, 2 * kSecond, kSecond);
    rig.show();
    rig.panel->setActiveTool(EditTool::Rolling);
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(2 * kSecond, t), rig.pointAt(2 * kSecond + 6 * kFrame, t),
             Qt::NoModifier, /*releaseAtEnd=*/false);
    ASSERT_TRUE(rig.panel->isDragInProgress());
    rig.escape();

    EXPECT_EQ(rig.state(), before);
    EXPECT_EQ(rig.stack.undoCount(), 0u);
}

// ─── Slip / slide ────────────────────────────────────────────────────────────

TEST(TimelineDrags, SlipShiftsOnlyTheSource)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t id = rig.addClip(v1, kSecond, kSecond, 2 * kSecond)->id();
    rig.show();
    rig.panel->setActiveTool(EditTool::Slip);
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(kSecond + kSecond / 2, t),
             rig.pointAt(kSecond + kSecond / 2 + 5 * kFrame, t));

    const Clip* clip = clipById(rig.timeline, id);
    EXPECT_EQ(clip->timelineIn(), kSecond);
    EXPECT_EQ(clip->duration(), kSecond);
    // The source moves with the mouse (Premiere): a drag to the right shows
    // earlier source frames.
    EXPECT_EQ(clip->sourceIn(), 2 * kSecond - 5 * kFrame);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, SlideMovesTheClipBetweenItsNeighbours)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, 0, kSecond)->id();
    const uint64_t b = rig.addClip(v1, kSecond, kSecond)->id();
    const uint64_t c = rig.addClip(v1, 2 * kSecond, kSecond)->id();
    rig.show();
    rig.panel->setActiveTool(EditTool::Slide);
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(kSecond + kSecond / 2, t),
             rig.pointAt(kSecond + kSecond / 2 + 6 * kFrame, t));

    EXPECT_EQ(clipById(rig.timeline, b)->timelineIn(), kSecond + 6 * kFrame);
    EXPECT_EQ(clipById(rig.timeline, b)->duration(), kSecond);
    EXPECT_EQ(clipById(rig.timeline, a)->timelineOut(), kSecond + 6 * kFrame);
    EXPECT_EQ(clipById(rig.timeline, c)->timelineIn(), 2 * kSecond + 6 * kFrame);
    EXPECT_EQ(clipById(rig.timeline, c)->timelineOut(), 3 * kSecond);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, EscapeDuringSlipAndSlideRestoresAndRecordsNothing)
{
    for (const EditTool tool : {EditTool::Slip, EditTool::Slide}) {
        DragRig rig;
        Track* v1 = rig.videoTrack("V1");
        rig.addClip(v1, 0, kSecond);
        rig.addClip(v1, kSecond, kSecond);
        rig.addClip(v1, 2 * kSecond, kSecond);
        rig.show();
        rig.panel->setActiveTool(tool);
        const uint64_t before = rig.state();
        const size_t t = rig.indexOf(v1);

        rig.drag(rig.pointAt(kSecond + kSecond / 2, t),
                 rig.pointAt(kSecond + kSecond / 2 + 6 * kFrame, t),
                 Qt::NoModifier, /*releaseAtEnd=*/false);
        ASSERT_TRUE(rig.panel->isDragInProgress());
        rig.escape();

        EXPECT_EQ(rig.state(), before) << static_cast<int>(tool);
        EXPECT_EQ(rig.stack.undoCount(), 0u) << static_cast<int>(tool);
    }
}

// ─── Transitions ─────────────────────────────────────────────────────────────

namespace {
/// Two touching clips with a 10-frame cross dissolve on the cut at 2 s.
struct DissolveScene
{
    uint64_t a{0}, b{0};
    size_t track{0};

    explicit DissolveScene(DragRig& rig)
    {
        Track* v1 = rig.videoTrack("V1");
        a = rig.addClip(v1, kSecond, kSecond)->id();
        b = rig.addClip(v1, 2 * kSecond, kSecond)->id();
        Transition t;
        t.duration = 10 * kFrame;
        t.leftClipId = a;
        t.rightClipId = b;
        t.editPointTick = 2 * kSecond;
        v1->addTransition(t);
        rig.show();
        track = rig.indexOf(v1);
    }
};
} // namespace

TEST(TimelineDrags, TransitionEdgeTrimChangesItsDuration)
{
    DragRig rig;
    DissolveScene scene(rig);
    const uint64_t before = rig.state();

    // Start edge at 2 s - 5 frames; drag it 3 frames further left.
    rig.drag(rig.pointAt(2 * kSecond - 5 * kFrame, scene.track),
             rig.pointAt(2 * kSecond - 8 * kFrame, scene.track));

    const Track* track = rig.timeline.track(scene.track);
    ASSERT_EQ(track->transitionCount(), 1u);
    EXPECT_EQ(track->transition(0)->duration, 16 * kFrame);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, EscapeDuringTransitionTrimRestoresAndRecordsNothing)
{
    DragRig rig;
    DissolveScene scene(rig);
    const uint64_t before = rig.state();

    rig.drag(rig.pointAt(2 * kSecond - 5 * kFrame, scene.track),
             rig.pointAt(2 * kSecond - 8 * kFrame, scene.track),
             Qt::NoModifier, /*releaseAtEnd=*/false);
    ASSERT_TRUE(rig.panel->isDragInProgress());
    rig.escape();

    EXPECT_EQ(rig.state(), before);
    EXPECT_EQ(rig.stack.undoCount(), 0u);
}

TEST(TimelineDrags, TransitionHandleDragAddsATransition)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    rig.addClip(v1, kSecond, kSecond);
    rig.addClip(v1, 2 * kSecond, kSecond);
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    // The handle: the top band of the left clip's tail, inside the clip.
    QPointF from = rig.pointAt(2 * kSecond, t);
    from.rx() -= 5.0;
    from.setY(rig.panel->trackRowRect(t).top() + 6);
    rig.drag(from, from - QPointF(80.0, 0.0));

    ASSERT_EQ(rig.timeline.track(t)->transitionCount(), 1u);
    EXPECT_GT(rig.timeline.track(t)->transition(0)->duration, 0);
    rig.expectOneExactUndoStep(before);
}

// ─── Marquee ─────────────────────────────────────────────────────────────────

TEST(TimelineDrags, MarqueeSelectsTheClipsItTouches)
{
    DragRig rig;
    Track* v2 = rig.videoTrack("V2");
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, kSecond, kSecond)->id();
    const uint64_t b = rig.addClip(v2, kSecond, kSecond)->id();
    const uint64_t far = rig.addClip(v1, 4 * kSecond, kSecond)->id();
    rig.show();
    const uint64_t before = rig.state();
    const size_t t1 = rig.indexOf(v1);
    const size_t t2 = rig.indexOf(v2);

    rig.drag(rig.pointAt(3 * kSecond, t1), rig.pointAt(kSecond + kSecond / 2, t2));

    EXPECT_TRUE(rig.panel->selection().isSelected({t1, a}));
    EXPECT_TRUE(rig.panel->selection().isSelected({t2, b}));
    EXPECT_FALSE(rig.panel->selection().isSelected({t1, far}));
    EXPECT_EQ(rig.state(), before);
    EXPECT_EQ(rig.stack.undoCount(), 0u);
}

TEST(TimelineDrags, ShiftMarqueeAddsToTheSelection)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, kSecond, kSecond)->id();
    const uint64_t far = rig.addClip(v1, 4 * kSecond, kSecond)->id();
    rig.show();
    const size_t t = rig.indexOf(v1);
    rig.panel->selection().selectClip({t, far}, false);

    rig.drag(rig.pointAt(3 * kSecond, t), rig.pointAt(kSecond + kSecond / 2, t),
             Qt::ShiftModifier);

    EXPECT_TRUE(rig.panel->selection().isSelected({t, a}));
    EXPECT_TRUE(rig.panel->selection().isSelected({t, far}));
}

// ─── Interrupted drags (lost mouse capture) ──────────────────────────────────

TEST(TimelineDrags, PressDuringAnUnfinishedMovePutsTheClipBack)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    rig.addClip(v1, kSecond, kSecond);
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(kSecond + kSecond / 2, t),
             rig.pointAt(2 * kSecond + kSecond / 2, t), Qt::NoModifier,
             /*releaseAtEnd=*/false);
    ASSERT_NE(rig.state(), before);
    // The release never arrives; the next press lands on empty space.
    rig.click(rig.pointAt(6 * kSecond, t));

    EXPECT_EQ(rig.state(), before);
    EXPECT_EQ(rig.stack.undoCount(), 0u);
}

TEST(TimelineDrags, PressDuringAnUnfinishedTrimKeepsItAsOneStep)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t id = rig.addClip(v1, kSecond, kSecond)->id();
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(2 * kSecond - kFrame / 4, t),
             rig.pointAt(2 * kSecond - kFrame / 4 + 9 * kFrame, t),
             Qt::NoModifier, /*releaseAtEnd=*/false);
    rig.click(rig.pointAt(6 * kSecond, t));

    EXPECT_EQ(clipById(rig.timeline, id)->duration(), kSecond + 9 * kFrame);
    rig.expectOneExactUndoStep(before);
}

// ─── Ripple tool ─────────────────────────────────────────────────────────────

// Pinned as found: the Ripple tool trims like the Selection tool and does
// NOT shift the clips after the edit (B stays at 3 s). Reported to the owner
// as a probable bug (rewrite roadmap, Stage 9).
TEST(TimelineDrags, RippleToolTrimsTheTail)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, kSecond, kSecond)->id();
    const uint64_t b = rig.addClip(v1, 3 * kSecond, kSecond)->id();
    rig.show();
    rig.panel->setActiveTool(EditTool::Ripple);
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(2 * kSecond - kFrame / 4, t),
             rig.pointAt(2 * kSecond - kFrame / 4 + 9 * kFrame, t));

    EXPECT_EQ(clipById(rig.timeline, a)->duration(), kSecond + 9 * kFrame);
    EXPECT_EQ(clipById(rig.timeline, b)->timelineIn(), 3 * kSecond);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, TrimMovesEverySelectedClipsEdge)
{
    // Two selected clips on two tracks: trimming one tail extends both by
    // the same delta.
    DragRig rig;
    Track* v2 = rig.videoTrack("V2");
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, kSecond, kSecond)->id();
    const uint64_t b = rig.addClip(v2, kSecond, kSecond / 2)->id();
    rig.show();
    const size_t t1 = rig.indexOf(v1);
    const size_t t2 = rig.indexOf(v2);
    rig.panel->selection().selectClip({t1, a}, false);
    rig.panel->selection().selectClip({t2, b}, true);
    const uint64_t before = rig.state();

    rig.drag(rig.pointAt(2 * kSecond - kFrame / 4, t1),
             rig.pointAt(2 * kSecond - kFrame / 4 + 7 * kFrame, t1));

    EXPECT_EQ(clipById(rig.timeline, a)->duration(), kSecond + 7 * kFrame);
    EXPECT_EQ(clipById(rig.timeline, b)->duration(), kSecond / 2 + 7 * kFrame);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, CtrlTrimDoesNotSnap)
{
    // The same drag as TrimSnapsToTheNeighboursEdge, with Ctrl held: the
    // tail stops where the mouse is (frame-quantized), short of the head.
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, kSecond, kSecond)->id();
    rig.addClip(v1, 3 * kSecond, kSecond);
    rig.show();
    const size_t t = rig.indexOf(v1);

    QPointF to = rig.pointAt(3 * kSecond - kFrame, t);
    rig.drag(rig.pointAt(2 * kSecond - kFrame / 4, t), to, Qt::ControlModifier);

    EXPECT_EQ(clipById(rig.timeline, a)->timelineOut(), 3 * kSecond - kFrame);
}

TEST(TimelineDrags, PressDuringAnUnfinishedRollPutsTheCutBack)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    rig.addClip(v1, kSecond, kSecond);
    rig.addClip(v1, 2 * kSecond, kSecond);
    rig.show();
    rig.panel->setActiveTool(EditTool::Rolling);
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(2 * kSecond, t), rig.pointAt(2 * kSecond + 6 * kFrame, t),
             Qt::NoModifier, /*releaseAtEnd=*/false);
    ASSERT_NE(rig.state(), before);
    rig.click(rig.pointAt(6 * kSecond, t));

    EXPECT_EQ(rig.state(), before);
    EXPECT_EQ(rig.stack.undoCount(), 0u);
}

TEST(TimelineDrags, RollingEditCarriesTheDissolveOnTheCut)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, kSecond, kSecond)->id();
    const uint64_t b = rig.addClip(v1, 2 * kSecond, kSecond)->id();
    Transition dissolve;
    dissolve.duration = 10 * kFrame;
    dissolve.leftClipId = a;
    dissolve.rightClipId = b;
    dissolve.editPointTick = 2 * kSecond;
    v1->addTransition(dissolve);
    rig.show();
    rig.panel->setActiveTool(EditTool::Rolling);
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(2 * kSecond, t), rig.pointAt(2 * kSecond - 5 * kFrame, t));

    ASSERT_EQ(rig.timeline.track(t)->transitionCount(), 1u);
    EXPECT_EQ(rig.timeline.track(t)->transition(0)->editPointTick, 2 * kSecond - 5 * kFrame);
    EXPECT_EQ(clipById(rig.timeline, a)->timelineOut(), 2 * kSecond - 5 * kFrame);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, RollingEditStopsAtTheRightClipsSourceStart)
{
    // The right clip's media starts 4 frames before its in-point: the cut
    // can roll left by at most 4 frames.
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, kSecond, kSecond)->id();
    const uint64_t b = rig.addClip(v1, 2 * kSecond, kSecond, 4 * kFrame)->id();
    rig.show();
    rig.panel->setActiveTool(EditTool::Rolling);
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(2 * kSecond, t), rig.pointAt(2 * kSecond - 15 * kFrame, t));

    EXPECT_EQ(clipById(rig.timeline, a)->timelineOut(), 2 * kSecond - 4 * kFrame);
    EXPECT_EQ(clipById(rig.timeline, b)->sourceIn(), 0);
}

namespace {
/// The transition handle on the tail of the clip ending at `tail`.
QPointF tailHandle(DragRig& rig, int64_t tail, size_t track)
{
    QPointF p = rig.pointAt(tail, track);
    p.rx() -= 5.0;
    p.setY(rig.panel->trackRowRect(track).top() + 6);
    return p;
}
} // namespace

TEST(TimelineDrags, ClickOnATransitionHandleAddsNothing)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    rig.addClip(v1, kSecond, kSecond);
    rig.addClip(v1, 2 * kSecond, kSecond);
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.click(tailHandle(rig, 2 * kSecond, t));

    EXPECT_EQ(rig.timeline.track(t)->transitionCount(), 0u);
    EXPECT_EQ(rig.state(), before);
    EXPECT_EQ(rig.stack.undoCount(), 0u);
}

TEST(TimelineDrags, EscapeDuringATransitionHandleDragAddsNothing)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    rig.addClip(v1, kSecond, kSecond);
    rig.addClip(v1, 2 * kSecond, kSecond);
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    const QPointF from = tailHandle(rig, 2 * kSecond, t);
    rig.drag(from, from - QPointF(80.0, 0.0), Qt::NoModifier, /*releaseAtEnd=*/false);
    ASSERT_EQ(rig.timeline.track(t)->transitionCount(), 1u) << "the drag should add it live";
    rig.escape();

    EXPECT_EQ(rig.timeline.track(t)->transitionCount(), 0u);
    EXPECT_EQ(rig.state(), before);
    EXPECT_EQ(rig.stack.undoCount(), 0u);
}

TEST(TimelineDrags, FadeOutEdgeTrimFollowsTheCursor)
{
    // A fade-out on a lone clip's tail: its start edge sits at the tail minus
    // the duration; dragging it left lengthens the fade.
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t a = rig.addClip(v1, kSecond, kSecond)->id();
    Transition fade;
    fade.duration = 10 * kFrame;
    fade.leftClipId = a;
    fade.rightClipId = 0;
    fade.editPointTick = 2 * kSecond;
    v1->addTransition(fade);
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(2 * kSecond - 10 * kFrame, t),
             rig.pointAt(2 * kSecond - 14 * kFrame, t));

    ASSERT_EQ(rig.timeline.track(t)->transitionCount(), 1u);
    EXPECT_EQ(rig.timeline.track(t)->transition(0)->duration, 14 * kFrame);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, AltDragCopiesTheClip)
{
    DragRig rig;
    Track* v1 = rig.videoTrack("V1");
    const uint64_t id = rig.addClip(v1, kSecond, kSecond)->id();
    rig.show();
    const uint64_t before = rig.state();
    const size_t t = rig.indexOf(v1);

    rig.drag(rig.pointAt(kSecond + kSecond / 2, t),
             rig.pointAt(3 * kSecond + kSecond / 2, t), Qt::AltModifier);

    const Track* track = rig.timeline.track(t);
    ASSERT_EQ(track->clipCount(), 2u) << "Alt-drag should leave the original and add a copy";
    EXPECT_EQ(clipById(rig.timeline, id)->timelineIn(), kSecond);
    bool copyAtThree = false;
    for (size_t i = 0; i < track->clipCount(); ++i)
        copyAtThree |= track->clip(i)->id() != id && track->clip(i)->timelineIn() == 3 * kSecond;
    EXPECT_TRUE(copyAtThree);
    rig.expectOneExactUndoStep(before);
}

namespace {
/// A clip on V1 with a 10-frame fade-in, and an empty V2 above.
struct FadedClipScene
{
    uint64_t clip{0};
    size_t v1{0}, v2{0};

    explicit FadedClipScene(DragRig& rig)
    {
        Track* top = rig.videoTrack("V2");
        Track* bottom = rig.videoTrack("V1");
        clip = rig.addClip(bottom, kSecond, kSecond)->id();
        Transition fade;
        fade.duration = 10 * kFrame;
        fade.leftClipId = 0;
        fade.rightClipId = clip;
        fade.editPointTick = kSecond;
        bottom->addTransition(fade);
        rig.show();
        v1 = rig.indexOf(bottom);
        v2 = rig.indexOf(top);
    }
};
} // namespace

TEST(TimelineDrags, CrossTrackMoveCarriesTheClipsTransition)
{
    DragRig rig;
    FadedClipScene scene(rig);
    const uint64_t before = rig.state();

    rig.drag(rig.pointAt(kSecond + kSecond / 2, scene.v1),
             rig.pointAt(kSecond + kSecond / 2, scene.v2));

    size_t track = SIZE_MAX;
    ASSERT_NE(clipById(rig.timeline, scene.clip, &track), nullptr);
    EXPECT_EQ(track, scene.v2);
    EXPECT_EQ(rig.timeline.track(scene.v2)->transitionCount(), 1u);
    EXPECT_EQ(rig.timeline.track(scene.v1)->transitionCount(), 0u);
    rig.expectOneExactUndoStep(before);
}

TEST(TimelineDrags, EscapeAfterACrossTrackMoveRestoresTheTransition)
{
    DragRig rig;
    FadedClipScene scene(rig);
    const uint64_t before = rig.state();

    rig.drag(rig.pointAt(kSecond + kSecond / 2, scene.v1),
             rig.pointAt(kSecond + kSecond / 2, scene.v2), Qt::NoModifier,
             /*releaseAtEnd=*/false);
    ASSERT_TRUE(rig.panel->isDragInProgress());
    rig.escape();

    EXPECT_EQ(rig.state(), before);
    EXPECT_EQ(rig.timeline.track(scene.v1)->transitionCount(), 1u);
    EXPECT_EQ(rig.stack.undoCount(), 0u);
}
