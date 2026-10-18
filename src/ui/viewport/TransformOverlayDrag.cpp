/*
 * TransformOverlayDrag.cpp — the Program Monitor's drags (TransformOverlayDrag.h)
 * except the mask drag (TransformOverlayMaskDrag.cpp).
 */

#include "viewport/TransformOverlayDrag.h"
#include "viewport/TransformOverlayEdit.h"
#include "viewport/VulkanViewport.h"
#include "timeline/OpacityMask.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/Keyframe.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "edit/EditSession.h"

#include <QMouseEvent>

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <utility>

namespace rt {

using namespace overlay_edit;

// ── Pan ─────────────────────────────────────────────────────────────────

OverlayPanDrag::OverlayPanDrag(TransformOverlayWidget& w, QPointF press)
    : m_w(w), m_start(press),
      m_startPanX(w.m_vulkanVp ? w.m_vulkanVp->viewPanX() : 0.0f),
      m_startPanY(w.m_vulkanVp ? w.m_vulkanVp->viewPanY() : 0.0f)
{
}

bool OverlayPanDrag::move(QMouseEvent* event)
{
    if (!m_w.m_vulkanVp) return false;
    const QPointF wPos = event->position();
    float dx = static_cast<float>(wPos.x() - m_start.x());
    float dy = static_cast<float>(wPos.y() - m_start.y());
    m_w.m_vulkanVp->setViewPan(m_startPanX + dx, m_startPanY + dy);
    m_w.update(); // redraw overlay at new pan
    event->accept();
    return true;
}

void OverlayPanDrag::release(QMouseEvent* /*event*/)
{
    m_w.applyCursor(Qt::ArrowCursor);
}

void OverlayPanDrag::cancel()
{
    if (m_w.m_vulkanVp) m_w.m_vulkanVp->setViewPan(m_startPanX, m_startPanY);
    m_w.applyCursor(Qt::ArrowCursor);
    m_w.update();
}

// ── Move / scale / rotate the box ───────────────────────────────────────

OverlayTransformDrag::OverlayTransformDrag(TransformOverlayWidget& w, Mode kind,
                                           QPointF press)
    : m_w(w), m_kind(kind), m_start(press),
      m_startPosX(editPositionX(w.m_overlay)), m_startPosY(editPositionY(w.m_overlay)),
      m_startScX(editScaleX(w.m_overlay)), m_startScY(editScaleY(w.m_overlay)),
      m_startRot(editRotation(w.m_overlay))
{
    if (kind == Mode::RotateCorner) {
        // Starting angle from the centre to the mouse
        QPointF corners[4];
        w.computeOverlayCorners(corners);
        QPointF center = (corners[0] + corners[2]) * 0.5;
        m_startAngle = static_cast<float>(
            std::atan2(press.y() - center.y(), press.x() - center.x())
            * 180.0 / 3.14159265358979);
    }
}

bool OverlayTransformDrag::move(QMouseEvent* event)
{
    switch (m_kind) {
    case Mode::MoveBody:    return moveBody(event);
    case Mode::ScaleCorner: return scale(event);
    default:                return rotate(event);
    }
}

bool OverlayTransformDrag::moveBody(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) return false;
    const QPointF wPos = event->position();
    QRectF fr = m_w.computeFrameRect();
    if (fr.isEmpty()) return true;

    // Two coordinate conventions, mirroring the anchor handler below:
    //   • Content-rect mode (graphic layers): posX/posY are CANVAS px
    //     (project resolution). On a 4K project canvas=3840, so using
    //     REF_1920 here divided the mouse-to-layer rate by 2 — the text
    //     visibly lagged the cursor.
    //   • Standard mode (video / image): posX/posY are REF-1920 px.
    float pxPerUnitX = 0.0f, pxPerUnitY = 0.0f;
    const bool outerClipTarget = editsOuterClipTransform(m_w.m_overlay);
    if (m_w.m_overlay.useContentRect && !outerClipTarget &&
        m_w.m_overlay.contentCanvasW > 0.0f && m_w.m_overlay.contentCanvasH > 0.0f)
    {
        pxPerUnitX = static_cast<float>(fr.width())  / m_w.m_overlay.contentCanvasW;
        pxPerUnitY = static_cast<float>(fr.height()) / m_w.m_overlay.contentCanvasH;
    } else {
        constexpr float REF_W = 1920.0f;
        constexpr float REF_H = 1080.0f;
        pxPerUnitX = static_cast<float>(fr.width())  / REF_W;
        pxPerUnitY = static_cast<float>(fr.height()) / REF_H;
    }
    if (pxPerUnitX < 0.001f || pxPerUnitY < 0.001f) return true;

    // Account for clip-level scale: layer position is in canvas space,
    // but the clip scale magnifies the whole canvas, so mouse movement
    // needs to be divided by the clip scale to get the correct delta.
    float effScaleX = outerClipTarget
        ? 1.0f : std::max(0.001f, std::abs(m_w.m_overlay.clipScaleX));
    float effScaleY = outerClipTarget
        ? 1.0f : std::max(0.001f, std::abs(m_w.m_overlay.clipScaleY));

    float dx = static_cast<float>(wPos.x() - m_start.x()) / (pxPerUnitX * effScaleX);
    float dy = static_cast<float>(wPos.y() - m_start.y()) / (pxPerUnitY * effScaleY);

    setEditPosition(m_w.m_overlay, m_startPosX + dx,
                    m_startPosY + dy);

    // Premiere-style Ctrl-snap: magnetise the overlay's AABB to the
    // frame edges and centre lines while Ctrl is held. Worked out in
    // widget pixels and converted back through the same px-per-unit
    // factor used for the drag so snap distance is zoom-invariant.
    if (event->modifiers() & Qt::ControlModifier) {
        QPointF corners[4];
        m_w.computeOverlayCorners(corners);
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
                m_w.m_overlay,
                editPositionX(m_w.m_overlay) +
                    static_cast<float>(bestDx / (pxPerUnitX * effScaleX)),
                editPositionY(m_w.m_overlay));
        if (bestDyAbs <= kSnapPx)
            setEditPosition(
                m_w.m_overlay, editPositionX(m_w.m_overlay),
                editPositionY(m_w.m_overlay) +
                    static_cast<float>(bestDy / (pxPerUnitY * effScaleY)));
    }

    m_w.syncMaskOwnerToEditedOuterTransform();
    emit m_w.transformPositionChanged(editPositionX(m_w.m_overlay),
                                      editPositionY(m_w.m_overlay));
    m_w.update();
    event->accept();
    return true;
}

bool OverlayTransformDrag::scale(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) return false;
    const QPointF wPos = event->position();
    QPointF corners[4];
    m_w.computeOverlayCorners(corners);
    QPointF center = (corners[0] + corners[2]) * 0.5;

    float startDist = std::hypot(
        static_cast<float>(m_start.x() - center.x()),
        static_cast<float>(m_start.y() - center.y()));
    float curDist = std::hypot(
        static_cast<float>(wPos.x() - center.x()),
        static_cast<float>(wPos.y() - center.y()));

    if (startDist > 1.0f) {
        if (event->modifiers() & Qt::ShiftModifier) {
            // Non-uniform (free) scale: separate X and Y ratios
            float startDx = std::abs(static_cast<float>(m_start.x() - center.x()));
            float startDy = std::abs(static_cast<float>(m_start.y() - center.y()));
            float curDx   = std::abs(static_cast<float>(wPos.x() - center.x()));
            float curDy   = std::abs(static_cast<float>(wPos.y() - center.y()));
            float ratioX  = (startDx > 1.0f) ? curDx / startDx : 1.0f;
            float ratioY  = (startDy > 1.0f) ? curDy / startDy : 1.0f;
            setEditScale(
                m_w.m_overlay,
                scaleWithOriginalSign(m_startScX, ratioX),
                scaleWithOriginalSign(m_startScY, ratioY));
        } else {
            // Uniform scale (default): both axes get the same value
            float ratio = curDist / startDist;
            setEditScale(
                m_w.m_overlay,
                scaleWithOriginalSign(m_startScX, ratio),
                scaleWithOriginalSign(m_startScY, ratio));
        }
        m_w.syncMaskOwnerToEditedOuterTransform();
        emit m_w.transformScaleChanged(editScaleX(m_w.m_overlay),
                                       editScaleY(m_w.m_overlay));
        m_w.update();
    }
    event->accept();
    return true;
}

bool OverlayTransformDrag::rotate(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) return false;
    const QPointF wPos = event->position();
    QPointF corners[4];
    m_w.computeOverlayCorners(corners);
    QPointF center = (corners[0] + corners[2]) * 0.5;

    float curAngle = static_cast<float>(
        std::atan2(wPos.y() - center.y(), wPos.x() - center.x())
        * 180.0 / 3.14159265358979);
    float deltaAngle = curAngle - m_startAngle;

    // Normalize to -180..180
    while (deltaAngle >  180.0f) deltaAngle -= 360.0f;
    while (deltaAngle < -180.0f) deltaAngle += 360.0f;

    float newRot = m_startRot + deltaAngle;
    setEditRotation(m_w.m_overlay, newRot);
    m_w.syncMaskOwnerToEditedOuterTransform();
    emit m_w.transformRotationChanged(editRotation(m_w.m_overlay));
    m_w.update();
    event->accept();
    return true;
}

void OverlayTransformDrag::release(QMouseEvent* /*event*/)
{
    float oldPX = m_startPosX, oldPY = m_startPosY;
    float oldSX = m_startScX,  oldSY = m_startScY;
    float oldRot = m_startRot;
    float newPX = editPositionX(m_w.m_overlay);
    float newPY = editPositionY(m_w.m_overlay);
    float newSX = editScaleX(m_w.m_overlay);
    float newSY = editScaleY(m_w.m_overlay);
    float newRot = editRotation(m_w.m_overlay);
    m_w.applyCursor(Qt::ArrowCursor);
    emit m_w.transformDragFinished(oldPX, oldPY, oldSX, oldSY, oldRot,
                                   newPX, newPY, newSX, newSY, newRot);
}

void OverlayTransformDrag::cancel()
{
    setEditPosition(m_w.m_overlay, m_startPosX, m_startPosY);
    setEditScale(m_w.m_overlay, m_startScX, m_startScY);
    setEditRotation(m_w.m_overlay, m_startRot);
    m_w.syncMaskOwnerToEditedOuterTransform();
    m_w.applyCursor(Qt::ArrowCursor);
    m_w.update();
}

// ── Anchor point ────────────────────────────────────────────────────────
// The anchor is the rotation/scale pivot (Premiere/AE-style). Two
// coordinate conventions:
//   • Content-rect mode (graphic layers): anchor is canvas-px → use
//     contentCanvasW/H to convert widget Δ → canvas Δ.
//   • Standard mode (video / image / etc.): anchor is REF-1920 px
//     stored on the clip's anchorX/Y tracks → convert widget Δ →
//     REF-1920 Δ using fr.width / REF_1920.

OverlayAnchorDrag::OverlayAnchorDrag(TransformOverlayWidget& w, QPointF press)
    : m_w(w), m_start(press),
      m_startAnchorX(w.m_overlay.anchorX), m_startAnchorY(w.m_overlay.anchorY)
{
}

bool OverlayAnchorDrag::move(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) return false;
    const QPointF wPos = event->position();
    QRectF fr = m_w.computeFrameRect();
    if (fr.isEmpty()) return true;
    float pxPerUnitX = 0.0f, pxPerUnitY = 0.0f;
    if (m_w.m_overlay.useContentRect &&
        m_w.m_overlay.contentCanvasW > 0.0f && m_w.m_overlay.contentCanvasH > 0.0f)
    {
        pxPerUnitX = static_cast<float>(fr.width())  / m_w.m_overlay.contentCanvasW;
        pxPerUnitY = static_cast<float>(fr.height()) / m_w.m_overlay.contentCanvasH;
    } else {
        constexpr float REF_W = 1920.0f;
        constexpr float REF_H = 1080.0f;
        pxPerUnitX = static_cast<float>(fr.width())  / REF_W;
        pxPerUnitY = static_cast<float>(fr.height()) / REF_H;
    }
    if (pxPerUnitX < 1e-4f || pxPerUnitY < 1e-4f) return true;

    const float effClipScaleX = std::max(0.001f, m_w.m_overlay.clipScaleX);
    const float effClipScaleY = std::max(0.001f, m_w.m_overlay.clipScaleY);

    const float dx = static_cast<float>(wPos.x() - m_start.x())
                     / (pxPerUnitX * effClipScaleX);
    const float dy = static_cast<float>(wPos.y() - m_start.y())
                     / (pxPerUnitY * effClipScaleY);

    m_w.m_overlay.anchorX = m_startAnchorX + dx;
    m_w.m_overlay.anchorY = m_startAnchorY + dy;

    emit m_w.transformAnchorChanged(m_w.m_overlay.anchorX, m_w.m_overlay.anchorY);
    m_w.update();
    event->accept();
    return true;
}

void OverlayAnchorDrag::release(QMouseEvent* /*event*/)
{
    const float oldX = m_startAnchorX;
    const float oldY = m_startAnchorY;
    const float newX = m_w.m_overlay.anchorX;
    const float newY = m_w.m_overlay.anchorY;
    m_w.applyCursor(Qt::ArrowCursor);
    emit m_w.transformAnchorDragFinished(oldX, oldY, newX, newY);
}

void OverlayAnchorDrag::cancel()
{
    m_w.m_overlay.anchorX = m_startAnchorX;
    m_w.m_overlay.anchorY = m_startAnchorY;
    m_w.applyCursor(Qt::ArrowCursor);
    m_w.update();
}

// ── Crop edge ───────────────────────────────────────────────────────────

OverlayCropDrag::OverlayCropDrag(TransformOverlayWidget& w, int handle, QPointF press)
    : m_w(w), m_handle(handle), m_start(press),
      m_startCrop{w.m_overlay.cropL, w.m_overlay.cropR, w.m_overlay.cropT, w.m_overlay.cropB}
{
}

bool OverlayCropDrag::move(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) return false;
    const QPointF wPos = event->position();
    QPointF corners[4];
    m_w.computeOverlayCorners(corners);   // outer box — crop never changes it
    const QPointF U = corners[1] - corners[0];   // left→right axis
    const QPointF V = corners[3] - corners[0];   // top→bottom axis
    const double dxw = wPos.x() - m_start.x();
    const double dyw = wPos.y() - m_start.y();

    // Fraction (0..1 of the box edge) the mouse moved along an axis.
    auto projFrac = [](double ex, double ey, double ax, double ay) -> double {
        const double len2 = ax * ax + ay * ay;
        if (len2 < 1e-9) return 0.0;
        return (ex * ax + ey * ay) / len2;
    };

    float l = m_startCrop[0], r = m_startCrop[1];
    float t = m_startCrop[2], b = m_startCrop[3];
    constexpr float kMaxSum = 95.0f;   // keep L+R<100 and T+B<100 (visible sliver)
    // The opposite edge can already exceed kMaxSum (the Properties/Effect
    // spins allow each edge 0..100 independently), so std::max(0,...) the
    // upper bound — otherwise std::clamp(value, 0, negative) is lo>hi UB and
    // would yield a negative crop.
    if (m_handle == 0) {           // Left edge
        double f = projFrac(dxw, dyw, U.x(), U.y()) * 100.0;
        l = std::clamp(static_cast<float>(m_startCrop[0] + f), 0.0f, std::max(0.0f, kMaxSum - r));
    } else if (m_handle == 1) {    // Right edge (grows as handle moves left)
        double f = projFrac(dxw, dyw, U.x(), U.y()) * 100.0;
        r = std::clamp(static_cast<float>(m_startCrop[1] - f), 0.0f, std::max(0.0f, kMaxSum - l));
    } else if (m_handle == 2) {    // Top edge
        double f = projFrac(dxw, dyw, V.x(), V.y()) * 100.0;
        t = std::clamp(static_cast<float>(m_startCrop[2] + f), 0.0f, std::max(0.0f, kMaxSum - b));
    } else {                           // Bottom edge (grows as handle moves up)
        double f = projFrac(dxw, dyw, V.x(), V.y()) * 100.0;
        b = std::clamp(static_cast<float>(m_startCrop[3] - f), 0.0f, std::max(0.0f, kMaxSum - t));
    }

    m_w.m_overlay.cropL = l; m_w.m_overlay.cropR = r;
    m_w.m_overlay.cropT = t; m_w.m_overlay.cropB = b;
    m_w.update();
    emit m_w.cropChanged(l, r, t, b);
    event->accept();
    return true;
}

void OverlayCropDrag::release(QMouseEvent* /*event*/)
{
    const float oldL = m_startCrop[0], oldR = m_startCrop[1];
    const float oldT = m_startCrop[2], oldB = m_startCrop[3];
    const float newL = m_w.m_overlay.cropL, newR = m_w.m_overlay.cropR;
    const float newT = m_w.m_overlay.cropT, newB = m_w.m_overlay.cropB;
    m_w.applyCursor(Qt::ArrowCursor);
    emit m_w.cropDragFinished(oldL, oldR, oldT, oldB, newL, newR, newT, newB);
}

void OverlayCropDrag::cancel()
{
    m_w.m_overlay.cropL = m_startCrop[0];
    m_w.m_overlay.cropR = m_startCrop[1];
    m_w.m_overlay.cropT = m_startCrop[2];
    m_w.m_overlay.cropB = m_startCrop[3];
    m_w.applyCursor(Qt::ArrowCursor);
    m_w.update();
}

// ── Motion-path spatial handle ──────────────────────────────────────────

OverlayMotionHandleDrag::OverlayMotionHandleDrag(TransformOverlayWidget& w,
                                                 int keyframeIndex, bool inHandle)
    : m_w(w), m_kfIdx(keyframeIndex), m_isIn(inHandle)
{
    const auto& kfx = w.m_motionX->keyframe(static_cast<size_t>(keyframeIndex));
    const auto& kfy = w.m_motionY->keyframe(static_cast<size_t>(keyframeIndex));
    m_kfTime   = kfx.time;
    m_origInX  = kfx.spatialInX;
    m_origInY  = kfy.spatialInY;
    m_origOutX = kfx.spatialOutX;
    m_origOutY = kfy.spatialOutY;
}

bool OverlayMotionHandleDrag::move(QMouseEvent* event)
{
    if (!m_w.m_motionX || !m_w.m_motionY || !(event->buttons() & Qt::LeftButton)) return false;
    const QPointF wPos = event->position();
    const QRectF fr = m_w.computeFrameRect();
    if (fr.isEmpty()) { event->accept(); return true; }
    if (m_kfIdx < 0 ||
        m_kfIdx >= static_cast<int>(m_w.m_motionX->keyframeCount())) {
        event->accept();
        return true;
    }
    auto& kfx = m_w.m_motionX->keyframe(static_cast<size_t>(m_kfIdx));
    auto& kfy = m_w.m_motionY->keyframe(static_cast<size_t>(m_kfIdx));

    // Convert widget pos to REF-1920 px and subtract the waypoint
    // position to get the handle offset.
    const QPointF refPos = m_w.widgetToRef(wPos, fr);
    const float newHX = static_cast<float>(refPos.x()) - kfx.value;
    const float newHY = static_cast<float>(refPos.y()) - kfy.value;

    if (m_isIn) {
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
    m_w.update();
    emit m_w.motionPathLiveUpdate();
    event->accept();
    return true;
}

void OverlayMotionHandleDrag::release(QMouseEvent* /*event*/)
{
    // Record an undoable command capturing the spatial-handle change —
    // unless the workspace's edit gesture for this drag records it.
    const EditSession* edits =
        m_w.m_motionCmdStack ? m_w.m_motionCmdStack->editSession() : nullptr;
    if (m_w.m_motionCmdStack && m_w.m_motionX && m_w.m_motionY &&
        m_kfIdx >= 0 && !(edits && edits->isOpen()))
    {
        auto* tx = m_w.m_motionX;
        auto* ty = m_w.m_motionY;
        const int64_t kfTime = m_kfTime;
        const float oldInX  = m_origInX,  oldInY  = m_origInY;
        const float oldOutX = m_origOutX, oldOutY = m_origOutY;
        const float newInX  = (m_kfIdx < static_cast<int>(tx->keyframeCount())
                               ? tx->keyframe(static_cast<size_t>(m_kfIdx)).spatialInX  : 0.0f);
        const float newInY  = (m_kfIdx < static_cast<int>(ty->keyframeCount())
                               ? ty->keyframe(static_cast<size_t>(m_kfIdx)).spatialInY  : 0.0f);
        const float newOutX = (m_kfIdx < static_cast<int>(tx->keyframeCount())
                               ? tx->keyframe(static_cast<size_t>(m_kfIdx)).spatialOutX : 0.0f);
        const float newOutY = (m_kfIdx < static_cast<int>(ty->keyframeCount())
                               ? ty->keyframe(static_cast<size_t>(m_kfIdx)).spatialOutY : 0.0f);

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
        m_w.m_motionCmdStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            "Move Motion-Path Handle",
            [applyHandles, newInX, newInY, newOutX, newOutY]() {
                applyHandles(newInX, newInY, newOutX, newOutY);
            },
            [applyHandles, oldInX, oldInY, oldOutX, oldOutY]() {
                applyHandles(oldInX, oldInY, oldOutX, oldOutY);
            }));
    }
    m_w.applyCursor(Qt::ArrowCursor);
}

void OverlayMotionHandleDrag::cancel()
{
    // The handles live on the clip's keyframes, so put them back here (the
    // workspace's gesture, when there is one, restores the same values).
    if (m_w.m_motionX && m_w.m_motionY) {
        for (size_t i = 0; i < m_w.m_motionX->keyframeCount(); ++i)
            if (m_w.m_motionX->keyframe(i).time == m_kfTime) {
                m_w.m_motionX->keyframe(i).spatialInX  = m_origInX;
                m_w.m_motionX->keyframe(i).spatialOutX = m_origOutX;
                break;
            }
        for (size_t i = 0; i < m_w.m_motionY->keyframeCount(); ++i)
            if (m_w.m_motionY->keyframe(i).time == m_kfTime) {
                m_w.m_motionY->keyframe(i).spatialInY  = m_origInY;
                m_w.m_motionY->keyframe(i).spatialOutY = m_origOutY;
                break;
            }
        emit m_w.motionPathLiveUpdate();
    }
    m_w.applyCursor(Qt::ArrowCursor);
    m_w.update();
}

// ── New masks ───────────────────────────────────────────────────────────

OverlayPenPointDrag::OverlayPenPointDrag(TransformOverlayWidget& w, QPointF press,
                                         QPointF pressLocal)
    : m_w(w), m_start(press), m_pressLocal(pressLocal)
{
}

bool OverlayPenPointDrag::move(QMouseEvent* event)
{
    if (!m_w.m_penDrawing || !(event->buttons() & Qt::LeftButton)
        || m_w.m_penDraft.base.vertices.empty()) return false;
    const QPointF wPos = event->position();
    QPointF local;
    if (m_w.widgetToMaskLocal(wPos, local)) {
        if (event->modifiers() & Qt::ShiftModifier) {
            QPointF delta = wPos - m_start;
            const double length = std::hypot(delta.x(), delta.y());
            if (length > 1.0e-6) {
                constexpr double step =
                    3.14159265358979323846 / 4.0;
                const double angle = std::round(
                    std::atan2(delta.y(), delta.x()) / step) * step;
                const QPointF constrained = m_start + QPointF(
                    std::cos(angle) * length,
                    std::sin(angle) * length);
                (void)m_w.widgetToMaskLocal(constrained, local);
            }
        }
        const float dx = static_cast<float>(local.x() - m_pressLocal.x());
        const float dy = static_cast<float>(local.y() - m_pressLocal.y());
        auto& vertex = m_w.m_penDraft.base.vertices.back();
        vertex.inTanX = -dx;
        vertex.inTanY = -dy;
        vertex.outTanX = dx;
        vertex.outTanY = dy;
        m_w.m_penHoverWidget = wPos;
        m_w.update();
    }
    event->accept();
    return true;
}

void OverlayPenPointDrag::release(QMouseEvent* event)
{
    m_w.m_penHoverWidget = event->position();
    m_w.applyCursor(TransformOverlayWidget::penCursor());
    m_w.update();
}

void OverlayPenPointDrag::cancel()
{
    // As Esc between clicks: the whole unfinished path is dropped.
    m_w.cancelPenMask();
}

bool OverlayShapeDrag::move(QMouseEvent* event)
{
    if (!m_w.m_shapeDrawing || !(event->buttons() & Qt::LeftButton)) return false;
    const QPointF wPos = event->position();
    m_w.updateShapeMask(wPos, event->modifiers());
    event->accept();
    return true;
}

void OverlayShapeDrag::release(QMouseEvent* event)
{
    m_w.updateShapeMask(event->position(), event->modifiers());
    m_w.commitShapeMask();
    m_w.applyCursor(Qt::CrossCursor);
}

void OverlayShapeDrag::cancel()
{
    m_w.cancelShapeMask();
    m_w.applyCursor(Qt::CrossCursor);
}

} // namespace rt
