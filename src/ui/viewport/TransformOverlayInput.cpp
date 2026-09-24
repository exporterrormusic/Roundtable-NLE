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

// Forgiving grab border (widget px) around a selected layer's body, so thin
// text boxes — which can be only a few px tall on screen at low zoom — are
// still easy to grab-and-move just outside their tight outline.
static constexpr double kBodyGrabMarginPx = 14.0;
static constexpr int kMaskTangentHandleBase = 10000;

static bool editsOuterClipTransform(const TransformOverlayInfo& info) noexcept
{
    return info.useContentRect && info.editOuterClipTransform;
}

static float editPositionX(const TransformOverlayInfo& info) noexcept
{
    return editsOuterClipTransform(info) ? info.clipPosX : info.posX;
}

static float editPositionY(const TransformOverlayInfo& info) noexcept
{
    return editsOuterClipTransform(info) ? info.clipPosY : info.posY;
}

static float editScaleX(const TransformOverlayInfo& info) noexcept
{
    return editsOuterClipTransform(info) ? info.clipScaleX : info.scaleX;
}

static float editScaleY(const TransformOverlayInfo& info) noexcept
{
    return editsOuterClipTransform(info) ? info.clipScaleY : info.scaleY;
}

static float editRotation(const TransformOverlayInfo& info) noexcept
{
    return editsOuterClipTransform(info) ? info.clipRotation : info.rotation;
}

static void setEditPosition(TransformOverlayInfo& info, float x, float y) noexcept
{
    if (editsOuterClipTransform(info)) {
        info.clipPosX = x;
        info.clipPosY = y;
    } else {
        info.posX = x;
        info.posY = y;
    }
}

static void setEditScale(TransformOverlayInfo& info, float x, float y) noexcept
{
    if (editsOuterClipTransform(info)) {
        info.clipScaleX = x;
        info.clipScaleY = y;
    } else {
        info.scaleX = x;
        info.scaleY = y;
    }
}

static void setEditRotation(TransformOverlayInfo& info, float value) noexcept
{
    if (editsOuterClipTransform(info))
        info.clipRotation = value;
    else
        info.rotation = value;
}

static float scaleWithOriginalSign(float start, float ratio) noexcept
{
    const float magnitude = std::max(0.01f, std::abs(start) * ratio);
    return std::signbit(start) ? -magnitude : magnitude;
}

void TransformOverlayWidget::syncMaskOwnerToEditedOuterTransform() noexcept
{
    if (!m_hasMaskOwnerOverlay || m_maskOwnerFollowsPrimary ||
        !editsOuterClipTransform(m_overlay))
        return;
    syncMaskOwnerFromOuterClip(m_overlay, m_maskOwnerOverlay);
}

void TransformOverlayWidget::mousePressEvent(QMouseEvent* event)
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
    if (event->button() == Qt::LeftButton && (m_editTool == 0 || m_editTool == 9)
        && (m_activeMaskIndex >= 0 || m_editTool == 9)
        && m_masks && !m_masks->empty()) {
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
        maskIdx = (m_activeMaskIndex >= 0 || m_editTool == 9)
            ? hitTestMaskBody(wPos) : -1;
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

        // Crop is deliberately Ctrl-only, matching the SHOT workflow. Without
        // Ctrl, crop handles must not steal a press intended for the normal
        // transform resize/body interaction.
        if (m_overlay.cropEnabled && cropGestureRequested(event->modifiers())) {
            int cropH = hitTestCropHandle(wPos);
            if (cropH >= 0) {
                m_dragMode = DragMode::CropEdge;
                m_cropHandle = cropH;
                m_dragStartWidget  = wPos;
                m_dragStartCrop[0] = m_overlay.cropL;
                m_dragStartCrop[1] = m_overlay.cropR;
                m_dragStartCrop[2] = m_overlay.cropT;
                m_dragStartCrop[3] = m_overlay.cropB;
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
            m_dragMode = DragMode::MoveBody;
            m_dragStartWidget = wPos;
            m_dragStartPosX = editPositionX(m_overlay);
            m_dragStartPosY = editPositionY(m_overlay);
            m_dragStartScX  = editScaleX(m_overlay);
            m_dragStartScY  = editScaleY(m_overlay);
            m_dragStartRot  = editRotation(m_overlay);
            applyCursor(Qt::ArrowCursor);   // no special move cursor
            event->accept();
            return;
        }

        int handle = hitTestHandle(wPos);
        if (handle >= 0) {
            m_dragMode = DragMode::ScaleCorner;
            m_dragHandle = handle;
            m_dragStartWidget = wPos;
            m_dragStartPosX = editPositionX(m_overlay);
            m_dragStartPosY = editPositionY(m_overlay);
            m_dragStartScX  = editScaleX(m_overlay);
            m_dragStartScY  = editScaleY(m_overlay);
            m_dragStartRot  = editRotation(m_overlay);
            applyCursor(Qt::SizeFDiagCursor);
            event->accept();
            return;
        }

        int rotHandle = hitTestRotate(wPos);
        if (rotHandle >= 0) {
            m_dragMode = DragMode::RotateCorner;
            m_dragHandle = rotHandle;
            m_dragStartWidget = wPos;
            m_dragStartPosX = editPositionX(m_overlay);
            m_dragStartPosY = editPositionY(m_overlay);
            m_dragStartScX  = editScaleX(m_overlay);
            m_dragStartScY  = editScaleY(m_overlay);
            m_dragStartRot  = editRotation(m_overlay);
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

    // ── Pan drag ────────────────────────────────────────────────────────
    if (m_dragMode == DragMode::Pan && m_vulkanVp) {
        float dx = static_cast<float>(wPos.x() - m_panStartPos.x());
        float dy = static_cast<float>(wPos.y() - m_panStartPos.y());
        m_vulkanVp->setViewPan(m_panStartVpX + dx, m_panStartVpY + dy);
        update(); // redraw overlay at new pan
        event->accept();
        return;
    }

    // ── Crop edge drag ──────────────────────────────────────────────────
    if (m_dragMode == DragMode::CropEdge && (event->buttons() & Qt::LeftButton)) {
        QPointF corners[4];
        computeOverlayCorners(corners);   // outer box — crop never changes it
        const QPointF U = corners[1] - corners[0];   // left→right axis
        const QPointF V = corners[3] - corners[0];   // top→bottom axis
        const double dxw = wPos.x() - m_dragStartWidget.x();
        const double dyw = wPos.y() - m_dragStartWidget.y();

        // Fraction (0..1 of the box edge) the mouse moved along an axis.
        auto projFrac = [](double ex, double ey, double ax, double ay) -> double {
            const double len2 = ax * ax + ay * ay;
            if (len2 < 1e-9) return 0.0;
            return (ex * ax + ey * ay) / len2;
        };

        float l = m_dragStartCrop[0], r = m_dragStartCrop[1];
        float t = m_dragStartCrop[2], b = m_dragStartCrop[3];
        constexpr float kMaxSum = 95.0f;   // keep L+R<100 and T+B<100 (visible sliver)
        // The opposite edge can already exceed kMaxSum (the Properties/Effect
        // spins allow each edge 0..100 independently), so std::max(0,...) the
        // upper bound — otherwise std::clamp(value, 0, negative) is lo>hi UB and
        // would yield a negative crop.
        if (m_cropHandle == 0) {           // Left edge
            double f = projFrac(dxw, dyw, U.x(), U.y()) * 100.0;
            l = std::clamp(static_cast<float>(m_dragStartCrop[0] + f), 0.0f, std::max(0.0f, kMaxSum - r));
        } else if (m_cropHandle == 1) {    // Right edge (grows as handle moves left)
            double f = projFrac(dxw, dyw, U.x(), U.y()) * 100.0;
            r = std::clamp(static_cast<float>(m_dragStartCrop[1] - f), 0.0f, std::max(0.0f, kMaxSum - l));
        } else if (m_cropHandle == 2) {    // Top edge
            double f = projFrac(dxw, dyw, V.x(), V.y()) * 100.0;
            t = std::clamp(static_cast<float>(m_dragStartCrop[2] + f), 0.0f, std::max(0.0f, kMaxSum - b));
        } else {                           // Bottom edge (grows as handle moves up)
            double f = projFrac(dxw, dyw, V.x(), V.y()) * 100.0;
            b = std::clamp(static_cast<float>(m_dragStartCrop[3] - f), 0.0f, std::max(0.0f, kMaxSum - t));
        }

        m_overlay.cropL = l; m_overlay.cropR = r;
        m_overlay.cropT = t; m_overlay.cropB = b;
        update();
        emit cropChanged(l, r, t, b);
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
    if (m_dragMode == DragMode::DrawMaskPoint && m_penDrawing
        && (event->buttons() & Qt::LeftButton)
        && !m_penDraft.base.vertices.empty()) {
        QPointF local;
        if (widgetToMaskLocal(wPos, local)) {
            if (event->modifiers() & Qt::ShiftModifier) {
                QPointF delta = wPos - m_dragStartWidget;
                const double length = std::hypot(delta.x(), delta.y());
                if (length > 1.0e-6) {
                    constexpr double step =
                        3.14159265358979323846 / 4.0;
                    const double angle = std::round(
                        std::atan2(delta.y(), delta.x()) / step) * step;
                    const QPointF constrained = m_dragStartWidget + QPointF(
                        std::cos(angle) * length,
                        std::sin(angle) * length);
                    (void)widgetToMaskLocal(constrained, local);
                }
            }
            const float dx = static_cast<float>(local.x() - m_penPressLocal.x());
            const float dy = static_cast<float>(local.y() - m_penPressLocal.y());
            auto& vertex = m_penDraft.base.vertices.back();
            vertex.inTanX = -dx;
            vertex.inTanY = -dy;
            vertex.outTanX = dx;
            vertex.outTanY = dy;
            m_penHoverWidget = wPos;
            update();
        }
        event->accept();
        return;
    }

    if (m_dragMode == DragMode::DragMaskPoint && m_masks &&
        (event->buttons() & Qt::LeftButton))
    {
        if (m_dragMaskIndex < 0
            || static_cast<size_t>(m_dragMaskIndex) >= m_masks->size())
            return;

        auto& mask = (*m_masks)[static_cast<size_t>(m_dragMaskIndex)];
        QPointF startLocal, currentLocal;
        if (!widgetToMaskLocal(
                m_dragStartWidget, startLocal, mask.coordinateSpace)
            || !widgetToMaskLocal(wPos, currentLocal, mask.coordinateSpace))
            return;
        float dxNorm = static_cast<float>(currentLocal.x() - startLocal.x());
        float dyNorm = static_cast<float>(currentLocal.y() - startLocal.y());

        // Shift constrains anchor/body movement to the dominant screen axis.
        // Tangent handles use 45-degree angle snapping further below.
        if ((event->modifiers() & Qt::ShiftModifier)
            && m_dragMaskHandle < kMaskTangentHandleBase
            && (mask.shape == MaskShape::FreeDrawBezier
                || m_dragMaskHandle == 4
                || m_dragMaskHandle == INT_MAX)) {
            QPointF delta = wPos - m_dragStartWidget;
            if (std::abs(delta.x()) >= std::abs(delta.y()))
                delta.setY(0.0);
            else
                delta.setX(0.0);
            QPointF constrainedLocal;
            if (widgetToMaskLocal(m_dragStartWidget + delta,
                                  constrainedLocal, mask.coordinateSpace)) {
                dxNorm = static_cast<float>(
                    constrainedLocal.x() - startLocal.x());
                dyNorm = static_cast<float>(
                    constrainedLocal.y() - startLocal.y());
            }
        }

        // Work on the geometry evaluated at the current time; write back
        // through writeGeometry (Premiere stopwatch model: updates the
        // static path, or records a Mask Path keyframe when animated).
        const MaskGeometry startGeo = m_dragStartMask.geometryAt(m_maskTime);
        MaskGeometry geo = mask.geometryAt(m_maskTime);

        // Resize parametric masks in their own rotated source-pixel axes.
        const QSizeF sourceSize =
            mask.coordinateSpace == MaskCoordinateSpace::LegacySequenceFrame
            ? QSizeF(std::max<uint32_t>(
                         1u, m_vulkanVp ? m_vulkanVp->srcWidth() : 1u),
                     std::max<uint32_t>(
                         1u, m_vulkanVp ? m_vulkanVp->srcHeight() : 1u))
            : maskSourceSize();
        const double radians = -static_cast<double>(startGeo.rotation)
            * 3.14159265358979323846 / 180.0;
        const double c = std::cos(radians), s = std::sin(radians);
        const double dxPx = static_cast<double>(dxNorm) * sourceSize.width();
        const double dyPx = static_cast<double>(dyNorm) * sourceSize.height();
        const float shapeDx = static_cast<float>(
            (dxPx * c - dyPx * s) / sourceSize.width());
        const float shapeDy = static_cast<float>(
            (dxPx * s + dyPx * c) / sourceSize.height());

        if (mask.shape == MaskShape::Ellipse) {
            if (m_dragMaskHandle == 4 || m_dragMaskHandle == INT_MAX) {
                // Move center
                geo.centerX = startGeo.centerX + dxNorm;
                geo.centerY = startGeo.centerY + dyNorm;
            } else if (m_dragMaskHandle == 0 || m_dragMaskHandle == 1) {
                // Right/left cardinal → scale width
                float d = (m_dragMaskHandle == 0) ? shapeDx : -shapeDx;
                geo.width = std::max(0.01f, startGeo.width + d * 2.0f);
                if ((event->modifiers() & Qt::ShiftModifier)
                    && startGeo.width > 1.0e-6f)
                    geo.height = std::max(
                        0.01f, startGeo.height * geo.width / startGeo.width);
            } else {
                // Bottom/top cardinal → scale height
                float d = (m_dragMaskHandle == 2) ? shapeDy : -shapeDy;
                geo.height = std::max(0.01f, startGeo.height + d * 2.0f);
                if ((event->modifiers() & Qt::ShiftModifier)
                    && startGeo.height > 1.0e-6f)
                    geo.width = std::max(
                        0.01f, startGeo.width * geo.height / startGeo.height);
            }
        }
        else if (mask.shape == MaskShape::Rectangle) {
            if (m_dragMaskHandle == 4 || m_dragMaskHandle == INT_MAX) {
                geo.centerX = startGeo.centerX + dxNorm;
                geo.centerY = startGeo.centerY + dyNorm;
            } else if (m_dragMaskHandle >= 5 && m_dragMaskHandle <= 8) {
                // Mid-edge handles: resize one dimension only
                // 5=top, 6=right, 7=bottom, 8=left
                if (m_dragMaskHandle == 5) {
                    // Top edge: shrink height from top
                    geo.height  = std::max(0.01f, startGeo.height - shapeDy * 2.0f);
                } else if (m_dragMaskHandle == 7) {
                    // Bottom edge: grow height from bottom
                    geo.height  = std::max(0.01f, startGeo.height + shapeDy * 2.0f);
                } else if (m_dragMaskHandle == 6) {
                    // Right edge: grow width from right
                    geo.width   = std::max(0.01f, startGeo.width + shapeDx * 2.0f);
                } else { // 8 = left
                    // Left edge: shrink width from left
                    geo.width   = std::max(0.01f, startGeo.width - shapeDx * 2.0f);
                }
            } else {
                // Corner drag → scale width/height symmetrically
                float signX = (m_dragMaskHandle == 1 || m_dragMaskHandle == 2) ? 1.0f : -1.0f;
                float signY = (m_dragMaskHandle == 2 || m_dragMaskHandle == 3) ? 1.0f : -1.0f;
                if ((event->modifiers() & Qt::ShiftModifier)
                    && startGeo.width > 1.0e-6f
                    && startGeo.height > 1.0e-6f) {
                    const float sx = signX * shapeDx * 2.0f / startGeo.width;
                    const float sy = signY * shapeDy * 2.0f / startGeo.height;
                    const float deltaScale = std::abs(sx) >= std::abs(sy)
                        ? sx : sy;
                    const float scale = std::max(0.01f, 1.0f + deltaScale);
                    geo.width = startGeo.width * scale;
                    geo.height = startGeo.height * scale;
                } else {
                    geo.width  = std::max(
                        0.01f, startGeo.width + signX * shapeDx * 2.0f);
                    geo.height = std::max(
                        0.01f, startGeo.height + signY * shapeDy * 2.0f);
                }
            }
        }
        else if (mask.shape == MaskShape::FreeDrawBezier) {
            // INT_MAX is the whole-mask body sentinel; do not interpret it
            // as an encoded Bezier tangent handle.
            if (m_dragMaskHandle != INT_MAX
                && m_dragMaskHandle >= kMaskTangentHandleBase) {
                const int encoded = m_dragMaskHandle - kMaskTangentHandleBase;
                const size_t tangentVertex = static_cast<size_t>(encoded / 2);
                const bool outgoing = (encoded % 2) == 1;
                if (tangentVertex < geo.vertices.size()
                    && tangentVertex < startGeo.vertices.size()) {
                    auto& vertex = geo.vertices[tangentVertex];
                    const auto& startVertex = startGeo.vertices[tangentVertex];
                    auto snapTangent = [&](float tx, float ty) {
                        if (!(event->modifiers() & Qt::ShiftModifier))
                            return std::pair<float, float>{tx, ty};
                        const QPointF anchor = maskLocalToWidget(
                            startVertex.x, startVertex.y,
                            mask.coordinateSpace);
                        QPointF vector = maskVectorToWidget(
                            tx, ty, mask.coordinateSpace);
                        const double length = std::hypot(vector.x(), vector.y());
                        if (length < 1.0e-6)
                            return std::pair<float, float>{tx, ty};
                        constexpr double step = 3.14159265358979323846 / 4.0;
                        const double angle = std::round(
                            std::atan2(vector.y(), vector.x()) / step) * step;
                        const QPointF target = anchor + QPointF(
                            std::cos(angle) * length,
                            std::sin(angle) * length);
                        QPointF localTarget;
                        if (!widgetToMaskLocal(
                                target, localTarget, mask.coordinateSpace))
                            return std::pair<float, float>{tx, ty};
                        return std::pair<float, float>{
                            static_cast<float>(localTarget.x() - startVertex.x),
                            static_cast<float>(localTarget.y() - startVertex.y)};
                    };
                    auto mirrorDirectionPreservingLength = [&](float tx, float ty,
                                                                float oldX,
                                                                float oldY) {
                        const float px = tx * static_cast<float>(sourceSize.width());
                        const float py = ty * static_cast<float>(sourceSize.height());
                        const float len = std::hypot(px, py);
                        const float oldLen = std::hypot(
                            oldX * static_cast<float>(sourceSize.width()),
                            oldY * static_cast<float>(sourceSize.height()));
                        const float useLen = oldLen > 1.0e-6f ? oldLen : len;
                        if (len < 1.0e-6f)
                            return std::pair<float, float>{0.0f, 0.0f};
                        return std::pair<float, float>{
                            -(px / len) * useLen /
                                static_cast<float>(sourceSize.width()),
                            -(py / len) * useLen /
                                static_cast<float>(sourceSize.height())};
                    };
                    if (outgoing) {
                        auto tangent = snapTangent(
                            startVertex.outTanX + dxNorm,
                            startVertex.outTanY + dyNorm);
                        vertex.outTanX = tangent.first;
                        vertex.outTanY = tangent.second;
                        if (!(event->modifiers() & Qt::AltModifier)) {
                            auto opposite = mirrorDirectionPreservingLength(
                                vertex.outTanX, vertex.outTanY,
                                startVertex.inTanX, startVertex.inTanY);
                            vertex.inTanX = opposite.first;
                            vertex.inTanY = opposite.second;
                        }
                    } else {
                        auto tangent = snapTangent(
                            startVertex.inTanX + dxNorm,
                            startVertex.inTanY + dyNorm);
                        vertex.inTanX = tangent.first;
                        vertex.inTanY = tangent.second;
                        if (!(event->modifiers() & Qt::AltModifier)) {
                            auto opposite = mirrorDirectionPreservingLength(
                                vertex.inTanX, vertex.inTanY,
                                startVertex.outTanX, startVertex.outTanY);
                            vertex.outTanX = opposite.first;
                            vertex.outTanY = opposite.second;
                        }
                    }
                }
            } else {
            auto vi = static_cast<size_t>(m_dragMaskHandle);
            if (vi < geo.vertices.size() && vi < startGeo.vertices.size()) {
                // Drag single vertex
                geo.vertices[vi].x = startGeo.vertices[vi].x + dxNorm;
                geo.vertices[vi].y = startGeo.vertices[vi].y + dyNorm;
            } else if (geo.vertices.size() == startGeo.vertices.size()) {
                // Body drag — translate all vertices
                for (size_t i = 0; i < geo.vertices.size(); ++i) {
                    geo.vertices[i].x = startGeo.vertices[i].x + dxNorm;
                    geo.vertices[i].y = startGeo.vertices[i].y + dyNorm;
                }
            }
            }
        }

        mask.writeGeometry(m_maskTime, geo);

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
        const bool outerClipTarget = editsOuterClipTransform(m_overlay);
        if (m_overlay.useContentRect && !outerClipTarget &&
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
        float effScaleX = outerClipTarget
            ? 1.0f : std::max(0.001f, std::abs(m_overlay.clipScaleX));
        float effScaleY = outerClipTarget
            ? 1.0f : std::max(0.001f, std::abs(m_overlay.clipScaleY));

        float dx = static_cast<float>(wPos.x() - m_dragStartWidget.x()) / (pxPerUnitX * effScaleX);
        float dy = static_cast<float>(wPos.y() - m_dragStartWidget.y()) / (pxPerUnitY * effScaleY);

        setEditPosition(m_overlay, m_dragStartPosX + dx,
                        m_dragStartPosY + dy);

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
                setEditPosition(
                    m_overlay,
                    editPositionX(m_overlay) +
                        static_cast<float>(bestDx / (pxPerUnitX * effScaleX)),
                    editPositionY(m_overlay));
            if (bestDyAbs <= kSnapPx)
                setEditPosition(
                    m_overlay, editPositionX(m_overlay),
                    editPositionY(m_overlay) +
                        static_cast<float>(bestDy / (pxPerUnitY * effScaleY)));
        }

        syncMaskOwnerToEditedOuterTransform();
        emit transformPositionChanged(editPositionX(m_overlay),
                                      editPositionY(m_overlay));
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
                setEditScale(
                    m_overlay,
                    scaleWithOriginalSign(m_dragStartScX, ratioX),
                    scaleWithOriginalSign(m_dragStartScY, ratioY));
            } else {
                // Uniform scale (default): both axes get the same value
                float ratio = curDist / startDist;
                setEditScale(
                    m_overlay,
                    scaleWithOriginalSign(m_dragStartScX, ratio),
                    scaleWithOriginalSign(m_dragStartScY, ratio));
            }
            syncMaskOwnerToEditedOuterTransform();
            emit transformScaleChanged(editScaleX(m_overlay),
                                       editScaleY(m_overlay));
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
        setEditRotation(m_overlay, newRot);
        syncMaskOwnerToEditedOuterTransform();
        emit transformRotationChanged(editRotation(m_overlay));
        update();
        event->accept();
        return;
    }

    // ── Cursor hint when hovering ───────────────────────────────────────
    if (m_editTool == 7 && m_dragMode == DragMode::None) {
        applyCursor(zoomCursor());
        event->accept();
        return;
    }

    if (m_editTool == 8 && m_dragMode == DragMode::None) {
        applyCursor(Qt::CrossCursor);
        event->accept();
        return;
    }

    // Text/Type tool: always show the I-beam over the monitor (Premiere
    // Pro behavior) — even when a clip is selected and its transform
    // handles are visible.
    if (m_editTool == 9 && m_dragMode == DragMode::None) {
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

    if (m_editTool == 6 && m_dragMode == DragMode::None) {
        applyCursor(Qt::IBeamCursor);
        event->accept();
        return;
    }

    if (m_dragMode == DragMode::None && m_masks && !m_masks->empty()
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

void TransformOverlayWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragMode == DragMode::Pan) {
        m_dragMode = DragMode::None;
        applyCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    if (m_dragMode == DragMode::DrawMaskPoint) {
        m_dragMode = DragMode::None;
        m_penHoverWidget = event->position();
        applyCursor(penCursor());
        update();
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

    if (m_dragMode == DragMode::CropEdge) {
        const float oldL = m_dragStartCrop[0], oldR = m_dragStartCrop[1];
        const float oldT = m_dragStartCrop[2], oldB = m_dragStartCrop[3];
        const float newL = m_overlay.cropL, newR = m_overlay.cropR;
        const float newT = m_overlay.cropT, newB = m_overlay.cropB;
        m_dragMode   = DragMode::None;
        m_cropHandle = -1;
        applyCursor(Qt::ArrowCursor);
        emit cropDragFinished(oldL, oldR, oldT, oldB, newL, newR, newT, newB);
        event->accept();
        return;
    }

    if (m_dragMode != DragMode::None) {
        float oldPX = m_dragStartPosX, oldPY = m_dragStartPosY;
        float oldSX = m_dragStartScX,  oldSY = m_dragStartScY;
        float oldRot = m_dragStartRot;
        float newPX = editPositionX(m_overlay);
        float newPY = editPositionY(m_overlay);
        float newSX = editScaleX(m_overlay);
        float newSY = editScaleY(m_overlay);
        float newRot = editRotation(m_overlay);
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
    // A release can be lost when Windows deactivates or tears down the native
    // viewport's mouse grab. Finish through the normal release path so undo
    // bookkeeping is retained, then drop application-wide cursor state.
    if (event->type() == QEvent::ApplicationDeactivate
        || event->type() == QEvent::WindowDeactivate
        || event->type() == QEvent::UngrabMouse) {
        if (m_dragMode != DragMode::None) {
            const QPoint global = QCursor::pos();
            QMouseEvent release(QEvent::MouseButtonRelease,
                                QPointF(mapFromGlobal(global)), QPointF(global),
                                Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            mouseReleaseEvent(&release);
        }
        m_dragMode = DragMode::None;
        m_dragHandle = -1;
        m_cropHandle = -1;
        m_dragMaskIndex = -1;
        m_dragMaskHandle = -1;
        m_dragMotionKfIdx = -1;
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
                        || ke->key() == Qt::Key_Return
                        || ke->key() == Qt::Key_Enter));
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
        if (m_dragMode == DragMode::None)
            clearCursorOverride();
        break;

    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}


} // namespace rt
