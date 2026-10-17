/*
 * test_edit_gestures.cpp — UI edits recorded through the EditSession.
 *
 * With an EditSession attached to the stack (as the app does):
 *   - a TimelinePanel edit (Paste Attributes → executeCommand) records ONE
 *     step that undoes/redoes the sequence exactly, keeping clip pointers;
 *   - an Effect Controls widget edit bracketed by the panel
 *     (EditBracketHost) records one step named by the widget's own commit,
 *     and an empty bracket records nothing.
 */

#include <gtest/gtest.h>

#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "edit/EditSession.h"
#include "effects/Effect.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/timeline/TimelinePanel.h"
#include "project/RenderStateHash.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QTimer>

namespace {

void ensureApp()
{
    static int argc = 1;
    static const char* argv[] = {"test_edit_gestures"};
    if (!QApplication::instance())
        new QApplication(argc, const_cast<char**>(argv));
}

struct Session
{
    rt::Timeline timeline;
    rt::CommandStack stack;
    rt::EditSession edits{stack};

    Session()
    {
        ensureApp();
        edits.setSequenceResolver([this](uint64_t id) -> rt::Timeline* {
            return id == timeline.id() ? &timeline : nullptr;
        });
    }
};

} // namespace

TEST(EditGestures, TimelinePanelEditIsOneExactUndoStep)
{
    Session s;
    rt::Track* v1 = s.timeline.addVideoTrack("V1");
    auto src = std::make_unique<rt::VideoClip>();
    src->setDuration(48000);
    src->effects().addEffect(rt::createEffect(rt::EffectType::Blur));
    src->opacity().addKeyframe(0, 0.5f);
    const uint64_t srcId = v1->addClip(std::move(src))->id();
    auto dst = std::make_unique<rt::VideoClip>();
    dst->setTimelineIn(96000);
    dst->setDuration(48000);
    rt::Clip* dstPtr = v1->addClip(std::move(dst));
    const uint64_t dstId = dstPtr->id();

    size_t trackIndex = 0;
    for (size_t i = 0; i < s.timeline.trackCount(); ++i)
        if (s.timeline.track(i) == v1) trackIndex = i;

    rt::TimelinePanel panel;
    panel.setCommandStack(&s.stack);
    panel.setTimeline(&s.timeline);

    panel.selection().selectClip({trackIndex, srcId});
    panel.copyAttributesFromSelection();
    panel.selection().selectClip({trackIndex, dstId});

    const uint64_t before = rt::hashTimelineState(s.timeline);
    QTimer::singleShot(0, []() {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
            dialog->accept();
    });
    panel.showPasteAttributesDialog();
    const uint64_t after = rt::hashTimelineState(s.timeline);
    ASSERT_NE(after, before) << "paste changed nothing";
    ASSERT_EQ(s.stack.undoCount(), 1u);

    // Undo must remove the pasted opacity keyframe too (the old hand-written
    // undo only reset default values and left it behind).
    ASSERT_TRUE(s.stack.undo());
    EXPECT_EQ(rt::hashTimelineState(s.timeline), before);
    EXPECT_EQ(v1->clip(v1->findClipIndexById(dstId)), dstPtr) << "clip object survives undo";
    ASSERT_TRUE(s.stack.redo());
    EXPECT_EQ(rt::hashTimelineState(s.timeline), after);
    EXPECT_EQ(v1->clip(v1->findClipIndexById(dstId)), dstPtr);
}

TEST(EditGestures, EffectControlsWidgetEditIsOneNamedStep)
{
    Session s;
    rt::Track* v1 = s.timeline.addVideoTrack("V1");
    auto clip = std::make_unique<rt::VideoClip>();
    clip->setDuration(48000);
    rt::Clip* c = v1->addClip(std::move(clip));

    rt::EffectControlsPanel panel;
    panel.setCommandStack(&s.stack);
    panel.setTimeline(&s.timeline);
    panel.setClip(c, v1);

    const uint64_t before = rt::hashTimelineState(s.timeline);
    panel.beginWidgetEdit();
    for (int i = 1; i <= 10; ++i)                 // a scrub: many live writes
        c->opacity().writeValue(0, 1.0f - 0.05f * static_cast<float>(i));
    s.stack.pushWithoutExecute(std::make_unique<rt::LambdaCommand>(
        "Set Opacity", [] {}, [] {}));            // the widget's own commit
    panel.endWidgetEdit();

    ASSERT_EQ(s.stack.undoCount(), 1u);
    EXPECT_EQ(s.stack.undoDescription(), "Set Opacity");
    ASSERT_TRUE(s.stack.undo());
    EXPECT_EQ(rt::hashTimelineState(s.timeline), before);

    // A bracket that changed nothing records nothing.
    panel.beginWidgetEdit();
    panel.endWidgetEdit();
    EXPECT_EQ(s.stack.undoCount(), 0u);
}

TEST(EditGestures, PropertiesEditRecordsExactUndo)
{
    Session s;
    rt::Track* v1 = s.timeline.addVideoTrack("V1");
    auto clip = std::make_unique<rt::VideoClip>();
    clip->setDuration(48000);
    clip->setLabel("Before");
    rt::Clip* c = v1->addClip(std::move(clip));

    rt::PropertiesPanel panel;
    panel.setCommandStack(&s.stack);
    panel.setTimeline(&s.timeline);
    panel.setClip(c, v1);

    const uint64_t before = rt::hashTimelineState(s.timeline);
    panel.labelEdit()->setText(QStringLiteral("After"));
    emit panel.labelEdit()->editingFinished();
    ASSERT_EQ(c->label(), "After");
    ASSERT_EQ(s.stack.undoCount(), 1u);
    EXPECT_EQ(s.stack.undoDescription(), "Change label");

    ASSERT_TRUE(s.stack.undo());
    EXPECT_EQ(c->label(), "Before");
    EXPECT_EQ(rt::hashTimelineState(s.timeline), before);
    ASSERT_TRUE(s.stack.redo());
    EXPECT_EQ(c->label(), "After");
}
