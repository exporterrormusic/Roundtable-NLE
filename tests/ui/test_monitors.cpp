/*
 * test_monitors.cpp — Tests for Step 15: Source & Program Monitors
 *
 * Tests pure-logic aspects of:
 *   - MiniTimeline: coordinate mapping, clamping, in/out points, selected duration
 *   - Viewport: fit mode layout, coordinate mapping, frame display state
 *   - SourceMonitor: source region computation
 *   - ProgramMonitor: output resolution, composite callback
 *
 * These tests exercise the logic without needing Vulkan or a visible window,
 * but they DO require QApplication for widget instantiation.
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QEventLoop>
#include <QFocusEvent>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalSpy>
#include <QThread>
#include <QTest>
#include <QTextLayout>

// MiniTimeline and Viewport are Qt widgets, need headers
#include "widgets/MiniTimeline.h"
#include "viewport/Viewport.h"
#include "viewport/VulkanViewport.h"
#include "viewport/OverlayMath.h"
#include "viewport/TransformOverlayWidget.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/monitors/ProgramMonitor.h"
#include "playback/PlaybackController.h"
#include "cache/FrameCache.h"
#include "timeline/Timeline.h"
#include "timeline/GraphicClip.h"
#include "timeline/CaptionClip.h"
#include "ClipRenderers.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

using namespace rt;

namespace rt {
std::shared_ptr<CachedFrame> renderGraphicClip(
    GraphicClip* clip, int64_t tick, uint32_t outW, uint32_t outH,
    uint32_t refW, uint32_t refH);
std::shared_ptr<CachedFrame> renderCaptionClip(
    CaptionClip* clip, int64_t tick, uint32_t outW, uint32_t outH,
    uint32_t refW, uint32_t refH);
}

TEST(TransformOverlayInput, CropGestureRequiresControlModifier)
{
    EXPECT_FALSE(TransformOverlayWidget::cropGestureRequested(Qt::NoModifier));
    EXPECT_FALSE(TransformOverlayWidget::cropGestureRequested(Qt::ShiftModifier));
    EXPECT_FALSE(TransformOverlayWidget::cropGestureRequested(Qt::AltModifier));
    EXPECT_TRUE(TransformOverlayWidget::cropGestureRequested(Qt::ControlModifier));
    EXPECT_TRUE(TransformOverlayWidget::cropGestureRequested(
        Qt::ControlModifier | Qt::ShiftModifier));
}

TEST(TransformOverlayInput, SelectionToolPreservesSelectedMask)
{
    std::vector<OpacityMask> masks(1);
    TransformOverlayWidget overlay(nullptr);
    overlay.setMasks(&masks);
    overlay.setActiveMaskIndex(0);

    // Effect Controls focuses the mask, then returns to the ordinary
    // Selection tool for path manipulation.
    overlay.setEditTool(9);
    overlay.setEditTool(0);
    EXPECT_EQ(overlay.activeMaskIndex(), 0);
}

TEST(TransformOverlayInput, PenToolUsesCrosshairCursor)
{
    TransformOverlayWidget overlay(nullptr);
    overlay.setEditTool(9);
    ASSERT_NE(QGuiApplication::overrideCursor(), nullptr);
    EXPECT_EQ(QGuiApplication::overrideCursor()->shape(), Qt::CrossCursor);

    overlay.setEditTool(0);
    EXPECT_EQ(QGuiApplication::overrideCursor(), nullptr);
}

TEST(TransformOverlayInput, InlineSelectionSurvivesCharacterFormatting)
{
    TransformOverlayWidget overlay(nullptr);
    overlay.resize(640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    info.useContentRect = true;
    info.contentL = 700.0f;
    info.contentT = 480.0f;
    info.contentR = 1220.0f;
    info.contentB = 600.0f;
    info.contentCanvasW = 1920.0f;
    info.contentCanvasH = 1080.0f;
    overlay.setTransformOverlay(info);
    overlay.show();

    QWidget formattingPanel;
    formattingPanel.setFocusPolicy(Qt::StrongFocus);
    formattingPanel.show();
    overlay.setInlineTextFormattingWidget(&formattingPanel);
    overlay.beginInlineTextEdit(QStringLiteral("alpha beta"),
                                QStringLiteral("Arial"), 72.0f, 400,
                                false, Qt::white);
    QApplication::processEvents();
    EXPECT_TRUE(overlay.currentInlineTextStyles().empty());
    QSignalSpy formatChanged(&overlay,
        &TransformOverlayWidget::inlineTextSelectionFormatChanged);

    overlay.setInlineTextSelection(6, 4);
    EXPECT_EQ(overlay.inlineTextSelection(), std::make_pair(6, 4));

    // Moving focus to Premiere-style character controls must not commit or
    // discard the monitor selection before the control's click is handled.
    formattingPanel.activateWindow();
    formattingPanel.setFocus(Qt::MouseFocusReason);
    QApplication::processEvents();
    EXPECT_TRUE(overlay.isInlineTextEditing());
    EXPECT_EQ(overlay.inlineTextSelection(), std::make_pair(6, 4));

    ASSERT_TRUE(overlay.applyInlineTextFontWeight(700));
    ASSERT_TRUE(overlay.applyInlineTextCapitalization(true, false));
    ASSERT_TRUE(overlay.applyInlineTextTracking(12.0f));
    ASSERT_TRUE(overlay.applyInlineTextBaselineShift(8.0f));
    ASSERT_TRUE(overlay.applyInlineTextLeading(24.0f));
    ASSERT_TRUE(overlay.applyInlineTextFontStyle(QStringLiteral("Regular")));
    ASSERT_TRUE(overlay.applyInlineTextKerning(3.0f));
    ASSERT_TRUE(overlay.applyInlineTextTabWidth(96.0f));
    ASSERT_TRUE(overlay.applyInlineTextTsume(15.0f));
    ASSERT_TRUE(overlay.applyInlineTextFauxStyles(true, true));
    ASSERT_TRUE(overlay.applyInlineTextUnderline(true));
    ASSERT_TRUE(overlay.applyInlineTextScript(true, false));
    ASSERT_TRUE(overlay.applyInlineTextFill(true, 0xFFFF3300u));
    ASSERT_TRUE(overlay.applyInlineTextStroke(
        true, 0xFF0033FFu, 5.0f, static_cast<int>(StrokePosition::Outer)));
    ASSERT_TRUE(overlay.applyInlineTextShadow(
        true, 0x99000000u, 8.0f, 45.0f, 6.0f, 0.5f));
    ASSERT_TRUE(overlay.applyInlineTextBackground(
        true, 0x8800FF00u, 7.0f));
    ASSERT_TRUE(overlay.applyInlineTextFontSize(90.0f));
    EXPECT_EQ(overlay.inlineTextSelection(), std::make_pair(6, 4));
    const auto runs = overlay.currentInlineTextStyles();
    const auto bold = std::find_if(runs.begin(), runs.end(),
        [](const TextStyleRun& run) {
            return run.start <= 6 && run.start + run.length >= 10
                && run.fontWeight >= 700
                && std::abs(run.fontSize - 90.0f) < 0.01f
                && run.allCaps && !run.smallCaps
                && std::abs(run.tracking - 12.0f) < 0.01f
                && std::abs(run.baselineShift - 8.0f) < 0.01f
                && std::abs(run.leading - 24.0f) < 0.01f
                && run.fontStyle == "Regular"
                && std::abs(run.kerning - 3.0f) < 0.01f
                && std::abs(run.tabWidth - 96.0f) < 0.01f
                && std::abs(run.tsume - 15.0f) < 0.01f
                && run.fauxBold && run.fauxItalic && run.underline
                && run.superscript && !run.subscript
                && run.appearance.fillColor == 0xFFFF3300u
                && run.appearance.strokeEnabled
                && run.appearance.strokeColor == 0xFF0033FFu
                && run.appearance.strokePosition == StrokePosition::Outer
                && run.appearance.shadowEnabled
                && run.appearance.backgroundEnabled
                && run.appearance.backgroundColor == 0x8800FF00u
                && (run.overrideMask & TextOverrideCapitalization)
                && (run.overrideMask & TextOverrideTracking)
                && (run.overrideMask & TextOverrideBaseline)
                && (run.overrideMask & TextOverrideLeading);
        });
    ASSERT_NE(bold, runs.end());
    EXPECT_EQ(bold->start, 6u);
    EXPECT_EQ(bold->length, 4u);

    // Selecting base and formatted characters produces explicit mixed
    // control state instead of reporting the first character as universal.
    formatChanged.clear();
    overlay.setInlineTextSelection(0, 10);
    ASSERT_FALSE(formatChanged.isEmpty());
    const uint32_t mixedFlags = formatChanged.last().at(9).toUInt();
    EXPECT_NE(mixedFlags & InlineMixedWeight, 0u);
    EXPECT_NE(mixedFlags & InlineMixedCapitalization, 0u);
    EXPECT_NE(mixedFlags & InlineMixedTracking, 0u);
    EXPECT_NE(mixedFlags & InlineMixedBaseline, 0u);
    EXPECT_NE(mixedFlags & InlineMixedLeading, 0u);
    EXPECT_NE(mixedFlags & InlineMixedKerning, 0u);
    EXPECT_NE(mixedFlags & InlineMixedDecoration, 0u);
    EXPECT_NE(mixedFlags & InlineMixedFill, 0u);
    EXPECT_NE(mixedFlags & InlineMixedStroke, 0u);
    EXPECT_NE(mixedFlags & InlineMixedShadow, 0u);
    EXPECT_NE(mixedFlags & InlineMixedBackground, 0u);

    // Leave no active top-level editor/focus state for the next UI test.
    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);
    QKeyEvent cancelPress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &cancelPress);
    EXPECT_FALSE(overlay.isInlineTextEditing());
}

TEST(TransformOverlayInput, ReplacingSelectedPlaceholderInheritsBaseAppearance)
{
    TransformOverlayWidget overlay(nullptr);
    overlay.resize(640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    overlay.setTransformOverlay(info);
    overlay.show();

    TextRunAppearance baseAppearance;
    baseAppearance.fillEnabled = true;
    baseAppearance.fillColor = 0xFFFFFFFFu;
    overlay.beginInlineTextEdit(
        QStringLiteral("Title"), QStringLiteral("Arial"), 72.0f, 400,
        false, Qt::white, 1.0f, Qt::AlignHCenter, {}, 1.0f, false,
        false, 0.0f, 0.0f, 1.2f, baseAppearance);
    QApplication::processEvents();

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(overlay.inlineTextSelection(), std::make_pair(0, 5));

    editor->insertPlainText(QStringLiteral("SOMEHOW, I GOT TO EDEN"));
    EXPECT_EQ(editor->toPlainText(),
              QStringLiteral("SOMEHOW, I GOT TO EDEN"));
    EXPECT_TRUE(overlay.currentInlineTextStyles().empty());

    QKeyEvent commitPress(QEvent::KeyPress, Qt::Key_Return,
                          Qt::ControlModifier);
    QApplication::sendEvent(editor, &commitPress);
    EXPECT_TRUE(overlay.committedInlineTextStyles().empty());
    EXPECT_FALSE(overlay.isInlineTextEditing());
}

TEST(TransformOverlayInput, CompositorBackedEditHidesDuplicateGlyphsAndPreviewsLive)
{
    TransformOverlayWidget overlay(nullptr);
    overlay.resize(640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    overlay.setTransformOverlay(info);
    overlay.show();

    QSignalSpy preview(&overlay,
        &TransformOverlayWidget::inlineTextPreviewChanged);
    overlay.beginInlineTextEdit(QStringLiteral("Impact title"),
        QStringLiteral("impact"), 72.0f, 400, false, Qt::white);
    QApplication::processEvents();

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->currentCharFormat().foreground().color().alpha(), 0);
    EXPECT_TRUE(editor->styleSheet().contains(
        QStringLiteral("color: transparent")));
    EXPECT_TRUE(editor->styleSheet().contains(
        QStringLiteral("selection-color: transparent")));
    EXPECT_TRUE(editor->styleSheet().contains(
        QStringLiteral("selection-background-color: transparent")));

    overlay.setInlineTextSelection(editor->toPlainText().size(), 0);
    QTest::keyClicks(editor, QStringLiteral("!"));
    QApplication::processEvents();
    ASSERT_GE(preview.count(), 1);
    EXPECT_EQ(preview.last().at(0).toString(),
              QStringLiteral("Impact title!"));

    // Appearance controls update compositor metadata without re-enabling the
    // editor's differently hinted duplicate glyph rendering.
    ASSERT_TRUE(overlay.applyInlineTextFill(true, 0xFFFF0000u));
    EXPECT_EQ(editor->currentCharFormat().foreground().color().alpha(), 0);

    QTest::keyClick(editor, Qt::Key_Escape);
    QApplication::processEvents();
    EXPECT_FALSE(overlay.isInlineTextEditing());
}

TEST(TransformOverlayInput, MultilineCaretSurfaceAnchorsToRenderedTextTop)
{
    VulkanViewport viewport;
    viewport.resize(640, 360);
    TransformOverlayWidget overlay(&viewport);
    overlay.setGeometry(0, 0, 640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    info.useContentRect = true;
    info.contentL = 600.0f;
    info.contentT = 300.0f;
    info.contentR = 1320.0f;
    info.contentB = 600.0f;
    info.contentCanvasW = 1920.0f;
    info.contentCanvasH = 1080.0f;
    overlay.setTransformOverlay(info);
    overlay.show();

    overlay.beginInlineTextEdit(
        QStringLiteral("first line\nsecond line\nthird line"),
        QStringLiteral("impact"), 72.0f, 400, false, Qt::white);
    QApplication::processEvents();

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);

    // Compare in global coordinates because this standalone test widget has
    // a native window frame on Windows, while the production overlay is a
    // frameless child. QWidget::geometry() and mapFromGlobal() otherwise
    // differ by that non-client frame.
    const int renderedInkTopGlobal =
        overlay.mapToGlobal(QPoint(0, 100)).y(); // 300 mapped by 640/1920
    EXPECT_LE(std::abs(editor->geometry().top()
                       - renderedInkTopGlobal), 2);

    const int anchoredTop = editor->geometry().top();
    overlay.setInlineTextSelection(editor->toPlainText().size(), 0);
    QTest::keyClick(editor, Qt::Key_Return);
    QTest::keyClicks(editor, QStringLiteral("fourth line"));
    QApplication::processEvents();
    EXPECT_EQ(editor->geometry().top(), anchoredTop);

    QTest::keyClick(editor, Qt::Key_Escape);
    QApplication::processEvents();
}

TEST(TransformOverlayInput, InlineEditorUsesLogicalTextRectNotPaddedHandleRect)
{
    VulkanViewport viewport;
    viewport.resize(640, 360);
    TransformOverlayWidget overlay(&viewport);
    overlay.setGeometry(0, 0, 640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    info.useContentRect = true;
    // Transform handles intentionally include generous breathing room.
    info.contentL = 480.0f;
    info.contentT = 240.0f;
    info.contentR = 1440.0f;
    info.contentB = 660.0f;
    info.contentCanvasW = 1920.0f;
    info.contentCanvasH = 1080.0f;
    // The renderer's actual pen/line-metric rectangle is narrower and lower.
    info.useTextLayoutRect = true;
    info.textLayoutL = 690.0f;
    info.textLayoutT = 300.0f;
    info.textLayoutR = 1230.0f;
    info.textLayoutB = 570.0f;
    overlay.setTransformOverlay(info);
    overlay.show();

    overlay.beginInlineTextEdit(
        QStringLiteral("first line\nsecond line\nthird line"),
        QStringLiteral("Arial"), 72.0f, 400, false, Qt::white,
        1.0f, Qt::AlignLeft);
    QApplication::processEvents();

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);

    const QPoint expectedGlobal = overlay.mapToGlobal(QPoint(230, 100));
    EXPECT_LE(std::abs(editor->geometry().left() - expectedGlobal.x()), 2);
    EXPECT_LE(std::abs(editor->geometry().top() - expectedGlobal.y()), 2);

    QTest::keyClick(editor, Qt::Key_Escape);
    QApplication::processEvents();
}

TEST(TransformOverlayInput, InlineCaretUsesFullResolutionRendererAdvances)
{
    VulkanViewport viewport;
    viewport.resize(640, 360);
    TransformOverlayWidget overlay(&viewport);
    overlay.setGeometry(0, 0, 640, 360);
    overlay.setSequenceResolution(1920, 1080);

    const QString value = QStringLiteral("WIDE lettering test");
    QFont rendererFont(QStringLiteral("Arial"));
    rendererFont.setPointSizeF(73.0);
    const QFontMetricsF metrics(rendererFont);
    const double lineWidth = metrics.horizontalAdvance(value);
    const double lineHeight = metrics.lineSpacing();

    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    info.useContentRect = true;
    info.contentCanvasW = 1920.0f;
    info.contentCanvasH = 1080.0f;
    info.useTextLayoutRect = true;
    info.textLayoutL = static_cast<float>(960.0 - lineWidth * 0.5);
    info.textLayoutR = static_cast<float>(960.0 + lineWidth * 0.5);
    info.textLayoutT = static_cast<float>(540.0 - lineHeight * 0.5);
    info.textLayoutB = static_cast<float>(540.0 + lineHeight * 0.5);
    info.contentL = info.textLayoutL - 40.0f;
    info.contentR = info.textLayoutR + 40.0f;
    info.contentT = info.textLayoutT - 30.0f;
    info.contentB = info.textLayoutB + 30.0f;
    QTextLayout shaped(value, rendererFont);
    shaped.beginLayout();
    QTextLine shapedLine = shaped.createLine();
    ASSERT_TRUE(shapedLine.isValid());
    shapedLine.setLineWidth(1.0e9);
    shaped.endLayout();
    info.textCarets.resize(static_cast<size_t>(value.size()) + 1);
    for (int position = 0; position <= static_cast<int>(value.size()); ++position) {
        info.textCarets[static_cast<size_t>(position)] = {
            static_cast<float>(960.0 - lineWidth * 0.5
                               + shapedLine.cursorToX(position)),
            info.textLayoutT, info.textLayoutB, true};
    }
    overlay.setTransformOverlay(info);
    overlay.show();

    overlay.beginInlineTextEdit(value, QStringLiteral("Arial"), 73.0f,
        400, false, Qt::white, 1.0f, Qt::AlignHCenter);
    QApplication::processEvents();
    constexpr int caretPosition = 13;
    overlay.setInlineTextSelection(caretPosition, 0);

    const double prefix = metrics.horizontalAdvance(value.left(caretPosition));
    const double expectedX = 320.0 + (prefix - lineWidth * 0.5) / 3.0;
    const QLineF caret = overlay.inlineTextCaretLine();
    EXPECT_FALSE(caret.isNull());
    EXPECT_NEAR(caret.p1().x(), expectedX, 1.0);
    const QPoint clickGlobal = overlay.mapToGlobal(
        caret.pointAt(0.5).toPoint());
    EXPECT_EQ(overlay.inlineTextPositionAtGlobal(clickGlobal), caretPosition);

    // Selection uses those same renderer boundaries, not QPlainTextEdit's
    // independently hinted on-screen glyph positions.
    constexpr int selectionStart = 5;
    constexpr int selectionLength = 4;
    overlay.setInlineTextSelection(selectionStart, selectionLength);
    const QRectF selectionBounds = overlay.inlineTextSelectionPath().boundingRect();
    const double expectedSelectionLeft = 320.0
        + (info.textCarets[selectionStart].x - 960.0) / 3.0;
    const double expectedSelectionRight = 320.0
        + (info.textCarets[selectionStart + selectionLength].x - 960.0) / 3.0;
    EXPECT_NEAR(selectionBounds.left(), expectedSelectionLeft, 1.0);
    EXPECT_NEAR(selectionBounds.right(), expectedSelectionRight, 1.0);

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->cursorWidth(), 0);
    QTest::keyClick(editor, Qt::Key_Escape);
    QApplication::processEvents();
}

TEST(TransformOverlayInput, ParagraphFormattingTargetsSelectedParagraphs)
{
    TransformOverlayWidget overlay(nullptr);
    overlay.resize(640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    overlay.setTransformOverlay(info);
    overlay.show();
    overlay.beginInlineTextEdit(QStringLiteral("one\ntwo"),
        QStringLiteral("Arial"), 72.0f, 400, false, Qt::white);
    QApplication::processEvents();
    overlay.setInlineTextSelection(4, 3);
    ASSERT_TRUE(overlay.applyInlineParagraphAlignment(
        static_cast<int>(GTextAlign::Right)));
    ASSERT_TRUE(overlay.applyInlineParagraphDirection(true));
    EXPECT_EQ(overlay.inlineTextSelection(), std::make_pair(4, 3));
    const auto paragraphs = overlay.currentInlineParagraphStyles();
    ASSERT_EQ(paragraphs.size(), 1u);
    EXPECT_EQ(paragraphs[0].start, 4u);
    EXPECT_EQ(paragraphs[0].alignment, GTextAlign::Right);
    EXPECT_TRUE(paragraphs[0].rightToLeft);

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);
    QKeyEvent cancelPress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &cancelPress);
}

TEST(TransformOverlayInput, ActivationFocusOutDoesNotImmediatelyCommit)
{
    TransformOverlayWidget overlay(nullptr);
    overlay.resize(640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    overlay.setTransformOverlay(info);
    overlay.show();

    QSignalSpy committed(&overlay,
        &TransformOverlayWidget::inlineTextCommitted);
    overlay.beginInlineTextEdit(QStringLiteral("focus race"),
        QStringLiteral("Arial"), 72.0f, 400, false, Qt::white);

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);

    // Windows can send this while the newly-created editor and the embedded
    // Vulkan HWND are still exchanging activation during the double-click.
    QFocusEvent activationHandoff(QEvent::FocusOut,
                                  Qt::ActiveWindowFocusReason);
    QApplication::sendEvent(editor, &activationHandoff);
    QTest::qWait(160);
    QApplication::processEvents();

    EXPECT_TRUE(overlay.isInlineTextEditing());
    EXPECT_EQ(committed.count(), 0);

    QTest::keyClick(editor, Qt::Key_Escape);
    QApplication::processEvents();
}

TEST(TransformOverlayInput, ReturnInsertsNewlineAndControlReturnCommits)
{
    TransformOverlayWidget overlay(nullptr);
    overlay.resize(640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    overlay.setTransformOverlay(info);
    overlay.show();

    QSignalSpy committed(&overlay,
        &TransformOverlayWidget::inlineTextCommitted);
    overlay.beginInlineTextEdit(QStringLiteral("alpha"),
        QStringLiteral("Arial"), 72.0f, 400, false, Qt::white);
    QApplication::processEvents();
    overlay.setInlineTextSelection(5, 0);

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);
    const int firstLineTop = editor->geometry().top();
    const int singleLineHeight = editor->height();
    QKeyEvent newlinePress(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &newlinePress);
    QTest::keyClicks(editor, QStringLiteral("beta"));
    QApplication::sendEvent(editor, &newlinePress);
    QTest::keyClicks(editor, QStringLiteral("gamma"));
    QApplication::processEvents();
    EXPECT_TRUE(overlay.isInlineTextEditing());
    EXPECT_EQ(committed.count(), 0);
    EXPECT_EQ(editor->geometry().top(), firstLineTop);
    EXPECT_GT(editor->height(), singleLineHeight * 2);
    EXPECT_EQ(editor->verticalScrollBar()->maximum(), 0);

    QKeyEvent commitPress(QEvent::KeyPress, Qt::Key_Return,
                          Qt::ControlModifier);
    QApplication::sendEvent(editor, &commitPress);
    ASSERT_EQ(committed.count(), 1);
    EXPECT_EQ(committed.at(0).at(0).toString(),
              QStringLiteral("alpha\nbeta\ngamma"));
    EXPECT_FALSE(overlay.isInlineTextEditing());
}

TEST(TransformOverlayInput, TypingClaimsSingleLetterToolShortcuts)
{
    TransformOverlayWidget overlay(nullptr);
    overlay.resize(640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    overlay.setTransformOverlay(info);
    overlay.show();

    QShortcut textToolShortcut(QKeySequence(Qt::Key_T), &overlay);
    textToolShortcut.setContext(Qt::ApplicationShortcut);
    int shortcutHits = 0;
    QObject::connect(&textToolShortcut, &QShortcut::activated,
                     [&shortcutHits]() { ++shortcutHits; });

    overlay.beginInlineTextEdit(QStringLiteral("a"),
        QStringLiteral("Arial"), 72.0f, 400, false, Qt::white);
    QApplication::processEvents();

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);
    QTextCursor cursor = editor->textCursor();
    cursor.clearSelection();
    cursor.movePosition(QTextCursor::End);
    editor->setTextCursor(cursor);
    QTest::keyClick(editor, Qt::Key_T);
    QApplication::processEvents();

    EXPECT_EQ(shortcutHits, 0);
    EXPECT_EQ(editor->toPlainText(), QStringLiteral("at"));

    QKeyEvent cancelPress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &cancelPress);
}

TEST(TransformOverlayInput, InlineEditorTransparentAreaRemainsMouseInteractive)
{
    TransformOverlayWidget overlay(nullptr);
    overlay.resize(640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    info.useContentRect = true;
    info.contentL = 600.0f;
    info.contentT = 480.0f;
    info.contentR = 1320.0f;
    info.contentB = 600.0f;
    info.contentCanvasW = 1920.0f;
    info.contentCanvasH = 1080.0f;
    overlay.setTransformOverlay(info);
    overlay.show();

    overlay.beginInlineTextEdit(QStringLiteral("alpha beta"),
        QStringLiteral("Arial"), 72.0f, 400, false, Qt::white);
    QApplication::processEvents();

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);

    // The transparent hit surface spans the original layer box, not merely
    // the glyph width. Dragging from blank space beside the word therefore
    // remains inside the editor and cannot create another text layer.
    EXPECT_GE(editor->width(), 150);

    // Windows performs per-pixel hit testing for WA_TranslucentBackground
    // windows.  A zero-alpha background lets a caret-placement click between
    // glyphs fall through to the Program Monitor and end the edit session.
    EXPECT_TRUE(editor->styleSheet().contains(
        QStringLiteral("background: rgba(0, 0, 0, 1)")));

    QKeyEvent cancelPress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &cancelPress);
    EXPECT_FALSE(overlay.isInlineTextEditing());
}

TEST(TransformOverlayInput, FallthroughClickInsideEditorRepositionsCaret)
{
    TransformOverlayWidget overlay(nullptr);
    overlay.resize(640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    info.useContentRect = true;
    info.contentL = 600.0f;
    info.contentT = 480.0f;
    info.contentR = 1320.0f;
    info.contentB = 600.0f;
    info.contentCanvasW = 1920.0f;
    info.contentCanvasH = 1080.0f;
    overlay.setTransformOverlay(info);
    overlay.show();
    overlay.beginInlineTextEdit(QStringLiteral("alpha beta gamma"),
        QStringLiteral("Arial"), 72.0f, 400, false, Qt::white);
    QApplication::processEvents();

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);
    overlay.setInlineTextSelection(editor->toPlainText().size(), 0);

    const QPoint viewportPoint(editor->viewport()->width() / 3,
                               editor->viewport()->height() / 2);
    const QPoint globalPoint =
        editor->viewport()->mapToGlobal(viewportPoint);
    const int expectedPosition =
        overlay.inlineTextPositionAtGlobal(globalPoint);

    // Mimic Windows assigning a transparent editor pixel to the native
    // surface below it. The application-level overlay filter must reclaim
    // the event and perform the same caret move as QPlainTextEdit.
    QWidget underlyingSurface;
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(1, 1),
                      QPointF(globalPoint), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&underlyingSurface, &press);
    EXPECT_TRUE(press.isAccepted());
    EXPECT_TRUE(overlay.isInlineTextEditing());
    EXPECT_EQ(overlay.inlineTextSelection(),
              std::make_pair(expectedPosition, 0));

    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(1, 1),
                        QPointF(globalPoint), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&underlyingSurface, &release);

    QKeyEvent cancelPress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &cancelPress);
}

TEST(TransformOverlayInput, InlineEditorUsesLatestMovedTextPosition)
{
    VulkanViewport viewport;
    viewport.resize(640, 360);
    viewport.show();
    QApplication::processEvents();

    TransformOverlayWidget overlay(&viewport);
    overlay.setGeometry(viewport.rect());
    overlay.setSequenceResolution(1920, 1080);
    overlay.show();

    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    info.useContentRect = true;
    info.contentL = 800.0f;
    info.contentT = 480.0f;
    info.contentR = 1120.0f;
    info.contentB = 600.0f;
    info.contentCanvasW = 1920.0f;
    info.contentCanvasH = 1080.0f;
    overlay.setTransformOverlay(info);

    // Simulate moving the layer after its first edit. The next edit must use
    // this latest overlay, not the original centre cached by a prior session.
    info.posX = 300.0f;
    overlay.setTransformOverlay(info);
    overlay.beginInlineTextEdit(QStringLiteral("moved text"),
        QStringLiteral("Arial"), 72.0f, 400, false, Qt::white);
    QApplication::processEvents();

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);
    const QPoint editorCenter = overlay.mapFromGlobal(editor->geometry().center());
    EXPECT_NEAR(editorCenter.x(), 420.0, 3.0);
    EXPECT_NEAR(editorCenter.y(), 180.0, 3.0);

    QKeyEvent cancelPress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &cancelPress);
}

TEST(TransformOverlayInput, DoubleClickUsesSequenceSpaceBeforePreviewArrives)
{
    VulkanViewport viewport;
    viewport.resize(640, 360);
    viewport.show();
    QApplication::processEvents();

    TransformOverlayWidget overlay(&viewport);
    overlay.setGeometry(viewport.rect());
    overlay.setSequenceResolution(3840, 2160);
    overlay.show();
    QApplication::processEvents();

    QSignalSpy editRequested(&overlay,
        &TransformOverlayWidget::textEditRequested);
    QTest::mouseDClick(&overlay, Qt::LeftButton, Qt::NoModifier,
                      QPoint(320, 180));

    ASSERT_EQ(editRequested.count(), 1);
    const QList<QVariant> args = editRequested.takeFirst();
    EXPECT_NEAR(args.at(0).toFloat(), 1920.0f, 2.0f);
    EXPECT_NEAR(args.at(1).toFloat(), 1080.0f, 2.0f);
}

TEST(TransformOverlayInput, NativeSurfaceDoubleClickReachesTextEditorRequest)
{
    VulkanViewport viewport;
    viewport.resize(640, 360);
    viewport.show();
    QApplication::processEvents();

    TransformOverlayWidget overlay(&viewport);
    overlay.setGeometry(viewport.rect());
    overlay.setSequenceResolution(1920, 1080);
    overlay.show();
    QApplication::processEvents();

    QSignalSpy editRequested(&overlay,
        &TransformOverlayWidget::textEditRequested);
    emit viewport.nativeLeftDoubleClicked(
        overlay.mapToGlobal(QPoint(320, 180)), Qt::NoModifier);
    QApplication::processEvents();

    ASSERT_EQ(editRequested.count(), 1);
    const QList<QVariant> args = editRequested.takeFirst();
    EXPECT_NEAR(args.at(0).toFloat(), 960.0f, 2.0f);
    EXPECT_NEAR(args.at(1).toFloat(), 540.0f, 2.0f);
}

TEST(TransformOverlayInput, ApplicationRouteHandlesOverlayOwnedDoubleClick)
{
    VulkanViewport viewport;
    viewport.resize(640, 360);
    viewport.show();
    QApplication::processEvents();

    TransformOverlayWidget overlay(&viewport);
    overlay.setGeometry(QRect(viewport.mapToGlobal(QPoint(0, 0)),
                              viewport.size()));
    overlay.setSequenceResolution(1920, 1080);
    overlay.show();
    QApplication::processEvents();

    QSignalSpy editRequested(&overlay,
        &TransformOverlayWidget::textEditRequested);
    const QPoint globalPoint = overlay.mapToGlobal(QPoint(320, 180));
    QMouseEvent event(QEvent::MouseButtonDblClick,
                      QPointF(320, 180), QPointF(globalPoint),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);

    // Deliver to the overlay rather than the native Vulkan QWindow. The
    // application-level route must still start the same monitor edit gesture.
    QApplication::sendEvent(&overlay, &event);
    QApplication::processEvents();

    ASSERT_EQ(editRequested.count(), 1);
    const QList<QVariant> args = editRequested.takeFirst();
    EXPECT_NEAR(args.at(0).toFloat(), 960.0f, 2.0f);
    EXPECT_NEAR(args.at(1).toFloat(), 540.0f, 2.0f);
}

TEST(TransformOverlayInput, NativeViewportOwnsTextAndResetDoubleClicks)
{
    EXPECT_TRUE(VulkanViewport::ownsNativeDoubleClick(Qt::MiddleButton));
    EXPECT_TRUE(VulkanViewport::ownsNativeDoubleClick(Qt::LeftButton));
    EXPECT_FALSE(VulkanViewport::ownsNativeDoubleClick(Qt::RightButton));
}

TEST(TransformOverlayInput, PartialFontGrowthExpandsEditorWithoutScrollingFirstLine)
{
    VulkanViewport viewport;
    viewport.resize(640, 360);
    viewport.show();
    QApplication::processEvents();

    TransformOverlayWidget overlay(&viewport);
    overlay.setGeometry(viewport.rect());
    overlay.setSequenceResolution(1920, 1080);
    overlay.show();
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    info.useContentRect = true;
    info.contentL = 810.0f;
    info.contentT = 490.0f;
    info.contentR = 1110.0f;
    info.contentB = 590.0f;
    info.contentCanvasW = 1920.0f;
    info.contentCanvasH = 1080.0f;
    overlay.setTransformOverlay(info);
    overlay.beginInlineTextEdit(QStringLiteral("alpha beta"),
        QStringLiteral("Arial"), 72.0f, 400, false, Qt::white);
    QApplication::processEvents();

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);
    const QSize before = editor->size();
    overlay.setInlineTextSelection(6, 4);
    ASSERT_TRUE(overlay.applyInlineTextFontSize(180.0f));
    QApplication::processEvents();

    EXPECT_GT(editor->width(), before.width());
    EXPECT_GT(editor->height(), before.height());
    EXPECT_EQ(editor->verticalScrollBar()->maximum(), 0);

    QKeyEvent cancelPress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &cancelPress);
}

TEST(TransformOverlayInput, PointTextExpandsPastOriginalBoxWithoutWrapping)
{
    TransformOverlayWidget overlay(nullptr);
    overlay.resize(640, 360);
    overlay.setSequenceResolution(1920, 1080);
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    info.useContentRect = true;
    info.contentL = 900.0f;
    info.contentT = 500.0f;
    info.contentR = 1020.0f;
    info.contentB = 580.0f;
    info.contentCanvasW = 1920.0f;
    info.contentCanvasH = 1080.0f;
    overlay.setTransformOverlay(info);
    overlay.show();
    overlay.beginInlineTextEdit(QStringLiteral("short"),
        QStringLiteral("Arial"), 72.0f, 400, false, Qt::white);
    QApplication::processEvents();

    QPlainTextEdit* editor = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("inlineTextEdit")
            && widget->isVisible()) {
            editor = qobject_cast<QPlainTextEdit*>(widget);
            break;
        }
    }
    ASSERT_NE(editor, nullptr);
    const int originalWidth = editor->width();
    EXPECT_EQ(editor->lineWrapMode(), QPlainTextEdit::NoWrap);

    editor->selectAll();
    editor->insertPlainText(QStringLiteral(
        "This title is deliberately much wider than its original text box"));
    QApplication::processEvents();
    EXPECT_GT(editor->width(), originalWidth);
    EXPECT_EQ(editor->horizontalScrollBar()->maximum(), 0);

    QKeyEvent cancelPress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &cancelPress);
}

TEST(GraphicTextRendering, CharacterStyleRunChangesCompositePixels)
{
    GraphicClip clip;
    clip.setTimelineIn(0);
    auto* text = clip.addTextLayer("mixed style");
    text->setFontFamily("Arial");
    text->setFontSize(72.0f);
    text->setFontWeight(400);

    const auto plain = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(plain, nullptr);

    TextStyleRun bold;
    bold.start = 6;
    bold.length = 5;
    bold.fontFamily = "Arial";
    bold.fontSize = 96.0f;
    bold.fontWeight = 900;
    text->setStyleRuns({bold});
    const auto mixed = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(mixed, nullptr);
    EXPECT_NE(plain->pixels, mixed->pixels);
}

TEST(GraphicTextRendering, StyledMeasurementTracksPartialFontGrowth)
{
    GraphicClip clip;
    auto* text = clip.addTextLayer("alpha beta");
    text->setFontFamily("Arial");
    text->setFontSize(72.0f);
    const auto base = measureGraphicTextLayout(
        text, 0, 960.0, 540.0, Qt::AlignHCenter, Qt::AlignVCenter);
    ASSERT_TRUE(base.valid);
    ASSERT_TRUE(base.layoutValid);
    ASSERT_EQ(base.carets.size(), QStringLiteral("alpha beta").size() + 1);
    for (const auto& caret : base.carets) EXPECT_TRUE(caret.valid);
    EXPECT_LT(base.layoutLeft, base.layoutRight);
    EXPECT_LT(base.layoutTop, base.layoutBottom);

    TextStyleRun run;
    run.start = 6;
    run.length = 4;
    run.fontFamily = "Arial";
    run.fontSize = 180.0f;
    run.fontWeight = 400;
    text->setStyleRuns({run});
    const auto grown = measureGraphicTextLayout(
        text, 0, 960.0, 540.0, Qt::AlignHCenter, Qt::AlignVCenter);
    ASSERT_TRUE(grown.valid);
    ASSERT_TRUE(grown.layoutValid);
    EXPECT_GT(grown.right - grown.left, base.right - base.left);
    EXPECT_GT(grown.bottom - grown.top, base.bottom - base.top);
    EXPECT_GT(grown.layoutRight - grown.layoutLeft,
              base.layoutRight - base.layoutLeft);
}

TEST(GraphicTextRendering, RangeAppearanceAndDecorationChangeCompositePixels)
{
    GraphicClip clip;
    auto* text = clip.addTextLayer("red blue");
    text->setFontFamily("Arial");
    text->setFontSize(72.0f);
    const auto plain = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(plain, nullptr);

    TextStyleRun run;
    run.start = 4;
    run.length = 4;
    run.fontFamily = "Arial";
    run.fontSize = 72.0f;
    run.fontWeight = 400;
    run.underline = true;
    run.kerning = 4.0f;
    run.appearance.fillEnabled = true;
    run.appearance.fillColor = 0xFF2277FFu;
    run.appearance.strokeEnabled = true;
    run.appearance.strokeColor = 0xFFFFFFFFu;
    run.appearance.strokeWidth = 3.0f;
    run.appearance.backgroundEnabled = true;
    run.appearance.backgroundColor = 0x88000000u;
    run.appearance.backgroundPadding = 5.0f;
    run.overrideMask = TextOverrideDecoration | TextOverrideKerning
        | TextOverrideFill | TextOverrideStroke | TextOverrideBackground;
    text->setStyleRuns({run});
    const auto styled = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(styled, nullptr);
    EXPECT_NE(plain->pixels, styled->pixels);
}

TEST(GraphicTextRendering, BackgroundIsAUniformTextBlockRectangle)
{
    GraphicClip clip;
    auto* text = clip.addTextLayer("Jagged gap");
    text->setFontFamily("Arial");
    text->setFontSize(72.0f);
    text->setFillForAll(false, 0xFFFFFFFFu);
    text->setStrokeForAll(false, 0xFFFFFFFFu, 0.0f,
                          StrokePosition::Outer);
    text->setBackgroundForAll(true, 0xFF00FF00u, 8.0f);

    const auto frame = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(frame, nullptr);

    int left = static_cast<int>(frame->width);
    int top = static_cast<int>(frame->height);
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < static_cast<int>(frame->height); ++y) {
        for (int x = 0; x < static_cast<int>(frame->width); ++x) {
            const size_t offset = static_cast<size_t>(y) * frame->stride
                + static_cast<size_t>(x) * 4;
            if (frame->pixels[offset + 3] == 0) continue;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    }
    ASSERT_GT(right, left);
    ASSERT_GT(bottom, top);

    // Ignore the anti-aliased outermost edge. Every pixel inside it must be
    // occupied; glyph-bounds backgrounds leave transparent holes here.
    for (int y = top + 1; y < bottom; ++y) {
        for (int x = left + 1; x < right; ++x) {
            const size_t offset = static_cast<size_t>(y) * frame->stride
                + static_cast<size_t>(x) * 4;
            EXPECT_EQ(frame->pixels[offset + 3], 255)
                << "transparent gap at (" << x << ", " << y << ')';
        }
    }
}

TEST(GraphicTextRendering, StyledParagraphTextHonorsWrapWidth)
{
    GraphicClip clip;
    auto* text = clip.addTextLayer("one two three four");
    text->setFontSize(72.0f);
    TextStyleRun run;
    run.start = 4;
    run.length = 9;
    run.fontFamily = "Arial";
    run.fontSize = 96.0f;
    run.fontWeight = 700;
    text->setStyleRuns({run});

    const auto pointText = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(pointText, nullptr);
    text->setUseParagraphBox(true);
    text->setBoxWidth(220.0f);
    const auto paragraph = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(paragraph, nullptr);
    EXPECT_NE(pointText->pixels, paragraph->pixels);
}

TEST(GraphicTextRendering, CharacterSpacingAndBaselineOverridesChangePixels)
{
    GraphicClip clip;
    auto* text = clip.addTextLayer("range style");
    text->setFontFamily("Arial");
    text->setFontSize(72.0f);
    const auto plain = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(plain, nullptr);

    TextStyleRun run;
    run.start = 0;
    run.length = 5;
    run.fontFamily = "Arial";
    run.fontSize = 72.0f;
    run.fontWeight = 400;
    run.tracking = 14.0f;
    run.baselineShift = 16.0f;
    run.overrideMask = TextOverrideTracking | TextOverrideBaseline;
    text->setStyleRuns({run});
    const auto ranged = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(ranged, nullptr);
    EXPECT_NE(plain->pixels, ranged->pixels);
}

TEST(GraphicTextRendering, CharacterLeadingOverrideChangesMultilineLayout)
{
    GraphicClip clip;
    auto* text = clip.addTextLayer("first line\nsecond line");
    text->setFontFamily("Arial");
    text->setFontSize(54.0f);
    const auto plain = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(plain, nullptr);

    TextStyleRun run;
    run.start = 0;
    run.length = 10;
    run.fontFamily = "Arial";
    run.fontSize = 54.0f;
    run.fontWeight = 400;
    run.leading = 40.0f;
    run.overrideMask = TextOverrideLeading;
    text->setStyleRuns({run});
    const auto ranged = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(ranged, nullptr);
    EXPECT_NE(plain->pixels, ranged->pixels);
}

TEST(GraphicTextRendering, PointTextAddsNewLinesBelowTheFirstLine)
{
    GraphicClip clip;
    auto* text = clip.addTextLayer("Anchor");
    text->setFontFamily("Arial");
    text->setFontSize(54.0f);
    const auto single = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(single, nullptr);

    int left = static_cast<int>(single->width);
    int top = static_cast<int>(single->height);
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < static_cast<int>(single->height); ++y) {
        for (int x = 0; x < static_cast<int>(single->width); ++x) {
            const size_t offset = static_cast<size_t>(y) * single->stride
                + static_cast<size_t>(x) * 4;
            if (single->pixels[offset + 3] == 0) continue;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    }
    ASSERT_GE(right, left);
    ASSERT_GE(bottom, top);

    text->setText("Anchor\nBelow");
    const auto multiline = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(multiline, nullptr);
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const size_t offset = static_cast<size_t>(y) * single->stride
                + static_cast<size_t>(x) * 4;
            for (int channel = 0; channel < 4; ++channel)
                EXPECT_EQ(multiline->pixels[offset + channel],
                          single->pixels[offset + channel]);
        }
    }
}

TEST(GraphicTextRendering, AugustTitleOuterTransformKeepsAddedLinesUnclipped)
{
    GraphicClip clip;
    auto* text = clip.addTextLayer(
        "I like to count the\nnumber of times\n\"...\" appears.\nFOURTH LINE\nFIFTH LINE");
    clip.positionX().setDefaultValue(-192.0f);
    clip.positionY().setDefaultValue(-554.182f);
    clip.scaleX().setDefaultValue(0.782339f);
    clip.scaleY().setDefaultValue(0.782339f);
    text->setFontFamily("impact");
    text->setFontSize(72.0f);
    text->transform().posX.setDefaultValue(-633.506f);
    text->transform().posY.setDefaultValue(207.795f);
    text->transform().scaleX.setDefaultValue(0.876575f);
    text->transform().scaleY.setDefaultValue(0.876575f);

    const auto frame = renderGraphicClip(&clip, 0, 1920, 1080, 1920, 1080);
    ASSERT_NE(frame, nullptr);

    // Count separated horizontal ink bands. This exact nested layer/clip
    // transform from the August sequence must retain lines added below the
    // original title. Before the outer transform was baked, lines four and
    // five fell below the intermediate 1080p canvas and were discarded even
    // though the clip transform moved them into the final visible frame.
    int bands = 0;
    bool inBand = false;
    for (int y = 0; y < static_cast<int>(frame->height); ++y) {
        bool rowHasInk = false;
        for (int x = 0; x < static_cast<int>(frame->width); ++x) {
            const size_t offset = static_cast<size_t>(y) * frame->stride
                + static_cast<size_t>(x) * 4;
            if (frame->pixels[offset + 3] != 0) {
                rowHasInk = true;
                break;
            }
        }
        if (rowHasInk && !inBand) ++bands;
        inBand = rowHasInk;
    }
    EXPECT_EQ(bands, 5);
}

TEST(GraphicTextRendering, AugustTitleCaretMatchesRenderedImpactPrefix)
{
    GraphicClip clip;
    const QString value = QStringLiteral(
        "I like to count the\nnumber of times\n\"...\" appears.");
    auto* text = clip.addTextLayer(value.toStdString());
    text->setFontFamily("impact");
    text->setFontSize(72.0f);

    const auto measured = measureGraphicTextLayout(
        text, 0, 960.0, 540.0, Qt::AlignHCenter, Qt::AlignVCenter);
    const int position = value.indexOf(QStringLiteral("times")) + 3;
    ASSERT_GE(position, 3);
    ASSERT_LT(position, static_cast<int>(measured.carets.size()));
    ASSERT_TRUE(measured.carets[static_cast<size_t>(position)].valid);

    const QString line = QStringLiteral("number of times");
    const QString prefix = QStringLiteral("number of tim");
    QFont rendererFont(QStringLiteral("impact"), 72);
    rendererFont.setWeight(static_cast<QFont::Weight>(text->fontWeight()));
    rendererFont.setItalic(text->isItalic());
    rendererFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.0);
    const QFontMetricsF metrics(rendererFont);
    const double expected = 960.0 - metrics.horizontalAdvance(line) * 0.5
        + metrics.horizontalAdvance(prefix);
    EXPECT_NEAR(measured.carets[static_cast<size_t>(position)].x,
                expected, 0.25);
}

TEST(GraphicTextRendering, ParagraphBoxHeightControlsVerticalAlignment)
{
    GraphicClip clip;
    auto* text = clip.addTextLayer("vertical");
    text->setFontSize(48.0f);
    text->setUseParagraphBox(true);
    text->setBoxWidth(400.0f);
    text->setBoxHeight(200.0f);
    text->setVAlignment(GTextVAlign::Top);
    const auto top = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(top, nullptr);

    text->setVAlignment(GTextVAlign::Bottom);
    const auto bottom = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(bottom, nullptr);
    EXPECT_NE(top->pixels, bottom->pixels);
}

TEST(GraphicTextRendering, MaskWithTextClearsPixelsOutsideGlyphs)
{
    GraphicClip clip;
    auto* shape = clip.addShapeLayer(ShapeType::Rectangle);
    shape->setShapeWidth(500.0f);
    shape->setShapeHeight(280.0f);
    shape->setFillColor(0xFFFF0000u);
    auto* text = clip.addTextLayer("MASK");
    text->setFontFamily("Arial");
    text->setFontSize(96.0f);

    const auto unmasked = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(unmasked, nullptr);
    text->setMaskWithText(true);
    const auto masked = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(masked, nullptr);

    const auto opaquePixels = [](const std::shared_ptr<CachedFrame>& frame) {
        size_t count = 0;
        for (size_t i = 3; i < frame->pixels.size(); i += 4)
            if (frame->pixels[i] != 0) ++count;
        return count;
    };
    EXPECT_LT(opaquePixels(masked), opaquePixels(unmasked) / 2);
}

TEST(GraphicTextRendering, RightToLeftReordersDifferentlyStyledRuns)
{
    GraphicClip clip;
    auto* text = clip.addTextLayer("RED BLUE");
    text->setFontFamily("Arial");
    text->setFontSize(72.0f);
    TextStyleRun red;
    red.start = 0;
    red.length = 3;
    red.appearance.fillEnabled = true;
    red.appearance.fillColor = 0xFFFF0000u;
    red.overrideMask = TextOverrideFill;
    TextStyleRun blue = red;
    blue.start = 4;
    blue.length = 4;
    blue.appearance.fillColor = 0xFF0000FFu;
    text->setStyleRuns({red, blue});

    const auto leftToRight = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(leftToRight, nullptr);
    text->setRightToLeft(true);
    const auto rightToLeft = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(rightToLeft, nullptr);
    EXPECT_NE(leftToRight->pixels, rightToLeft->pixels);
}

TEST(GraphicTextRendering, ShadowSoftnessChangesRenderedFalloff)
{
    GraphicClip clip;
    auto* text = clip.addTextLayer("soft shadow");
    text->setFontFamily("Arial");
    text->setFontSize(72.0f);
    text->setShadowForAll(true, 0xCC000000u, 8.0f, 45.0f, 0.0f, 0.8f);
    const auto hard = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(hard, nullptr);

    text->setShadowForAll(true, 0xCC000000u, 8.0f, 45.0f, 18.0f, 0.8f);
    const auto soft = renderGraphicClip(&clip, 0, 640, 360, 640, 360);
    ASSERT_NE(soft, nullptr);
    EXPECT_NE(hard->pixels, soft->pixels);
}

TEST(CaptionTextRendering, CharacterStyleRunChangesCompositePixels)
{
    CaptionClip caption;
    caption.setText("mixed caption");
    caption.setBgColor(0x00000000);
    const auto plain = renderCaptionClip(&caption, 0, 640, 360, 640, 360);
    ASSERT_NE(plain, nullptr);

    TextStyleRun run;
    run.start = 6;
    run.length = 7;
    run.fontFamily = "Arial";
    run.fontSize = 58.0f;
    run.fontWeight = 900;
    run.italic = true;
    caption.setStyleRuns({run});
    const auto mixed = renderCaptionClip(&caption, 0, 640, 360, 640, 360);
    ASSERT_NE(mixed, nullptr);
    EXPECT_NE(plain->pixels, mixed->pixels);
}

TEST(CaptionTextRendering, CharacterLeadingOverrideChangesMultilineLayout)
{
    CaptionClip caption;
    caption.setText("first line\nsecond line");
    caption.setBgColor(0x00000000);
    const auto plain = renderCaptionClip(&caption, 0, 640, 360, 640, 360);
    ASSERT_NE(plain, nullptr);

    TextStyleRun run;
    run.start = 0;
    run.length = 10;
    run.fontFamily = caption.fontFamily();
    run.fontSize = caption.fontSize();
    run.fontWeight = caption.isBold() ? 700 : 400;
    run.leading = 32.0f;
    run.overrideMask = TextOverrideLeading;
    caption.setStyleRuns({run});
    const auto ranged = renderCaptionClip(&caption, 0, 640, 360, 640, 360);
    ASSERT_NE(ranged, nullptr);
    EXPECT_NE(plain->pixels, ranged->pixels);
}

namespace {
class TestableTransformOverlay final : public TransformOverlayWidget
{
public:
    using TransformOverlayWidget::TransformOverlayWidget;

    void pressAt(const QPointF& pos)
    {
        QMouseEvent event(QEvent::MouseButtonPress, pos, pos, pos,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        mousePressEvent(&event);
    }

    void moveTo(const QPointF& pos)
    {
        QMouseEvent event(QEvent::MouseMove, pos, pos, pos,
                          Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        mouseMoveEvent(&event);
    }

    void releaseAt(const QPointF& pos)
    {
        QMouseEvent event(QEvent::MouseButtonRelease, pos, pos, pos,
                          Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        mouseReleaseEvent(&event);
    }
};
}

TEST(TransformOverlayInput, SelectedMaskBodyMovesWithoutMovingClip)
{
    VulkanViewport viewport;
    viewport.resize(640, 360);
    viewport.show();
    QApplication::processEvents();
    TestableTransformOverlay overlay(&viewport);
    overlay.setGeometry(viewport.rect());
    overlay.show();
    QApplication::processEvents();
    overlay.setSequenceResolution(1920, 1080);

    TransformOverlayInfo clipOverlay;
    clipOverlay.visible = true;
    clipOverlay.srcW = 1920;
    clipOverlay.srcH = 1080;
    overlay.setTransformOverlay(clipOverlay);
    overlay.setMaskOwnerOverlay(clipOverlay, true);

    OpacityMask mask;
    mask.shape = MaskShape::Ellipse;
    mask.base.centerX = 0.5f;
    mask.base.centerY = 0.5f;
    mask.base.width = 0.4f;
    mask.base.height = 0.4f;
    std::vector<OpacityMask> masks{mask};
    overlay.setMasks(&masks);
    overlay.setActiveMaskIndex(0);
    overlay.setEditTool(0);

    QSignalSpy maskFinished(&overlay,
                            &TransformOverlayWidget::maskDragFinished);
    const QPointF start(320.0, 180.0);
    const QPointF end(384.0, 180.0);
    overlay.pressAt(start);
    overlay.moveTo(end);
    overlay.releaseAt(end);

    EXPECT_NEAR(masks[0].base.centerX, 0.6f, 0.001f);
    EXPECT_NEAR(masks[0].base.centerY, 0.5f, 0.001f);
    EXPECT_FLOAT_EQ(overlay.transformOverlay().posX, 0.0f);
    EXPECT_FLOAT_EQ(overlay.transformOverlay().posY, 0.0f);
    EXPECT_EQ(maskFinished.count(), 1);
}

TEST(TransformOverlayInput, SelectedFreeDrawMaskMovesAsOneShape)
{
    VulkanViewport viewport;
    viewport.resize(640, 360);
    viewport.show();
    QApplication::processEvents();
    TestableTransformOverlay overlay(&viewport);
    overlay.setGeometry(viewport.rect());
    overlay.show();
    QApplication::processEvents();
    overlay.setSequenceResolution(1920, 1080);

    TransformOverlayInfo clipOverlay;
    clipOverlay.visible = true;
    clipOverlay.srcW = 1920;
    clipOverlay.srcH = 1080;
    overlay.setTransformOverlay(clipOverlay);
    overlay.setMaskOwnerOverlay(clipOverlay, true);

    OpacityMask mask;
    mask.shape = MaskShape::FreeDrawBezier;
    mask.base.vertices = {
        {0.3f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.7f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.5f, 0.7f, 0.0f, 0.0f, 0.0f, 0.0f},
    };
    std::vector<OpacityMask> masks{mask};
    overlay.setMasks(&masks);
    overlay.setActiveMaskIndex(0);
    // This is the state transition performed when a Pen mask is closed.
    overlay.setEditTool(9);
    overlay.setEditTool(0);

    QSignalSpy maskFinished(&overlay,
                            &TransformOverlayWidget::maskDragFinished);
    const QPointF start(320.0, 180.0); // safely inside, away from vertices
    const QPointF end(384.0, 180.0);
    overlay.pressAt(start);
    overlay.moveTo(end);
    overlay.releaseAt(end);

    ASSERT_EQ(masks[0].base.vertices.size(), 3u);
    EXPECT_NEAR(masks[0].base.vertices[0].x, 0.4f, 0.001f);
    EXPECT_NEAR(masks[0].base.vertices[1].x, 0.8f, 0.001f);
    EXPECT_NEAR(masks[0].base.vertices[2].x, 0.6f, 0.001f);
    EXPECT_NEAR(masks[0].base.vertices[0].y, 0.3f, 0.001f);
    EXPECT_FLOAT_EQ(overlay.transformOverlay().posX, 0.0f);
    EXPECT_EQ(maskFinished.count(), 1);
}

TEST(OverlayMath, SourceQuarterTurnUsesDisplayedDimensions)
{
    TransformOverlayInfo info;
    info.visible = true;
    info.srcW = 1920;
    info.srcH = 1080;
    info.containFit = true;
    const QRectF frameRect(0.0, 0.0, 1920.0, 1080.0);
    const auto identity = [](float x, float y) { return QPointF(x, y); };

    QPointF corners[4];
    computeOverlayCorners(info, 1920.0f, 1080.0f,
                          frameRect, identity, corners);
    EXPECT_NEAR(corners[1].x() - corners[0].x(), 1920.0, 0.01);
    EXPECT_NEAR(corners[3].y() - corners[0].y(), 1080.0, 0.01);

    info.srcRotation = 90;
    computeOverlayCorners(info, 1920.0f, 1080.0f,
                          frameRect, identity, corners);
    EXPECT_NEAR(corners[1].x() - corners[0].x(), 607.5, 0.01);
    EXPECT_NEAR(corners[3].y() - corners[0].y(), 1080.0, 0.01);
}

TEST(OverlayMath, MaskOwnerFollowsEditedOuterGraphicClipTransform)
{
    const QRectF frameRect(0.0, 0.0, 1920.0, 1080.0);
    const auto identity = [](float x, float y) { return QPointF(x, y); };

    TransformOverlayInfo edited;
    edited.visible = true;
    edited.useContentRect = true;
    edited.editOuterClipTransform = true;
    edited.contentCanvasW = 1920.0f;
    edited.contentCanvasH = 1080.0f;
    edited.contentL = 0.0f;
    edited.contentT = 0.0f;
    edited.contentR = 1920.0f;
    edited.contentB = 1080.0f;
    edited.clipPosX = 240.0f;
    edited.clipPosY = -90.0f;
    edited.clipScaleX = 1.3f;
    edited.clipScaleY = 0.75f;
    edited.clipRotation = 23.0f;
    edited.clipAnchorX = 110.0f;
    edited.clipAnchorY = -45.0f;

    TransformOverlayInfo maskOwner;
    maskOwner.visible = true;
    maskOwner.srcW = 1920;
    maskOwner.srcH = 1080;
    syncMaskOwnerFromOuterClip(edited, maskOwner);

    EXPECT_FLOAT_EQ(maskOwner.posX, edited.clipPosX);
    EXPECT_FLOAT_EQ(maskOwner.posY, edited.clipPosY);
    EXPECT_FLOAT_EQ(maskOwner.scaleX, edited.clipScaleX);
    EXPECT_FLOAT_EQ(maskOwner.scaleY, edited.clipScaleY);
    EXPECT_FLOAT_EQ(maskOwner.rotation, edited.clipRotation);
    EXPECT_FLOAT_EQ(maskOwner.anchorX, edited.clipAnchorX);
    EXPECT_FLOAT_EQ(maskOwner.anchorY, edited.clipAnchorY);

    QPointF contentCorners[4];
    QPointF maskOwnerCorners[4];
    computeOverlayCorners(edited, 1920.0f, 1080.0f,
                          frameRect, identity, contentCorners);
    computeOverlayCorners(maskOwner, 1920.0f, 1080.0f,
                          frameRect, identity, maskOwnerCorners);
    for (int i = 0; i < 4; ++i) {
        EXPECT_NEAR(contentCorners[i].x(), maskOwnerCorners[i].x(), 0.01);
        EXPECT_NEAR(contentCorners[i].y(), maskOwnerCorners[i].y(), 0.01);
    }

    // A local mask point uses the same two axes as its owner, so it stays at
    // the same relative point within the transformed clip.
    const auto mapUv = [](const QPointF corners[4], double u, double v) {
        return corners[0] + (corners[1] - corners[0]) * u
                          + (corners[3] - corners[0]) * v;
    };
    const QPointF contentPoint = mapUv(contentCorners, 0.27, 0.63);
    const QPointF maskPoint = mapUv(maskOwnerCorners, 0.27, 0.63);
    EXPECT_NEAR(contentPoint.x(), maskPoint.x(), 0.01);
    EXPECT_NEAR(contentPoint.y(), maskPoint.y(), 0.01);
}

// ═════════════════════════════════════════════════════════════════════════════
//  QApplication fixture
// ═════════════════════════════════════════════════════════════════════════════

static QApplication* g_app = nullptr;

class MonitorTestEnv : public ::testing::Environment
{
public:
    void SetUp() override
    {
        if (!g_app)
        {
            static int    argc = 1;
            static char   arg0[] = "test_monitors";
            static char*  argv[] = { arg0, nullptr };
            g_app = new QApplication(argc, argv);
        }
    }
};

static auto* g_env = ::testing::AddGlobalTestEnvironment(new MonitorTestEnv);

// ═════════════════════════════════════════════════════════════════════════════
//  MiniTimeline tests
// ═════════════════════════════════════════════════════════════════════════════

class MiniTimelineTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        mt = std::make_unique<MiniTimeline>();
        // Give it a known width for predictable coordinate mapping.
        // Derive from kMarginH so a margin change can't silently rot
        // the coordinate expectations below: barWidth is always 400.
        mt->resize(400 + 2 * MiniTimeline::kMarginH, MiniTimeline::kBarHeight);
    }

    std::unique_ptr<MiniTimeline> mt;
};

TEST_F(MiniTimelineTest, DefaultState)
{
    EXPECT_EQ(mt->duration(), 0);
    EXPECT_EQ(mt->playhead(), 0);
    EXPECT_EQ(mt->inPoint(), -1);
    EXPECT_EQ(mt->outPoint(), -1);
    EXPECT_FALSE(mt->hasInPoint());
    EXPECT_FALSE(mt->hasOutPoint());
    EXPECT_DOUBLE_EQ(mt->frameRate(), 24.0);
}

TEST_F(MiniTimelineTest, SetDuration)
{
    mt->setDuration(48000);  // 1 second
    EXPECT_EQ(mt->duration(), 48000);

    // Negative duration clamped to 0
    mt->setDuration(-100);
    EXPECT_EQ(mt->duration(), 0);
}

TEST_F(MiniTimelineTest, SetPlayhead)
{
    mt->setDuration(48000);
    mt->setPlayhead(24000);
    EXPECT_EQ(mt->playhead(), 24000);

    // Clamped to duration
    mt->setPlayhead(96000);
    EXPECT_EQ(mt->playhead(), 48000);

    // Clamped to 0
    mt->setPlayhead(-100);
    EXPECT_EQ(mt->playhead(), 0);
}

TEST_F(MiniTimelineTest, ClampTick)
{
    mt->setDuration(48000);
    EXPECT_EQ(mt->clampTick(-100), 0);
    EXPECT_EQ(mt->clampTick(0), 0);
    EXPECT_EQ(mt->clampTick(24000), 24000);
    EXPECT_EQ(mt->clampTick(48000), 48000);
    EXPECT_EQ(mt->clampTick(96000), 48000);
}

TEST_F(MiniTimelineTest, ClampTickZeroDuration)
{
    mt->setDuration(0);
    EXPECT_EQ(mt->clampTick(-100), 0);
    EXPECT_EQ(mt->clampTick(0), 0);
    EXPECT_EQ(mt->clampTick(100), 0);
}

TEST_F(MiniTimelineTest, PositionToTick)
{
    mt->setDuration(48000);  // 1 second, barWidth=400
    const double m = MiniTimeline::kMarginH;

    // Left edge (x = margin) → tick 0
    EXPECT_EQ(mt->positionToTick(m), 0);

    // Right edge (margin + barWidth) → tick 48000
    EXPECT_EQ(mt->positionToTick(m + 400.0), 48000);

    // Middle → tick 24000
    EXPECT_EQ(mt->positionToTick(m + 200.0), 24000);

    // Before left edge → clamped to 0
    EXPECT_EQ(mt->positionToTick(-10.0), 0);

    // After right edge → clamped to 48000
    EXPECT_EQ(mt->positionToTick(m + 500.0), 48000);
}

TEST_F(MiniTimelineTest, TickToPosition)
{
    mt->setDuration(48000);  // barWidth=400
    const double m = MiniTimeline::kMarginH;

    // Tick 0 → left edge
    EXPECT_DOUBLE_EQ(mt->tickToPosition(0), m);

    // Tick 48000 → right edge
    EXPECT_DOUBLE_EQ(mt->tickToPosition(48000), m + 400.0);

    // Tick 24000 → middle
    EXPECT_DOUBLE_EQ(mt->tickToPosition(24000), m + 200.0);
}

TEST_F(MiniTimelineTest, PositionToTickRoundtrip)
{
    mt->setDuration(96000);

    for (int64_t tick = 0; tick <= 96000; tick += 4800)
    {
        double pos = mt->tickToPosition(tick);
        int64_t recovered = mt->positionToTick(pos);
        // Allow 1 tick tolerance due to integer rounding
        EXPECT_NEAR(recovered, tick, 1) << "Roundtrip failed for tick=" << tick;
    }
}

TEST_F(MiniTimelineTest, InOutPoints)
{
    mt->setDuration(48000);

    mt->setInPoint(10000);
    EXPECT_TRUE(mt->hasInPoint());
    EXPECT_EQ(mt->inPoint(), 10000);

    mt->setOutPoint(40000);
    EXPECT_TRUE(mt->hasOutPoint());
    EXPECT_EQ(mt->outPoint(), 40000);

    // Negative input means "not set" (the -1 sentinel) — the widget
    // deliberately does NOT clamp negatives to 0 (see setInPoint).
    mt->setInPoint(-100);
    EXPECT_EQ(mt->inPoint(), -1);
    EXPECT_FALSE(mt->hasInPoint());

    // Positive overshoot IS clamped to the duration.
    mt->setOutPoint(99000);
    EXPECT_EQ(mt->outPoint(), 48000);
}

TEST_F(MiniTimelineTest, ClearInOutPoints)
{
    mt->setDuration(48000);
    mt->setInPoint(10000);
    mt->setOutPoint(40000);

    mt->clearInOutPoints();
    EXPECT_FALSE(mt->hasInPoint());
    EXPECT_FALSE(mt->hasOutPoint());
    EXPECT_EQ(mt->inPoint(), -1);
    EXPECT_EQ(mt->outPoint(), -1);
}

TEST_F(MiniTimelineTest, SelectedDuration)
{
    mt->setDuration(48000);

    // No in/out → full duration
    EXPECT_EQ(mt->selectedDuration(), 48000);

    // With in/out
    mt->setInPoint(10000);
    mt->setOutPoint(40000);
    EXPECT_EQ(mt->selectedDuration(), 30000);

    // In > Out → returns full duration
    mt->setInPoint(40000);
    mt->setOutPoint(10000);
    EXPECT_EQ(mt->selectedDuration(), 48000);
}

TEST_F(MiniTimelineTest, FrameRate)
{
    mt->setFrameRate(30.0);
    EXPECT_DOUBLE_EQ(mt->frameRate(), 30.0);

    mt->setFrameRate(60.0);
    EXPECT_DOUBLE_EQ(mt->frameRate(), 60.0);

    // Invalid FPS → default to 24
    mt->setFrameRate(0.0);
    EXPECT_DOUBLE_EQ(mt->frameRate(), 24.0);

    mt->setFrameRate(-10.0);
    EXPECT_DOUBLE_EQ(mt->frameRate(), 24.0);
}

TEST_F(MiniTimelineTest, ZeroDurationMapping)
{
    mt->setDuration(0);

    // positionToTick should return 0 for any position
    EXPECT_EQ(mt->positionToTick(0.0), 0);
    EXPECT_EQ(mt->positionToTick(200.0), 0);

    // tickToPosition should return margin for any tick
    EXPECT_DOUBLE_EQ(mt->tickToPosition(0), MiniTimeline::kMarginH);
    EXPECT_DOUBLE_EQ(mt->tickToPosition(48000), MiniTimeline::kMarginH);
}

TEST_F(MiniTimelineTest, SizeHints)
{
    QSize sh = mt->sizeHint();
    EXPECT_GE(sh.width(), 100);
    EXPECT_EQ(sh.height(), MiniTimeline::kBarHeight);

    QSize msh = mt->minimumSizeHint();
    EXPECT_GE(msh.width(), 100);
    EXPECT_EQ(msh.height(), MiniTimeline::kBarHeight);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Viewport tests
// ═════════════════════════════════════════════════════════════════════════════

class ViewportTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        vp = std::make_unique<Viewport>();
        vp->resize(640, 360);
    }

    std::unique_ptr<Viewport> vp;
};

TEST_F(ViewportTest, DefaultState)
{
    EXPECT_FALSE(vp->hasFrame());
    EXPECT_EQ(vp->frameWidth(), 0u);
    EXPECT_EQ(vp->frameHeight(), 0u);
    EXPECT_EQ(vp->fitMode(), ViewportFitMode::Fit);
    EXPECT_FALSE(vp->safeAreasVisible());
}

TEST_F(ViewportTest, DisplayRawFrame)
{
    // Create a 4x4 BGRA test frame
    std::vector<uint8_t> pixels(4 * 4 * 4, 128);
    vp->displayRaw(pixels.data(), 4, 4);

    EXPECT_TRUE(vp->hasFrame());
    EXPECT_EQ(vp->frameWidth(), 4u);
    EXPECT_EQ(vp->frameHeight(), 4u);
}

TEST_F(ViewportTest, DisplayCachedFrame)
{
    CachedFrame frame;
    frame.width  = 8;
    frame.height = 6;
    frame.stride = 8 * 4;
    frame.pixels.resize(8 * 6 * 4, 200);

    vp->displayFrame(frame);

    EXPECT_TRUE(vp->hasFrame());
    EXPECT_EQ(vp->frameWidth(), 8u);
    EXPECT_EQ(vp->frameHeight(), 6u);
}

TEST_F(ViewportTest, ClearFrame)
{
    // Display a frame first
    std::vector<uint8_t> pixels(4 * 4 * 4, 128);
    vp->displayRaw(pixels.data(), 4, 4);
    EXPECT_TRUE(vp->hasFrame());

    // Clear
    vp->clearFrame();
    EXPECT_FALSE(vp->hasFrame());
    EXPECT_EQ(vp->frameWidth(), 0u);
    EXPECT_EQ(vp->frameHeight(), 0u);
}

TEST_F(ViewportTest, DisplayEmptyFrame)
{
    CachedFrame frame;
    frame.width  = 0;
    frame.height = 0;

    vp->displayFrame(frame);
    EXPECT_FALSE(vp->hasFrame());
}

TEST_F(ViewportTest, DisplayNullData)
{
    vp->displayRaw(nullptr, 100, 100);
    EXPECT_FALSE(vp->hasFrame());
}

TEST_F(ViewportTest, FitModeSwitch)
{
    vp->setFitMode(ViewportFitMode::Fill);
    EXPECT_EQ(vp->fitMode(), ViewportFitMode::Fill);

    vp->setFitMode(ViewportFitMode::Actual);
    EXPECT_EQ(vp->fitMode(), ViewportFitMode::Actual);

    vp->setFitMode(ViewportFitMode::Fit);
    EXPECT_EQ(vp->fitMode(), ViewportFitMode::Fit);
}

// Fit mode reserves a 5% margin on EVERY side (kFitPadding = 0.90 in
// Viewport.cpp) so the frame never touches the monitor edges — matching
// Premiere's monitor styling.  The expectations below bake that in.
TEST_F(ViewportTest, FrameRectFitMode)
{
    constexpr double kFitPadding = 0.90;  // mirrors Viewport.cpp Fit mode

    // 640x360 widget, 1920x1080 frame (same aspect ratio 16:9)
    std::vector<uint8_t> pixels(1920 * 1080 * 4, 100);
    vp->displayRaw(pixels.data(), 1920, 1080);

    vp->setFitMode(ViewportFitMode::Fit);
    QRectF fr = vp->frameRect();

    // Aspect matches the widget, so the frame fills the padded box:
    // 640*0.9 = 576 wide, 360*0.9 = 324 tall, centered.
    EXPECT_NEAR(fr.width(), 640.0 * kFitPadding, 1.0);
    EXPECT_NEAR(fr.height(), 360.0 * kFitPadding, 1.0);
    EXPECT_NEAR(fr.x(), (640.0 - 640.0 * kFitPadding) / 2.0, 1.0);
    EXPECT_NEAR(fr.y(), (360.0 - 360.0 * kFitPadding) / 2.0, 1.0);
}

TEST_F(ViewportTest, FrameRectFitModeLetterbox)
{
    constexpr double kFitPadding = 0.90;  // mirrors Viewport.cpp Fit mode
    vp->resize(640, 640);  // Square widget

    // 1920x1080 frame in square widget → letterbox (horizontal bars).
    std::vector<uint8_t> pixels(1920 * 1080 * 4, 100);
    vp->displayRaw(pixels.data(), 1920, 1080);

    vp->setFitMode(ViewportFitMode::Fit);
    QRectF fr = vp->frameRect();

    // Width limited: 640*0.9 = 576, height = 576 * (1080/1920) = 324.
    EXPECT_NEAR(fr.width(), 640.0 * kFitPadding, 1.0);
    EXPECT_NEAR(fr.height(), 640.0 * kFitPadding * (1080.0 / 1920.0), 1.0);
    // Centered vertically: y = (640 - 324) / 2 = 158
    EXPECT_NEAR(fr.y(), (640.0 - 640.0 * kFitPadding * (1080.0 / 1920.0)) / 2.0, 1.0);
}

TEST_F(ViewportTest, FrameRectFitModePillarbox)
{
    vp->resize(640, 640);  // Square widget

    // 1080x1920 frame in square widget → pillarbox (vertical bars)
    std::vector<uint8_t> pixels(1080 * 1920 * 4, 100);
    vp->displayRaw(pixels.data(), 1080, 1920);

    vp->setFitMode(ViewportFitMode::Fit);
    QRectF fr = vp->frameRect();

    // Height limited: 640*0.9 = 576, width = 576 * (1080/1920) = 324.
    constexpr double kFitPadding = 0.90;  // mirrors Viewport.cpp Fit mode
    EXPECT_NEAR(fr.height(), 640.0 * kFitPadding, 1.0);
    EXPECT_NEAR(fr.width(), 640.0 * kFitPadding * (1080.0 / 1920.0), 1.0);
    // Centered horizontally: x = (640 - 324) / 2 = 158
    EXPECT_NEAR(fr.x(), (640.0 - 640.0 * kFitPadding * (1080.0 / 1920.0)) / 2.0, 1.0);
}

TEST_F(ViewportTest, FrameRectFillMode)
{
    vp->resize(640, 640);  // Square widget

    // 1920x1080 frame in square widget with Fill → should more than fill
    std::vector<uint8_t> pixels(1920 * 1080 * 4, 100);
    vp->displayRaw(pixels.data(), 1920, 1080);

    vp->setFitMode(ViewportFitMode::Fill);
    QRectF fr = vp->frameRect();

    // Height fills 640, width = 640 * (1920/1080) ≈ 1137.8
    EXPECT_NEAR(fr.height(), 640.0, 1.0);
    EXPECT_GT(fr.width(), 640.0);
}

TEST_F(ViewportTest, FrameRectActualMode)
{
    vp->resize(640, 360);

    std::vector<uint8_t> pixels(320 * 240 * 4, 100);
    vp->displayRaw(pixels.data(), 320, 240);

    vp->setFitMode(ViewportFitMode::Actual);
    QRectF fr = vp->frameRect();

    // Frame should be at 1:1 pixel size, centered
    EXPECT_NEAR(fr.width(), 320.0, 0.1);
    EXPECT_NEAR(fr.height(), 240.0, 0.1);
    EXPECT_NEAR(fr.x(), (640.0 - 320.0) / 2.0, 0.1);
    EXPECT_NEAR(fr.y(), (360.0 - 240.0) / 2.0, 0.1);
}

TEST_F(ViewportTest, CoordinateMappingNoFrame)
{
    QPointF fp = vp->widgetToFrame(QPointF(100, 100));
    EXPECT_LT(fp.x(), 0);  // Should return (-1,-1)
    EXPECT_LT(fp.y(), 0);
}

TEST_F(ViewportTest, CoordinateMappingWithFrame)
{
    vp->resize(640, 360);

    // 640x360 frame in 640x360 widget → perfect fit
    std::vector<uint8_t> pixels(640 * 360 * 4, 100);
    vp->displayRaw(pixels.data(), 640, 360);

    vp->setFitMode(ViewportFitMode::Fit);

    // Center of widget → center of frame
    QPointF fp = vp->widgetToFrame(QPointF(320, 180));
    EXPECT_NEAR(fp.x(), 320.0, 2.0);
    EXPECT_NEAR(fp.y(), 180.0, 2.0);

    // Top-left → (0,0) in frame
    QPointF tl = vp->widgetToFrame(QPointF(0, 0));
    EXPECT_NEAR(tl.x(), 0.0, 2.0);
    EXPECT_NEAR(tl.y(), 0.0, 2.0);
}

TEST_F(ViewportTest, CoordinateRoundtrip)
{
    vp->resize(640, 360);

    std::vector<uint8_t> pixels(1920 * 1080 * 4, 100);
    vp->displayRaw(pixels.data(), 1920, 1080);

    vp->setFitMode(ViewportFitMode::Fit);

    QPointF framePos(960.0, 540.0);  // Center of frame
    QPointF widgetPos = vp->frameToWidget(framePos);
    QPointF recovered = vp->widgetToFrame(widgetPos);

    EXPECT_NEAR(recovered.x(), framePos.x(), 2.0);
    EXPECT_NEAR(recovered.y(), framePos.y(), 2.0);
}

TEST_F(ViewportTest, SafeAreas)
{
    EXPECT_FALSE(vp->safeAreasVisible());
    vp->setSafeAreasVisible(true);
    EXPECT_TRUE(vp->safeAreasVisible());
    vp->setSafeAreasVisible(false);
    EXPECT_FALSE(vp->safeAreasVisible());
}

TEST_F(ViewportTest, SizeHints)
{
    QSize sh = vp->sizeHint();
    EXPECT_GE(sh.width(), 160);
    EXPECT_GE(sh.height(), 90);

    QSize msh = vp->minimumSizeHint();
    EXPECT_EQ(msh.width(), 160);
    EXPECT_EQ(msh.height(), 90);
}

TEST_F(ViewportTest, FrameRectEmptyWhenNoFrame)
{
    QRectF fr = vp->frameRect();
    EXPECT_TRUE(fr.isEmpty() || fr.isNull());
}

TEST_F(ViewportTest, OverlayText)
{
    // Should not crash, overlay text is purely visual
    vp->setOverlayText(QStringLiteral("00:00:01:00"));
    // No assertion needed — just verify no crash
}

TEST_F(ViewportTest, DisplayWithCustomStride)
{
    // Stride > width * 4 (padding at end of each row)
    uint32_t w = 10, h = 10;
    uint32_t stride = 48; // 10*4=40, plus 8 padding
    std::vector<uint8_t> pixels(stride * h, 128);
    vp->displayRaw(pixels.data(), w, h, stride);

    EXPECT_TRUE(vp->hasFrame());
    EXPECT_EQ(vp->frameWidth(), 10u);
    EXPECT_EQ(vp->frameHeight(), 10u);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SourceMonitor tests
// ═════════════════════════════════════════════════════════════════════════════

class SourceMonitorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        sm = std::make_unique<SourceMonitor>();
    }

    std::unique_ptr<SourceMonitor> sm;
};

TEST_F(SourceMonitorTest, DefaultState)
{
    EXPECT_FALSE(sm->hasClip());
    EXPECT_EQ(sm->mediaHandle(), 0u);
    EXPECT_EQ(sm->currentTick(), 0);
    EXPECT_FALSE(sm->hasInPoint());
    EXPECT_FALSE(sm->hasOutPoint());
    EXPECT_EQ(sm->frameCount(), 0);
    EXPECT_EQ(sm->clipDuration(), 0);
}

TEST_F(SourceMonitorTest, ControllerExists)
{
    EXPECT_NE(sm->controller(), nullptr);
}

TEST_F(SourceMonitorTest, SourceRegionDefault)
{
    auto region = sm->sourceRegion();
    EXPECT_EQ(region.mediaHandle, 0u);
    EXPECT_EQ(region.sourceIn, 0);
    EXPECT_EQ(region.sourceOut, 0);
    EXPECT_EQ(region.duration, 0);
}

TEST_F(SourceMonitorTest, ClearClipResets)
{
    sm->clearClip();
    EXPECT_FALSE(sm->hasClip());
    EXPECT_EQ(sm->mediaHandle(), 0u);
    EXPECT_EQ(sm->frameCount(), 0);
    EXPECT_EQ(sm->clipDuration(), 0);
}

TEST_F(SourceMonitorTest, LoadClipNullPool)
{
    sm->loadClip(1, nullptr);
    EXPECT_FALSE(sm->hasClip());
}

TEST_F(SourceMonitorTest, SizeHint)
{
    QSize sh = sm->sizeHint();
    EXPECT_GE(sh.width(), 200);
    EXPECT_GE(sh.height(), 200);
}

TEST_F(SourceMonitorTest, FullSequencePreviewRequestsFullCanvasAndTier)
{
    sm->setPlaybackResolutionIndex(0);

    uint32_t capturedW = 0;
    uint32_t capturedH = 0;
    ResolutionTier capturedTier = ResolutionTier::Quarter;
    bool capturedScrub = true;
    bool capturedStill = false;

    sm->loadSequence(
        0, QStringLiteral("1080p sequence"), 48000, 30.0,
        1920, 1080,
        [&](int64_t, uint32_t w, uint32_t h, bool scrub,
            ResolutionTier tier, bool still) -> std::shared_ptr<CachedFrame> {
            capturedW = w;
            capturedH = h;
            capturedTier = tier;
            capturedScrub = scrub;
            capturedStill = still;
            return nullptr;
        });

    EXPECT_EQ(capturedW, 1920u);
    EXPECT_EQ(capturedH, 1080u);
    EXPECT_EQ(capturedTier, ResolutionTier::Full);
    EXPECT_FALSE(capturedScrub);
    EXPECT_TRUE(capturedStill);
}

// ═════════════════════════════════════════════════════════════════════════════
//  ProgramMonitor tests
// ═════════════════════════════════════════════════════════════════════════════

class ProgramMonitorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        pm = std::make_unique<ProgramMonitor>();
    }

    std::unique_ptr<ProgramMonitor> pm;
};

TEST_F(ProgramMonitorTest, DefaultState)
{
    EXPECT_EQ(pm->controller(), nullptr);
    EXPECT_EQ(pm->timeline(), nullptr);
    EXPECT_EQ(pm->outputWidth(), 1920u);
    EXPECT_EQ(pm->outputHeight(), 1080u);
}

TEST_F(ProgramMonitorTest, SetOutputResolution)
{
    pm->setOutputResolution(3840, 2160);
    EXPECT_EQ(pm->outputWidth(), 3840u);
    EXPECT_EQ(pm->outputHeight(), 2160u);
}

TEST_F(ProgramMonitorTest, SetController)
{
    PlaybackController ctrl;
    pm->setController(&ctrl);
    EXPECT_EQ(pm->controller(), &ctrl);
}

TEST_F(ProgramMonitorTest, SetTimeline)
{
    Timeline tl;
    pm->setTimeline(&tl);
    EXPECT_EQ(pm->timeline(), &tl);
}

TEST_F(ProgramMonitorTest, ViewportAccess)
{
    EXPECT_NE(pm->viewport(), nullptr);
}

TEST_F(ProgramMonitorTest, MiniTimelineAccess)
{
    EXPECT_NE(pm->miniTimeline(), nullptr);
}

TEST_F(ProgramMonitorTest, SetCompositeCallback)
{
    bool called = false;
    pm->setCompositeCallback([&](int64_t /*tick*/, uint32_t /*w*/, uint32_t /*h*/,
                                 bool /*scrub*/, bool /*still*/) -> std::shared_ptr<CachedFrame> {
        called = true;
        return nullptr;
    });

    // The callback is stored but not called without a controller
    EXPECT_FALSE(called);
}

// refresh() is ASYNCHRONOUS: it posts a frame request to the playback
// pipeline (PlaybackScheduler → FrameProducer thread), which invokes the
// composite callback on the PRODUCER thread — never synchronously on the
// UI thread (see the "SYNC PATH" comment in ProgramMonitor::updateDisplay
// and PLAYBACK_ARCHITECTURE.md).  These tests poll with a timeout.
// Request coalescing/scheduling policy itself is covered by
// test_playback_scheduler.
namespace {
bool pollUntil(const std::function<bool()>& done, int timeoutMs = 3000)
{
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 10) {
        if (done()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(10);
    }
    return done();
}
} // namespace

TEST_F(ProgramMonitorTest, RefreshInvokesCallbackAsync)
{
    PlaybackController ctrl;
    pm->setController(&ctrl);

    std::atomic<int64_t> capturedTick{-100};
    pm->setCompositeCallback([&](int64_t tick, uint32_t /*w*/, uint32_t /*h*/,
                                 bool /*scrub*/, bool /*still*/) -> std::shared_ptr<CachedFrame> {
        capturedTick.store(tick);
        return nullptr;
    });

    pm->refresh();
    ASSERT_TRUE(pollUntil([&] { return capturedTick.load() != -100; }))
        << "composite callback never invoked by the producer thread";
    EXPECT_EQ(capturedTick.load(), 0);  // Controller starts at tick 0
}

TEST_F(ProgramMonitorTest, PausedFullRequests1080pStillQuality)
{
    PlaybackController ctrl;
    pm->setController(&ctrl);
    pm->setOutputResolution(1920, 1080);
    pm->setPlaybackResolutionIndex(0);

    std::atomic<uint32_t> capturedW{0};
    std::atomic<uint32_t> capturedH{0};
    std::atomic<bool> capturedScrub{true};
    std::atomic<bool> capturedStill{false};
    pm->setCompositeCallback(
        [&](int64_t, uint32_t w, uint32_t h, bool scrub,
            bool still) -> std::shared_ptr<CachedFrame> {
            capturedW.store(w);
            capturedH.store(h);
            capturedScrub.store(scrub);
            capturedStill.store(still);
            return nullptr;
        });

    pm->refresh();
    ASSERT_TRUE(pollUntil([&] { return capturedW.load() != 0; }));
    EXPECT_EQ(capturedW.load(), 1920u);
    EXPECT_EQ(capturedH.load(), 1080u);
    EXPECT_FALSE(capturedScrub.load());
    EXPECT_TRUE(capturedStill.load());
}

TEST_F(ProgramMonitorTest, RefreshWithFrameData)
{
    PlaybackController ctrl;
    pm->setController(&ctrl);

    // Create a test frame
    auto testFrame = std::make_shared<CachedFrame>();
    testFrame->width  = 320;
    testFrame->height = 240;
    testFrame->stride = 320 * 4;
    testFrame->pixels.resize(320 * 240 * 4, 128);

    pm->setCompositeCallback([&](int64_t /*tick*/, uint32_t /*w*/, uint32_t /*h*/,
                                 bool /*scrub*/, bool /*still*/) -> std::shared_ptr<CachedFrame> {
        return testFrame;
    });

    pm->refresh();

    // The produced frame travels producer → presenter → present callback
    // → viewport, with the poll timer marshalling onto the UI thread —
    // poll the event loop until it lands.
    ASSERT_TRUE(pollUntil([&] { return pm->viewport()->hasFrame(); }))
        << "frame never reached the viewport through the async pipeline";
    EXPECT_EQ(pm->viewport()->frameWidth(), 320u);
    EXPECT_EQ(pm->viewport()->frameHeight(), 240u);
}

// Explicit refresh() FORCES a re-render even at an unchanged tick (it
// resets m_lastRenderedTick — that is its purpose after edits).  The
// same-tick dedup only applies to passive poll cycles.
TEST_F(ProgramMonitorTest, RefreshForcesRerenderAtSameTick)
{
    PlaybackController ctrl;
    pm->setController(&ctrl);

    std::atomic<int> callCount{0};
    pm->setCompositeCallback([&](int64_t /*tick*/, uint32_t /*w*/, uint32_t /*h*/,
                                 bool /*scrub*/, bool /*still*/) -> std::shared_ptr<CachedFrame> {
        callCount.fetch_add(1);
        return nullptr;
    });

    pm->refresh();
    ASSERT_TRUE(pollUntil([&] { return callCount.load() >= 1; }));
    const int afterFirst = callCount.load();

    // The playhead hasn't moved, but an explicit refresh still forces a
    // re-render (that's what makes edits show up immediately).
    pm->refresh();
    ASSERT_TRUE(pollUntil([&] { return callCount.load() > afterFirst; }))
        << "explicit refresh at the same tick did not force a re-render";
}

TEST_F(ProgramMonitorTest, SizeHint)
{
    QSize sh = pm->sizeHint();
    EXPECT_GE(sh.width(), 200);
    EXPECT_GE(sh.height(), 200);
}

TEST_F(ProgramMonitorTest, TimelineUpdatesMinitimeline)
{
    Timeline tl;
    (void)tl.addVideoTrack("V1");
    // (Can't add clips easily without full Clip creation, but duration should be 0 initially)
    pm->setTimeline(&tl);

    // Mini-timeline should reflect timeline duration
    EXPECT_EQ(pm->miniTimeline()->duration(), tl.duration());
}

TEST_F(ProgramMonitorTest, PollStartStop)
{
    // Should not crash
    pm->startPolling(32);
    pm->stopPolling();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Integration tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(MonitorIntegration, MiniTimelineVariousWidths)
{
    MiniTimeline mt;
    mt.setDuration(96000); // 2 seconds

    // Test at various widget widths
    int widths[] = { 100, 200, 400, 800, 1600 };
    for (int w : widths)
    {
        mt.resize(w, MiniTimeline::kBarHeight);

        // Tick 0 → position at margin
        double pos0 = mt.tickToPosition(0);
        EXPECT_DOUBLE_EQ(pos0, MiniTimeline::kMarginH)
            << "Width=" << w;

        // Full duration → position at widget_width - margin
        double posEnd = mt.tickToPosition(96000);
        EXPECT_NEAR(posEnd, w - MiniTimeline::kMarginH, 0.1)
            << "Width=" << w;

        // Roundtrip at mid-point
        double posMid = mt.tickToPosition(48000);
        int64_t tickMid = mt.positionToTick(posMid);
        EXPECT_NEAR(tickMid, 48000, 1) << "Width=" << w;
    }
}

TEST(MonitorIntegration, ViewportMultipleFrameSizes)
{
    Viewport vp;
    vp.resize(640, 360);

    struct Case { uint32_t w, h; };
    Case cases[] = {
        {1920, 1080}, {1280, 720}, {3840, 2160},
        {100, 100}, {4, 4}, {1, 1}
    };

    for (const auto& c : cases)
    {
        std::vector<uint8_t> px(c.w * c.h * 4, 100);
        vp.displayRaw(px.data(), c.w, c.h);

        EXPECT_TRUE(vp.hasFrame()) << c.w << "x" << c.h;
        EXPECT_EQ(vp.frameWidth(), c.w);
        EXPECT_EQ(vp.frameHeight(), c.h);

        QRectF fr = vp.frameRect();
        EXPECT_GT(fr.width(), 0) << c.w << "x" << c.h;
        EXPECT_GT(fr.height(), 0) << c.w << "x" << c.h;
    }
}

TEST(MonitorIntegration, ProgramMonitorWithTimelineInOut)
{
    ProgramMonitor pm;
    Timeline tl;

    tl.setInPoint(10000);
    tl.setOutPoint(40000);

    pm.setTimeline(&tl);

    // Timeline has no clips, so duration=0. The in/out points from the timeline
    // are clamped by MiniTimeline to [0, duration]. Since duration=0, they become 0.
    // This test verifies the wiring: setTimeline propagates in/out to MiniTimeline.
    // The clamping is correct behavior.
    EXPECT_EQ(pm.miniTimeline()->inPoint(), pm.miniTimeline()->clampTick(10000));
    EXPECT_EQ(pm.miniTimeline()->outPoint(), pm.miniTimeline()->clampTick(40000));
}
