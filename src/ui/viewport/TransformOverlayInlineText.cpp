/*
 * TransformOverlayInlineText.cpp - On-canvas rich-text editing for the
 * transform overlay: the inline QPlainTextEdit, its selection/caret geometry,
 * and the per-run formatting operations (font, size, tracking, fill, stroke,
 * shadow, paragraph settings) driven by the Graphics editor.
 * Split from TransformOverlayInput.cpp, which keeps mouse/wheel handling.
 */
#include "viewport/TransformOverlayWidget.h"
#include "viewport/VulkanViewport.h"
#include "timeline/OpacityMask.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/Keyframe.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "command/commands/KeyframeCmds.h"
#include "Theme.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QCoreApplication>
#include <QApplication>
#include <QScreen>
#include <QMenu>
#include <QAction>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QScrollBar>
#include <QKeyEvent>
#include <QKeySequence>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextOption>
#include <QTextBlockFormat>
#include <QSignalBlocker>
#include <QPointer>
#include <QTimer>
#include <QStyleHints>
#include <algorithm>
#include <limits>
#include <cmath>
#include <spdlog/spdlog.h>

namespace rt {

namespace {
constexpr int kInlineLeadingProperty = QTextFormat::UserProperty + 1;
constexpr int kInlineFontStyleProperty = QTextFormat::UserProperty + 2;
constexpr int kInlineKerningProperty = QTextFormat::UserProperty + 3;
constexpr int kInlineTabWidthProperty = QTextFormat::UserProperty + 4;
constexpr int kInlineTsumeProperty = QTextFormat::UserProperty + 5;
constexpr int kInlineFauxBoldProperty = QTextFormat::UserProperty + 6;
constexpr int kInlineFauxItalicProperty = QTextFormat::UserProperty + 7;
constexpr int kInlineFillEnabledProperty = QTextFormat::UserProperty + 8;
constexpr int kInlineStrokeEnabledProperty = QTextFormat::UserProperty + 9;
constexpr int kInlineStrokePositionProperty = QTextFormat::UserProperty + 10;
constexpr int kInlineShadowEnabledProperty = QTextFormat::UserProperty + 11;
constexpr int kInlineShadowColorProperty = QTextFormat::UserProperty + 12;
constexpr int kInlineShadowDistanceProperty = QTextFormat::UserProperty + 13;
constexpr int kInlineShadowAngleProperty = QTextFormat::UserProperty + 14;
constexpr int kInlineShadowSoftnessProperty = QTextFormat::UserProperty + 15;
constexpr int kInlineShadowOpacityProperty = QTextFormat::UserProperty + 16;
constexpr int kInlineBackgroundEnabledProperty = QTextFormat::UserProperty + 17;
constexpr int kInlineBackgroundPaddingProperty = QTextFormat::UserProperty + 18;
constexpr int kInlineFillColorProperty = QTextFormat::UserProperty + 19;
constexpr int kInlineStrokeColorProperty = QTextFormat::UserProperty + 20;
constexpr int kInlineStrokeWidthProperty = QTextFormat::UserProperty + 21;
constexpr int kInlineBackgroundColorProperty = QTextFormat::UserProperty + 22;
constexpr int kInlineTrackingProperty = QTextFormat::UserProperty + 23;
constexpr int kInlineActualWeightProperty = QTextFormat::UserProperty + 24;
constexpr int kInlineActualItalicProperty = QTextFormat::UserProperty + 25;

void updateInlineBlockLeading(QPlainTextEdit* edit, float baseLeading,
                              double pointScale)
{
    if (!edit) return;
    const QSignalBlocker blocker(edit);
    for (QTextBlock block = edit->document()->begin();
         block.isValid(); block = block.next()) {
        float lineLeading = 0.0f;
        bool haveFragment = false;
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const QTextCharFormat format = fragment.charFormat();
            const float fragmentLeading = format.hasProperty(
                kInlineLeadingProperty)
                ? format.property(kInlineLeadingProperty).toFloat()
                : baseLeading;
            lineLeading = std::max(lineLeading, fragmentLeading);
            haveFragment = true;
        }
        if (!haveFragment) lineLeading = baseLeading;
        QTextCursor cursor(block);
        QTextBlockFormat blockFormat = cursor.blockFormat();
        blockFormat.setLineHeight(
            std::max(0.0, static_cast<double>(lineLeading) * pointScale),
            QTextBlockFormat::LineDistanceHeight);
        cursor.setBlockFormat(blockFormat);
    }
}

void updateInlineDocumentTabWidth(QPlainTextEdit* edit, float baseTabWidth,
                                   double pointScale)
{
    if (!edit) return;
    float tabWidth = baseTabWidth;
    for (QTextBlock block = edit->document()->begin();
         block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const QTextCharFormat format = fragment.charFormat();
            if (format.hasProperty(kInlineTabWidthProperty))
                tabWidth = std::max(tabWidth,
                    format.property(kInlineTabWidthProperty).toFloat());
        }
    }
    QTextOption option = edit->document()->defaultTextOption();
    option.setTabStopDistance(std::max(1.0,
        static_cast<double>(tabWidth) * pointScale));
    edit->document()->setDefaultTextOption(option);
}

QFont inlineRendererFont(const QTextCharFormat& format, double pointScale,
                         double sizeScale = 1.0)
{
    const QFont editorFont = format.font();
    QFont font(editorFont.family());
    const double safeScale = pointScale > 1.0e-6 ? pointScale : 1.0;
    const double editorPoints = editorFont.pointSizeF() > 0.0
        ? editorFont.pointSizeF() : 1.0;
    font.setPointSizeF(std::max(1.0,
        editorPoints / safeScale * sizeScale));
    font.setWeight(static_cast<QFont::Weight>(std::clamp(
        format.hasProperty(kInlineActualWeightProperty)
            ? format.property(kInlineActualWeightProperty).toInt()
            : static_cast<int>(editorFont.weight()), 1, 1000)));
    font.setItalic(format.hasProperty(kInlineActualItalicProperty)
        ? format.property(kInlineActualItalicProperty).toBool()
        : editorFont.italic());
    if (format.hasProperty(kInlineFontStyleProperty)) {
        const QString style = format.property(
            kInlineFontStyleProperty).toString();
        if (!style.isEmpty()) font.setStyleName(style);
    }
    if (format.property(kInlineFauxBoldProperty).toBool())
        font.setWeight(static_cast<QFont::Weight>(
            std::max(700, static_cast<int>(font.weight()))));
    if (format.property(kInlineFauxItalicProperty).toBool())
        font.setItalic(true);
    if (format.verticalAlignment() == QTextCharFormat::AlignSuperScript
        || format.verticalAlignment() == QTextCharFormat::AlignSubScript) {
        font.setPointSizeF(std::max(1.0, font.pointSizeF() * 0.6));
    }
    const double tracking = format.hasProperty(kInlineTrackingProperty)
        ? format.property(kInlineTrackingProperty).toDouble() : 0.0;
    const double kerning = format.hasProperty(kInlineKerningProperty)
        ? format.property(kInlineKerningProperty).toDouble() : 0.0;
    font.setLetterSpacing(QFont::AbsoluteSpacing, tracking + kerning);
    const double tsume = format.hasProperty(kInlineTsumeProperty)
        ? format.property(kInlineTsumeProperty).toDouble() : 0.0;
    font.setStretch(std::clamp(static_cast<int>(std::lround(
        100.0 * std::clamp(1.0 - tsume / 100.0, 0.1, 1.0))), 1, 4000));
    font.setCapitalization(QFont::MixedCase);
    font.setUnderline(false);
    return font;
}

double inlineRendererAdvance(const QTextCharFormat& format,
                             const QString& source, double pointScale)
{
    if (source.isEmpty()) return 0.0;
    const QFont editorFont = format.font();
    const bool allCaps = editorFont.capitalization() == QFont::AllUppercase;
    const bool smallCaps = editorFont.capitalization() == QFont::SmallCaps;
    const double tabWidth = format.hasProperty(kInlineTabWidthProperty)
        ? format.property(kInlineTabWidthProperty).toDouble() : 48.0;

    double width = 0.0;
    QString run;
    bool runSmall = false;
    auto flush = [&]() {
        if (run.isEmpty()) return;
        width += QFontMetricsF(inlineRendererFont(
            format, pointScale, runSmall ? 0.8 : 1.0)).horizontalAdvance(run);
        run.clear();
    };
    for (int i = 0; i < source.size(); ++i) {
        const QChar ch = source.at(i);
        if (ch == QChar('\t')) {
            flush();
            width += tabWidth;
            continue;
        }
        const bool thisSmall = smallCaps && !allCaps && ch.isLower();
        if (!run.isEmpty() && thisSmall != runSmall) flush();
        runSmall = thisSmall;
        QString glyph(ch);
        if (ch.isHighSurrogate() && i + 1 < source.size()
            && source.at(i + 1).isLowSurrogate()) {
            glyph += source.at(++i);
        }
        run += (allCaps || smallCaps) ? glyph.toUpper() : glyph;
    }
    flush();
    return width;
}

double inlineBlockAdvance(const QTextBlock& block, int utf16Length,
                          double pointScale,
                          const QTextCharFormat& fallbackFormat)
{
    const int blockLength = static_cast<int>(block.text().size());
    const int wanted = std::clamp(utf16Length, 0, blockLength);
    double width = 0.0;
    bool measured = false;
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (!fragment.isValid()) continue;
        const int localStart = fragment.position() - block.position();
        if (localStart >= wanted) break;
        const int take = std::min(fragment.length(), wanted - localStart);
        if (take <= 0) continue;
        width += inlineRendererAdvance(fragment.charFormat(),
            fragment.text().left(take), pointScale);
        measured = true;
    }
    if (!measured && wanted > 0)
        width = inlineRendererAdvance(fallbackFormat,
            block.text().left(wanted), pointScale);
    return width;
}
}

bool TransformOverlayWidget::isInlineTextEditing() const noexcept
{
    return m_inlineTextEdit && m_inlineTextEdit->isVisible();
}

void TransformOverlayWidget::beginInlineTextEdit(const QString& initial,
                                                  const QString& fontFamily,
                                                  float fontSizeRef,
                                                  int fontWeight,
                                                  bool italic,
                                                  const QColor& textColor,
                                                  float horizontalStretch,
                                                  Qt::Alignment hAlignFlag,
                                                  const std::vector<TextStyleRun>& styleRuns,
                                                  float verticalScale,
                                                  bool allCaps,
                                                  bool smallCaps,
                                                  float tracking,
                                                  float baselineShift,
                                                  float leading,
                                                  const TextRunAppearance& baseAppearance,
                                                  const std::vector<TextParagraphStyle>& paragraphStyles,
                                                  const QString& fontStyle,
                                                  float kerning,
                                                  float tabWidth,
                                                  float tsume,
                                                  bool fauxBold,
                                                  bool fauxItalic,
                                                  bool underline,
                                                  bool superscript,
                                                  bool subscript,
                                                  bool rightToLeft,
                                                  bool paragraphBox)
{
    Q_UNUSED(textColor);
    const uint64_t editSession = ++m_inlineEditSession;
    m_inlineEditorHasFocused = false;
    m_inlineEditorFocusSettling = true;
    m_inlinePointerRerouted = false;
    spdlog::debug("[INLINE-TEXT] begin session={} textLength={} font='{}'",
                 editSession, initial.size(), fontFamily.toStdString());

    // AABB of the selected layer's transform box (widget coords). We use
    // the LEFT / RIGHT / CENTER X coordinates of this box (depending on
    // hAlignFlag) as the anchor the editor sticks to as the user types —
    // so center-aligned text doesn't drift left and left-aligned text
    // doesn't get centered.
    double leftX, centerX, rightX, topY, centerY;
    {
        QPointF c[4];
        // Inline input belongs to the renderer's logical line box, not the
        // deliberately padded transform-handle bounds.
        TransformOverlayInfo editLayout = m_overlay;
        if (editLayout.useContentRect && editLayout.useTextLayoutRect) {
            editLayout.contentL = editLayout.textLayoutL;
            editLayout.contentT = editLayout.textLayoutT;
            editLayout.contentR = editLayout.textLayoutR;
            editLayout.contentB = editLayout.textLayoutB;
        }
        computeOverlayCornersFor(editLayout, c);
        double minX = c[0].x(), minY = c[0].y(), maxX = c[0].x(), maxY = c[0].y();
        for (int i = 1; i < 4; ++i) {
            minX = std::min(minX, c[i].x()); maxX = std::max(maxX, c[i].x());
            minY = std::min(minY, c[i].y()); maxY = std::max(maxY, c[i].y());
        }
        if (maxX - minX < 1.0 || maxY - minY < 1.0) {
            leftX   = width()  * 0.5;
            centerX = width()  * 0.5;
            rightX  = width()  * 0.5;
            topY    = height() * 0.5;
            centerY = height() * 0.5;
        } else {
            leftX   = minX;
            centerX = (minX + maxX) * 0.5;
            rightX  = maxX;
            topY    = minY;
            centerY = (minY + maxY) * 0.5;
        }
    }
    m_inlineEditAlignH = hAlignFlag;

    // Hide the transform overlay while editing text inline so the box and
    // anchor point don't appear in a mismatched spot (Premiere Pro behavior:
    // the transform gizmo disappears while you're editing text in the
    // Program Monitor).
    m_savedOverlayBeforeEdit = m_overlay;
    m_overlay.visible = false;
    m_preEditOriginalText = initial.toStdString();
    m_originalInlineTextStyles = styleRuns;
    m_committedInlineTextStyles = styleRuns;
    m_originalInlineParagraphStyles = paragraphStyles;
    m_committedInlineParagraphStyles = paragraphStyles;
    m_inlineBaseFontFamily = fontFamily;
    m_inlineBaseFontSize = fontSizeRef;
    m_inlineBaseFontWeight = fontWeight;
    m_inlineBaseItalic = italic;
    m_inlineBaseAllCaps = allCaps;
    m_inlineBaseSmallCaps = smallCaps;
    m_inlineBaseTracking = tracking;
    m_inlineBaseBaselineShift = baselineShift;
    m_inlineBaseLeading = leading;
    m_inlineBaseFontStyle = fontStyle;
    m_inlineBaseKerning = kerning;
    m_inlineBaseTabWidth = tabWidth;
    m_inlineBaseTsume = tsume;
    m_inlineBaseFauxBold = fauxBold;
    m_inlineBaseFauxItalic = fauxItalic;
    m_inlineBaseUnderline = underline;
    m_inlineBaseSuperscript = superscript;
    m_inlineBaseSubscript = subscript;
    m_inlineBaseRightToLeft = rightToLeft;
    m_inlineUsesParagraphBox = paragraphBox;
    m_inlineBaseAppearance = baseAppearance;
    update();

    if (!m_inlineTextEdit) {
        // Independent top-level frameless window. A child of this overlay
        // inherits its WindowDoesNotAcceptFocus, so it could never accept
        // keyboard input; a Tool window parented to the overlay has the
        // same issue plus loses focus when the app deactivates. A real
        // top-level Window with no parent is the only reliable way to get
        // both visibility above the native Vulkan surface AND keyboard
        // focus. Ownership: we delete it in the overlay destructor.
        //
        // QPlainTextEdit provides normal multi-line editing. Return inserts a
        // newline; Ctrl/Cmd+Return commits the monitor edit.
        m_inlineTextEdit = new QPlainTextEdit(nullptr);
        m_inlineTextEdit->setWindowFlags(Qt::Window
                                         | Qt::FramelessWindowHint
                                         | Qt::WindowStaysOnTopHint
                                         | Qt::NoDropShadowWindowHint);
        // WA_TranslucentBackground is THE attribute that lets rgba(...)
        // in the stylesheet actually composite over the screen. Without
        // it the top-level widget has an opaque backing surface and the
        // rgba alpha just blends within that surface — i.e. you get a
        // solid grey/black box no matter what alpha you specify.
        m_inlineTextEdit->setAttribute(Qt::WA_TranslucentBackground, true);
        m_inlineTextEdit->setAttribute(Qt::WA_NoSystemBackground, true);
        m_inlineTextEdit->setAttribute(Qt::WA_ShowWithoutActivating, false);
        m_inlineTextEdit->setAttribute(Qt::WA_DeleteOnClose, false);
        m_inlineTextEdit->setObjectName(QStringLiteral("inlineTextEdit"));
        // The editor is input/selection infrastructure only. Its native caret
        // uses the editor's independently hinted glyph positions and leaks
        // through a transparent foreground on Windows, creating a second,
        // misaligned blinking line. The compositor-aligned caret below is the
        // sole visible insertion indicator.
        m_inlineTextEdit->setCursorWidth(0);
        m_inlineCaretBlinkTimer = new QTimer(this);
        connect(m_inlineCaretBlinkTimer, &QTimer::timeout, this, [this]() {
            if (!isInlineTextEditing()) return;
            m_inlineCaretVisible = !m_inlineCaretVisible;
            update();
        });
        // No document margin — text hugs the edge so the editor's glyph
        // origin matches the renderer's exactly. Any padding here would
        // shift the live text away from the original rendered position.
        m_inlineTextEdit->document()->setDocumentMargin(0);
        // Both Qt's QFrame border AND scrollbars would visually create a
        // "box" over the video (the thing the user reported as a stray
        // tiny transform box). Disable both so the editor is invisible
        // except for the glyphs and the selection highlight.
        m_inlineTextEdit->setFrameStyle(QFrame::NoFrame);
        m_inlineTextEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_inlineTextEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // Do not center-scroll when a later line grows. The style-aware
        // auto-grow below gives point text enough room while leaving Qt's
        // normal wrapping available for paragraph-box text.
        m_inlineTextEdit->setCenterOnScroll(false);
        // Plain text only — no rich-text paste surprises.
        m_inlineTextEdit->setTabChangesFocus(false);

        // Key/focus events are intercepted by this overlay's application-wide
        // event filter. Installing the same filter on the editor as well would
        // deliver FocusIn/FocusOut twice.
        connect(m_inlineTextEdit, &QPlainTextEdit::selectionChanged,
                this, &TransformOverlayWidget::notifyInlineTextSelectionFormat);
        connect(m_inlineTextEdit, &QPlainTextEdit::cursorPositionChanged,
                this, &TransformOverlayWidget::notifyInlineTextSelectionFormat);
        connect(m_inlineTextEdit, &QPlainTextEdit::cursorPositionChanged,
                this, [this]() {
            // Typing or clicking restarts the blink cycle with the caret on,
            // matching native editor behaviour.
            m_inlineCaretVisible = true;
            if (m_inlineCaretBlinkTimer && m_inlineCaretBlinkTimer->isActive())
                m_inlineCaretBlinkTimer->start();
            update();
        });
        connect(m_inlineTextEdit->document(), &QTextDocument::contentsChanged,
                this, [this]() {
            if (m_initializingInlineText || m_committingInlineText
                || !m_inlineTextEdit || !m_inlineTextEdit->isVisible()) {
                return;
            }
            emit inlineTextPreviewChanged(m_inlineTextEdit->toPlainText());
        });

        // Auto-grow as the user types: QPlainTextEdit doesn't auto-expand.
        // Recompute the screen geometry from the current text's pixel size
        // and re-anchor by alignment so the editor's text stays where the
        // renderer would draw it: left-aligned text grows to the right,
        // right-aligned grows to the left, center-aligned grows both ways.
        connect(m_inlineTextEdit, &QPlainTextEdit::textChanged, this,
                [this]() {
            if (!m_inlineTextEdit) return;
            // Character formatting lives in QTextFragments; the base-font
            // calculation below is retained only for the one-time document
            // initialization.  Normal edits must use the style-aware path or
            // a large selected run gets clipped and the editor scrolls the
            // preceding text out of view.
            resizeInlineTextEditorToDocument();
            if (!m_initializingInlineText) return;
            QFontMetricsF fm(m_inlineTextEdit->font(), m_inlineTextEdit);
            const QString t = m_inlineTextEdit->toPlainText();

            // Width: longest line + slack for caret.
            double maxLineW = 0.0;
            const QStringList lines = t.split(QChar('\n'));
            for (const auto& line : lines)
                maxLineW = std::max(maxLineW, fm.horizontalAdvance(line));
            double slackW = std::max(8.0, fm.averageCharWidth());
            double wantW = maxLineW + slackW;
            int minW = m_inlineEditMinWidth;
            int newW = std::max(minW, static_cast<int>(std::ceil(wantW)));

            // Height: number of lines × line height (no border padding —
            // editor is borderless so the box matches the glyph bounds).
            int nLines = std::max(1, static_cast<int>(lines.size()));
            double lineH = fm.height() + fm.leading();
            double wantH = lineH * nLines;
            int minH = std::max(12, static_cast<int>(std::ceil(lineH)));
            int newH = std::max(minH, static_cast<int>(std::ceil(wantH)));

            int newX;
            if (m_inlineEditAlignH & Qt::AlignRight)
                newX = m_inlineEditAnchorRightX - newW;
            else if (m_inlineEditAlignH & Qt::AlignLeft)
                newX = m_inlineEditAnchorLeftX;
            else
                newX = m_inlineEditAnchorCenterX - newW / 2;

            QRect r(newX, m_inlineEditAnchorTopY, newW, newH);
            if (r != m_inlineTextEdit->geometry()) {
                m_inlineTextEdit->setGeometry(r);
                // Trigger the overlay widget to repaint so the live
                // bounding-box drawn around the editor follows the new
                // size in lockstep with the text.
                update();
            }
        });
    }

    // Match the renderer's font sizing exactly. renderGraphicClip()
    // builds its QFont with POINT size and rasterises into a canvas at
    // the PROJECT/sequence resolution, which is then downscaled to the
    // composite output and displayed in the on-screen frame rect. So
    // the on-screen point size = layer pointSize × frameRect.height /
    // projectHeight. The reference height here MUST be the project
    // resolution (m_seqH, e.g. 2160 for a 4K project), NOT a hardcoded
    // 1080 — that hardcode made the inline editor's text ~2× too big
    // during edit for non-1080 projects, then snap back on commit.
    QRectF fr = computeFrameRect();
    double scaleHeight = (fr.height() > 1.0) ? fr.height()
                                              : double(height());
    double refH = (m_seqH > 0) ? static_cast<double>(m_seqH) : 1080.0;
    if (!std::isfinite(verticalScale) || verticalScale <= 0.0f)
        verticalScale = 1.0f;
    m_inlinePixelScale = verticalScale * scaleHeight / refH;
    // QPainterPath resolves point sizes at the application's render-screen
    // DPI, while the top-level inline editor resolves them on whichever
    // monitor contains the Program panel. Correct that DPI conversion so
    // opening edit mode cannot stretch or resize the same font on a second
    // monitor with different display scaling.
    const QPoint screenPoint = mapToGlobal(QPoint(
        static_cast<int>(std::lround(centerX)),
        static_cast<int>(std::lround(centerY))));
    QScreen* renderScreen = QGuiApplication::primaryScreen();
    QScreen* editorScreen = QGuiApplication::screenAt(screenPoint);
    const double renderDpi = renderScreen
        ? renderScreen->logicalDotsPerInchY() : 96.0;
    const double editorDpi = editorScreen
        ? editorScreen->logicalDotsPerInchY() : renderDpi;
    const double dpiCorrection = editorDpi > 1.0
        ? renderDpi / editorDpi : 1.0;
    m_inlineFontPointScale = m_inlinePixelScale * dpiCorrection;
    double fontPt = std::max(1.0,
                             double(fontSizeRef) * m_inlineFontPointScale);
    spdlog::debug("[INLINE-TEXT] font sizing ref={} pointScale={} pixelScale={} editorPt={} renderDpi={} editorDpi={}",
                 fontSizeRef, m_inlineFontPointScale, m_inlinePixelScale,
                 fontPt, renderDpi, editorDpi);

    // Premiere-style invisible input surface. The compositor paints the
    // glyphs; the overlay paints the compositor-aligned caret and bounds.
    const QString styleSheet = QStringLiteral(
        "QPlainTextEdit { "
        "font-family: \"%1\"; "
        "font-size: %2pt; "
        "font-weight: %3; "
        "font-style: %4; "
        // A fully transparent pixel in a translucent top-level window is
        // click-through on Windows.  That made clicks between glyphs land on
        // the Vulkan monitor, whose focus change committed the edit and put
        // the layer back in move mode.  Alpha 1 is visually transparent but
        // keeps the whole editor rectangle mouse-interactive for normal caret
        // placement and drag selection.
        "background: rgba(0, 0, 0, 1); "
        // Keep the editor's glyphs and native caret invisible so Qt cannot
        // redraw or vertically position them from its rounded line boxes.
        "color: transparent; "
        "border: none; "
        // The native selection uses this small, screen-hinted document's
        // advances and can land a character away from the compositor glyphs.
        // TransformOverlayWidget paints the selection from renderer-owned
        // caret boundaries instead, just as it already does for the caret.
        "selection-background-color: transparent; "
        "selection-color: transparent; "
        "padding: 0px; }")
        .arg(fontFamily)
        .arg(fontPt, 0, 'f', 1)
        .arg(std::clamp(fontWeight, 1, 1000))
        .arg(italic ? "italic" : "normal");
    QFont qf(fontFamily, -1, std::clamp(fontWeight, 1, 1000), italic);
    if (!fontStyle.isEmpty()) qf.setStyleName(fontStyle);
    qf.setPointSizeF(fontPt);
    if (fauxBold) qf.setWeight(static_cast<QFont::Weight>(
        std::max(700, static_cast<int>(qf.weight()))));
    if (fauxItalic) qf.setItalic(true);
    qf.setUnderline(underline);
    qf.setCapitalization(allCaps ? QFont::AllUppercase
        : (smallCaps ? QFont::SmallCaps : QFont::MixedCase));
    qf.setLetterSpacing(QFont::AbsoluteSpacing,
                        (tracking + kerning) * m_inlinePixelScale);
    // Anisotropic-scale support: the renderer applies painter.scale(sx, sy)
    // which stretches glyph WIDTHS by sx/sy relative to height. QFont has
    // no general anisotropic transform, but setStretch() is exactly this:
    // a percentage applied to glyph advance/width. QSS has no font-stretch
    // property so setStyleSheet below won't override it. QFontMetricsF
    // honours it too, so the textChanged auto-grow stays accurate.
    if (std::isfinite(horizontalStretch) && horizontalStretch > 0.0f) {
        const double tsumeScale = std::clamp(
            1.0 - static_cast<double>(tsume) / 100.0, 0.1, 1.0);
        const int stretchPct = std::clamp(static_cast<int>(std::round(
            horizontalStretch * 100.0 * tsumeScale)), 1, 4000);
        qf.setStretch(stretchPct);
    }
    m_inlineFontStretch = qf.stretch();
    m_inlineTextEdit->setFont(qf);
    m_inlineTextEdit->setStyleSheet(styleSheet);

    // Save vertical metrics from the same full-size font used by the
    // compositor. Scaling those metrics once is more accurate than asking
    // Qt's smaller, hinted editor font which line it visually belongs to.
    QFont rendererFont(fontFamily,
        std::max(1, static_cast<int>(fontSizeRef)));
    if (!fontStyle.isEmpty()) rendererFont.setStyleName(fontStyle);
    rendererFont.setWeight(static_cast<QFont::Weight>(
        std::clamp(fontWeight, 1, 1000)));
    rendererFont.setItalic(italic || fauxItalic);
    if (fauxBold) rendererFont.setWeight(static_cast<QFont::Weight>(
        std::max(700, static_cast<int>(rendererFont.weight()))));
    const QFontMetricsF rendererMetrics(rendererFont);
    m_inlineRendererLineAdvancePx = std::max(1.0,
        (rendererMetrics.lineSpacing() + static_cast<double>(leading))
            * m_inlinePixelScale);
    m_inlineRendererCaretHeightPx = std::max(2.0,
        rendererMetrics.height() * m_inlinePixelScale);
    // Point text has no authored right edge: keep it on one visual line and
    // grow the editor as the user types. QPlainTextEdit's default wrapping
    // clipped or wrapped characters past the original selection box.
    m_inlineTextEdit->setLineWrapMode(paragraphBox
        ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);

    // Apply horizontal text alignment to the document so the editor's
    // glyphs sit on the same side of its box as the renderer drew them.
    {
        QTextOption opt = m_inlineTextEdit->document()->defaultTextOption();
        opt.setAlignment(hAlignFlag);
        opt.setTextDirection(rightToLeft ? Qt::RightToLeft : Qt::LeftToRight);
        opt.setTabStopDistance(std::max(1.0,
            static_cast<double>(tabWidth) * m_inlinePixelScale));
        m_inlineTextEdit->document()->setDefaultTextOption(opt);
    }

    // Derive the editor's pixel size from the actual rendered font
    // metrics — width = enough to hold the initial string plus caret slack,
    // height = one glyph line. No border/margin padding (borderless editor),
    // so the box matches glyph bounds.
    QFontMetricsF fm(qf, m_inlineTextEdit);
    const QStringList initialLines = initial.split(QChar('\n'));
    const int initialLineCount = std::max(1,
        static_cast<int>(initialLines.size()));
    const double editorLineHeight = fm.height() + fm.leading();
    int editH = std::max(12, static_cast<int>(std::ceil(
        editorLineHeight * initialLineCount)));
    double slack0 = std::max(8.0, fm.averageCharWidth());
    double maxLineW0 = 0.0;
    for (const QString& line : initialLines)
        maxLineW0 = std::max(maxLineW0, fm.horizontalAdvance(line));
    m_inlineEditMinWidth = m_savedOverlayBeforeEdit.useContentRect
        ? std::max(40, static_cast<int>(std::ceil(
              std::max(0.0, rightX - leftX))))
        : 40;
    int editW = std::max(m_inlineEditMinWidth,
        static_cast<int>(std::ceil(maxLineW0 + slack0)));

    // Position the editor so its alignment-anchor edge sits on the
    // corresponding edge of the layer's content rect. That way:
    //   • Left-aligned text: editor's LEFT == content's LEFT, text grows right.
    //   • Right-aligned: editor's RIGHT == content's RIGHT, text grows left.
    //   • Center-aligned: editor's CENTER == content's CENTER, grows both ways.
    int editX;
    if (hAlignFlag & Qt::AlignRight)
        editX = static_cast<int>(std::round(rightX)) - editW;
    else if (hAlignFlag & Qt::AlignLeft)
        editX = static_cast<int>(std::round(leftX));
    else
        editX = static_cast<int>(std::round(centerX - editW * 0.5));

    // The saved content rect already is the compositor's measured glyph
    // bounds. Anchor the invisible editor directly to that exact Y. Applying
    // an additional font-ascent estimate moved the caret/selection surface a
    // few pixels above the text because QTextEdit's cursor geometry already
    // includes its own line-box ascent.
    const bool hasMeasuredContent =
        m_savedOverlayBeforeEdit.useContentRect
        && rightX - leftX >= 1.0;
    const double editTop = hasMeasuredContent && initialLineCount > 1
        ? topY
        : centerY - editH * 0.5;
    QRect g(editX, static_cast<int>(std::round(editTop)), editW, editH);

    // Convert to GLOBAL screen coordinates: a top-level window's geometry
    // is screen-relative, not parent-relative.
    QPoint globalTL = mapToGlobal(g.topLeft());
    QRect screenRect(globalTL, g.size());

    // Remember the editor's alignment-anchor coords (screen) so textChanged
    // can re-position the editor as it grows/shrinks.
    QPoint globalLeft   = mapToGlobal(QPoint(static_cast<int>(std::round(leftX)),
                                             static_cast<int>(std::round(centerY))));
    QPoint globalRight  = mapToGlobal(QPoint(static_cast<int>(std::round(rightX)),
                                             static_cast<int>(std::round(centerY))));
    QPoint globalCenter = mapToGlobal(QPoint(static_cast<int>(std::round(centerX)),
                                             static_cast<int>(std::round(centerY))));
    m_inlineEditAnchorLeftX   = globalLeft.x();
    m_inlineEditAnchorRightX  = globalRight.x();
    m_inlineEditAnchorCenterX = globalCenter.x();
    m_inlineEditAnchorCenterY = globalCenter.y();
    m_inlineEditAnchorTopY    = screenRect.top();
    m_inlineEditCenter = screenRect.center();          // legacy field
    m_inlineEditHeight = screenRect.height();

    m_inlineTextEdit->setGeometry(screenRect);
    m_initializingInlineText = true;
    m_inlineRetainedSelectionStart = 0;
    m_inlineRetainedSelectionEnd = initial.size();
    m_inlineTextEdit->setPlainText(initial);

    // QPlainTextEdit keeps a rich QTextDocument internally. Seed the whole
    // string with the layer defaults, then overlay the persisted character
    // runs. Clipboard/input remains plain text, so pasted HTML cannot leak in.
    QTextCursor all(m_inlineTextEdit->document());
    all.select(QTextCursor::Document);
    QTextCharFormat baseFormat;
    baseFormat.setFont(qf, QTextCharFormat::FontPropertiesAll);
    // Preserve authored appearance in the private properties below, but do
    // not paint editor glyphs. The compositor supplies the exact same vector
    // outlines used for final playback while this document supplies input,
    // caret placement and selection geometry only.
    baseFormat.setForeground(QColor(Qt::transparent));
    baseFormat.setBaselineOffset(fontSizeRef > 0.0f
        ? 100.0 * baselineShift / fontSizeRef : 0.0);
    if (superscript)
        baseFormat.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    else if (subscript)
        baseFormat.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    baseFormat.setProperty(kInlineLeadingProperty, leading);
    baseFormat.setProperty(kInlineTrackingProperty, tracking);
    baseFormat.setProperty(kInlineActualWeightProperty, fontWeight);
    baseFormat.setProperty(kInlineActualItalicProperty, italic);
    baseFormat.setProperty(kInlineFontStyleProperty, fontStyle);
    baseFormat.setProperty(kInlineKerningProperty, kerning);
    baseFormat.setProperty(kInlineTabWidthProperty, tabWidth);
    baseFormat.setProperty(kInlineTsumeProperty, tsume);
    baseFormat.setProperty(kInlineFauxBoldProperty, fauxBold);
    baseFormat.setProperty(kInlineFauxItalicProperty, fauxItalic);
    baseFormat.setProperty(kInlineFillEnabledProperty,
                           baseAppearance.fillEnabled);
    baseFormat.setProperty(kInlineFillColorProperty,
                           baseAppearance.fillColor);
    baseFormat.setProperty(kInlineStrokeEnabledProperty,
                           baseAppearance.strokeEnabled);
    baseFormat.setProperty(kInlineStrokePositionProperty,
        static_cast<int>(baseAppearance.strokePosition));
    baseFormat.setProperty(kInlineStrokeColorProperty,
                           baseAppearance.strokeColor);
    baseFormat.setProperty(kInlineStrokeWidthProperty,
                           baseAppearance.strokeWidth);
    baseFormat.setTextOutline(QPen(Qt::NoPen));
    baseFormat.setProperty(kInlineShadowEnabledProperty,
                           baseAppearance.shadowEnabled);
    baseFormat.setProperty(kInlineShadowColorProperty,
                           baseAppearance.shadowColor);
    baseFormat.setProperty(kInlineShadowDistanceProperty,
                           baseAppearance.shadowDistance);
    baseFormat.setProperty(kInlineShadowAngleProperty,
                           baseAppearance.shadowAngle);
    baseFormat.setProperty(kInlineShadowSoftnessProperty,
                           baseAppearance.shadowSoftness);
    baseFormat.setProperty(kInlineShadowOpacityProperty,
                           baseAppearance.shadowOpacity);
    baseFormat.setProperty(kInlineBackgroundEnabledProperty,
                           baseAppearance.backgroundEnabled);
    baseFormat.setProperty(kInlineBackgroundPaddingProperty,
                           baseAppearance.backgroundPadding);
    baseFormat.setProperty(kInlineBackgroundColorProperty,
                           baseAppearance.backgroundColor);
    baseFormat.setBackground(QColor(Qt::transparent));
    all.setCharFormat(baseFormat);
    for (const auto& run : styleRuns) {
        const int start = std::clamp<int>(static_cast<int>(run.start), 0,
                                          initial.size());
        const int end = std::clamp<int>(
            start + static_cast<int>(run.length), start, initial.size());
        if (end <= start) continue;

        QFont runFont(QString::fromStdString(run.fontFamily));
        runFont.setPointSizeF(std::max(1.0,
            static_cast<double>(run.fontSize) * m_inlineFontPointScale));
        runFont.setWeight(static_cast<QFont::Weight>(
            std::clamp(run.fontWeight, 1, 1000)));
        runFont.setItalic(run.italic);
        const QString runFontStyle = run.overrideMask & TextOverrideFontStyle
            ? QString::fromStdString(run.fontStyle) : fontStyle;
        if (!runFontStyle.isEmpty()) runFont.setStyleName(runFontStyle);
        const bool runAllCaps = run.overrideMask & TextOverrideCapitalization
            ? run.allCaps : allCaps;
        const bool runSmallCaps = run.overrideMask & TextOverrideCapitalization
            ? run.smallCaps : smallCaps;
        const float runTracking = run.overrideMask & TextOverrideTracking
            ? run.tracking : tracking;
        const float runBaseline = run.overrideMask & TextOverrideBaseline
            ? run.baselineShift : baselineShift;
        const float runLeading = run.overrideMask & TextOverrideLeading
            ? run.leading : leading;
        const float runKerning = run.overrideMask & TextOverrideKerning
            ? run.kerning : kerning;
        const float runTabWidth = run.overrideMask & TextOverrideTabWidth
            ? run.tabWidth : tabWidth;
        const float runTsume = run.overrideMask & TextOverrideTsume
            ? run.tsume : tsume;
        const bool runFauxBold = run.overrideMask & TextOverrideFauxStyle
            ? run.fauxBold : fauxBold;
        const bool runFauxItalic = run.overrideMask & TextOverrideFauxStyle
            ? run.fauxItalic : fauxItalic;
        const bool runUnderline = run.overrideMask & TextOverrideDecoration
            ? run.underline : underline;
        const bool runSuperscript = run.overrideMask & TextOverrideScript
            ? run.superscript : superscript;
        const bool runSubscript = run.overrideMask & TextOverrideScript
            ? run.subscript : subscript;
        if (runFauxBold) runFont.setWeight(static_cast<QFont::Weight>(
            std::max(700, static_cast<int>(runFont.weight()))));
        if (runFauxItalic) runFont.setItalic(true);
        runFont.setUnderline(runUnderline);
        const double baseTsumeScale = std::clamp(
            1.0 - static_cast<double>(tsume) / 100.0, 0.1, 1.0);
        const double runTsumeScale = std::clamp(
            1.0 - static_cast<double>(runTsume) / 100.0, 0.1, 1.0);
        runFont.setStretch(std::clamp(static_cast<int>(std::round(
            m_inlineFontStretch * runTsumeScale / baseTsumeScale)), 1, 4000));
        runFont.setCapitalization(runAllCaps ? QFont::AllUppercase
            : (runSmallCaps ? QFont::SmallCaps : QFont::MixedCase));
        runFont.setLetterSpacing(QFont::AbsoluteSpacing,
            (runTracking + runKerning) * m_inlinePixelScale);
        QTextCharFormat runFormat;
        runFormat.setFont(runFont, QTextCharFormat::FontPropertiesAll);
        TextRunAppearance runAppearance = baseAppearance;
        if (run.overrideMask & TextOverrideFill) {
            runAppearance.fillEnabled = run.appearance.fillEnabled;
            runAppearance.fillColor = run.appearance.fillColor;
        }
        if (run.overrideMask & TextOverrideStroke) {
            runAppearance.strokeEnabled = run.appearance.strokeEnabled;
            runAppearance.strokeColor = run.appearance.strokeColor;
            runAppearance.strokeWidth = run.appearance.strokeWidth;
            runAppearance.strokePosition = run.appearance.strokePosition;
        }
        if (run.overrideMask & TextOverrideShadow) {
            runAppearance.shadowEnabled = run.appearance.shadowEnabled;
            runAppearance.shadowColor = run.appearance.shadowColor;
            runAppearance.shadowDistance = run.appearance.shadowDistance;
            runAppearance.shadowAngle = run.appearance.shadowAngle;
            runAppearance.shadowSoftness = run.appearance.shadowSoftness;
            runAppearance.shadowOpacity = run.appearance.shadowOpacity;
        }
        if (run.overrideMask & TextOverrideBackground) {
            runAppearance.backgroundEnabled = run.appearance.backgroundEnabled;
            runAppearance.backgroundColor = run.appearance.backgroundColor;
            runAppearance.backgroundPadding = run.appearance.backgroundPadding;
        }
        runFormat.setForeground(QColor(Qt::transparent));
        runFormat.setBaselineOffset(run.fontSize > 0.0f
            ? 100.0 * runBaseline / run.fontSize : 0.0);
        if (runSuperscript)
            runFormat.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
        else if (runSubscript)
            runFormat.setVerticalAlignment(QTextCharFormat::AlignSubScript);
        runFormat.setProperty(kInlineLeadingProperty, runLeading);
        runFormat.setProperty(kInlineTrackingProperty, runTracking);
        runFormat.setProperty(kInlineActualWeightProperty, run.fontWeight);
        runFormat.setProperty(kInlineActualItalicProperty, run.italic);
        runFormat.setProperty(kInlineFontStyleProperty, runFontStyle);
        runFormat.setProperty(kInlineKerningProperty, runKerning);
        runFormat.setProperty(kInlineTabWidthProperty, runTabWidth);
        runFormat.setProperty(kInlineTsumeProperty, runTsume);
        runFormat.setProperty(kInlineFauxBoldProperty, runFauxBold);
        runFormat.setProperty(kInlineFauxItalicProperty, runFauxItalic);
        runFormat.setProperty(kInlineFillEnabledProperty,
                              runAppearance.fillEnabled);
        runFormat.setProperty(kInlineFillColorProperty,
                              runAppearance.fillColor);
        runFormat.setProperty(kInlineStrokeEnabledProperty,
                              runAppearance.strokeEnabled);
        runFormat.setProperty(kInlineStrokePositionProperty,
            static_cast<int>(runAppearance.strokePosition));
        runFormat.setProperty(kInlineStrokeColorProperty,
                              runAppearance.strokeColor);
        runFormat.setProperty(kInlineStrokeWidthProperty,
                              runAppearance.strokeWidth);
        runFormat.setTextOutline(QPen(Qt::NoPen));
        runFormat.setProperty(kInlineShadowEnabledProperty,
                              runAppearance.shadowEnabled);
        runFormat.setProperty(kInlineShadowColorProperty,
                              runAppearance.shadowColor);
        runFormat.setProperty(kInlineShadowDistanceProperty,
                              runAppearance.shadowDistance);
        runFormat.setProperty(kInlineShadowAngleProperty,
                              runAppearance.shadowAngle);
        runFormat.setProperty(kInlineShadowSoftnessProperty,
                              runAppearance.shadowSoftness);
        runFormat.setProperty(kInlineShadowOpacityProperty,
                              runAppearance.shadowOpacity);
        runFormat.setProperty(kInlineBackgroundEnabledProperty,
                              runAppearance.backgroundEnabled);
        runFormat.setProperty(kInlineBackgroundPaddingProperty,
                              runAppearance.backgroundPadding);
        runFormat.setProperty(kInlineBackgroundColorProperty,
                              runAppearance.backgroundColor);
        runFormat.setBackground(QColor(Qt::transparent));
        QTextCursor cursor(m_inlineTextEdit->document());
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.setCharFormat(runFormat);
    }
    auto qtAlignmentFor = [](GTextAlign alignment) {
        switch (alignment) {
        case GTextAlign::Left: return Qt::Alignment(Qt::AlignLeft);
        case GTextAlign::Right: return Qt::Alignment(Qt::AlignRight);
        case GTextAlign::Justify: return Qt::Alignment(Qt::AlignJustify);
        case GTextAlign::Center:
        default: return Qt::Alignment(Qt::AlignHCenter);
        }
    };
    QTextBlockFormat baseBlockFormat;
    baseBlockFormat.setAlignment(hAlignFlag);
    baseBlockFormat.setLayoutDirection(
        rightToLeft ? Qt::RightToLeft : Qt::LeftToRight);
    all.setBlockFormat(baseBlockFormat);
    for (const auto& paragraph : paragraphStyles) {
        const int start = std::clamp<int>(paragraph.start, 0, initial.size());
        const int end = std::clamp<int>(
            start + static_cast<int>(paragraph.length), start, initial.size());
        QTextCursor cursor(m_inlineTextEdit->document());
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        QTextBlockFormat format;
        format.setAlignment(qtAlignmentFor(paragraph.alignment));
        format.setLayoutDirection(paragraph.rightToLeft
            ? Qt::RightToLeft : Qt::LeftToRight);
        cursor.mergeBlockFormat(format);
    }
    updateInlineBlockLeading(m_inlineTextEdit, leading,
                             m_inlinePixelScale);
    updateInlineDocumentTabWidth(m_inlineTextEdit, tabWidth,
                                 m_inlinePixelScale);
    m_initializingInlineText = false;
    resizeInlineTextEditorToDocument();
    spdlog::debug("[INLINE-TEXT] show session={} geometry=({}, {}, {}, {})",
                 editSession, m_inlineTextEdit->geometry().x(),
                 m_inlineTextEdit->geometry().y(),
                 m_inlineTextEdit->geometry().width(),
                 m_inlineTextEdit->geometry().height());
    m_inlineTextEdit->show();
    m_inlineTextEdit->raise();
    m_inlineTextEdit->setCursorWidth(0);
    m_inlineCaretVisible = true;
    if (m_inlineCaretBlinkTimer) {
        const int flashTime = QGuiApplication::styleHints()->cursorFlashTime();
        if (flashTime > 0) {
            m_inlineCaretBlinkTimer->setInterval(std::max(100, flashTime / 2));
            m_inlineCaretBlinkTimer->start();
        } else {
            m_inlineCaretBlinkTimer->stop();
        }
    }
    // Defer activation to the next event-loop tick so the window is fully
    // realised before Windows is asked to give it foreground focus. Same
    // pattern PropertiesPanel::focusGraphicTextField uses, for the same
    // "setFocus is ignored on not-yet-shown widget" reason.
    QPointer<QPlainTextEdit> edit(m_inlineTextEdit);
    QPointer<TransformOverlayWidget> overlay(this);
    const bool placeCaretAtClick = m_hasPendingInlineCaret;
    const QPoint caretGlobal = m_pendingInlineCaretGlobal;
    m_hasPendingInlineCaret = false;
    QTimer::singleShot(0, edit, [edit, overlay, editSession,
                                placeCaretAtClick, caretGlobal]() {
        if (!edit || !overlay || overlay->m_inlineEditSession != editSession)
            return;
        edit->activateWindow();
        edit->setFocus(Qt::MouseFocusReason);
        if (placeCaretAtClick) {
            QTextCursor cursor(edit->document());
            cursor.setPosition(overlay->inlineTextPositionAtGlobal(caretGlobal));
            edit->setTextCursor(cursor);
        } else {
            edit->selectAll();
        }
        overlay->notifyInlineTextSelectionFormat();
        spdlog::debug("[INLINE-TEXT] activation session={} focus={} activeWindow={}",
                     editSession, edit->hasFocus(), edit->isActiveWindow());
    });

    // A native createWindowContainer can reclaim focus at the tail of the
    // same double-click. Retry once after that mouse gesture has completed,
    // then end the settling window. A real later click outside the editor is
    // still handled by the normal FocusOut commit path below.
    QTimer::singleShot(120, edit, [edit, overlay, editSession]() {
        if (!edit || !overlay || overlay->m_inlineEditSession != editSession
            || !edit->isVisible()) return;
        if (!overlay->focusIsInInlineFormattingUi()) {
            edit->raise();
            edit->activateWindow();
            edit->setFocus(Qt::MouseFocusReason);
            spdlog::debug("[INLINE-TEXT] focus retry session={} focus={} activeWindow={}",
                         editSession, edit->hasFocus(), edit->isActiveWindow());
        }
    });
    QTimer::singleShot(250, this, [this, editSession]() {
        if (m_inlineEditSession != editSession) return;
        m_inlineEditorFocusSettling = false;
        spdlog::debug("[INLINE-TEXT] focus settled session={} focused={} formattingUi={}",
                     editSession, m_inlineTextEdit && m_inlineTextEdit->hasFocus(),
                     focusIsInInlineFormattingUi());
    });
}

void TransformOverlayWidget::resizeInlineTextEditorToDocument()
{
    if (!m_inlineTextEdit || m_initializingInlineText) return;

    double maxLineWidth = 0.0;
    double totalHeight = 0.0;
    const QFontMetricsF defaultMetrics(m_inlineTextEdit->font(),
                                       m_inlineTextEdit);
    for (QTextBlock block = m_inlineTextEdit->document()->begin();
         block.isValid(); block = block.next()) {
        double lineWidth = 0.0;
        double lineHeight = defaultMetrics.height();
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid()) continue;
            const QFont fragmentFont = fragment.charFormat().font();
            const QFontMetricsF metrics(fragmentFont, m_inlineTextEdit);
            lineWidth += metrics.horizontalAdvance(fragment.text());
            lineHeight = std::max(lineHeight, metrics.height());
        }
        maxLineWidth = std::max(maxLineWidth, lineWidth);
        totalHeight += lineHeight;
    }

    const double slack = std::max(8.0, defaultMetrics.averageCharWidth());
    const int newWidth = m_inlineUsesParagraphBox
        ? m_inlineEditMinWidth
        : std::max(m_inlineEditMinWidth,
              static_cast<int>(std::ceil(maxLineWidth + slack)));
    // A few pixels of vertical slack cover QPlainTextEdit's caret/layout
    // rounding.  Without it the document retains a tiny scroll range and Qt
    // can scroll the first line away when a later line or selected run grows.
    // QTextBlock fragments describe logical lines, while the document layout
    // owns the actual line cells (including final-line descent and wrapped
    // lines). Honor both measurements. This prevents the last line from
    // retaining a scroll range and appearing vertically cut off.
    double wantedHeight = std::max({totalHeight, defaultMetrics.height(),
        m_inlineTextEdit->document()->size().height()});
    const int newHeight = std::max(12,
        static_cast<int>(std::ceil(wantedHeight + 4.0)));

    int newX = m_inlineEditAnchorCenterX - newWidth / 2;
    if (m_inlineEditAlignH & Qt::AlignRight)
        newX = m_inlineEditAnchorRightX - newWidth;
    else if (m_inlineEditAlignH & Qt::AlignLeft)
        newX = m_inlineEditAnchorLeftX;

    const QRect geometry(newX, m_inlineEditAnchorTopY,
                         newWidth, newHeight);
    if (geometry != m_inlineTextEdit->geometry()) {
        m_inlineTextEdit->setGeometry(geometry);
        update();
    }
    if (auto* bar = m_inlineTextEdit->verticalScrollBar(); bar && bar->maximum() > 0)
        bar->setValue(0);
}

QTextCursor TransformOverlayWidget::inlineTextFormattingCursor() const
{
    if (!m_inlineTextEdit) return {};
    QTextCursor cursor = m_inlineTextEdit->textCursor();
    if (cursor.hasSelection() || m_inlineTextEdit->hasFocus()
        || m_inlineRetainedSelectionEnd <= m_inlineRetainedSelectionStart)
        return cursor;

    const int textLength = m_inlineTextEdit->toPlainText().size();
    const int start = std::clamp(m_inlineRetainedSelectionStart, 0, textLength);
    const int end = std::clamp(m_inlineRetainedSelectionEnd, start, textLength);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    return cursor;
}

std::vector<TextStyleRun> TransformOverlayWidget::collectInlineTextStyles() const
{
    std::vector<TextStyleRun> runs;
    if (!m_inlineTextEdit) return runs;

    const double inverseScale = m_inlineFontPointScale > 1.0e-6
        ? 1.0 / m_inlineFontPointScale : 1.0;
    for (QTextBlock block = m_inlineTextEdit->document()->begin();
         block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.length() <= 0) continue;
            const QTextCharFormat format = fragment.charFormat();
            const QFont font = format.font();
            TextStyleRun run;
            run.start = static_cast<uint32_t>(fragment.position());
            run.length = static_cast<uint32_t>(fragment.length());
            run.fontFamily = font.family().toStdString();
            run.fontSize = static_cast<float>(
                std::max(1.0, font.pointSizeF() * inverseScale));
            run.fontStyle = format.hasProperty(kInlineFontStyleProperty)
                ? format.property(kInlineFontStyleProperty).toString().toStdString()
                : m_inlineBaseFontStyle.toStdString();
            run.fontWeight = format.hasProperty(kInlineActualWeightProperty)
                ? format.property(kInlineActualWeightProperty).toInt()
                : font.weight();
            run.italic = format.hasProperty(kInlineActualItalicProperty)
                ? format.property(kInlineActualItalicProperty).toBool()
                : font.italic();
            run.allCaps = font.capitalization() == QFont::AllUppercase;
            run.smallCaps = font.capitalization() == QFont::SmallCaps;
            run.tracking = format.hasProperty(kInlineTrackingProperty)
                ? format.property(kInlineTrackingProperty).toFloat()
                : static_cast<float>(font.letterSpacing() * inverseScale);
            run.baselineShift = static_cast<float>(
                format.baselineOffset() * run.fontSize / 100.0);
            run.leading = format.hasProperty(
                kInlineLeadingProperty)
                ? format.property(kInlineLeadingProperty).toFloat()
                : m_inlineBaseLeading;
            run.kerning = format.hasProperty(kInlineKerningProperty)
                ? format.property(kInlineKerningProperty).toFloat()
                : m_inlineBaseKerning;
            run.tabWidth = format.hasProperty(kInlineTabWidthProperty)
                ? format.property(kInlineTabWidthProperty).toFloat()
                : m_inlineBaseTabWidth;
            run.tsume = format.hasProperty(kInlineTsumeProperty)
                ? format.property(kInlineTsumeProperty).toFloat()
                : m_inlineBaseTsume;
            run.fauxBold = format.hasProperty(kInlineFauxBoldProperty)
                ? format.property(kInlineFauxBoldProperty).toBool()
                : m_inlineBaseFauxBold;
            run.fauxItalic = format.hasProperty(kInlineFauxItalicProperty)
                ? format.property(kInlineFauxItalicProperty).toBool()
                : m_inlineBaseFauxItalic;
            run.underline = font.underline();
            run.superscript = format.verticalAlignment()
                == QTextCharFormat::AlignSuperScript;
            run.subscript = format.verticalAlignment()
                == QTextCharFormat::AlignSubScript;
            // Replacing the editor's auto-selected placeholder can make Qt
            // create a fresh QTextFragment without our private properties.
            // Missing metadata means "inherit the layer", not false/zero.
            // Treating it as false/zero produced a full-length transparent
            // fill override and made newly edited text disappear on commit.
            auto propertyOr = [&format](int property, const QVariant& fallback) {
                return format.hasProperty(property)
                    ? format.property(property) : fallback;
            };
            run.appearance.fillEnabled = propertyOr(
                kInlineFillEnabledProperty,
                m_inlineBaseAppearance.fillEnabled).toBool();
            run.appearance.fillColor = propertyOr(
                kInlineFillColorProperty,
                m_inlineBaseAppearance.fillColor).toUInt();
            run.appearance.strokeEnabled = propertyOr(
                kInlineStrokeEnabledProperty,
                m_inlineBaseAppearance.strokeEnabled).toBool();
            run.appearance.strokeColor = propertyOr(
                kInlineStrokeColorProperty,
                m_inlineBaseAppearance.strokeColor).toUInt();
            run.appearance.strokeWidth = propertyOr(
                kInlineStrokeWidthProperty,
                m_inlineBaseAppearance.strokeWidth).toFloat();
            run.appearance.strokePosition = static_cast<StrokePosition>(
                propertyOr(kInlineStrokePositionProperty,
                           static_cast<int>(m_inlineBaseAppearance.strokePosition))
                    .toInt());
            run.appearance.shadowEnabled = propertyOr(
                kInlineShadowEnabledProperty,
                m_inlineBaseAppearance.shadowEnabled).toBool();
            run.appearance.shadowColor = propertyOr(
                kInlineShadowColorProperty,
                m_inlineBaseAppearance.shadowColor).toUInt();
            run.appearance.shadowDistance = propertyOr(
                kInlineShadowDistanceProperty,
                m_inlineBaseAppearance.shadowDistance).toFloat();
            run.appearance.shadowAngle = propertyOr(
                kInlineShadowAngleProperty,
                m_inlineBaseAppearance.shadowAngle).toFloat();
            run.appearance.shadowSoftness = propertyOr(
                kInlineShadowSoftnessProperty,
                m_inlineBaseAppearance.shadowSoftness).toFloat();
            run.appearance.shadowOpacity = propertyOr(
                kInlineShadowOpacityProperty,
                m_inlineBaseAppearance.shadowOpacity).toFloat();
            run.appearance.backgroundEnabled = propertyOr(
                kInlineBackgroundEnabledProperty,
                m_inlineBaseAppearance.backgroundEnabled).toBool();
            run.appearance.backgroundColor = propertyOr(
                kInlineBackgroundColorProperty,
                m_inlineBaseAppearance.backgroundColor).toUInt();
            run.appearance.backgroundPadding = propertyOr(
                kInlineBackgroundPaddingProperty,
                m_inlineBaseAppearance.backgroundPadding).toFloat();
            if (run.allCaps != m_inlineBaseAllCaps
                || run.smallCaps != m_inlineBaseSmallCaps)
                run.overrideMask |= TextOverrideCapitalization;
            if (std::abs(run.tracking - m_inlineBaseTracking) > 0.01f)
                run.overrideMask |= TextOverrideTracking;
            if (std::abs(run.baselineShift - m_inlineBaseBaselineShift) > 0.01f)
                run.overrideMask |= TextOverrideBaseline;
            if (std::abs(run.leading - m_inlineBaseLeading) > 0.01f)
                run.overrideMask |= TextOverrideLeading;
            if (QString::fromStdString(run.fontStyle) != m_inlineBaseFontStyle)
                run.overrideMask |= TextOverrideFontStyle;
            if (std::abs(run.kerning - m_inlineBaseKerning) > 0.01f)
                run.overrideMask |= TextOverrideKerning;
            if (std::abs(run.tabWidth - m_inlineBaseTabWidth) > 0.01f)
                run.overrideMask |= TextOverrideTabWidth;
            if (std::abs(run.tsume - m_inlineBaseTsume) > 0.01f)
                run.overrideMask |= TextOverrideTsume;
            if (run.fauxBold != m_inlineBaseFauxBold
                || run.fauxItalic != m_inlineBaseFauxItalic)
                run.overrideMask |= TextOverrideFauxStyle;
            if (run.underline != m_inlineBaseUnderline)
                run.overrideMask |= TextOverrideDecoration;
            if (run.superscript != m_inlineBaseSuperscript
                || run.subscript != m_inlineBaseSubscript)
                run.overrideMask |= TextOverrideScript;
            if (run.appearance.fillEnabled != m_inlineBaseAppearance.fillEnabled
                || run.appearance.fillColor != m_inlineBaseAppearance.fillColor)
                run.overrideMask |= TextOverrideFill;
            if (run.appearance.strokeEnabled != m_inlineBaseAppearance.strokeEnabled
                || run.appearance.strokeColor != m_inlineBaseAppearance.strokeColor
                || std::abs(run.appearance.strokeWidth
                            - m_inlineBaseAppearance.strokeWidth) > 0.01f
                || run.appearance.strokePosition
                    != m_inlineBaseAppearance.strokePosition)
                run.overrideMask |= TextOverrideStroke;
            if (run.appearance.shadowEnabled != m_inlineBaseAppearance.shadowEnabled
                || run.appearance.shadowColor != m_inlineBaseAppearance.shadowColor
                || std::abs(run.appearance.shadowDistance
                            - m_inlineBaseAppearance.shadowDistance) > 0.01f
                || std::abs(run.appearance.shadowAngle
                            - m_inlineBaseAppearance.shadowAngle) > 0.01f
                || std::abs(run.appearance.shadowSoftness
                            - m_inlineBaseAppearance.shadowSoftness) > 0.01f
                || std::abs(run.appearance.shadowOpacity
                            - m_inlineBaseAppearance.shadowOpacity) > 0.01f)
                run.overrideMask |= TextOverrideShadow;
            if (run.appearance.backgroundEnabled
                    != m_inlineBaseAppearance.backgroundEnabled
                || run.appearance.backgroundColor
                    != m_inlineBaseAppearance.backgroundColor
                || std::abs(run.appearance.backgroundPadding
                            - m_inlineBaseAppearance.backgroundPadding) > 0.01f)
                run.overrideMask |= TextOverrideBackground;

            const bool isBaseStyle =
                QString::fromStdString(run.fontFamily) == m_inlineBaseFontFamily
                && std::abs(run.fontSize - m_inlineBaseFontSize) < 0.01f
                && run.fontWeight == m_inlineBaseFontWeight
                && run.italic == m_inlineBaseItalic
                && run.overrideMask == 0;
            if (isBaseStyle) continue;
            runs.push_back(std::move(run));
        }
    }
    return runs;
}

std::vector<TextParagraphStyle>
TransformOverlayWidget::collectInlineParagraphStyles() const
{
    std::vector<TextParagraphStyle> styles;
    if (!m_inlineTextEdit) return styles;
    const int documentLength = m_inlineTextEdit->toPlainText().size();
    auto alignmentFromQt = [](Qt::Alignment alignment) {
        if (alignment.testFlag(Qt::AlignRight)) return GTextAlign::Right;
        if (alignment.testFlag(Qt::AlignJustify)) return GTextAlign::Justify;
        if (alignment.testFlag(Qt::AlignHCenter)) return GTextAlign::Center;
        return GTextAlign::Left;
    };
    const GTextAlign baseAlignment = alignmentFromQt(m_inlineEditAlignH);
    for (QTextBlock block = m_inlineTextEdit->document()->begin();
         block.isValid(); block = block.next()) {
        const QTextBlockFormat format = block.blockFormat();
        const GTextAlign alignment = alignmentFromQt(format.alignment());
        const bool rightToLeft = format.layoutDirection() == Qt::RightToLeft;
        if (alignment == baseAlignment
            && rightToLeft == m_inlineBaseRightToLeft) continue;
        TextParagraphStyle style;
        style.start = static_cast<uint32_t>(block.position());
        style.length = static_cast<uint32_t>(std::max(0, std::min(
            block.length(), documentLength - block.position())));
        style.alignment = alignment;
        style.rightToLeft = rightToLeft;
        styles.push_back(style);
    }
    return styles;
}

void TransformOverlayWidget::notifyInlineTextSelectionFormat()
{
    if (!isInlineTextEditing() || m_initializingInlineText) return;

    // Qt's native caret is transparent; repaint the custom caret at every
    // document cursor/selection change.
    update();

    QTextCursor cursor = m_inlineTextEdit->textCursor();
    if (cursor.hasSelection()) {
        m_inlineRetainedSelectionStart = cursor.selectionStart();
        m_inlineRetainedSelectionEnd = cursor.selectionEnd();
    } else if (m_inlineTextEdit->hasFocus()) {
        // A deliberate caret click replaces the old range. A focus transfer
        // to a formatting control must not silently turn range formatting
        // into a whole-layer operation.
        m_inlineRetainedSelectionStart = cursor.position();
        m_inlineRetainedSelectionEnd = cursor.position();
    }
    struct SelectionStyle {
        QString family;
        QString fontStyle;
        float pointSize{1.0f};
        int weight{400};
        bool italic{false};
        bool allCaps{false};
        bool smallCaps{false};
        float tracking{0.0f};
        float baselineShift{0.0f};
        float leading{0.0f};
        float kerning{0.0f};
        float tabWidth{48.0f};
        float tsume{0.0f};
        bool fauxBold{false};
        bool fauxItalic{false};
        bool underline{false};
        bool superscript{false};
        bool subscript{false};
        TextRunAppearance appearance;
    };

    const auto styleFromFormat = [this](const QTextCharFormat& format) {
        SelectionStyle style;
        const QFont font = format.font();
        style.family = font.family();
        if (style.family.isEmpty()) style.family = m_inlineBaseFontFamily;
        double screenPointSize = font.pointSizeF();
        if (screenPointSize <= 0.0)
            screenPointSize = m_inlineTextEdit->font().pointSizeF();
        const double scale = m_inlineFontPointScale > 1.0e-6
            ? m_inlineFontPointScale : 1.0;
        style.pointSize = static_cast<float>(
            std::max(1.0, screenPointSize / scale));
        style.fontStyle = format.hasProperty(kInlineFontStyleProperty)
            ? format.property(kInlineFontStyleProperty).toString()
            : m_inlineBaseFontStyle;
        style.weight = format.hasProperty(kInlineActualWeightProperty)
            ? format.property(kInlineActualWeightProperty).toInt()
            : font.weight();
        style.italic = format.hasProperty(kInlineActualItalicProperty)
            ? format.property(kInlineActualItalicProperty).toBool()
            : font.italic();
        style.allCaps = font.capitalization() == QFont::AllUppercase;
        style.smallCaps = font.capitalization() == QFont::SmallCaps;
        style.tracking = format.hasProperty(kInlineTrackingProperty)
            ? format.property(kInlineTrackingProperty).toFloat()
            : m_inlineBaseTracking;
        style.baselineShift = static_cast<float>(
            format.baselineOffset() * style.pointSize / 100.0);
        style.leading = format.hasProperty(kInlineLeadingProperty)
            ? format.property(kInlineLeadingProperty).toFloat()
            : m_inlineBaseLeading;
        style.kerning = format.hasProperty(kInlineKerningProperty)
            ? format.property(kInlineKerningProperty).toFloat()
            : m_inlineBaseKerning;
        style.tabWidth = format.hasProperty(kInlineTabWidthProperty)
            ? format.property(kInlineTabWidthProperty).toFloat()
            : m_inlineBaseTabWidth;
        style.tsume = format.hasProperty(kInlineTsumeProperty)
            ? format.property(kInlineTsumeProperty).toFloat()
            : m_inlineBaseTsume;
        style.fauxBold = format.property(kInlineFauxBoldProperty).toBool();
        style.fauxItalic = format.property(kInlineFauxItalicProperty).toBool();
        style.underline = font.underline();
        style.superscript = format.verticalAlignment()
            == QTextCharFormat::AlignSuperScript;
        style.subscript = format.verticalAlignment()
            == QTextCharFormat::AlignSubScript;
        style.appearance.fillEnabled = format.property(
            kInlineFillEnabledProperty).toBool();
        style.appearance.fillColor = format.property(
            kInlineFillColorProperty).toUInt();
        style.appearance.strokeEnabled = format.property(
            kInlineStrokeEnabledProperty).toBool();
        style.appearance.strokeColor = format.property(
            kInlineStrokeColorProperty).toUInt();
        style.appearance.strokeWidth = format.property(
            kInlineStrokeWidthProperty).toFloat();
        style.appearance.strokePosition = static_cast<StrokePosition>(
            format.property(kInlineStrokePositionProperty).toInt());
        style.appearance.shadowEnabled = format.property(
            kInlineShadowEnabledProperty).toBool();
        style.appearance.shadowColor = format.property(
            kInlineShadowColorProperty).toUInt();
        style.appearance.shadowDistance = format.property(
            kInlineShadowDistanceProperty).toFloat();
        style.appearance.shadowAngle = format.property(
            kInlineShadowAngleProperty).toFloat();
        style.appearance.shadowSoftness = format.property(
            kInlineShadowSoftnessProperty).toFloat();
        style.appearance.shadowOpacity = format.property(
            kInlineShadowOpacityProperty).toFloat();
        style.appearance.backgroundEnabled = format.property(
            kInlineBackgroundEnabledProperty).toBool();
        style.appearance.backgroundColor = format.property(
            kInlineBackgroundColorProperty).toUInt();
        style.appearance.backgroundPadding = format.property(
            kInlineBackgroundPaddingProperty).toFloat();
        return style;
    };

    SelectionStyle representative;
    bool haveRepresentative = false;
    uint32_t mixedFlags = 0;
    auto includeStyle = [&](const SelectionStyle& style) {
        if (!haveRepresentative) {
            representative = style;
            haveRepresentative = true;
            return;
        }
        if (representative.family != style.family)
            mixedFlags |= InlineMixedFamily;
        if (std::abs(representative.pointSize - style.pointSize) > 0.01f)
            mixedFlags |= InlineMixedSize;
        if (representative.weight != style.weight)
            mixedFlags |= InlineMixedWeight;
        if (representative.italic != style.italic)
            mixedFlags |= InlineMixedItalic;
        if (representative.allCaps != style.allCaps
            || representative.smallCaps != style.smallCaps)
            mixedFlags |= InlineMixedCapitalization;
        if (std::abs(representative.tracking - style.tracking) > 0.01f)
            mixedFlags |= InlineMixedTracking;
        if (std::abs(representative.baselineShift - style.baselineShift) > 0.01f)
            mixedFlags |= InlineMixedBaseline;
        if (std::abs(representative.leading - style.leading) > 0.01f)
            mixedFlags |= InlineMixedLeading;
        if (representative.fontStyle != style.fontStyle)
            mixedFlags |= InlineMixedFontStyle;
        if (std::abs(representative.kerning - style.kerning) > 0.01f)
            mixedFlags |= InlineMixedKerning;
        if (std::abs(representative.tabWidth - style.tabWidth) > 0.01f)
            mixedFlags |= InlineMixedTabWidth;
        if (std::abs(representative.tsume - style.tsume) > 0.01f)
            mixedFlags |= InlineMixedTsume;
        if (representative.fauxBold != style.fauxBold
            || representative.fauxItalic != style.fauxItalic)
            mixedFlags |= InlineMixedFauxStyle;
        if (representative.underline != style.underline)
            mixedFlags |= InlineMixedDecoration;
        if (representative.superscript != style.superscript
            || representative.subscript != style.subscript)
            mixedFlags |= InlineMixedScript;
        if (representative.appearance.fillEnabled
                != style.appearance.fillEnabled
            || representative.appearance.fillColor
                != style.appearance.fillColor)
            mixedFlags |= InlineMixedFill;
        if (representative.appearance.strokeEnabled
                != style.appearance.strokeEnabled
            || representative.appearance.strokeColor
                != style.appearance.strokeColor
            || std::abs(representative.appearance.strokeWidth
                        - style.appearance.strokeWidth) > 0.01f
            || representative.appearance.strokePosition
                != style.appearance.strokePosition)
            mixedFlags |= InlineMixedStroke;
        if (representative.appearance.shadowEnabled
                != style.appearance.shadowEnabled
            || representative.appearance.shadowColor
                != style.appearance.shadowColor
            || std::abs(representative.appearance.shadowDistance
                        - style.appearance.shadowDistance) > 0.01f
            || std::abs(representative.appearance.shadowAngle
                        - style.appearance.shadowAngle) > 0.01f
            || std::abs(representative.appearance.shadowSoftness
                        - style.appearance.shadowSoftness) > 0.01f
            || std::abs(representative.appearance.shadowOpacity
                        - style.appearance.shadowOpacity) > 0.01f)
            mixedFlags |= InlineMixedShadow;
        if (representative.appearance.backgroundEnabled
                != style.appearance.backgroundEnabled
            || representative.appearance.backgroundColor
                != style.appearance.backgroundColor
            || std::abs(representative.appearance.backgroundPadding
                        - style.appearance.backgroundPadding) > 0.01f)
            mixedFlags |= InlineMixedBackground;
    };

    if (cursor.hasSelection()) {
        const int selectionStart = cursor.selectionStart();
        const int selectionEnd = cursor.selectionEnd();
        for (QTextBlock block = m_inlineTextEdit->document()->begin();
             block.isValid(); block = block.next()) {
            if (block.position() >= selectionEnd) break;
            if (block.position() + block.length() <= selectionStart) continue;
            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid()) continue;
                const int start = std::max(fragment.position(), selectionStart);
                const int end = std::min(fragment.position() + fragment.length(),
                                         selectionEnd);
                if (start < end) includeStyle(styleFromFormat(fragment.charFormat()));
            }
        }
    }
    if (!haveRepresentative)
        includeStyle(styleFromFormat(m_inlineTextEdit->currentCharFormat()));

    emit inlineTextSelectionFormatChanged(
        representative.family, representative.pointSize,
        representative.weight, representative.italic,
        representative.allCaps, representative.smallCaps,
        representative.tracking, representative.baselineShift,
        representative.leading,
        mixedFlags);
    emit inlineTextAdvancedFormatChanged(
        representative.fontStyle, representative.kerning,
        representative.tabWidth, representative.tsume,
        representative.fauxBold, representative.fauxItalic,
        representative.underline, representative.superscript,
        representative.subscript, mixedFlags);
    emit inlineTextSelectionAppearanceChanged(
        representative.appearance.fillEnabled,
        representative.appearance.fillColor,
        representative.appearance.strokeEnabled,
        representative.appearance.strokeColor,
        representative.appearance.strokeWidth,
        static_cast<int>(representative.appearance.strokePosition),
        representative.appearance.shadowEnabled,
        representative.appearance.shadowColor,
        representative.appearance.shadowDistance,
        representative.appearance.shadowAngle,
        representative.appearance.shadowSoftness,
        representative.appearance.shadowOpacity,
        representative.appearance.backgroundEnabled,
        representative.appearance.backgroundColor,
        representative.appearance.backgroundPadding,
        mixedFlags);

    auto alignmentFromQt = [](Qt::Alignment alignment) {
        if (alignment.testFlag(Qt::AlignRight)) return GTextAlign::Right;
        if (alignment.testFlag(Qt::AlignJustify)) return GTextAlign::Justify;
        if (alignment.testFlag(Qt::AlignHCenter)) return GTextAlign::Center;
        return GTextAlign::Left;
    };
    GTextAlign paragraphAlignment = alignmentFromQt(
        cursor.blockFormat().alignment());
    bool paragraphRtl = cursor.blockFormat().layoutDirection()
        == Qt::RightToLeft;
    uint32_t paragraphMixed = mixedFlags;
    if (cursor.hasSelection()) {
        const int start = cursor.selectionStart();
        const int end = cursor.selectionEnd();
        bool haveBlock = false;
        for (QTextBlock block = m_inlineTextEdit->document()->findBlock(start);
             block.isValid() && block.position() < end; block = block.next()) {
            const GTextAlign alignment = alignmentFromQt(
                block.blockFormat().alignment());
            const bool rtl = block.blockFormat().layoutDirection()
                == Qt::RightToLeft;
            if (!haveBlock) {
                paragraphAlignment = alignment;
                paragraphRtl = rtl;
                haveBlock = true;
            } else {
                if (paragraphAlignment != alignment)
                    paragraphMixed |= InlineMixedParagraph;
                if (paragraphRtl != rtl)
                    paragraphMixed |= InlineMixedDirection;
            }
        }
    }
    emit inlineParagraphFormatChanged(static_cast<int>(paragraphAlignment),
                                      paragraphRtl, paragraphMixed);
}

bool TransformOverlayWidget::focusIsInInlineFormattingUi() const
{
    QObject* focus = QApplication::focusWidget();
    if (focus == m_inlineTextEdit) return true;
    auto belongsTo = [](QObject* object, QWidget* panel) {
        if (!panel) return false;
        for (; object; object = object->parent())
            if (object == panel) return true;
        return false;
    };
    auto isFormattingUi = [&](QObject* object) {
        if (belongsTo(object, m_inlineTextFormattingWidget)) return true;
        for (const auto& panel : m_inlineTextAdditionalFormattingWidgets)
            if (belongsTo(object, panel)) return true;
        return false;
    };
    if (isFormattingUi(focus)) return true;
    if (isFormattingUi(QApplication::activePopupWidget())) return true;
    return false;
}

void TransformOverlayWidget::finishInlineTextEdit(bool cancel)
{
    if (!m_inlineTextEdit || !m_inlineTextEdit->isVisible()
        || m_committingInlineText) return;

    m_committingInlineText = true;
    m_inlineEditorFocusSettling = false;
    m_inlinePointerRerouted = false;
    spdlog::debug("[INLINE-TEXT] finish session={} cancel={} focused={}",
                 m_inlineEditSession, cancel, m_inlineTextEdit->hasFocus());
    const QString text = cancel
        ? QString::fromStdString(m_preEditOriginalText)
        : m_inlineTextEdit->toPlainText();
    m_committedInlineTextStyles = cancel
        ? m_originalInlineTextStyles
        : collectInlineTextStyles();
    m_committedInlineParagraphStyles = cancel
        ? m_originalInlineParagraphStyles
        : collectInlineParagraphStyles();
    m_inlineTextEdit->hide();
    if (m_inlineCaretBlinkTimer) m_inlineCaretBlinkTimer->stop();
    m_inlineCaretVisible = false;
    m_overlay = m_savedOverlayBeforeEdit;
    m_preEditOriginalText.clear();
    update();
    emit inlineTextCommitted(text);
    m_committingInlineText = false;
    setFocus();
}

void TransformOverlayWidget::setInlineTextSelection(int start, int length)
{
    if (!m_inlineTextEdit) return;
    const int textLength = m_inlineTextEdit->toPlainText().size();
    start = std::clamp(start, 0, textLength);
    const int end = std::clamp(start + std::max(0, length), start, textLength);
    QTextCursor cursor(m_inlineTextEdit->document());
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    m_inlineTextEdit->setTextCursor(cursor);
}

std::pair<int, int> TransformOverlayWidget::inlineTextSelection() const
{
    if (!m_inlineTextEdit) return {0, 0};
    const QTextCursor cursor = m_inlineTextEdit->textCursor();
    return {cursor.selectionStart(),
            cursor.selectionEnd() - cursor.selectionStart()};
}

QPainterPath TransformOverlayWidget::inlineTextSelectionPath() const
{
    QPainterPath path;
    if (!m_inlineTextEdit || !m_inlineTextEdit->isVisible()) return path;

    const QTextCursor selection = m_inlineTextEdit->textCursor();
    if (!selection.hasSelection()) return path;

    const QString source = m_inlineTextEdit->toPlainText();
    const int sourceSize = static_cast<int>(source.size());
    const int start = std::clamp(selection.selectionStart(), 0, sourceSize);
    const int end = std::clamp(selection.selectionEnd(), start, sourceSize);
    const TransformOverlayInfo layout = inlineTextLayoutOverlay();

    // Preferred path: every rectangle comes directly from two adjacent
    // renderer-owned insertion boundaries. These are the same coordinates
    // used by inlineTextCaretLine() and inlineTextPositionAtGlobal().
    if (layout.useContentRect
        && layout.textCarets.size() >= static_cast<size_t>(source.size() + 1)) {
        for (int position = start; position < end; ++position) {
            if (source.at(position) == QChar('\n')
                || source.at(position) == QChar('\r')) {
                continue;
            }
            const auto& before = layout.textCarets[static_cast<size_t>(position)];
            const auto& after = layout.textCarets[static_cast<size_t>(position + 1)];
            if (!before.valid || !after.valid) continue;

            // A wrapped-line boundary can put the two caret positions on
            // different rows. Never bridge those rows with one giant quad.
            const double beforeCenter = (before.top + before.bottom) * 0.5;
            const double afterCenter = (after.top + after.bottom) * 0.5;
            const double lineTolerance = std::max(1.0,
                std::min(before.bottom - before.top,
                         after.bottom - after.top) * 0.25);
            if (std::abs(beforeCenter - afterCenter) > lineTolerance)
                continue;

            TransformOverlayInfo character = m_savedOverlayBeforeEdit;
            character.contentL = std::min(before.x, after.x);
            character.contentR = std::max(before.x, after.x);
            character.contentT = std::min(before.top, after.top);
            character.contentB = std::max(before.bottom, after.bottom);
            QPointF corners[4];
            computeOverlayCornersFor(character, corners);
            QPolygonF polygon;
            for (const QPointF& corner : corners) polygon << corner;
            polygon << corners[0];
            path.addPolygon(polygon);
        }
        return path;
    }

    // Captions and legacy overlays may not expose renderer carets yet. Keep
    // their selection visible with Qt's own cursor rectangles, while still
    // painting it in the shared overlay instead of allowing a second native
    // highlight surface to drift independently.
    QWidget* viewport = m_inlineTextEdit->viewport();
    for (int position = start; position < end; ++position) {
        if (source.at(position) == QChar('\n')
            || source.at(position) == QChar('\r')) {
            continue;
        }
        QTextCursor before(m_inlineTextEdit->document());
        QTextCursor after(m_inlineTextEdit->document());
        before.setPosition(position);
        after.setPosition(position + 1);
        const QRect beforeRect = m_inlineTextEdit->cursorRect(before);
        const QRect afterRect = m_inlineTextEdit->cursorRect(after);
        if (std::abs(beforeRect.center().y() - afterRect.center().y())
            > std::max(beforeRect.height(), afterRect.height()) / 2) {
            continue;
        }
        const QPoint globalTopLeft = viewport->mapToGlobal(QPoint(
            std::min(beforeRect.left(), afterRect.left()),
            std::min(beforeRect.top(), afterRect.top())));
        const QPoint globalBottomRight = viewport->mapToGlobal(QPoint(
            std::max(beforeRect.left(), afterRect.left()),
            std::max(beforeRect.bottom(), afterRect.bottom())));
        const QPoint localTopLeft = mapFromGlobal(globalTopLeft);
        const QPoint localBottomRight = mapFromGlobal(globalBottomRight);
        path.addRect(QRectF(localTopLeft, localBottomRight).normalized());
    }
    return path;
}

QLineF TransformOverlayWidget::inlineTextCaretLine() const
{
    if (!m_inlineTextEdit || !m_inlineTextEdit->isVisible()) return {};
    const QTextCursor cursor = m_inlineTextEdit->textCursor();
    if (cursor.hasSelection()) return {};

    const TransformOverlayInfo layout = inlineTextLayoutOverlay();
    const int cursorPosition = cursor.position();
    if (cursorPosition >= 0
        && cursorPosition < static_cast<int>(layout.textCarets.size())) {
        const auto& exact = layout.textCarets[static_cast<size_t>(cursorPosition)];
        if (exact.valid) {
            TransformOverlayInfo caretOverlay = m_savedOverlayBeforeEdit;
            caretOverlay.contentL = caretOverlay.contentR = exact.x;
            caretOverlay.contentT = exact.top;
            caretOverlay.contentB = exact.bottom;
            QPointF caretCorners[4];
            computeOverlayCornersFor(caretOverlay, caretCorners);
            return QLineF(caretCorners[0], caretCorners[3]);
        }
    }
    QPointF layoutCorners[4];
    computeOverlayCornersFor(layout, layoutCorners);
    QPointF verticalAxis = layoutCorners[3] - layoutCorners[0];
    const double verticalLength = std::hypot(verticalAxis.x(), verticalAxis.y());
    if (verticalLength > 1.0e-6)
        verticalAxis /= verticalLength;
    else
        verticalAxis = QPointF(0.0, 1.0);

    QPointF start;
    const QTextBlock block = cursor.block();
    const bool canUseRendererMetrics = block.isValid()
        && layout.useTextLayoutRect && !m_inlineUsesParagraphBox
        && !m_inlineBaseRightToLeft
        && layout.contentCanvasW > 0.0f
        && layout.textLayoutR > layout.textLayoutL;
    if (canUseRendererMetrics) {
        // Recreate the renderer's full-resolution advances. Measuring the
        // hidden editor's small on-screen font lets hinting/rounding error
        // accumulate until the visible caret is a character or more away.
        const int blockLength = static_cast<int>(block.text().size());
        const int positionInBlock = std::clamp(
            cursor.position() - block.position(), 0, blockLength);
        const QTextCharFormat fallback = cursor.charFormat();
        const double lineWidth = inlineBlockAdvance(
            block, block.text().size(), m_inlineFontPointScale, fallback);
        const double prefixWidth = inlineBlockAdvance(
            block, positionInBlock, m_inlineFontPointScale, fallback);

        Qt::Alignment alignment = block.blockFormat().alignment();
        if (!(alignment & (Qt::AlignLeft | Qt::AlignRight
                           | Qt::AlignHCenter | Qt::AlignJustify)))
            alignment = m_inlineEditAlignH;
        double offsetFromAnchor = prefixWidth;
        if (alignment & Qt::AlignRight)
            offsetFromAnchor -= lineWidth;
        else if (!(alignment & Qt::AlignLeft))
            offsetFromAnchor -= lineWidth * 0.5;

        // The renderer aligns every point-text line around canvas centre.
        // Map that exact canvas anchor and one canvas-X unit through the same
        // layer + clip transforms as the visible compositor output.
        TransformOverlayInfo anchor = m_savedOverlayBeforeEdit;
        const float canvasCenterX = anchor.contentCanvasW * 0.5f;
        anchor.contentL = anchor.contentR = canvasCenterX;
        anchor.contentT = anchor.contentB = anchor.textLayoutT;
        QPointF anchorCorners[4];
        computeOverlayCornersFor(anchor, anchorCorners);

        const double layoutCanvasWidth = static_cast<double>(
            layout.textLayoutR - layout.textLayoutL);
        const QPointF pixelsPerCanvasX =
            (layoutCorners[1] - layoutCorners[0]) / layoutCanvasWidth;
        start = anchorCorners[0] + pixelsPerCanvasX * offsetFromAnchor;
    } else {
        // Paragraph wrapping and RTL reordering remain owned by QTextLayout.
        // Preserve their native horizontal cursor mapping while still using
        // the compositor's stable vertical line origin.
        const QRect nativeCursor = m_inlineTextEdit->cursorRect(cursor);
        const QPoint nativeGlobal = m_inlineTextEdit->viewport()
            ->mapToGlobal(nativeCursor.topLeft());
        const QPoint nativeLocal = mapFromGlobal(nativeGlobal);
        start = QPointF(nativeLocal.x(), layoutCorners[0].y());
    }

    start += verticalAxis
        * (std::max(0, cursor.blockNumber()) * m_inlineRendererLineAdvancePx);
    return QLineF(start, start + verticalAxis * m_inlineRendererCaretHeightPx);
}

int TransformOverlayWidget::inlineTextPositionAtGlobal(const QPoint& global) const
{
    if (!m_inlineTextEdit) return 0;
    const TransformOverlayInfo layout = inlineTextLayoutOverlay();
    if (!layout.textCarets.empty()) {
        const QPointF widgetPoint = mapFromGlobal(global);
        int closestPosition = 0;
        double closestDistanceSquared = std::numeric_limits<double>::max();
        for (size_t position = 0; position < layout.textCarets.size(); ++position) {
            const auto& exact = layout.textCarets[position];
            if (!exact.valid) continue;
            TransformOverlayInfo caretOverlay = m_savedOverlayBeforeEdit;
            caretOverlay.contentL = caretOverlay.contentR = exact.x;
            caretOverlay.contentT = exact.top;
            caretOverlay.contentB = exact.bottom;
            QPointF caretCorners[4];
            computeOverlayCornersFor(caretOverlay, caretCorners);
            const QPointF a = caretCorners[0];
            const QPointF segment = caretCorners[3] - a;
            const double lengthSquared = QPointF::dotProduct(segment, segment);
            double t = lengthSquared > 1.0e-12
                ? QPointF::dotProduct(widgetPoint - a, segment) / lengthSquared
                : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            const QPointF delta = widgetPoint - (a + segment * t);
            const double distanceSquared = QPointF::dotProduct(delta, delta);
            if (distanceSquared < closestDistanceSquared) {
                closestDistanceSquared = distanceSquared;
                closestPosition = static_cast<int>(position);
            }
        }
        return closestPosition;
    }
    const bool canUseRendererMetrics = layout.useTextLayoutRect
        && !m_inlineUsesParagraphBox && !m_inlineBaseRightToLeft
        && layout.contentCanvasW > 0.0f
        && layout.textLayoutR > layout.textLayoutL;
    if (!canUseRendererMetrics) {
        const QPoint viewportPoint =
            m_inlineTextEdit->viewport()->mapFromGlobal(global);
        return m_inlineTextEdit->cursorForPosition(viewportPoint).position();
    }

    QPointF layoutCorners[4];
    computeOverlayCornersFor(layout, layoutCorners);
    QPointF verticalAxis = layoutCorners[3] - layoutCorners[0];
    const double verticalLength = std::hypot(verticalAxis.x(), verticalAxis.y());
    if (verticalLength > 1.0e-6)
        verticalAxis /= verticalLength;
    else
        verticalAxis = QPointF(0.0, 1.0);

    const QPointF widgetPoint = mapFromGlobal(global);
    const QPointF fromTop = widgetPoint - layoutCorners[0];
    const double verticalDistance = QPointF::dotProduct(fromTop, verticalAxis);
    int lineIndex = static_cast<int>(std::floor(
        verticalDistance / std::max(1.0, m_inlineRendererLineAdvancePx)));
    lineIndex = std::clamp(lineIndex, 0,
        std::max(0, m_inlineTextEdit->document()->blockCount() - 1));

    QTextBlock block = m_inlineTextEdit->document()->begin();
    for (int i = 0; i < lineIndex && block.isValid(); ++i)
        block = block.next();
    if (!block.isValid()) return m_inlineTextEdit->toPlainText().size();

    TransformOverlayInfo anchor = m_savedOverlayBeforeEdit;
    const float canvasCenterX = anchor.contentCanvasW * 0.5f;
    anchor.contentL = anchor.contentR = canvasCenterX;
    anchor.contentT = anchor.contentB = anchor.textLayoutT;
    QPointF anchorCorners[4];
    computeOverlayCornersFor(anchor, anchorCorners);
    const double layoutCanvasWidth = static_cast<double>(
        layout.textLayoutR - layout.textLayoutL);
    const QPointF pixelsPerCanvasX =
        (layoutCorners[1] - layoutCorners[0]) / layoutCanvasWidth;
    const double horizontalScaleSquared = QPointF::dotProduct(
        pixelsPerCanvasX, pixelsPerCanvasX);
    if (horizontalScaleSquared < 1.0e-12) return block.position();
    const double clickedOffset = QPointF::dotProduct(
        widgetPoint - anchorCorners[0], pixelsPerCanvasX)
        / horizontalScaleSquared;

    const int blockLength = static_cast<int>(block.text().size());
    const QTextCharFormat fallback = m_inlineTextEdit->textCursor().charFormat();
    const double lineWidth = inlineBlockAdvance(
        block, blockLength, m_inlineFontPointScale, fallback);
    Qt::Alignment alignment = block.blockFormat().alignment();
    if (!(alignment & (Qt::AlignLeft | Qt::AlignRight
                       | Qt::AlignHCenter | Qt::AlignJustify)))
        alignment = m_inlineEditAlignH;
    const double lineOrigin = (alignment & Qt::AlignRight) ? -lineWidth
        : ((alignment & Qt::AlignLeft) ? 0.0 : -lineWidth * 0.5);

    int closest = 0;
    double closestDistance = std::numeric_limits<double>::max();
    for (int position = 0; position <= blockLength; ++position) {
        const double boundary = lineOrigin + inlineBlockAdvance(
            block, position, m_inlineFontPointScale, fallback);
        const double distance = std::abs(boundary - clickedOffset);
        if (distance < closestDistance) {
            closestDistance = distance;
            closest = position;
        }
    }
    return block.position() + closest;
}

namespace {
void refocusInlineEditor(QPlainTextEdit* edit)
{
    if (!edit) return;
    edit->show();
    edit->raise();
    edit->activateWindow();
    edit->setFocus(Qt::OtherFocusReason);
}
}

bool TransformOverlayWidget::applyInlineTextFontFamily(const QString& family)
{
    if (!isInlineTextEditing() || family.isEmpty()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setFontFamilies(QStringList{family});
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextFontSize(float pointSizeRef)
{
    if (!isInlineTextEditing() || !std::isfinite(pointSizeRef)
        || pointSizeRef <= 0.0f) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    const double scale = m_inlineFontPointScale > 1.0e-6
        ? m_inlineFontPointScale : 1.0;
    const auto applyToCursor = [this, pointSizeRef, scale](QTextCursor& target) {
        const QTextCharFormat existing = target.charFormat();
        double oldScreenSize = existing.fontPointSize();
        if (oldScreenSize <= 0.0)
            oldScreenSize = m_inlineTextEdit->font().pointSizeF();
        const double oldReferenceSize = std::max(1.0, oldScreenSize / scale);
        const double absoluteBaseline =
            existing.baselineOffset() * oldReferenceSize / 100.0;
        QTextCharFormat format;
        format.setFontPointSize(std::max(1.0,
            static_cast<double>(pointSizeRef) * scale));
        format.setBaselineOffset(100.0 * absoluteBaseline / pointSizeRef);
        target.mergeCharFormat(format);
    };
    if (selection.hasSelection()) {
        for (int position = selection.selectionStart();
             position < selection.selectionEnd(); ++position) {
            QTextCursor character(m_inlineTextEdit->document());
            character.setPosition(position);
            character.movePosition(QTextCursor::NextCharacter,
                                   QTextCursor::KeepAnchor);
            applyToCursor(character);
        }
    } else {
        QTextCursor caret = selection;
        applyToCursor(caret);
        m_inlineTextEdit->setCurrentCharFormat(caret.charFormat());
    }
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextFontWeight(int weight)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setFontWeight(std::clamp(weight, 1, 1000));
    format.setProperty(kInlineActualWeightProperty,
                       std::clamp(weight, 1, 1000));
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextItalic(bool italic)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setFontItalic(italic);
    format.setProperty(kInlineActualItalicProperty, italic);
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextCapitalization(bool allCaps,
                                                            bool smallCaps)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setFontCapitalization(allCaps ? QFont::AllUppercase
        : smallCaps ? QFont::SmallCaps : QFont::MixedCase);
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextTracking(float trackingRef)
{
    if (!isInlineTextEditing() || !std::isfinite(trackingRef)) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    auto applyToCursor = [this, trackingRef](QTextCursor& target) {
        const QTextCharFormat existing = target.charFormat();
        const float kerning = existing.hasProperty(kInlineKerningProperty)
            ? existing.property(kInlineKerningProperty).toFloat()
            : m_inlineBaseKerning;
        QTextCharFormat format;
        format.setProperty(kInlineTrackingProperty, trackingRef);
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing((trackingRef + kerning)
                                    * m_inlinePixelScale);
        target.mergeCharFormat(format);
    };
    if (selection.hasSelection()) {
        for (int position = selection.selectionStart();
             position < selection.selectionEnd(); ++position) {
            QTextCursor character(m_inlineTextEdit->document());
            character.setPosition(position);
            character.movePosition(QTextCursor::NextCharacter,
                                   QTextCursor::KeepAnchor);
            applyToCursor(character);
        }
    } else {
        QTextCursor caret = selection;
        applyToCursor(caret);
        m_inlineTextEdit->setCurrentCharFormat(caret.charFormat());
    }
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextBaselineShift(float baselineShiftRef)
{
    if (!isInlineTextEditing() || !std::isfinite(baselineShiftRef)) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    const auto applyToCursor = [this, baselineShiftRef](QTextCursor& target) {
        const QTextCharFormat existing = target.charFormat();
        double screenPointSize = existing.fontPointSize();
        if (screenPointSize <= 0.0)
            screenPointSize = m_inlineTextEdit->font().pointSizeF();
        const double scale = m_inlineFontPointScale > 1.0e-6
            ? m_inlineFontPointScale : 1.0;
        const double referencePointSize = std::max(1.0, screenPointSize / scale);
        QTextCharFormat format;
        // Qt expresses baseline offset as a percentage of the active font
        // size; converting per character preserves an absolute shift across
        // a mixed-size selection, as Premiere does.
        format.setBaselineOffset(100.0 * baselineShiftRef / referencePointSize);
        target.mergeCharFormat(format);
    };

    if (selection.hasSelection()) {
        for (int position = selection.selectionStart();
             position < selection.selectionEnd(); ++position) {
            QTextCursor character(m_inlineTextEdit->document());
            character.setPosition(position);
            character.movePosition(QTextCursor::NextCharacter,
                                   QTextCursor::KeepAnchor);
            applyToCursor(character);
        }
    } else {
        QTextCursor caret = selection;
        applyToCursor(caret);
        m_inlineTextEdit->setCurrentCharFormat(caret.charFormat());
    }
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextLeading(float leadingRef)
{
    if (!isInlineTextEditing() || !std::isfinite(leadingRef)
        || leadingRef < 0.0f) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setProperty(kInlineLeadingProperty, leadingRef);
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    updateInlineBlockLeading(m_inlineTextEdit, m_inlineBaseLeading,
                             m_inlinePixelScale);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextFontStyle(const QString& styleName)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    QFont font = m_inlineTextEdit->currentCharFormat().font();
    font.setStyleName(styleName);
    format.setFont(font, QTextCharFormat::FontPropertiesSpecifiedOnly);
    format.setProperty(kInlineFontStyleProperty, styleName);
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextKerning(float kerningRef)
{
    if (!isInlineTextEditing() || !std::isfinite(kerningRef)) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    auto applyToCursor = [this, kerningRef](QTextCursor& target) {
        const QTextCharFormat existing = target.charFormat();
        const float tracking = existing.hasProperty(kInlineTrackingProperty)
            ? existing.property(kInlineTrackingProperty).toFloat()
            : m_inlineBaseTracking;
        QTextCharFormat format;
        format.setProperty(kInlineKerningProperty, kerningRef);
        format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        format.setFontLetterSpacing((tracking + kerningRef)
                                    * m_inlinePixelScale);
        target.mergeCharFormat(format);
    };
    if (selection.hasSelection()) {
        for (int position = selection.selectionStart();
             position < selection.selectionEnd(); ++position) {
            QTextCursor character(m_inlineTextEdit->document());
            character.setPosition(position);
            character.movePosition(QTextCursor::NextCharacter,
                                   QTextCursor::KeepAnchor);
            applyToCursor(character);
        }
    } else {
        QTextCursor caret = selection;
        applyToCursor(caret);
        m_inlineTextEdit->setCurrentCharFormat(caret.charFormat());
    }
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextTabWidth(float tabWidthRef)
{
    if (!isInlineTextEditing() || !std::isfinite(tabWidthRef)
        || tabWidthRef <= 0.0f) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setProperty(kInlineTabWidthProperty, tabWidthRef);
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    updateInlineDocumentTabWidth(m_inlineTextEdit, m_inlineBaseTabWidth,
                                 m_inlinePixelScale);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextTsume(float tsumeRef)
{
    if (!isInlineTextEditing() || !std::isfinite(tsumeRef)) return false;
    tsumeRef = std::clamp(tsumeRef, 0.0f, 90.0f);
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setProperty(kInlineTsumeProperty, tsumeRef);
    QFont font = m_inlineTextEdit->currentCharFormat().font();
    const double baseScale = std::clamp(
        1.0 - static_cast<double>(m_inlineBaseTsume) / 100.0, 0.1, 1.0);
    const double scale = std::clamp(
        1.0 - static_cast<double>(tsumeRef) / 100.0, 0.1, 1.0);
    font.setStretch(std::clamp(static_cast<int>(std::round(
        m_inlineFontStretch * scale / baseScale)), 1, 4000));
    format.setFontStretch(font.stretch());
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextFauxStyles(bool fauxBold,
                                                        bool fauxItalic)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setProperty(kInlineFauxBoldProperty, fauxBold);
    format.setProperty(kInlineFauxItalicProperty, fauxItalic);
    const QTextCharFormat current = m_inlineTextEdit->currentCharFormat();
    const int actualWeight = current.hasProperty(kInlineActualWeightProperty)
        ? current.property(kInlineActualWeightProperty).toInt()
        : current.fontWeight();
    const bool actualItalic = current.hasProperty(kInlineActualItalicProperty)
        ? current.property(kInlineActualItalicProperty).toBool()
        : current.fontItalic();
    format.setFontWeight(fauxBold ? std::max(700, actualWeight) : actualWeight);
    format.setFontItalic(fauxItalic || actualItalic);
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextUnderline(bool underline)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setFontUnderline(underline);
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextScript(bool superscript,
                                                    bool subscript)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setVerticalAlignment(superscript
        ? QTextCharFormat::AlignSuperScript
        : subscript ? QTextCharFormat::AlignSubScript
                    : QTextCharFormat::AlignNormal);
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextFill(bool enabled, uint32_t color)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setProperty(kInlineFillEnabledProperty, enabled);
    format.setProperty(kInlineFillColorProperty, color);
    format.setForeground(QColor(Qt::transparent));
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextStroke(bool enabled,
                                                    uint32_t color,
                                                    float width,
                                                    int position)
{
    if (!isInlineTextEditing() || !std::isfinite(width)) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setProperty(kInlineStrokeEnabledProperty, enabled);
    format.setProperty(kInlineStrokeColorProperty, color);
    format.setProperty(kInlineStrokeWidthProperty, std::max(0.0f, width));
    format.setProperty(kInlineStrokePositionProperty,
                       std::clamp(position, 0, 2));
    format.setTextOutline(QPen(Qt::NoPen));
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextShadow(bool enabled,
                                                    uint32_t color,
                                                    float distance,
                                                    float angle,
                                                    float softness,
                                                    float opacity)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setProperty(kInlineShadowEnabledProperty, enabled);
    format.setProperty(kInlineShadowColorProperty, color);
    format.setProperty(kInlineShadowDistanceProperty, distance);
    format.setProperty(kInlineShadowAngleProperty, angle);
    format.setProperty(kInlineShadowSoftnessProperty, softness);
    format.setProperty(kInlineShadowOpacityProperty,
                       std::clamp(opacity, 0.0f, 1.0f));
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineTextBackground(bool enabled,
                                                        uint32_t color,
                                                        float padding)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextCharFormat format;
    format.setProperty(kInlineBackgroundEnabledProperty, enabled);
    format.setProperty(kInlineBackgroundColorProperty, color);
    format.setProperty(kInlineBackgroundPaddingProperty,
                       std::max(0.0f, padding));
    format.setBackground(QColor(Qt::transparent));
    m_inlineTextEdit->mergeCurrentCharFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineParagraphAlignment(int alignment)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextBlockFormat format;
    switch (static_cast<GTextAlign>(alignment)) {
    case GTextAlign::Left: format.setAlignment(Qt::AlignLeft); break;
    case GTextAlign::Right: format.setAlignment(Qt::AlignRight); break;
    case GTextAlign::Justify: format.setAlignment(Qt::AlignJustify); break;
    case GTextAlign::Center:
    default: format.setAlignment(Qt::AlignHCenter); break;
    }
    QTextCursor blocks = selection;
    blocks.mergeBlockFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    resizeInlineTextEditorToDocument();
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

bool TransformOverlayWidget::applyInlineParagraphDirection(bool rightToLeft)
{
    if (!isInlineTextEditing()) return false;
    const QTextCursor selection = inlineTextFormattingCursor();
    QTextBlockFormat format;
    format.setLayoutDirection(rightToLeft
        ? Qt::RightToLeft : Qt::LeftToRight);
    QTextCursor blocks = selection;
    blocks.mergeBlockFormat(format);
    m_inlineTextEdit->setTextCursor(selection);
    notifyInlineTextSelectionFormat();
    refocusInlineEditor(m_inlineTextEdit);
    return true;
}

} // namespace rt
