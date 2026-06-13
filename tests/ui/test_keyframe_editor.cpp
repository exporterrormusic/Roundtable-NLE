/*
 * test_keyframe_editor.cpp — Tests for Step 21: Keyframe Editor / Graph Editor
 *
 * Tests KeyframeEditor widget: clip binding, curve display, selection,
 * keyframe operations, clipboard, view management, coordinate transforms.
 */

#include <gtest/gtest.h>

#include "panels/effects/KeyframeEditor.h"

#include "timeline/Clip.h"
#include "timeline/VideoClip.h"
#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"
#include "command/CommandStack.h"
#include "command/commands/KeyframeCmds.h"

#include <QApplication>
#include <QSignalSpy>
#include <QTest>

#include <cmath>
#include <memory>

// ═══════════════════════════════════════════════════════════════════════════
//  QApplication fixture
// ═══════════════════════════════════════════════════════════════════════════

static int   s_argc = 1;
static char  s_arg0[] = "test_keyframe_editor";
static char* s_argv[] = { s_arg0, nullptr };

class KeyframeEditorTestEnv : public ::testing::Environment
{
public:
    void SetUp() override
    {
        if (!QApplication::instance())
            m_app = new QApplication(s_argc, s_argv);
    }
    void TearDown() override {}
private:
    QApplication* m_app{nullptr};
};

static auto* g_env = ::testing::AddGlobalTestEnvironment(new KeyframeEditorTestEnv);

// ═══════════════════════════════════════════════════════════════════════════
//  Helpers
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int64_t TPS = 48000; // TICKS_PER_SECOND

class KeyframeEditorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_clip = std::make_unique<rt::VideoClip>();
        m_clip->setDuration(TPS * 10); // 10 seconds
        // Tracks start empty; add initial keyframes at t=0 to simulate animation-enabled state
        m_clip->opacity().addKeyframe(0, 1.0f);
        m_clip->positionX().addKeyframe(0, 0.0f);
        m_clip->positionY().addKeyframe(0, 0.0f);
        m_clip->scaleX().addKeyframe(0, 1.0f);
        m_clip->scaleY().addKeyframe(0, 1.0f);
        m_clip->rotation().addKeyframe(0, 0.0f);
        m_stack = std::make_unique<rt::CommandStack>();

        m_editor = std::make_unique<rt::KeyframeEditor>();
        m_editor->setCommandStack(m_stack.get());
        m_editor->resize(800, 400);
        m_editor->show();
        QApplication::processEvents();
    }

    void TearDown() override
    {
        m_editor.reset();
        m_clip.reset();
        m_stack.reset();
    }

    std::unique_ptr<rt::VideoClip>       m_clip;
    std::unique_ptr<rt::CommandStack>     m_stack;
    std::unique_ptr<rt::KeyframeEditor>   m_editor;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Construction
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KeyframeEditorTest, DefaultConstruction)
{
    rt::KeyframeEditor editor;
    EXPECT_EQ(editor.clip(), nullptr);
    EXPECT_EQ(editor.commandStack(), nullptr);
    EXPECT_EQ(editor.curveCount(), 0);
    EXPECT_TRUE(editor.selectedKeys().empty());
    EXPECT_FALSE(editor.isDragging());
    EXPECT_FALSE(editor.isBoxSelecting());
}

TEST_F(KeyframeEditorTest, SetCommandStack)
{
    EXPECT_EQ(m_editor->commandStack(), m_stack.get());
}

// ═══════════════════════════════════════════════════════════════════════════
//  Clip binding
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KeyframeEditorTest, BindClipPopulatesCurves)
{
    m_editor->setClip(m_clip.get());
    EXPECT_EQ(m_editor->curveCount(), 6);
    EXPECT_EQ(m_editor->curve(0).name, "Opacity");
    EXPECT_EQ(m_editor->curve(1).name, "Position X");
    EXPECT_EQ(m_editor->curve(2).name, "Position Y");
    EXPECT_EQ(m_editor->curve(3).name, "Scale X");
    EXPECT_EQ(m_editor->curve(4).name, "Scale Y");
    EXPECT_EQ(m_editor->curve(5).name, "Rotation");
}

TEST_F(KeyframeEditorTest, BindNullClipClearsCurves)
{
    m_editor->setClip(m_clip.get());
    EXPECT_EQ(m_editor->curveCount(), 6);
    m_editor->setClip(nullptr);
    EXPECT_EQ(m_editor->curveCount(), 0);
}

TEST_F(KeyframeEditorTest, CurvePointsToTrack)
{
    m_editor->setClip(m_clip.get());
    // Opacity curve should point to clip's opacity track
    EXPECT_EQ(m_editor->curve(0).track, &m_clip->opacity());
    EXPECT_EQ(m_editor->curve(1).track, &m_clip->positionX());
}

TEST_F(KeyframeEditorTest, CurveVisibility)
{
    m_editor->setClip(m_clip.get());
    EXPECT_TRUE(m_editor->curve(0).visible);
    m_editor->setCurveVisible(0, false);
    EXPECT_FALSE(m_editor->curve(0).visible);
    m_editor->setCurveVisible(0, true);
    EXPECT_TRUE(m_editor->curve(0).visible);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Selection
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KeyframeEditorTest, SelectKey)
{
    m_editor->setClip(m_clip.get());

    QSignalSpy spy(m_editor.get(), &rt::KeyframeEditor::selectionChanged);

    m_editor->selectKey(0, 0);
    EXPECT_EQ(m_editor->selectedKeys().size(), 1u);
    EXPECT_GE(spy.count(), 1);

    // Add to selection with shift
    m_editor->selectKey(1, 0, true);
    EXPECT_EQ(m_editor->selectedKeys().size(), 2u);
}

TEST_F(KeyframeEditorTest, ClearSelection)
{
    m_editor->setClip(m_clip.get());
    m_editor->selectKey(0, 0);
    EXPECT_FALSE(m_editor->selectedKeys().empty());
    m_editor->clearSelection();
    EXPECT_TRUE(m_editor->selectedKeys().empty());
}

TEST_F(KeyframeEditorTest, SelectAll)
{
    m_editor->setClip(m_clip.get());
    // Each track starts with 1 default keyframe, 6 tracks = 6 keyframes
    m_editor->selectAll();
    EXPECT_EQ(m_editor->selectedKeys().size(), 6u);
}

TEST_F(KeyframeEditorTest, SelectKeyWithoutAdd)
{
    m_editor->setClip(m_clip.get());
    m_editor->selectKey(0, 0);
    EXPECT_EQ(m_editor->selectedKeys().size(), 1u);
    // Selecting without add replaces
    m_editor->selectKey(1, 0);
    EXPECT_EQ(m_editor->selectedKeys().size(), 1u);
    EXPECT_EQ(m_editor->selectedKeys().begin()->curveIndex, 1);
}

TEST_F(KeyframeEditorTest, BoxSelect)
{
    m_editor->setClip(m_clip.get());
    // Default keyframes are at time 0. Add one at time 1s for posX
    m_clip->positionX().addKeyframe(TPS, 100.0f, rt::InterpMode::Linear);

    // Rebind to refresh
    m_editor->setClip(m_clip.get());

    // Box select around time 0, all values — should get all keyframes at time 0
    // keyframes at t=0 have values: 1.0 (opacity), 0.0 (posX,posY), 1.0 (scaleX,scaleY), 0.0 (rotation)
    QRectF graphBox(-1000, -2.0, 2000, 5.0);
    m_editor->boxSelect(graphBox);
    // Should select keyframes at time=0 (6 of them)
    EXPECT_EQ(m_editor->selectedKeys().size(), 6u);
}

TEST_F(KeyframeEditorTest, BoxSelectAdditive)
{
    m_editor->setClip(m_clip.get());
    m_editor->selectKey(0, 0);
    EXPECT_EQ(m_editor->selectedKeys().size(), 1u);

    // Box select with add — include time=0 area
    QRectF graphBox(-1000, -2.0, 2000, 5.0);
    m_editor->boxSelect(graphBox, true);
    EXPECT_GE(m_editor->selectedKeys().size(), 6u);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Keyframe operations
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KeyframeEditorTest, AddKeyframe)
{
    m_editor->setClip(m_clip.get());

    QSignalSpy spy(m_editor.get(), &rt::KeyframeEditor::keyframeChanged);

    // Initially 1 keyframe (default) on positionX
    EXPECT_EQ(m_clip->positionX().keyframeCount(), 1u);

    m_editor->addKeyframe(1, TPS * 2, 50.0f); // curveIndex=1 is Position X
    EXPECT_EQ(m_clip->positionX().keyframeCount(), 2u);
    EXPECT_GE(spy.count(), 1);

    // Verify through CommandStack (should be undoable)
    EXPECT_TRUE(m_stack->canUndo());
}

TEST_F(KeyframeEditorTest, AddKeyframeUndo)
{
    m_editor->setClip(m_clip.get());
    m_editor->addKeyframe(1, TPS * 2, 50.0f);
    EXPECT_EQ(m_clip->positionX().keyframeCount(), 2u);

    m_stack->undo();
    EXPECT_EQ(m_clip->positionX().keyframeCount(), 1u);
}

TEST_F(KeyframeEditorTest, DeleteSelectedKeyframes)
{
    m_editor->setClip(m_clip.get());

    // Add a keyframe
    m_clip->positionX().addKeyframe(TPS, 100.0f, rt::InterpMode::Linear);
    m_editor->setClip(m_clip.get()); // refresh

    EXPECT_EQ(m_clip->positionX().keyframeCount(), 2u);

    // Select the second keyframe (index 1 on curve 1)
    m_editor->selectKey(1, 1);
    m_editor->deleteSelectedKeyframes();
    EXPECT_EQ(m_clip->positionX().keyframeCount(), 1u);
    EXPECT_TRUE(m_editor->selectedKeys().empty());
}

TEST_F(KeyframeEditorTest, DeleteSelectedUndoable)
{
    m_editor->setClip(m_clip.get());
    m_clip->positionX().addKeyframe(TPS, 100.0f, rt::InterpMode::Linear);
    m_editor->setClip(m_clip.get());

    m_editor->selectKey(1, 1);
    m_editor->deleteSelectedKeyframes();
    EXPECT_EQ(m_clip->positionX().keyframeCount(), 1u);

    m_stack->undo();
    EXPECT_EQ(m_clip->positionX().keyframeCount(), 2u);
}

TEST_F(KeyframeEditorTest, SetInterpolationLinear)
{
    m_editor->setClip(m_clip.get());
    m_editor->selectKey(0, 0);
    m_editor->setInterpolation(0); // Linear
    EXPECT_EQ(m_clip->opacity().keyframe(0).interp, rt::InterpMode::Linear);
}

TEST_F(KeyframeEditorTest, SetInterpolationBezier)
{
    m_editor->setClip(m_clip.get());
    m_editor->selectKey(0, 0);
    m_editor->setInterpolation(1); // Bezier
    EXPECT_EQ(m_clip->opacity().keyframe(0).interp, rt::InterpMode::Bezier);
}

TEST_F(KeyframeEditorTest, SetInterpolationHold)
{
    m_editor->setClip(m_clip.get());
    m_editor->selectKey(0, 0);
    m_editor->setInterpolation(2); // Hold
    EXPECT_EQ(m_clip->opacity().keyframe(0).interp, rt::InterpMode::Hold);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Clipboard
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KeyframeEditorTest, CopyPasteKeyframes)
{
    m_editor->setClip(m_clip.get());

    // Add a unique keyframe
    m_editor->addKeyframe(1, TPS, 42.0f);
    // Select it (keyframe index 1 on curve 1)
    m_editor->selectKey(1, 1);

    m_editor->copySelectedKeyframes();
    EXPECT_TRUE(m_editor->hasClipboardData());

    // Paste at 3 seconds
    m_editor->pasteKeyframes(TPS * 3);
    EXPECT_EQ(m_clip->positionX().keyframeCount(), 3u);

    // Verify pasted keyframe
    bool found = false;
    for (size_t i = 0; i < m_clip->positionX().keyframeCount(); ++i) {
        if (m_clip->positionX().keyframe(i).time == TPS * 3) {
            EXPECT_FLOAT_EQ(m_clip->positionX().keyframe(i).value, 42.0f);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(KeyframeEditorTest, CopyEmptySelectionIsNoOp)
{
    m_editor->setClip(m_clip.get());
    m_editor->clearSelection();
    m_editor->copySelectedKeyframes();
    EXPECT_FALSE(m_editor->hasClipboardData());
}

TEST_F(KeyframeEditorTest, PasteEmptyClipboardIsNoOp)
{
    m_editor->setClip(m_clip.get());
    size_t before = m_clip->positionX().keyframeCount();
    m_editor->pasteKeyframes(TPS * 5);
    EXPECT_EQ(m_clip->positionX().keyframeCount(), before);
}

TEST_F(KeyframeEditorTest, PasteSelectsPasted)
{
    m_editor->setClip(m_clip.get());
    m_editor->addKeyframe(1, TPS, 10.0f);
    m_editor->selectKey(1, 1);
    m_editor->copySelectedKeyframes();
    m_editor->pasteKeyframes(TPS * 5);
    // Pasted keyframes should be selected
    EXPECT_FALSE(m_editor->selectedKeys().empty());
}

// ═══════════════════════════════════════════════════════════════════════════
//  View management
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KeyframeEditorTest, SetViewRange)
{
    QSignalSpy spy(m_editor.get(), &rt::KeyframeEditor::viewChanged);
    m_editor->setViewRange(0, 100000, -5.0, 5.0);
    EXPECT_DOUBLE_EQ(m_editor->viewTimeMin(), 0);
    EXPECT_DOUBLE_EQ(m_editor->viewTimeMax(), 100000);
    EXPECT_DOUBLE_EQ(m_editor->viewValueMin(), -5.0);
    EXPECT_DOUBLE_EQ(m_editor->viewValueMax(), 5.0);
    EXPECT_GE(spy.count(), 1);
}

TEST_F(KeyframeEditorTest, FitViewToAll)
{
    m_editor->setClip(m_clip.get());
    m_clip->positionX().addKeyframe(TPS * 5, 200.0f);
    m_editor->setClip(m_clip.get());

    m_editor->fitViewToAll();
    // View should encompass time 0 to 5s and values 0 to 200
    EXPECT_LE(m_editor->viewTimeMin(), 0);
    EXPECT_GE(m_editor->viewTimeMax(), TPS * 5);
}

TEST_F(KeyframeEditorTest, FitViewToSelection)
{
    m_editor->setClip(m_clip.get());
    m_clip->positionX().addKeyframe(TPS * 3, 150.0f);
    m_editor->setClip(m_clip.get());

    m_editor->selectKey(1, 1); // The keyframe at 3s
    m_editor->fitViewToSelection();

    // View should center around time=3s, value=150
    double midTime = (m_editor->viewTimeMin() + m_editor->viewTimeMax()) / 2.0;
    EXPECT_NEAR(midTime, TPS * 3, TPS * 2); // Within 2s tolerance
}

// ═══════════════════════════════════════════════════════════════════════════
//  Coordinate transforms
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KeyframeEditorTest, GraphToPixelRoundTrip)
{
    m_editor->setViewRange(0, TPS * 10, -1.0, 1.0);

    double testTime  = TPS * 5;
    double testValue = 0.5;

    QPointF px = m_editor->graphToPixel(testTime, testValue);
    QPointF gp = m_editor->pixelToGraph(px.x(), px.y());

    EXPECT_NEAR(gp.x(), testTime, 1.0);
    EXPECT_NEAR(gp.y(), testValue, 0.01);
}

TEST_F(KeyframeEditorTest, GraphToPixelCorners)
{
    m_editor->setViewRange(0, TPS * 10, 0.0, 100.0);

    // Top-left of plot area should be (timeMin, valueMax)
    QPointF topLeft = m_editor->graphToPixel(0, 100.0);
    EXPECT_NEAR(topLeft.x(), 50, 1);  // kMarginLeft = 50
    EXPECT_NEAR(topLeft.y(), 10, 1);  // kMarginTop = 10
}

TEST_F(KeyframeEditorTest, PixelToGraphMinCorner)
{
    m_editor->setViewRange(1000, 5000, -2.0, 2.0);

    // Pixel at bottom-left margin corner should map to (timeMin, valueMin)
    QPointF gp = m_editor->pixelToGraph(50, 400 - 25); // (kMarginLeft, height - kMarginBottom)
    EXPECT_NEAR(gp.x(), 1000, 1.0);
    EXPECT_NEAR(gp.y(), -2.0, 0.01);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Signals
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KeyframeEditorTest, KeyframeChangedSignal)
{
    m_editor->setClip(m_clip.get());
    QSignalSpy spy(m_editor.get(), &rt::KeyframeEditor::keyframeChanged);
    m_editor->addKeyframe(0, TPS, 0.5f);
    EXPECT_GE(spy.count(), 1);
}

TEST_F(KeyframeEditorTest, SelectionChangedSignal)
{
    m_editor->setClip(m_clip.get());
    QSignalSpy spy(m_editor.get(), &rt::KeyframeEditor::selectionChanged);
    m_editor->selectKey(0, 0);
    EXPECT_GE(spy.count(), 1);
}

TEST_F(KeyframeEditorTest, ViewChangedSignal)
{
    QSignalSpy spy(m_editor.get(), &rt::KeyframeEditor::viewChanged);
    m_editor->setViewRange(0, 1000, 0, 1);
    EXPECT_GE(spy.count(), 1);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Operations without CommandStack
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KeyframeEditorTest, AddKeyframeWithoutStack)
{
    rt::KeyframeEditor editor;
    auto clip = std::make_unique<rt::VideoClip>();
    editor.setClip(clip.get());
    // No command stack set
    editor.addKeyframe(1, TPS, 99.0f);
    EXPECT_EQ(clip->positionX().keyframeCount(), 2u);
    EXPECT_FLOAT_EQ(clip->positionX().keyframe(1).value, 99.0f);
}

TEST_F(KeyframeEditorTest, DeleteWithoutStack)
{
    rt::KeyframeEditor editor;
    auto clip = std::make_unique<rt::VideoClip>();
    clip->positionX().addKeyframe(TPS, 50.0f);
    editor.setClip(clip.get());

    editor.selectKey(1, 1);
    editor.deleteSelectedKeyframes();
    EXPECT_EQ(clip->positionX().keyframeCount(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Edge cases
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(KeyframeEditorTest, AddKeyframeInvalidCurve)
{
    m_editor->setClip(m_clip.get());
    size_t before = m_clip->positionX().keyframeCount();
    m_editor->addKeyframe(99, TPS, 10.0f); // Invalid curve index
    // Should not crash, nothing changes
    EXPECT_EQ(m_clip->positionX().keyframeCount(), before);
}

TEST_F(KeyframeEditorTest, SetInterpolationNoSelection)
{
    m_editor->setClip(m_clip.get());
    m_editor->clearSelection();
    // Should not crash
    m_editor->setInterpolation(1);
}

TEST_F(KeyframeEditorTest, DeleteEmptySelection)
{
    m_editor->setClip(m_clip.get());
    m_editor->clearSelection();
    // Should not crash
    m_editor->deleteSelectedKeyframes();
}

TEST_F(KeyframeEditorTest, OperationsWithoutClip)
{
    // No clip set — all operations should be no-ops
    m_editor->addKeyframe(0, TPS, 1.0f);
    m_editor->selectAll();
    m_editor->deleteSelectedKeyframes();
    m_editor->copySelectedKeyframes();
    m_editor->pasteKeyframes(0);
    m_editor->fitViewToAll();
    // Simply should not crash
}

TEST_F(KeyframeEditorTest, CopyPastePreservesInterpolation)
{
    m_editor->setClip(m_clip.get());

    // Add bezier keyframe
    m_clip->positionX().addKeyframe(TPS, 100.0f, rt::InterpMode::Bezier);
    m_editor->setClip(m_clip.get());

    m_editor->selectKey(1, 1);
    m_editor->copySelectedKeyframes();
    m_editor->pasteKeyframes(TPS * 5);

    // Find pasted keyframe and check interpolation
    bool found = false;
    for (size_t i = 0; i < m_clip->positionX().keyframeCount(); ++i) {
        if (m_clip->positionX().keyframe(i).time == TPS * 5) {
            EXPECT_EQ(m_clip->positionX().keyframe(i).interp, rt::InterpMode::Bezier);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(KeyframeEditorTest, MultiSelectDelete)
{
    m_editor->setClip(m_clip.get());
    // Add 3 keyframes to positionX
    m_clip->positionX().addKeyframe(TPS * 1, 10.0f);
    m_clip->positionX().addKeyframe(TPS * 2, 20.0f);
    m_clip->positionX().addKeyframe(TPS * 3, 30.0f);
    m_editor->setClip(m_clip.get());

    EXPECT_EQ(m_clip->positionX().keyframeCount(), 4u); // default + 3

    // Select indices 1 and 2
    m_editor->selectKey(1, 1);
    m_editor->selectKey(1, 2, true);
    EXPECT_EQ(m_editor->selectedKeys().size(), 2u);

    m_editor->deleteSelectedKeyframes();
    EXPECT_EQ(m_clip->positionX().keyframeCount(), 2u); // default + 1 remaining
}

TEST_F(KeyframeEditorTest, CurveColorsAssigned)
{
    m_editor->setClip(m_clip.get());
    // Each curve should have a distinct color
    for (int i = 0; i < m_editor->curveCount(); ++i)
        EXPECT_TRUE(m_editor->curve(i).color.isValid());

    // At least first two should be different
    EXPECT_NE(m_editor->curve(0).color, m_editor->curve(1).color);
}

TEST_F(KeyframeEditorTest, DefaultViewRange)
{
    // Default view: 0 to 5 seconds, -1 to 2
    EXPECT_DOUBLE_EQ(m_editor->viewTimeMin(), 0.0);
    EXPECT_DOUBLE_EQ(m_editor->viewTimeMax(), TPS * 5.0);
    EXPECT_DOUBLE_EQ(m_editor->viewValueMin(), -1.0);
    EXPECT_DOUBLE_EQ(m_editor->viewValueMax(), 2.0);
}

TEST_F(KeyframeEditorTest, PaintDoesNotCrash)
{
    // Paint without clip
    m_editor->repaint();

    // Paint with clip
    m_editor->setClip(m_clip.get());
    m_clip->positionX().addKeyframe(TPS, 50.0f, rt::InterpMode::Linear);
    m_clip->positionX().addKeyframe(TPS * 2, 100.0f, rt::InterpMode::Bezier);
    m_clip->opacity().addKeyframe(TPS * 3, 0.5f, rt::InterpMode::Hold);
    m_editor->setClip(m_clip.get());
    m_editor->selectKey(1, 0);
    m_editor->repaint();
    QApplication::processEvents();
    // If we get here without crash, the test passed
}
