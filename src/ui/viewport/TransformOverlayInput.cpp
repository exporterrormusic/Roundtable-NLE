/*
 * TransformOverlayInput.cpp - Mouse/keyboard input handling for TransformOverlayWidget.
 */

#include "viewport/TransformOverlayWidget.h"
#include "viewport/TransformOverlayDrag.h"
#include "viewport/TransformOverlayEdit.h"
#include "viewport/VulkanViewport.h"
#include "timeline/OpacityMask.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/Keyframe.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "edit/EditSession.h"
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
#include <memory>
#include <utility>

#include <cmath>

#include <spdlog/spdlog.h>

namespace rt {

// Forgiving grab border (widget px) around a selected layer's body, so thin
// text boxes — which can be only a few px tall on screen at low zoom — are
// still easy to grab-and-move just outside their tight outline.
static constexpr double kBodyGrabMarginPx = 14.0;

using namespace overlay_edit;

void TransformOverlayWidget::syncMaskOwnerToEditedOuterTransform() noexcept
{
    if (!m_hasMaskOwnerOverlay || m_maskOwnerFollowsPrimary ||
        !editsOuterClipTransform(m_overlay))
        return;
    syncMaskOwnerFromOuterClip(m_overlay, m_maskOwnerOverlay);
}

TransformOverlayWidget::DragMode TransformOverlayWidget::dragMode() const noexcept
{
    return m_drag ? m_drag->mode() : DragMode::None;
}

void TransformOverlayWidget::beginMaskDrag(int maskIndex, int handle, QPointF widgetPos)
{
    m_drag = std::make_shared<OverlayMaskDrag>(*this, maskIndex, handle, widgetPos);
}

bool TransformOverlayWidget::cancelActiveDrag()
{
    if (!m_drag) return false;
    const std::shared_ptr<TransformOverlayDrag> drag = std::exchange(m_drag, nullptr);
    drag->cancel();
    if (m_editDragOpen) {
        m_editDragOpen = false;
        emit editDragCancelled();
    }
    return true;
}

void TransformOverlayWidget::mousePressEvent(QMouseEvent* event)
{
    handleMousePress(event);
    switch (dragMode()) {
    case DragMode::MoveBody:
    case DragMode::ScaleCorner:
    case DragMode::RotateCorner:
    case DragMode::DragMaskPoint:
    case DragMode::DragMotionHandle:
    case DragMode::MoveAnchor:
    case DragMode::CropEdge:
        if (!m_editDragOpen) {
            m_editDragOpen = true;
            emit editDragStarted();
        }
        break;
    default:
        break;
    }
}

void TransformOverlayWidget::mouseReleaseEvent(QMouseEvent* event)
{
    handleMouseRelease(event);
    if (m_editDragOpen && !m_drag) {
        m_editDragOpen = false;
        emit editDragEnded();
    }
}

void TransformOverlayWidget::handleMousePress(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        m_lastLeftPressHitSelectedBody = false;

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
                        executeTimelineEdit(m_motionCmdStack, nullptr, 
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
        m_drag = std::make_shared<OverlayPanDrag>(*this, event->position());
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

    // ── Pen Mask tool ──
    // Click creates a corner, click-drag creates symmetric Bezier handles,
    // and clicking the first point closes the path. When hovering an existing
    // mask the same tool becomes Premiere's add/delete/convert-point tool.
    if (event->button() == Qt::LeftButton && m_editTool == 9 && m_masks) {
        const QPointF wPos = event->position();
        if (m_penDrawing && m_penDraft.base.vertices.size() >= 3) {
            const auto& first = m_penDraft.base.vertices.front();
            const QPointF firstWidget = maskLocalToWidget(first.x, first.y);
            if (std::hypot(wPos.x() - firstWidget.x(),
                           wPos.y() - firstWidget.y()) <= 18.0) {
                commitPenMask();
                event->accept();
                return;
            }
        }
        if (!m_penDrawing && editExistingMaskWithPen(wPos, event->modifiers())) {
            event->accept();
            return;
        }
        beginPenPoint(wPos);
        event->accept();
        return;
    }

    // ── Ellipse / Rectangle Mask tools ──
    // Press-drag sweeps out a new shape. Handles of an existing mask stay
    // grabbable so a freshly drawn shape can be resized without switching
    // back to the Selection tool.
    if (event->button() == Qt::LeftButton && isShapeMaskTool() && m_masks) {
        const QPointF wPos = event->position();
        int maskIdx = -1;
        const int handleIdx = hitTestMaskHandle(wPos, maskIdx);
        if (handleIdx >= 0 && maskIdx >= 0) {
            beginMaskDrag(maskIdx, handleIdx, wPos);
            event->accept();
            return;
        }
        beginShapeMask(wPos);
        event->accept();
        return;
    }

    // ── Ctrl+Click: add point on mask border ──────────────────────────
    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)
        && m_masks && !m_masks->empty()) {
        int maskIdx = -1;
        if (addPointOnMaskEdge(event->position(), maskIdx)) {
            event->accept();
            return;
        }
    }

    // ── Motion-path spatial handle drag ─────────────────────────────────
    if (event->button() == Qt::LeftButton && m_motionX && m_motionY) {
        int kfIdx = -1;
        bool isIn = false;
        if (hitTestMotionHandle(event->position(), kfIdx, isIn)) {
            m_drag = std::make_shared<OverlayMotionHandleDrag>(*this, kfIdx, isIn);
            applyCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
    }

    // ── Mask control point interaction ──────────────────────────────────
    if (event->button() == Qt::LeftButton && (m_editTool == 0 || m_editTool == 9)
        && (m_activeMaskIndex >= 0 || m_editTool == 9)
        && m_masks && !m_masks->empty()) {
        QPointF wPos = event->position();
        int maskIdx = -1;
        int handleIdx = hitTestMaskHandle(wPos, maskIdx);
        if (handleIdx >= 0 && maskIdx >= 0) {
            beginMaskDrag(maskIdx, handleIdx, wPos);
            applyCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
        // Click inside mask body → move the mask
        maskIdx = (m_activeMaskIndex >= 0 || m_editTool == 9)
            ? hitTestMaskBody(wPos) : -1;
        if (maskIdx >= 0) {
            beginMaskDrag(maskIdx, INT_MAX, wPos);   // INT_MAX: the whole mask
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
                        m_drag = std::make_shared<OverlayAnchorDrag>(*this, wPos);
                        applyCursor(Qt::SizeAllCursor);
                        event->accept();
                        return;
                    }
                }
            }
        }

        // Crop is deliberately Ctrl-only, matching the SHOT workflow. Without
        // Ctrl, crop handles must not steal a press intended for the normal
        // transform resize/body interaction.
        if (m_overlay.cropEnabled && cropGestureRequested(event->modifiers())) {
            int cropH = hitTestCropHandle(wPos);
            if (cropH >= 0) {
                m_drag = std::make_shared<OverlayCropDrag>(*this, cropH, wPos);
                applyCursor(cropH < 2 ? Qt::SizeHorCursor : Qt::SizeVerCursor);
                event->accept();
                return;
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
        bool selectedBodyHit = bodyHit;
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
        // Forgiving grab border: a thin text box can be only a few px tall on
        // screen, so clicks just outside it should still move the layer. The
        // margin yields to the corner scale/rotate handles so those stay
        // reachable — only the non-handle border becomes draggable.
        if (!bodyHit &&
            hitTestBodyMargin(wPos, kBodyGrabMarginPx) &&
            hitTestHandle(wPos) < 0 && hitTestRotate(wPos) < 0) {
            bodyHit = true;
            selectedBodyHit = true;
        }
        if (bodyHit) {
            m_lastLeftPressHitSelectedBody = selectedBodyHit;
            m_drag = std::make_shared<OverlayTransformDrag>(*this, DragMode::MoveBody, wPos);
            applyCursor(Qt::ArrowCursor);   // no special move cursor
            event->accept();
            return;
        }

        int handle = hitTestHandle(wPos);
        if (handle >= 0) {
            m_drag = std::make_shared<OverlayTransformDrag>(*this, DragMode::ScaleCorner, wPos);
            applyCursor(Qt::SizeFDiagCursor);
            event->accept();
            return;
        }

        int rotHandle = hitTestRotate(wPos);
        if (rotHandle >= 0) {
            m_drag = std::make_shared<OverlayTransformDrag>(*this, DragMode::RotateCorner, wPos);
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
            QRectF frameRect = computeFrameRect();
            float srcW = static_cast<float>(m_vulkanVp->srcWidth());
            float srcH = static_cast<float>(m_vulkanVp->srcHeight());
            if (!frameRect.isEmpty() && srcW > 0.0f && srcH > 0.0f)
            {
                QPointF wPos = event->position();
                float frameX = static_cast<float>(
                    (wPos.x() - frameRect.x()) / frameRect.width()) * srcW;
                float frameY = static_cast<float>(
                    (wPos.y() - frameRect.y()) / frameRect.height()) * srcH;
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
    if (event->button() == Qt::LeftButton && m_editTool == 9
        && m_penDrawing && m_penDraft.base.vertices.size() >= 3) {
        commitPenMask();
        event->accept();
        return;
    }

    // Double-click -> edit the text layer under the cursor (Premiere Pro).
    // Emit in sequence-canvas coordinates. The displayed Vulkan image may
    // be a reduced preview, so its source dimensions are not a stable space
    // for selecting layers in the full-resolution graphic canvas.
    if (event->button() == Qt::LeftButton) {
        if (m_vulkanVp) {
            QRectF frameRect = computeFrameRect();
            const float canvasW = m_seqW > 0
                ? static_cast<float>(m_seqW)
                : static_cast<float>(m_vulkanVp->srcWidth());
            const float canvasH = m_seqH > 0
                ? static_cast<float>(m_seqH)
                : static_cast<float>(m_vulkanVp->srcHeight());
            const QPointF wPos = event->position();
            spdlog::debug("[INLINE-TEXT] overlay double-click pos=({}, {}) frame=({}, {}, {}, {}) canvas={}x{} inside={}",
                         wPos.x(), wPos.y(), frameRect.x(), frameRect.y(),
                         frameRect.width(), frameRect.height(), canvasW, canvasH,
                         frameRect.contains(wPos));
            if (!frameRect.isEmpty() && canvasW > 0.0f && canvasH > 0.0f
                && frameRect.contains(wPos)) {
                float frameX = static_cast<float>(
                    (wPos.x() - frameRect.x()) / frameRect.width()) * canvasW;
                float frameY = static_cast<float>(
                    (wPos.y() - frameRect.y()) / frameRect.height()) * canvasH;
                m_pendingInlineCaretGlobal = event->globalPosition().toPoint();
                m_hasPendingInlineCaret = true;
                m_doubleClickStartedOnSelectedBody =
                    m_lastLeftPressHitSelectedBody;
                spdlog::debug("[INLINE-TEXT] requesting layer edit canvas=({}, {}) selectedBody={}",
                             frameX, frameY, m_doubleClickStartedOnSelectedBody);
                emit textEditRequested(frameX, frameY);
                // Direct UI connections consume this synchronously in
                // beginInlineTextEdit(). If nothing was hit, do not let a
                // stale click reposition a later programmatic edit session.
                m_hasPendingInlineCaret = false;
                m_doubleClickStartedOnSelectedBody = false;
                event->accept();
                return;
            }
        }
    }
    event->ignore();
}

void TransformOverlayWidget::mouseMoveEvent(QMouseEvent* event)
{
    QPointF wPos = event->position();

    // ── An active drag (TransformOverlayDrag.h) ─────────────────────────
    // Held locally: a signal the move emits may end the drag.
    if (const auto drag = m_drag; drag && drag->move(event))
        return;

    // ── Cursor hint when hovering ───────────────────────────────────────
    if (m_editTool == 7 && !m_drag) {
        applyCursor(zoomCursor());
        event->accept();
        return;
    }

    if (m_editTool == 8 && !m_drag) {
        applyCursor(Qt::CrossCursor);
        event->accept();
        return;
    }

    if (isShapeMaskTool() && !m_drag) {
        int maskIndex = -1;
        const int handle = hitTestMaskHandle(wPos, maskIndex);
        const int hoverIndex = handle >= 0 ? maskIndex : -1;
        if (hoverIndex != m_hoverMaskIndex || handle != m_hoverMaskHandle) {
            m_hoverMaskIndex = hoverIndex;
            m_hoverMaskHandle = handle;
            update();
        }
        applyCursor(handle >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
        event->accept();
        return;
    }

    // Text/Type tool: always show the I-beam over the monitor (Premiere
    // Pro behavior) — even when a clip is selected and its transform
    // handles are visible.
    if (m_editTool == 9 && !m_drag) {
        m_penHoverWidget = wPos;
        int maskIndex = -1;
        const int handle = hitTestMaskHandle(wPos, maskIndex);
        if (handle >= 0) {
            m_hoverMaskIndex = maskIndex;
            m_hoverMaskHandle = handle;
        } else {
            m_hoverMaskIndex = -1;
            m_hoverMaskHandle = -1;
        }
        applyCursor(penCursor());
        update();
        event->accept();
        return;
    }

    if (m_editTool == 6 && !m_drag) {
        applyCursor(Qt::IBeamCursor);
        event->accept();
        return;
    }

    if (!m_drag && m_masks && !m_masks->empty()
        && (m_activeMaskIndex >= 0 || m_editTool == 9)) {
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
    if (m_overlay.visible && !m_drag) {
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

        // Only advertise crop hit targets while Ctrl is held. Plain edge and
        // corner hover continues through the normal transform cursor path.
        int cropHoverH = (m_overlay.cropEnabled
                          && cropGestureRequested(event->modifiers()))
            ? hitTestCropHandle(wPos) : -1;
        if (cropHoverH >= 0)
            applyCursor(cropHoverH < 2 ? Qt::SizeHorCursor : Qt::SizeVerCursor);
        else if (hitTestHandle(wPos) >= 0)
            applyCursor(Qt::SizeFDiagCursor);
        else if (hitTestRotate(wPos) >= 0)
            applyCursor(rotateCursor());
        else if (hitTestBody(wPos) || overSiblingBody ||
                 hitTestBodyMargin(wPos, kBodyGrabMarginPx))
            applyCursor(Qt::SizeAllCursor);  // Premiere-style move cursor
        else
            applyCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    event->ignore();
}

void TransformOverlayWidget::handleMouseRelease(QMouseEvent* event)
{
    if (!m_drag) {
        event->ignore();
        return;
    }
    // Ended before its release runs: the release's signals may start or
    // clear things, and must see no drag in progress.
    const std::shared_ptr<TransformOverlayDrag> drag = std::exchange(m_drag, nullptr);
    drag->release(event);
    event->accept();
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

// Premiere's titler shortcuts while editing monitor text, from Premiere's
// "Adobe Premiere Pro Defaults V6.kys" (cmd.titler.misc.size/lead,
// cmd.graphicsinspector.align*):
//   Ctrl+Alt+Right / Left   font size +1 / -1   (Shift: 5)
//   Alt+Up / Down           leading   +1 / -1   (Shift: 5)
//   Ctrl+Shift+L / C / R    align left / center / right
// plus Alt+Right / Left for kerning +5 / -5 (the Adobe text-tool habit;
// Premiere ships its kerning commands unbound).
bool TransformOverlayWidget::isInlineTitlerShortcut(const QKeyEvent* ke)
{
    const auto modifiers = ke->modifiers();
    const bool ctrl = modifiers & (Qt::ControlModifier | Qt::MetaModifier);
    const bool alt = modifiers & Qt::AltModifier;
    const bool shift = modifiers & Qt::ShiftModifier;
    const int key = ke->key();
    const bool horizontal = key == Qt::Key_Left || key == Qt::Key_Right;
    const bool vertical = key == Qt::Key_Up || key == Qt::Key_Down;
    if (alt && horizontal) return true;
    if (alt && !ctrl && vertical) return true;
    return ctrl && shift && !alt
        && (key == Qt::Key_L || key == Qt::Key_C || key == Qt::Key_R);
}

bool TransformOverlayWidget::handleInlineTitlerShortcut(const QKeyEvent* ke)
{
    if (!isInlineTitlerShortcut(ke)) return false;
    const auto modifiers = ke->modifiers();
    const bool ctrl = modifiers & (Qt::ControlModifier | Qt::MetaModifier);
    const bool shift = modifiers & Qt::ShiftModifier;
    const int key = ke->key();
    const float step = shift ? 5.0f : 1.0f;
    switch (key) {
    case Qt::Key_Left:
    case Qt::Key_Right: {
        const float sign = key == Qt::Key_Right ? 1.0f : -1.0f;
        if (ctrl) nudgeInlineTextFontSize(sign * step);
        else      nudgeInlineTextKerning(sign * 5.0f);
        break;
    }
    case Qt::Key_Up:
    case Qt::Key_Down:
        nudgeInlineTextLeading((key == Qt::Key_Up ? 1.0f : -1.0f) * step);
        break;
    case Qt::Key_L:
        applyInlineParagraphAlignment(static_cast<int>(GTextAlign::Left));
        break;
    case Qt::Key_C:
        applyInlineParagraphAlignment(static_cast<int>(GTextAlign::Center));
        break;
    case Qt::Key_R:
        applyInlineParagraphAlignment(static_cast<int>(GTextAlign::Right));
        break;
    default:
        return false;
    }
    return true;
}

bool TransformOverlayWidget::eventFilter(QObject* watched, QEvent* event)
{
    // Esc cancels a drag in progress, as in the timeline. Watched app-wide
    // because keyboard focus is rarely on the overlay; inert without a drag.
    if ((event->type() == QEvent::KeyPress
         || event->type() == QEvent::ShortcutOverride)
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && m_drag) {
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept();       // deliver it as a KeyPress instead
            return true;
        }
        cancelActiveDrag();
        return true;
    }

    // A release can be lost when Windows deactivates or tears down the native
    // viewport's mouse grab. Finish through the normal release path so undo
    // bookkeeping is retained, then drop application-wide cursor state.
    if (event->type() == QEvent::ApplicationDeactivate
        || event->type() == QEvent::WindowDeactivate
        || event->type() == QEvent::UngrabMouse) {
        if (m_drag) {
            const QPoint global = QCursor::pos();
            QMouseEvent release(QEvent::MouseButtonRelease,
                                QPointF(mapFromGlobal(global)), QPointF(global),
                                Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            mouseReleaseEvent(&release);
        }
        m_drag.reset();
        m_inlinePointerRerouted = false;
        clearCursorOverride();
    }

    // The editor is a translucent top-level window above a native Vulkan
    // QWindow. Windows occasionally gives transparent pixels between glyphs
    // to that underlying HWND even though the point is inside the editor's
    // rectangle. Reclaim those events before the native viewport interprets
    // the press as an outside click and commits the edit.
    if (m_inlineTextEdit && m_inlineTextEdit->isVisible()) {
        const QEvent::Type type = event->type();
        const bool pointerEvent = type == QEvent::MouseButtonPress
            || type == QEvent::MouseButtonDblClick
            || type == QEvent::MouseMove
            || type == QEvent::MouseButtonRelease;
        if (pointerEvent) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint global = mouse->globalPosition().toPoint();
            const QRect editorRect(m_inlineTextEdit->mapToGlobal(QPoint(0, 0)),
                                   m_inlineTextEdit->size());
            const bool insideEditor = editorRect.adjusted(-2, -2, 2, 2)
                .contains(global);
            const bool leftPress = mouse->button() == Qt::LeftButton
                && type == QEvent::MouseButtonPress;
            const bool leftDoubleClick = mouse->button() == Qt::LeftButton
                && type == QEvent::MouseButtonDblClick;
            if ((leftPress || leftDoubleClick) && insideEditor) {
                QTextCursor cursor(m_inlineTextEdit->document());
                cursor.setPosition(inlineTextPositionAtGlobal(global));
                if (leftDoubleClick) {
                    cursor.select(QTextCursor::WordUnderCursor);
                    m_inlinePointerRerouted = false;
                } else if (mouse->modifiers() & Qt::ShiftModifier) {
                    const int position = cursor.position();
                    cursor = m_inlineTextEdit->textCursor();
                    m_inlinePointerAnchor = cursor.anchor();
                    cursor.setPosition(position, QTextCursor::KeepAnchor);
                    m_inlinePointerRerouted = true;
                } else {
                    m_inlinePointerAnchor = cursor.position();
                    m_inlinePointerRerouted = true;
                }
                m_inlineTextEdit->setTextCursor(cursor);
                m_inlineTextEdit->raise();
                m_inlineTextEdit->activateWindow();
                m_inlineTextEdit->setFocus(Qt::MouseFocusReason);
                notifyInlineTextSelectionFormat();
                spdlog::debug("[INLINE-TEXT] reclaimed pointer event type={} global=({}, {}) cursor={}",
                             static_cast<int>(type), global.x(), global.y(),
                             cursor.position());
                event->accept();
                return true;
            }
            if (m_inlinePointerRerouted && type == QEvent::MouseMove
                && (mouse->buttons() & Qt::LeftButton)) {
                const int position = inlineTextPositionAtGlobal(global);
                QTextCursor cursor(m_inlineTextEdit->document());
                cursor.setPosition(std::clamp(
                    m_inlinePointerAnchor, 0,
                    static_cast<int>(m_inlineTextEdit->toPlainText().size())));
                cursor.setPosition(position, QTextCursor::KeepAnchor);
                m_inlineTextEdit->setTextCursor(cursor);
                event->accept();
                return true;
            }
            if (m_inlinePointerRerouted
                && type == QEvent::MouseButtonRelease
                && mouse->button() == Qt::LeftButton) {
                m_inlinePointerRerouted = false;
                notifyInlineTextSelectionFormat();
                event->accept();
                return true;
            }
        }
    }

    // ── Inline text editor key handling ──────────────────────────────
    // Return inserts a newline; Ctrl/Cmd+Return commits; Esc cancels.
    if (m_inlineTextEdit && watched == m_inlineTextEdit) {
        if (event->type() == QEvent::FocusIn) {
            m_inlineEditorHasFocused = true;
            spdlog::debug("[INLINE-TEXT] FocusIn session={}", m_inlineEditSession);
        }
        if (event->type() == QEvent::ShortcutOverride) {
            auto* ke = static_cast<QKeyEvent*>(event);
            const auto modifiers = ke->modifiers();
            const bool plainTyping =
                !(modifiers & (Qt::ControlModifier | Qt::MetaModifier
                               | Qt::AltModifier));
            const bool textEditingShortcut =
                ke->matches(QKeySequence::Cut)
                || ke->matches(QKeySequence::Copy)
                || ke->matches(QKeySequence::Paste)
                || ke->matches(QKeySequence::Undo)
                || ke->matches(QKeySequence::Redo)
                || ke->matches(QKeySequence::SelectAll)
                || ((modifiers & (Qt::ControlModifier | Qt::MetaModifier))
                    && (ke->key() == Qt::Key_B || ke->key() == Qt::Key_I
                        || ke->key() == Qt::Key_U
                        || ke->key() == Qt::Key_Return
                        || ke->key() == Qt::Key_Enter))
                || isInlineTitlerShortcut(ke);
            if (plainTyping || textEditingShortcut) {
                // Shortcut dispatch happens before KeyPress. Claim ordinary
                // typing here so application tool bindings (T, A, B, R, ...)
                // cannot steal letters from the monitor text editor.
                event->accept();
                return true;
            }
        }
        if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if ((ke->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))
                && ke->key() == Qt::Key_B) {
                const int current =
                    m_inlineTextEdit->currentCharFormat().fontWeight();
                applyInlineTextFontWeight(current >= 700 ? 400 : 700);
                return true;
            }
            if ((ke->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))
                && ke->key() == Qt::Key_I) {
                const bool current =
                    m_inlineTextEdit->currentCharFormat().fontItalic();
                applyInlineTextItalic(!current);
                return true;
            }
            if ((ke->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))
                && !(ke->modifiers() & Qt::AltModifier)
                && ke->key() == Qt::Key_U) {
                applyInlineTextUnderline(
                    !m_inlineTextEdit->currentCharFormat().fontUnderline());
                return true;
            }
            if (handleInlineTitlerShortcut(ke)) return true;
            // Plain printable typing: insert it ourselves so each word is
            // its own undo step (insertInlineTypedText).
            if (!(ke->modifiers() & (Qt::ControlModifier | Qt::MetaModifier
                                     | Qt::AltModifier))
                && !ke->text().isEmpty() && ke->text().front().isPrint()
                && !m_inlineTextEdit->isReadOnly()) {
                insertInlineTypedText(ke->text());
                return true;
            }
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                if (ke->modifiers()
                    & (Qt::ControlModifier | Qt::MetaModifier)) {
                    finishInlineTextEdit(false);
                    return true;
                }
                // Handle this explicitly instead of relying on propagation
                // through the frameless top-level editor. On Windows that
                // propagation could coincide with a focus transition and end
                // the edit, making the line that was just typed appear to
                // vanish. Return/Enter always creates another text row;
                // Ctrl/Cmd+Return remains the explicit commit shortcut.
                m_inlineTextEdit->insertPlainText(QStringLiteral("\n"));
                return true;
            }
            if (ke->key() == Qt::Key_Escape) {
                finishInlineTextEdit(true);
                return true;
            }
        }
        // Defer focus-out handling until Qt has assigned the destination.
        // Font/style controls deliberately keep the session alive so their
        // click can operate on the retained monitor selection.
        if (event->type() == QEvent::FocusOut && !m_committingInlineText
            && m_inlineTextEdit->isVisible()) {
            const uint64_t editSession = m_inlineEditSession;
            spdlog::debug("[INLINE-TEXT] FocusOut session={} hadFocus={} settling={}",
                         editSession, m_inlineEditorHasFocused,
                         m_inlineEditorFocusSettling);
            QTimer::singleShot(0, this, [this, editSession]() {
                if (!m_inlineTextEdit || !m_inlineTextEdit->isVisible()
                    || m_committingInlineText
                    || m_inlineEditSession != editSession) return;
                if (m_inlineEditorFocusSettling || !m_inlineEditorHasFocused) {
                    // This is the native-window activation race, not a user
                    // click away from an established edit session.
                    m_inlineTextEdit->raise();
                    m_inlineTextEdit->activateWindow();
                    m_inlineTextEdit->setFocus(Qt::MouseFocusReason);
                    spdlog::debug("[INLINE-TEXT] ignored activation FocusOut session={} focus={}",
                                 editSession, m_inlineTextEdit->hasFocus());
                    return;
                }
                if (!focusIsInInlineFormattingUi())
                    finishInlineTextEdit(false);
            });
        }
        return QWidget::eventFilter(watched, event);
    }

    // Only intercept events from the VulkanViewport's native QWindow.
    if (!m_vulkanVp || watched != m_vulkanVp->nativeWindow())
        return QWidget::eventFilter(watched, event);

    auto forwardMouse = [this, event](
        void (TransformOverlayWidget::*handler)(QMouseEvent*)) {
        auto* source = static_cast<QMouseEvent*>(event);
        const QPointF overlayPos = QPointF(
            mapFromGlobal(source->globalPosition().toPoint()));
        QMouseEvent mapped(source->type(), overlayPos,
                           source->globalPosition(), source->button(),
                           source->buttons(), source->modifiers());
        (this->*handler)(&mapped);
        event->setAccepted(mapped.isAccepted());
        return mapped.isAccepted();
    };

    switch (event->type()) {
    case QEvent::KeyPress:
    {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (m_shapeDrawing && keyEvent->key() == Qt::Key_Escape) {
            cancelShapeMask();
            return true;
        }
        if (m_editTool == 9 && m_penDrawing) {
            if (keyEvent->key() == Qt::Key_Escape) {
                cancelPenMask();
                return true;
            }
            if (keyEvent->key() == Qt::Key_Return
                || keyEvent->key() == Qt::Key_Enter) {
                if (commitPenMask()) return true;
            }
        }
        break;
    }

    case QEvent::MouseButtonPress:
        return forwardMouse(&TransformOverlayWidget::mousePressEvent);

    case QEvent::MouseButtonDblClick:
        // VulkanViewport explicitly routes native left double-clicks through
        // nativeLeftDoubleClicked. Returning false here lets that single
        // owner handle the gesture without relying on filter ordering.
        if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
            return false;
        // Preserve overlay-owned double-click behavior for any other button;
        // ignored events continue to VulkanViewport (middle resets the view).
        return forwardMouse(&TransformOverlayWidget::mouseDoubleClickEvent);

    case QEvent::MouseMove:
        return forwardMouse(&TransformOverlayWidget::mouseMoveEvent);

    case QEvent::MouseButtonRelease:
        return forwardMouse(&TransformOverlayWidget::mouseReleaseEvent);

    case QEvent::Leave:
        // Pointer left the Vulkan surface — drop any override cursor so it
        // doesn't persist application-wide outside the viewport.
        if (!m_drag)
            clearCursorOverride();
        break;

    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}


} // namespace rt
