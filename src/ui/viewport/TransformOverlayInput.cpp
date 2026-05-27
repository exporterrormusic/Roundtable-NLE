/*
 * TransformOverlayInput.cpp - Mouse/keyboard input handling for TransformOverlayWidget.
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
#include <QMenu>
#include <QAction>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QKeyEvent>
#include <QFontMetricsF>
#include <QPointer>
#include <QTimer>
#include <algorithm>

#include <cmath>
#include <algorithm>

#include <spdlog/spdlog.h>

namespace rt {

void TransformOverlayWidget::mousePressEvent(QMouseEvent* event)
{
    // ── Right button on a motion-path waypoint → Spatial Interpolation menu
    if (event->button() == Qt::RightButton && m_motionX && m_motionY) {
        int wp = hitTestMotionWaypoint(event->position());
        if (wp >= 0) {
            const int64_t kfTime = m_motionX->keyframe(static_cast<size_t>(wp)).time;

            QMenu menu(this);
            QMenu* sub = menu.addMenu(QStringLiteral("Spatial Interpolation"));
            auto addAction = [&](const QString& label, InterpMode mode) {
                QAction* a = sub->addAction(label);
                connect(a, &QAction::triggered, this, [this, kfTime, mode]() {
                    if (!m_motionX || !m_motionY) return;
                    if (m_motionCmdStack) {
                        m_motionCmdStack->execute(
                            std::make_unique<SetKeyframeSpatialInterpCommand>(
                                m_motionX, m_motionY, kfTime, mode));
                    } else {
                        // No undo stack — still apply the change.
                        for (size_t i = 0; i < m_motionX->keyframeCount(); ++i)
                            if (m_motionX->keyframe(i).time == kfTime) {
                                m_motionX->keyframe(i).spatialInterp = mode;
                                break;
                            }
                        for (size_t i = 0; i < m_motionY->keyframeCount(); ++i)
                            if (m_motionY->keyframe(i).time == kfTime) {
                                m_motionY->keyframe(i).spatialInterp = mode;
                                break;
                            }
                    }
                    update();
                });
            };
            addAction(QStringLiteral("Linear"),            InterpMode::Linear);
            addAction(QStringLiteral("Bezier"),            InterpMode::Bezier);
            addAction(QStringLiteral("Auto Bezier"),       InterpMode::AutoBezier);
            addAction(QStringLiteral("Continuous Bezier"), InterpMode::ContinuousBezier);

            menu.exec(event->globalPosition().toPoint());
            event->accept();
            return;
        }
    }

    // ── Middle button → pan ─────────────────────────────────────────────
    if (event->button() == Qt::MiddleButton && m_vulkanVp) {
        m_dragMode = DragMode::Pan;
        m_panStartPos = event->position();
        m_panStartVpX = m_vulkanVp->viewPanX();
        m_panStartVpY = m_vulkanVp->viewPanY();
        applyCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    // ── Eyedropper tool: pick color at click location ─────────────────
    if (event->button() == Qt::LeftButton && m_editTool == 8) {
        QRectF fr = computeFrameRect();
        if (!fr.isEmpty() && m_vulkanVp) {
            QPointF wPos = event->position();
            float srcW = static_cast<float>(m_vulkanVp->srcWidth());
            float srcH = static_cast<float>(m_vulkanVp->srcHeight());
            if (srcW > 0.0f && srcH > 0.0f) {
                float frameX = static_cast<float>((wPos.x() - fr.x()) / fr.width()) * srcW;
                float frameY = static_cast<float>((wPos.y() - fr.y()) / fr.height()) * srcH;
                emit colorPicked(frameX, frameY);
                event->accept();
                return;
            }
        }
    }

    // ── Ctrl+Click: add point on mask border ──────────────────────────
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)
        && m_masks && !m_masks->empty()) {
        QRectF fr = computeFrameRect();
        if (!fr.isEmpty()) {
            int maskIdx = -1;
            if (addPointOnMaskEdge(event->position(), fr, maskIdx)) {
                event->accept();
                return;
            }
        }
    }

    // ── Motion-path spatial handle drag ─────────────────────────────────
    if (event->button() == Qt::LeftButton && m_motionX && m_motionY) {
        int kfIdx = -1;
        bool isIn = false;
        if (hitTestMotionHandle(event->position(), kfIdx, isIn)) {
            m_dragMode        = DragMode::DragMotionHandle;
            m_dragMotionKfIdx = kfIdx;
            m_dragMotionIsIn  = isIn;
            const auto& kfx   = m_motionX->keyframe(static_cast<size_t>(kfIdx));
            const auto& kfy   = m_motionY->keyframe(static_cast<size_t>(kfIdx));
            m_dragKfTime      = kfx.time;
            m_dragOrigInX     = kfx.spatialInX;
            m_dragOrigInY     = kfy.spatialInY;
            m_dragOrigOutX    = kfx.spatialOutX;
            m_dragOrigOutY    = kfy.spatialOutY;
            applyCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
    }

    // ── Mask control point interaction ──────────────────────────────────
    if (event->button() == Qt::LeftButton && m_masks && !m_masks->empty()) {
        QPointF wPos = event->position();
        int maskIdx = -1;
        int handleIdx = hitTestMaskHandle(wPos, maskIdx);
        if (handleIdx >= 0 && maskIdx >= 0) {
            m_dragMode = DragMode::DragMaskPoint;
            m_dragMaskIndex = maskIdx;
            m_dragMaskHandle = handleIdx;
            m_dragStartMask = (*m_masks)[static_cast<size_t>(maskIdx)];
            m_dragStartWidget = wPos;
            applyCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
        // Click inside mask body → move the mask
        maskIdx = hitTestMaskBody(wPos);
        if (maskIdx >= 0) {
            m_dragMode = DragMode::DragMaskPoint;
            m_dragMaskIndex = maskIdx;
            m_dragMaskHandle = INT_MAX; // body drag sentinel
            m_dragStartMask = (*m_masks)[static_cast<size_t>(maskIdx)];
            m_dragStartWidget = wPos;
            applyCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
    }

    // ── Transform overlay interaction ───────────────────────────────────
    if (event->button() == Qt::LeftButton && m_overlay.visible) {
        QPointF wPos = event->position();

        // Ctrl-modified click on the anchor crosshair → anchor drag.
        // Checked BEFORE the body test because the anchor sits inside
        // the body and the body would otherwise always win. Without Ctrl
        // the anchor is completely ungrabbable, eliminating the dead zone
        // users saw near the center of the transform box.
        if ((event->modifiers() & Qt::ControlModifier) && m_vulkanVp) {
            QRectF fr = computeFrameRect();
            if (!fr.isEmpty()) {
                float canvasW = 0.0f, canvasH = 0.0f;
                float anchorPxX = 0.0f, anchorPxY = 0.0f;
                float posPxX    = 0.0f, posPxY    = 0.0f;
                bool  testAnchor = false;
                if (m_overlay.useContentRect &&
                    m_overlay.contentCanvasW > 0.0f && m_overlay.contentCanvasH > 0.0f)
                {
                    canvasW = m_overlay.contentCanvasW;
                    canvasH = m_overlay.contentCanvasH;
                    anchorPxX = m_overlay.anchorX;
                    anchorPxY = m_overlay.anchorY;
                    posPxX    = m_overlay.posX;
                    posPxY    = m_overlay.posY;
                    testAnchor = true;
                } else if (m_vulkanVp->srcWidth() > 0 && m_vulkanVp->srcHeight() > 0) {
                    canvasW = static_cast<float>(m_vulkanVp->srcWidth());
                    canvasH = static_cast<float>(m_vulkanVp->srcHeight());
                    constexpr float REF_W = 1920.0f;
                    constexpr float REF_H = 1080.0f;
                    anchorPxX = m_overlay.anchorX * (canvasW / REF_W);
                    anchorPxY = m_overlay.anchorY * (canvasH / REF_H);
                    posPxX    = m_overlay.posX    * (canvasW / REF_W);
                    posPxY    = m_overlay.posY    * (canvasH / REF_H);
                    testAnchor = true;
                }
                if (testAnchor) {
                    const float ax = canvasW * 0.5f + posPxX + anchorPxX;
                    const float ay = canvasH * 0.5f + posPxY + anchorPxY;
                    const QPointF anchorPt(
                        fr.x() + (static_cast<double>(ax) / canvasW) * fr.width(),
                        fr.y() + (static_cast<double>(ay) / canvasH) * fr.height());
                    const double dx = wPos.x() - anchorPt.x();
                    const double dy = wPos.y() - anchorPt.y();
                    // Generous Ctrl-only hit radius — no risk of intercepting
                    // body clicks since Ctrl gates the test.
                    constexpr double kAnchorCtrlHitRadius = 14.0;
                    if (dx * dx + dy * dy <= kAnchorCtrlHitRadius * kAnchorCtrlHitRadius) {
                        m_dragMode = DragMode::MoveAnchor;
                        m_dragStartWidget  = wPos;
                        m_dragStartAnchorX = m_overlay.anchorX;
                        m_dragStartAnchorY = m_overlay.anchorY;
                        applyCursor(Qt::SizeAllCursor);
                        event->accept();
                        return;
                    }
                }
            }
        }

        // Body move has priority over corner/anchor handles (Premiere Pro
        // behavior — anything inside the transform box moves the layer).
        //
        // For multi-selection: a click inside ANY of the selected boxes
        // (focused OR sibling) initiates the body drag. The focused
        // layer's pos still drives the delta — the group-move logic in
        // the workspace applies that delta to every sibling — so the
        // user can grab any selected box and pull the whole group.
        bool bodyHit = hitTestBody(wPos);
        if (!bodyHit) {
            for (const auto& sov : m_secondaryOverlays) {
                if (!sov.visible) continue;
                QPointF sc[4];
                computeOverlayCornersFor(sov, sc);
                if (rt::hitTestBody(wPos, sc)) {
                    bodyHit = true;
                    break;
                }
            }
        }
        if (bodyHit) {
            // When masks are active, don't let a body click move the layer;
            // body clicks should move the mask instead (handled above).
            if (m_masks && !m_masks->empty()) {
                event->accept();
                return;
            }
            m_dragMode = DragMode::MoveBody;
            m_dragStartWidget = wPos;
            m_dragStartPosX = m_overlay.posX;
            m_dragStartPosY = m_overlay.posY;
            m_dragStartScX  = m_overlay.scaleX;
            m_dragStartScY  = m_overlay.scaleY;
            m_dragStartRot  = m_overlay.rotation;
            applyCursor(Qt::ArrowCursor);   // no special move cursor
            event->accept();
            return;
        }

        int handle = hitTestHandle(wPos);
        if (handle >= 0) {
            m_dragMode = DragMode::ScaleCorner;
            m_dragHandle = handle;
            m_dragStartWidget = wPos;
            m_dragStartPosX = m_overlay.posX;
            m_dragStartPosY = m_overlay.posY;
            m_dragStartScX  = m_overlay.scaleX;
            m_dragStartScY  = m_overlay.scaleY;
            m_dragStartRot  = m_overlay.rotation;
            applyCursor(Qt::SizeFDiagCursor);
            event->accept();
            return;
        }

        int rotHandle = hitTestRotate(wPos);
        if (rotHandle >= 0) {
            m_dragMode = DragMode::RotateCorner;
            m_dragHandle = rotHandle;
            m_dragStartWidget = wPos;
            m_dragStartPosX = m_overlay.posX;
            m_dragStartPosY = m_overlay.posY;
            m_dragStartScX  = m_overlay.scaleX;
            m_dragStartScY  = m_overlay.scaleY;
            m_dragStartRot  = m_overlay.rotation;
            // Compute starting angle from center to mouse
            QPointF corners[4];
            computeOverlayCorners(corners);
            QPointF center = (corners[0] + corners[2]) * 0.5;
            m_dragStartAngle = static_cast<float>(
                std::atan2(wPos.y() - center.y(), wPos.x() - center.x())
                * 180.0 / 3.14159265358979);
            applyCursor(rotateCursor());
            event->accept();
            return;
        }

    }

    // ── Left-click on empty area: emit signal for text tool etc. ────────
    if (event->button() == Qt::LeftButton) {
        if (m_vulkanVp) {
            // IMPORTANT: this handler is reached via eventFilter() forwarding
            // mouse events from m_vulkanVp's native QWindow (HWND). So
            // event->position() is in HWND-LOCAL coordinates — NOT the
            // overlay widget's local coords. computeFrameRect() returns a
            // rect in overlay-widget-local coords (it applies
            // `+ hwndOff - vpOffset` to shift FROM HWND space INTO overlay
            // space). Mixing those two spaces produces a constant offset
            // exactly equal to (hwndOff - vpOffset) — historically seen as
            // text/shapes landing ~31 px below the cursor when the panel
            // header clipped the HWND upward. Compute the click in the
            // composite's source pixels directly from the GPU draw rect in
            // HWND space (gpuNorm × surface), which is the SAME space the
            // event coordinates are in.
            QRectF gnorm = m_vulkanVp->gpuFrameRect();
            QSize  surf  = m_vulkanVp->nativeSurfaceSize();
            float srcW = static_cast<float>(m_vulkanVp->srcWidth());
            float srcH = static_cast<float>(m_vulkanVp->srcHeight());
            if (!gnorm.isEmpty() && surf.width() > 0 && surf.height() > 0 &&
                srcW > 0.0f && srcH > 0.0f)
            {
                QPointF wPos = event->position();
                double drawX = gnorm.x() * surf.width();
                double drawY = gnorm.y() * surf.height();
                double drawW = gnorm.width()  * surf.width();
                double drawH = gnorm.height() * surf.height();
                float frameX = static_cast<float>((wPos.x() - drawX) / drawW) * srcW;
                float frameY = static_cast<float>((wPos.y() - drawY) / drawH) * srcH;
                emit emptyAreaClicked(frameX, frameY, event->modifiers());
                event->accept();
                return;
            }
        }
    }

    // Not handled — pass through
    event->ignore();
}

void TransformOverlayWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Double-click → edit the text layer under the cursor (Premiere Pro).
    // The preceding single click already selected the layer and bound it
    // to the panels (see emptyAreaClicked handler), so we only need to
    // signal "enter text editing" with the frame-space coordinates.
    if (event->button() == Qt::LeftButton) {
        if (m_vulkanVp) {
            // Same coord-space caveat as emptyAreaClicked: events forwarded
            // from m_vulkanVp's native QWindow are in HWND-local coords, so
            // map the click directly through the GPU draw rect rather than
            // computeFrameRect (which is in overlay-widget-local coords).
            QRectF gnorm = m_vulkanVp->gpuFrameRect();
            QSize  surf  = m_vulkanVp->nativeSurfaceSize();
            float srcW = static_cast<float>(m_vulkanVp->srcWidth());
            float srcH = static_cast<float>(m_vulkanVp->srcHeight());
            if (!gnorm.isEmpty() && surf.width() > 0 && surf.height() > 0 &&
                srcW > 0.0f && srcH > 0.0f)
            {
                QPointF wPos = event->position();
                double drawX = gnorm.x() * surf.width();
                double drawY = gnorm.y() * surf.height();
                double drawW = gnorm.width()  * surf.width();
                double drawH = gnorm.height() * surf.height();
                float frameX = static_cast<float>((wPos.x() - drawX) / drawW) * srcW;
                float frameY = static_cast<float>((wPos.y() - drawY) / drawH) * srcH;
                emit textEditRequested(frameX, frameY);
                event->accept();
                return;
            }
        }
    }
    event->ignore();
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
                                                  Qt::Alignment hAlignFlag)
{
    // AABB of the selected layer's transform box (widget coords). We use
    // the LEFT / RIGHT / CENTER X coordinates of this box (depending on
    // hAlignFlag) as the anchor the editor sticks to as the user types —
    // so center-aligned text doesn't drift left and left-aligned text
    // doesn't get centered.
    double leftX, centerX, rightX, centerY;
    {
        QPointF c[4];
        computeOverlayCorners(c);
        double minX = c[0].x(), minY = c[0].y(), maxX = c[0].x(), maxY = c[0].y();
        for (int i = 1; i < 4; ++i) {
            minX = std::min(minX, c[i].x()); maxX = std::max(maxX, c[i].x());
            minY = std::min(minY, c[i].y()); maxY = std::max(maxY, c[i].y());
        }
        if (maxX - minX < 1.0 || maxY - minY < 1.0) {
            leftX   = width()  * 0.5;
            centerX = width()  * 0.5;
            rightX  = width()  * 0.5;
            centerY = height() * 0.5;
        } else {
            leftX   = minX;
            centerX = (minX + maxX) * 0.5;
            rightX  = maxX;
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
        // QPlainTextEdit replaces QLineEdit so that Shift+Enter inserts a
        // newline (Premiere Pro behavior) while plain Enter commits.
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
        // Plain text only — no rich-text paste surprises.
        m_inlineTextEdit->setTabChangesFocus(false);

        auto commit = [this]() {
            if (!m_inlineTextEdit || !m_inlineTextEdit->isVisible()) return;
            if (m_committingInlineText) return;
            m_committingInlineText = true;
            const QString t = m_inlineTextEdit->toPlainText();
            m_inlineTextEdit->hide();
            // Restore the transform overlay after editing.
            m_overlay = m_savedOverlayBeforeEdit;
            m_preEditOriginalText.clear();
            update();
            emit inlineTextCommitted(t);
            m_committingInlineText = false;
            setFocus();
        };

        // Intercept key presses: Enter commits (Premiere Pro), Shift+Enter
        // inserts a newline (Premiere Pro), Esc cancels (restores original).
        m_inlineTextEdit->installEventFilter(this);

        // Auto-grow as the user types: QPlainTextEdit doesn't auto-expand.
        // Recompute the screen geometry from the current text's pixel size
        // and re-anchor by alignment so the editor's text stays where the
        // renderer would draw it: left-aligned text grows to the right,
        // right-aligned grows to the left, center-aligned grows both ways.
        connect(m_inlineTextEdit, &QPlainTextEdit::textChanged, this,
                [this]() {
            if (!m_inlineTextEdit) return;
            QFontMetricsF fm(m_inlineTextEdit->font());
            const QString t = m_inlineTextEdit->toPlainText();

            // Width: longest line + slack for caret.
            double maxLineW = 0.0;
            const QStringList lines = t.split(QChar('\n'));
            for (const auto& line : lines)
                maxLineW = std::max(maxLineW, fm.horizontalAdvance(line));
            double slackW = std::max(8.0, fm.averageCharWidth());
            double wantW = maxLineW + slackW;
            int minW = 40;
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

            QRect r(newX, m_inlineEditAnchorCenterY - newH / 2, newW, newH);
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
    double fontPt = std::max(6.0, double(fontSizeRef) * scaleHeight / refH);

    // Force opaque alpha on the text color (the layer's fill might be
    // semi-transparent and would otherwise be unreadable while editing).
    QColor visibleColor = textColor;
    visibleColor.setAlpha(255);

    // Premiere-style invisible editor: transparent background, no border.
    // Only the glyphs and the selection highlight should be visible, so
    // the live edit looks identical to the renderer's output (just with
    // a selection block over the highlighted text).
    const QString styleSheet = QStringLiteral(
        "QPlainTextEdit { "
        "font-family: \"%1\"; "
        "font-size: %2pt; "
        "font-weight: %3; "
        "font-style: %4; "
        "background: transparent; "
        "color: %5; "
        "border: none; "
        "selection-background-color: rgba(77,158,255,180); "
        "selection-color: white; "
        "padding: 0px; }")
        .arg(fontFamily)
        .arg(fontPt, 0, 'f', 1)
        .arg(std::clamp(fontWeight, 1, 1000))
        .arg(italic ? "italic" : "normal")
        .arg(visibleColor.name(QColor::HexRgb));
    QFont qf(fontFamily, -1, std::clamp(fontWeight, 1, 1000), italic);
    qf.setPointSizeF(fontPt);
    // Anisotropic-scale support: the renderer applies painter.scale(sx, sy)
    // which stretches glyph WIDTHS by sx/sy relative to height. QFont has
    // no general anisotropic transform, but setStretch() is exactly this:
    // a percentage applied to glyph advance/width. QSS has no font-stretch
    // property so setStyleSheet below won't override it. QFontMetricsF
    // honours it too, so the textChanged auto-grow stays accurate.
    if (std::isfinite(horizontalStretch) && horizontalStretch > 0.0f) {
        const int stretchPct = std::clamp(
            static_cast<int>(std::round(horizontalStretch * 100.0f)), 1, 4000);
        qf.setStretch(stretchPct);
    }
    m_inlineTextEdit->setFont(qf);
    m_inlineTextEdit->setStyleSheet(styleSheet);

    // Apply horizontal text alignment to the document so the editor's
    // glyphs sit on the same side of its box as the renderer drew them.
    {
        QTextOption opt = m_inlineTextEdit->document()->defaultTextOption();
        opt.setAlignment(hAlignFlag);
        m_inlineTextEdit->document()->setDefaultTextOption(opt);
    }

    // Derive the editor's pixel size from the actual rendered font
    // metrics — width = enough to hold the initial string plus caret slack,
    // height = one glyph line. No border/margin padding (borderless editor),
    // so the box matches glyph bounds.
    QFontMetricsF fm(qf);
    int editH = std::max(12, static_cast<int>(std::ceil(fm.height())));
    double slack0 = std::max(8.0, fm.averageCharWidth());
    double maxLineW0 = fm.horizontalAdvance(initial);
    int editW = std::max(40,
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

    QRect g(editX,
            static_cast<int>(std::round(centerY - editH * 0.5)),
            editW, editH);

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
    m_inlineEditCenter = screenRect.center();          // legacy field
    m_inlineEditHeight = screenRect.height();

    m_inlineTextEdit->setGeometry(screenRect);
    m_inlineTextEdit->setPlainText(initial);
    m_inlineTextEdit->show();
    m_inlineTextEdit->raise();
    // Defer activation to the next event-loop tick so the window is fully
    // realised before Windows is asked to give it foreground focus. Same
    // pattern PropertiesPanel::focusGraphicTextField uses, for the same
    // "setFocus is ignored on not-yet-shown widget" reason.
    QPointer<QPlainTextEdit> edit(m_inlineTextEdit);
    QTimer::singleShot(0, edit, [edit]() {
        if (!edit) return;
        edit->activateWindow();
        edit->setFocus(Qt::MouseFocusReason);
        edit->selectAll();
    });
}

void TransformOverlayWidget::mouseMoveEvent(QMouseEvent* event)
{
    QPointF wPos = event->position();

    // ── Pan drag ────────────────────────────────────────────────────────
    if (m_dragMode == DragMode::Pan && m_vulkanVp) {
        float dx = static_cast<float>(wPos.x() - m_panStartPos.x());
        float dy = static_cast<float>(wPos.y() - m_panStartPos.y());
        m_vulkanVp->setViewPan(m_panStartVpX + dx, m_panStartVpY + dy);
        update(); // redraw overlay at new pan
        event->accept();
        return;
    }

    // ── Motion-path spatial handle drag ─────────────────────────────────
    if (m_dragMode == DragMode::DragMotionHandle && m_motionX && m_motionY &&
        (event->buttons() & Qt::LeftButton))
    {
        const QRectF fr = computeFrameRect();
        if (fr.isEmpty()) { event->accept(); return; }
        if (m_dragMotionKfIdx < 0 ||
            m_dragMotionKfIdx >= static_cast<int>(m_motionX->keyframeCount())) {
            event->accept();
            return;
        }
        auto& kfx = m_motionX->keyframe(static_cast<size_t>(m_dragMotionKfIdx));
        auto& kfy = m_motionY->keyframe(static_cast<size_t>(m_dragMotionKfIdx));

        // Convert widget pos to REF-1920 px and subtract the waypoint
        // position to get the handle offset.
        const QPointF refPos = widgetToRef(wPos, fr);
        const float newHX = static_cast<float>(refPos.x()) - kfx.value;
        const float newHY = static_cast<float>(refPos.y()) - kfy.value;

        if (m_dragMotionIsIn) {
            kfx.spatialInX = newHX;
            kfy.spatialInY = newHY;
            // Continuous Bezier: mirror the out handle collinearly with same length.
            if (kfx.spatialInterp == InterpMode::ContinuousBezier) {
                kfx.spatialOutX = -newHX;
                kfy.spatialOutY = -newHY;
            }
        } else {
            kfx.spatialOutX = newHX;
            kfy.spatialOutY = newHY;
            if (kfx.spatialInterp == InterpMode::ContinuousBezier) {
                kfx.spatialInX = -newHX;
                kfy.spatialInY = -newHY;
            }
        }
        update();
        emit motionPathLiveUpdate();
        event->accept();
        return;
    }

    // ── Mask point drag ─────────────────────────────────────────────────
    if (m_dragMode == DragMode::DragMaskPoint && m_masks &&
        (event->buttons() & Qt::LeftButton))
    {
        QRectF fr = computeFrameRect();
        if (fr.isEmpty()) return;

        auto& mask = (*m_masks)[static_cast<size_t>(m_dragMaskIndex)];
        float dxNorm = static_cast<float>((wPos.x() - m_dragStartWidget.x()) / fr.width());
        float dyNorm = static_cast<float>((wPos.y() - m_dragStartWidget.y()) / fr.height());

        if (mask.shape == MaskShape::Ellipse) {
            if (m_dragMaskHandle == 4 || m_dragMaskHandle == INT_MAX) {
                // Move center
                mask.centerX = m_dragStartMask.centerX + dxNorm;
                mask.centerY = m_dragStartMask.centerY + dyNorm;
            } else if (m_dragMaskHandle == 0 || m_dragMaskHandle == 1) {
                // Right/left cardinal → scale width
                float d = (m_dragMaskHandle == 0) ? dxNorm : -dxNorm;
                mask.width = std::max(0.01f, m_dragStartMask.width + d * 2.0f);
            } else {
                // Bottom/top cardinal → scale height
                float d = (m_dragMaskHandle == 2) ? dyNorm : -dyNorm;
                mask.height = std::max(0.01f, m_dragStartMask.height + d * 2.0f);
            }
        }
        else if (mask.shape == MaskShape::Rectangle) {
            if (m_dragMaskHandle == 4 || m_dragMaskHandle == INT_MAX) {
                mask.centerX = m_dragStartMask.centerX + dxNorm;
                mask.centerY = m_dragStartMask.centerY + dyNorm;
            } else if (m_dragMaskHandle >= 5 && m_dragMaskHandle <= 8) {
                // Mid-edge handles: resize one dimension only
                // 5=top, 6=right, 7=bottom, 8=left
                if (m_dragMaskHandle == 5) {
                    // Top edge: shrink height from top
                    mask.height  = std::max(0.01f, m_dragStartMask.height - dyNorm * 2.0f);
                } else if (m_dragMaskHandle == 7) {
                    // Bottom edge: grow height from bottom
                    mask.height  = std::max(0.01f, m_dragStartMask.height + dyNorm * 2.0f);
                } else if (m_dragMaskHandle == 6) {
                    // Right edge: grow width from right
                    mask.width   = std::max(0.01f, m_dragStartMask.width + dxNorm * 2.0f);
                } else { // 8 = left
                    // Left edge: shrink width from left
                    mask.width   = std::max(0.01f, m_dragStartMask.width - dxNorm * 2.0f);
                }
            } else {
                // Corner drag → scale width/height symmetrically
                float signX = (m_dragMaskHandle == 1 || m_dragMaskHandle == 2) ? 1.0f : -1.0f;
                float signY = (m_dragMaskHandle == 2 || m_dragMaskHandle == 3) ? 1.0f : -1.0f;
                mask.width  = std::max(0.01f, m_dragStartMask.width  + signX * dxNorm * 2.0f);
                mask.height = std::max(0.01f, m_dragStartMask.height + signY * dyNorm * 2.0f);
            }
        }
        else if (mask.shape == MaskShape::FreeDrawBezier) {
            auto vi = static_cast<size_t>(m_dragMaskHandle);
            if (vi < mask.vertices.size()) {
                // Drag single vertex
                mask.vertices[vi].x = m_dragStartMask.vertices[vi].x + dxNorm;
                mask.vertices[vi].y = m_dragStartMask.vertices[vi].y + dyNorm;
            } else {
                // Body drag — translate all vertices
                for (size_t i = 0; i < mask.vertices.size(); ++i) {
                    mask.vertices[i].x = m_dragStartMask.vertices[i].x + dxNorm;
                    mask.vertices[i].y = m_dragStartMask.vertices[i].y + dyNorm;
                }
            }
        }

        emit maskLiveUpdate();
        update();
        event->accept();
        return;
    }

    // ── Move body drag ──────────────────────────────────────────────────
    if (m_dragMode == DragMode::MoveBody && (event->buttons() & Qt::LeftButton)) {
        QRectF fr = computeFrameRect();
        if (fr.isEmpty()) return;

        // Two coordinate conventions, mirroring the anchor handler below:
        //   • Content-rect mode (graphic layers): posX/posY are CANVAS px
        //     (project resolution). On a 4K project canvas=3840, so using
        //     REF_1920 here divided the mouse-to-layer rate by 2 — the text
        //     visibly lagged the cursor.
        //   • Standard mode (video / image): posX/posY are REF-1920 px.
        float pxPerUnitX = 0.0f, pxPerUnitY = 0.0f;
        if (m_overlay.useContentRect &&
            m_overlay.contentCanvasW > 0.0f && m_overlay.contentCanvasH > 0.0f)
        {
            pxPerUnitX = static_cast<float>(fr.width())  / m_overlay.contentCanvasW;
            pxPerUnitY = static_cast<float>(fr.height()) / m_overlay.contentCanvasH;
        } else {
            constexpr float REF_W = 1920.0f;
            constexpr float REF_H = 1080.0f;
            pxPerUnitX = static_cast<float>(fr.width())  / REF_W;
            pxPerUnitY = static_cast<float>(fr.height()) / REF_H;
        }
        if (pxPerUnitX < 0.001f || pxPerUnitY < 0.001f) return;

        // Account for clip-level scale: layer position is in canvas space,
        // but the clip scale magnifies the whole canvas, so mouse movement
        // needs to be divided by the clip scale to get the correct delta.
        float effScaleX = std::max(0.001f, m_overlay.clipScaleX);
        float effScaleY = std::max(0.001f, m_overlay.clipScaleY);

        float dx = static_cast<float>(wPos.x() - m_dragStartWidget.x()) / (pxPerUnitX * effScaleX);
        float dy = static_cast<float>(wPos.y() - m_dragStartWidget.y()) / (pxPerUnitY * effScaleY);

        m_overlay.posX = m_dragStartPosX + dx;
        m_overlay.posY = m_dragStartPosY + dy;

        // Premiere-style Ctrl-snap: magnetise the overlay's AABB to the
        // frame edges and centre lines while Ctrl is held. Worked out in
        // widget pixels and converted back through the same px-per-unit
        // factor used for the drag so snap distance is zoom-invariant.
        if (event->modifiers() & Qt::ControlModifier) {
            QPointF corners[4];
            computeOverlayCorners(corners);
            double minX = corners[0].x(), maxX = corners[0].x();
            double minY = corners[0].y(), maxY = corners[0].y();
            for (int i = 1; i < 4; ++i) {
                minX = std::min(minX, corners[i].x());
                maxX = std::max(maxX, corners[i].x());
                minY = std::min(minY, corners[i].y());
                maxY = std::max(maxY, corners[i].y());
            }
            const double cX = (minX + maxX) * 0.5;
            const double cY = (minY + maxY) * 0.5;

            constexpr double kSnapPx = 10.0;
            const double tgtsX[3] = {
                fr.left(), fr.right(), fr.center().x()
            };
            const double tgtsY[3] = {
                fr.top(), fr.bottom(), fr.center().y()
            };
            const double srcsX[3] = { minX, maxX, cX };
            const double srcsY[3] = { minY, maxY, cY };

            double bestDx = 0.0, bestDxAbs = kSnapPx + 1.0;
            for (double src : srcsX) for (double tgt : tgtsX) {
                double d = std::abs(src - tgt);
                if (d < bestDxAbs) { bestDxAbs = d; bestDx = tgt - src; }
            }
            double bestDy = 0.0, bestDyAbs = kSnapPx + 1.0;
            for (double src : srcsY) for (double tgt : tgtsY) {
                double d = std::abs(src - tgt);
                if (d < bestDyAbs) { bestDyAbs = d; bestDy = tgt - src; }
            }
            if (bestDxAbs <= kSnapPx)
                m_overlay.posX += static_cast<float>(bestDx / (pxPerUnitX * effScaleX));
            if (bestDyAbs <= kSnapPx)
                m_overlay.posY += static_cast<float>(bestDy / (pxPerUnitY * effScaleY));
        }

        emit transformPositionChanged(m_overlay.posX, m_overlay.posY);
        update();
        event->accept();
        return;
    }

    // ── Anchor point drag ───────────────────────────────────────────────
    // The anchor is the rotation/scale pivot (Premiere/AE-style). Two
    // coordinate conventions:
    //   • Content-rect mode (graphic layers): anchor is canvas-px → use
    //     contentCanvasW/H to convert widget Δ → canvas Δ.
    //   • Standard mode (video / image / etc.): anchor is REF-1920 px
    //     stored on the clip's anchorX/Y tracks → convert widget Δ →
    //     REF-1920 Δ using fr.width / REF_1920.
    if (m_dragMode == DragMode::MoveAnchor && (event->buttons() & Qt::LeftButton)) {
        QRectF fr = computeFrameRect();
        if (fr.isEmpty()) return;
        float pxPerUnitX = 0.0f, pxPerUnitY = 0.0f;
        if (m_overlay.useContentRect &&
            m_overlay.contentCanvasW > 0.0f && m_overlay.contentCanvasH > 0.0f)
        {
            pxPerUnitX = static_cast<float>(fr.width())  / m_overlay.contentCanvasW;
            pxPerUnitY = static_cast<float>(fr.height()) / m_overlay.contentCanvasH;
        } else {
            constexpr float REF_W = 1920.0f;
            constexpr float REF_H = 1080.0f;
            pxPerUnitX = static_cast<float>(fr.width())  / REF_W;
            pxPerUnitY = static_cast<float>(fr.height()) / REF_H;
        }
        if (pxPerUnitX < 1e-4f || pxPerUnitY < 1e-4f) return;

        const float effClipScaleX = std::max(0.001f, m_overlay.clipScaleX);
        const float effClipScaleY = std::max(0.001f, m_overlay.clipScaleY);

        const float dx = static_cast<float>(wPos.x() - m_dragStartWidget.x())
                         / (pxPerUnitX * effClipScaleX);
        const float dy = static_cast<float>(wPos.y() - m_dragStartWidget.y())
                         / (pxPerUnitY * effClipScaleY);

        m_overlay.anchorX = m_dragStartAnchorX + dx;
        m_overlay.anchorY = m_dragStartAnchorY + dy;

        emit transformAnchorChanged(m_overlay.anchorX, m_overlay.anchorY);
        update();
        event->accept();
        return;
    }

    // ── Scale corner drag ───────────────────────────────────────────────
    if (m_dragMode == DragMode::ScaleCorner && (event->buttons() & Qt::LeftButton)) {
        QPointF corners[4];
        computeOverlayCorners(corners);
        QPointF center = (corners[0] + corners[2]) * 0.5;

        float startDist = std::hypot(
            static_cast<float>(m_dragStartWidget.x() - center.x()),
            static_cast<float>(m_dragStartWidget.y() - center.y()));
        float curDist = std::hypot(
            static_cast<float>(wPos.x() - center.x()),
            static_cast<float>(wPos.y() - center.y()));

        if (startDist > 1.0f) {
            if (event->modifiers() & Qt::ShiftModifier) {
                // Non-uniform (free) scale: separate X and Y ratios
                float startDx = std::abs(static_cast<float>(m_dragStartWidget.x() - center.x()));
                float startDy = std::abs(static_cast<float>(m_dragStartWidget.y() - center.y()));
                float curDx   = std::abs(static_cast<float>(wPos.x() - center.x()));
                float curDy   = std::abs(static_cast<float>(wPos.y() - center.y()));
                float ratioX  = (startDx > 1.0f) ? curDx / startDx : 1.0f;
                float ratioY  = (startDy > 1.0f) ? curDy / startDy : 1.0f;
                m_overlay.scaleX = std::max(0.01f, m_dragStartScX * ratioX);
                m_overlay.scaleY = std::max(0.01f, m_dragStartScY * ratioY);
            } else {
                // Uniform scale (default): both axes get the same value
                float ratio = curDist / startDist;
                float newScale = std::max(0.01f, m_dragStartScX * ratio);
                m_overlay.scaleX = newScale;
                m_overlay.scaleY = newScale;
            }
            emit transformScaleChanged(m_overlay.scaleX, m_overlay.scaleY);
            update();
        }
        event->accept();
        return;
    }

    // ── Rotate corner drag ──────────────────────────────────────────────
    if (m_dragMode == DragMode::RotateCorner && (event->buttons() & Qt::LeftButton)) {
        QPointF corners[4];
        computeOverlayCorners(corners);
        QPointF center = (corners[0] + corners[2]) * 0.5;

        float curAngle = static_cast<float>(
            std::atan2(wPos.y() - center.y(), wPos.x() - center.x())
            * 180.0 / 3.14159265358979);
        float deltaAngle = curAngle - m_dragStartAngle;

        // Normalize to -180..180
        while (deltaAngle >  180.0f) deltaAngle -= 360.0f;
        while (deltaAngle < -180.0f) deltaAngle += 360.0f;

        float newRot = m_dragStartRot + deltaAngle;
        m_overlay.rotation = newRot;
        emit transformRotationChanged(m_overlay.rotation);
        update();
        event->accept();
        return;
    }

    // ── Cursor hint when hovering ───────────────────────────────────────
    if (m_editTool == 8 && m_dragMode == DragMode::None) {
        applyCursor(zoomCursor());
        event->accept();
        return;
    }

    // Text/Type tool: always show the I-beam over the monitor (Premiere
    // Pro behavior) — even when a clip is selected and its transform
    // handles are visible.
    if (m_editTool == 6 && m_dragMode == DragMode::None) {
        applyCursor(Qt::IBeamCursor);
        event->accept();
        return;
    }

    if (m_dragMode == DragMode::None && m_masks && !m_masks->empty()) {
        // Ctrl+hover near mask edge → show pen cursor
        if ((event->modifiers() & Qt::ControlModifier) && hitTestMaskEdge(wPos)) {
            if (m_hoverMaskIndex != -1 || m_hoverMaskHandle != -1) {
                m_hoverMaskIndex = -1;
                m_hoverMaskHandle = -1;
                update();
            }
            applyCursor(penCursor());
            event->accept();
            return;
        }
        int maskIdx = -1;
        int hHandle = hitTestMaskHandle(wPos, maskIdx);
        if (hHandle >= 0) {
            if (m_hoverMaskIndex != maskIdx || m_hoverMaskHandle != hHandle) {
                m_hoverMaskIndex = maskIdx;
                m_hoverMaskHandle = hHandle;
                update(); // repaint to show glow
            }
            applyCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
        if (hitTestMaskBody(wPos) >= 0) {
            if (m_hoverMaskIndex != -1 || m_hoverMaskHandle != -1) {
                m_hoverMaskIndex = -1;
                m_hoverMaskHandle = -1;
                update();
            }
            applyCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
        // Not hovering any mask element
        if (m_hoverMaskIndex != -1 || m_hoverMaskHandle != -1) {
            m_hoverMaskIndex = -1;
            m_hoverMaskHandle = -1;
            update();
        }
    }
    if (m_overlay.visible && m_dragMode == DragMode::None) {
        // Sibling-body hover → same move cursor as the focused body so
        // the user knows they can grab any selected box to drag the group.
        bool overSiblingBody = false;
        for (const auto& sov : m_secondaryOverlays) {
            if (!sov.visible) continue;
            QPointF sc[4];
            computeOverlayCornersFor(sov, sc);
            if (rt::hitTestBody(wPos, sc)) {
                overSiblingBody = true;
                break;
            }
        }

        if (hitTestHandle(wPos) >= 0)
            applyCursor(Qt::SizeFDiagCursor);
        else if (hitTestRotate(wPos) >= 0)
            applyCursor(rotateCursor());
        else if (hitTestBody(wPos) || overSiblingBody)
            applyCursor(Qt::SizeAllCursor);  // Premiere-style move cursor
        else
            applyCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    event->ignore();
}

void TransformOverlayWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragMode == DragMode::Pan) {
        m_dragMode = DragMode::None;
        applyCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    if (m_dragMode == DragMode::DragMotionHandle) {
        // Record an undoable command capturing the spatial-handle change.
        if (m_motionCmdStack && m_motionX && m_motionY &&
            m_dragMotionKfIdx >= 0)
        {
            auto* tx = m_motionX;
            auto* ty = m_motionY;
            const int64_t kfTime = m_dragKfTime;
            const float oldInX  = m_dragOrigInX,  oldInY  = m_dragOrigInY;
            const float oldOutX = m_dragOrigOutX, oldOutY = m_dragOrigOutY;
            const float newInX  = (m_dragMotionKfIdx < static_cast<int>(tx->keyframeCount())
                                   ? tx->keyframe(static_cast<size_t>(m_dragMotionKfIdx)).spatialInX  : 0.0f);
            const float newInY  = (m_dragMotionKfIdx < static_cast<int>(ty->keyframeCount())
                                   ? ty->keyframe(static_cast<size_t>(m_dragMotionKfIdx)).spatialInY  : 0.0f);
            const float newOutX = (m_dragMotionKfIdx < static_cast<int>(tx->keyframeCount())
                                   ? tx->keyframe(static_cast<size_t>(m_dragMotionKfIdx)).spatialOutX : 0.0f);
            const float newOutY = (m_dragMotionKfIdx < static_cast<int>(ty->keyframeCount())
                                   ? ty->keyframe(static_cast<size_t>(m_dragMotionKfIdx)).spatialOutY : 0.0f);

            auto applyHandles = [tx, ty, kfTime](float ix, float iy, float ox, float oy) {
                for (size_t i = 0; i < tx->keyframeCount(); ++i)
                    if (tx->keyframe(i).time == kfTime) {
                        tx->keyframe(i).spatialInX  = ix;
                        tx->keyframe(i).spatialOutX = ox;
                        break;
                    }
                for (size_t i = 0; i < ty->keyframeCount(); ++i)
                    if (ty->keyframe(i).time == kfTime) {
                        ty->keyframe(i).spatialInY  = iy;
                        ty->keyframe(i).spatialOutY = oy;
                        break;
                    }
            };
            m_motionCmdStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                "Move Motion-Path Handle",
                [applyHandles, newInX, newInY, newOutX, newOutY]() {
                    applyHandles(newInX, newInY, newOutX, newOutY);
                },
                [applyHandles, oldInX, oldInY, oldOutX, oldOutY]() {
                    applyHandles(oldInX, oldInY, oldOutX, oldOutY);
                }));
        }
        m_dragMode        = DragMode::None;
        m_dragMotionKfIdx = -1;
        applyCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    if (m_dragMode == DragMode::DragMaskPoint) {
        if (m_masks && m_dragMaskIndex >= 0 &&
            static_cast<size_t>(m_dragMaskIndex) < m_masks->size())
        {
            emit maskDragFinished(m_dragMaskIndex, m_dragStartMask,
                                  (*m_masks)[static_cast<size_t>(m_dragMaskIndex)]);
        }
        m_dragMode = DragMode::None;
        m_dragMaskIndex = -1;
        m_dragMaskHandle = -1;
        applyCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    // Anchor drag doesn't update pos/scale/rotation, so don't fire the
    // generic transformDragFinished (it would push a no-op undo command
    // for pos/scale/rot). Emit the anchor-specific finished signal with
    // pre/post values so the workspace can record one undo command for
    // the whole drag.
    if (m_dragMode == DragMode::MoveAnchor) {
        const float oldX = m_dragStartAnchorX;
        const float oldY = m_dragStartAnchorY;
        const float newX = m_overlay.anchorX;
        const float newY = m_overlay.anchorY;
        m_dragMode = DragMode::None;
        applyCursor(Qt::ArrowCursor);
        emit transformAnchorDragFinished(oldX, oldY, newX, newY);
        event->accept();
        return;
    }

    if (m_dragMode != DragMode::None) {
        float oldPX = m_dragStartPosX, oldPY = m_dragStartPosY;
        float oldSX = m_dragStartScX,  oldSY = m_dragStartScY;
        float oldRot = m_dragStartRot;
        float newPX = m_overlay.posX,  newPY = m_overlay.posY;
        float newSX = m_overlay.scaleX, newSY = m_overlay.scaleY;
        float newRot = m_overlay.rotation;
        m_dragMode = DragMode::None;
        applyCursor(Qt::ArrowCursor);
        emit transformDragFinished(oldPX, oldPY, oldSX, oldSY, oldRot,
                                   newPX, newPY, newSX, newSY, newRot);
        event->accept();
        return;
    }

    event->ignore();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Event filter — intercept mouse events from the native QWindow
// ═════════════════════════════════════════════════════════════════════════════

void TransformOverlayWidget::wheelEvent(QWheelEvent* event)
{
    // Forward wheel events to VulkanViewport for zoom/scroll.
    if (m_vulkanVp) {
        QCoreApplication::sendEvent(m_vulkanVp, event);
        update(); // redraw overlay at new zoom
    }
}

bool TransformOverlayWidget::eventFilter(QObject* watched, QEvent* event)
{
    // ── Inline text editor key handling ──────────────────────────────
    // Enter commits (Premiere Pro), Shift+Enter inserts newline, Esc cancels.
    if (m_inlineTextEdit && watched == m_inlineTextEdit) {
        if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                if (ke->modifiers() & Qt::ShiftModifier) {
                    // Shift+Enter → insert a newline (Premiere Pro behavior).
                    // Let QPlainTextEdit handle it natively — it already
                    // inserts a line break with Shift+Enter by default.
                    return QWidget::eventFilter(watched, event);
                }
                // Plain Enter → commit the text and close the editor.
                if (!m_committingInlineText) {
                    m_committingInlineText = true;
                    const QString t = m_inlineTextEdit->toPlainText();
                    m_inlineTextEdit->hide();
                    // Restore the transform overlay after editing.
                    m_overlay = m_savedOverlayBeforeEdit;
                    m_preEditOriginalText.clear();
                    update();
                    emit inlineTextCommitted(t);
                    m_committingInlineText = false;
                    setFocus();
                }
                return true;
            }
            if (ke->key() == Qt::Key_Escape) {
                // Esc → cancel, restore original text.
                if (!m_committingInlineText && !m_preEditOriginalText.empty()) {
                    m_committingInlineText = true;
                    QString orig = QString::fromStdString(m_preEditOriginalText);
                    m_inlineTextEdit->hide();
                    m_overlay = m_savedOverlayBeforeEdit;
                    m_preEditOriginalText.clear();
                    update();
                    // Emit the original text — the wiring code detects
                    // newVal==oldVal and restores without an undo entry.
                    emit inlineTextCommitted(orig);
                    m_committingInlineText = false;
                    setFocus();
                }
                return true;
            }
        }
        // Also commit on focus out (editingFinished equivalent).
        if (event->type() == QEvent::FocusOut && !m_committingInlineText
            && m_inlineTextEdit->isVisible()) {
            m_committingInlineText = true;
            const QString t = m_inlineTextEdit->toPlainText();
            m_inlineTextEdit->hide();
            m_overlay = m_savedOverlayBeforeEdit;
            m_preEditOriginalText.clear();
            update();
            emit inlineTextCommitted(t);
            m_committingInlineText = false;
            setFocus();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    // Only intercept events from the VulkanViewport's native QWindow.
    if (!m_vulkanVp || watched != m_vulkanVp->nativeWindow())
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::MouseButtonPress:
        mousePressEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();

    case QEvent::MouseButtonDblClick:
        // Without this, Qt's auto-generated double-click never reaches our
        // mouseDoubleClickEvent — the user double-clicks a text layer, the
        // event fires on the native Vulkan window, and the overlay filter
        // drops it. Forwarding here is what enables the in-place text
        // editor flow.
        mouseDoubleClickEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();

    case QEvent::MouseMove:
        mouseMoveEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();

    case QEvent::MouseButtonRelease:
        mouseReleaseEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();

    case QEvent::Leave:
        // Pointer left the Vulkan surface — drop any override cursor so it
        // doesn't persist application-wide outside the viewport.
        if (m_dragMode == DragMode::None)
            clearCursorOverride();
        break;

    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}


} // namespace rt
