/*
 * TransformOverlayWidget.cpp — Transparent overlay for transform handles + pan.
 */

#include "viewport/TransformOverlayWidget.h"
#include "viewport/OverlayMath.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/Position2D.h"
#include "command/CommandStack.h"
#include "command/commands/KeyframeCmds.h"

#include <climits>
#include "viewport/VulkanViewport.h"
#include "timeline/OpacityMask.h"
#include "Theme.h"

#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPolygonF>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QApplication>

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════════

TransformOverlayWidget::TransformOverlayWidget(VulkanViewport* viewport,
                                               QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus)
    , m_vulkanVp(viewport)
{
    // Top-level frameless tool window with per-pixel alpha transparency.
    // ProgramMonitor positions this window to exactly cover the Vulkan
    // surface on screen.  WA_TranslucentBackground works correctly for
    // top-level windows — Qt manages the layered-window alpha surface.
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setMouseTracking(true);
    setAttribute(Qt::WA_SetCursor, true); // enable per-tool cursor changes

    // A click in the visible Program Monitor can be delivered either to the
    // embedded native Vulkan QWindow or to this top-level translucent overlay,
    // depending on the alpha at that exact pixel. Observe application mouse
    // events so one owner handles double-click-to-edit in both cases.
    qApp->installEventFilter(this);

    // The VulkanViewport uses createWindowContainer() which embeds a native
    // QWindow that sits on top of all regular Qt widgets in the Z-order.
    // ProgramMonitor::syncOverlayGeometry() calls SetWindowPos(HWND_TOP)
    // to keep this overlay visually above the native surface.  Mouse
    // events still need an event filter because the transparent areas
    // (WA_TranslucentBackground with alpha=0) pass input through to the
    // native QWindow underneath.
    if (m_vulkanVp) {
        connect(m_vulkanVp, &VulkanViewport::nativeLeftDoubleClicked,
                this, [this](const QPointF& globalPosition,
                             Qt::KeyboardModifiers modifiers) {
            const QPointF overlayPosition = QPointF(
                mapFromGlobal(globalPosition.toPoint()));
            spdlog::warn("[INLINE-TEXT] routed native double-click global=({}, {}) overlay=({}, {})",
                         globalPosition.x(), globalPosition.y(),
                         overlayPosition.x(), overlayPosition.y());
            QMouseEvent mapped(QEvent::MouseButtonDblClick,
                               overlayPosition, globalPosition,
                               Qt::LeftButton, Qt::LeftButton, modifiers);
            mouseDoubleClickEvent(&mapped);
        });
        if (m_vulkanVp->nativeWindow())
            m_vulkanVp->nativeWindow()->installEventFilter(this);
    }

}

TransformOverlayWidget::~TransformOverlayWidget()
{
    qApp->removeEventFilter(this);
    clearCursorOverride();  // never leave a dangling app override cursor
    // m_inlineTextEdit is a parent-less top-level widget — delete it
    // explicitly so it doesn't leak when the overlay is destroyed.
    if (m_inlineTextEdit) {
        m_inlineTextEdit->deleteLater();
        m_inlineTextEdit = nullptr;
    }
}

void TransformOverlayWidget::setEditTool(uint8_t tool) noexcept
{
    if (m_editTool == 9 && tool != 9)
        cancelPenMask();
    // Selection is also Premiere's mask-manipulation tool. Do not clear the
    // selected mask here: Effect Controls selects the path, then activates
    // Selection so its vertices/body move without moving the owning clip.
    m_editTool = tool;
    // Immediately apply the correct cursor when switching tools.
    if (tool == 7)  // zoom tool
        applyCursor(zoomCursor());
    else if (tool == 9)  // Pen Mask tool
        applyCursor(penCursor());
    else if (tool == 6)  // text/type tool — I-beam like Premiere Pro
        applyCursor(Qt::IBeamCursor);
    else if (tool == 8)  // eyedropper
        applyCursor(Qt::CrossCursor);
    else
        applyCursor(Qt::ArrowCursor);
}

void TransformOverlayWidget::setMasks(std::vector<OpacityMask>* masks) noexcept
{
    if (m_masks != masks) {
        cancelPenMask();
        m_activeMaskIndex = -1;
    }
    m_masks = masks;
    if (!m_masks || m_masks->empty()
        || m_activeMaskIndex >= static_cast<int>(m_masks->size()))
        m_activeMaskIndex = -1;
    update();
}

void TransformOverlayWidget::enterEvent(QEnterEvent* /*event*/)
{
    // Re-apply tool cursor when mouse enters overlay area.
    if (m_editTool == 7)
        applyCursor(zoomCursor());
    else if (m_editTool == 9)
        applyCursor(penCursor());
    else if (m_editTool == 6)
        applyCursor(Qt::IBeamCursor);
    else if (m_editTool == 8)
        applyCursor(Qt::CrossCursor);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Overlay control
// ═════════════════════════════════════════════════════════════════════════════

void TransformOverlayWidget::setTransformOverlay(const TransformOverlayInfo& info)
{
    // While inline editing, keep the latest compositor-measured content
    // geometry for the edit indicators but leave the normal transform gizmo
    // hidden until the session ends.
    if (m_inlineTextEdit && m_inlineTextEdit->isVisible()) {
        m_savedOverlayBeforeEdit = info;
        m_savedOverlayBeforeEdit.visible = true;
        update();
        return;
    }
    m_overlay = info;
    if (!info.visible)
        clearCursorOverride();  // no selection → no special cursor
    update();
}

void TransformOverlayWidget::setSecondaryOverlays(
    const std::vector<TransformOverlayInfo>& extras)
{
    // Even during inline text editing we keep the secondary list up-to-date
    // — these are sibling layers that aren't being edited, so showing their
    // boxes still makes sense.
    m_secondaryOverlays = extras;
    update();
}

void TransformOverlayWidget::clearTransformOverlay()
{
    cancelPenMask();
    m_overlay.visible = false;
    m_masks = nullptr;
    m_activeMaskIndex = -1;
    m_hasMaskOwnerOverlay = false;
    m_maskOwnerFollowsPrimary = false;
    m_secondaryOverlays.clear();
    m_dragMode = DragMode::None;
    applyCursor(Qt::ArrowCursor);
    update();
}

void TransformOverlayWidget::setMotionPathTracks(KeyframeTrack<float>* trackX,
                                                  KeyframeTrack<float>* trackY,
                                                  CommandStack* commandStack) noexcept
{
    m_motionX = trackX;
    m_motionY = trackY;
    m_motionCmdStack = commandStack;
    update();
}

void TransformOverlayWidget::clearMotionPath() noexcept
{
    m_motionX = nullptr;
    m_motionY = nullptr;
    m_motionCmdStack = nullptr;
    update();
}

QPointF TransformOverlayWidget::refToWidget(float refX, float refY, const QRectF& frameRect) const
{
    // REF-1920 keyframe values are offsets from frame center; convert to
    // frame-space pixels (origin at top-left), then to widget pixels.
    const float fxScale = (m_seqW > 0 ? static_cast<float>(m_seqW) : 1920.0f) / 1920.0f;
    const float fyScale = (m_seqH > 0 ? static_cast<float>(m_seqH) : 1080.0f) / 1080.0f;
    const float frameX  = refX * fxScale + static_cast<float>(m_seqW) * 0.5f;
    const float frameY  = refY * fyScale + static_cast<float>(m_seqH) * 0.5f;

    // Map frame -> widget using the same math frameToWidget() uses, but
    // taking the frameRect we already have so we don't recompute it.
    const float u = frameX / static_cast<float>(m_seqW > 0 ? m_seqW : 1);
    const float v = frameY / static_cast<float>(m_seqH > 0 ? m_seqH : 1);
    return { frameRect.left() + u * frameRect.width(),
             frameRect.top()  + v * frameRect.height() };
}

QPointF TransformOverlayWidget::widgetToRef(const QPointF& widgetPos, const QRectF& frameRect) const
{
    if (frameRect.width() <= 0.0 || frameRect.height() <= 0.0)
        return QPointF(0.0, 0.0);
    const double u = (widgetPos.x() - frameRect.left()) / frameRect.width();
    const double v = (widgetPos.y() - frameRect.top())  / frameRect.height();
    const double seqW = (m_seqW > 0) ? static_cast<double>(m_seqW) : 1920.0;
    const double seqH = (m_seqH > 0) ? static_cast<double>(m_seqH) : 1080.0;
    const double frameX = u * seqW;
    const double frameY = v * seqH;
    // Inverse of refToWidget: refX = (frameX - seqW/2) * 1920 / seqW.
    const double refX = (frameX - seqW * 0.5) * 1920.0 / seqW;
    const double refY = (frameY - seqH * 0.5) * 1080.0 / seqH;
    return { refX, refY };
}

bool TransformOverlayWidget::hitTestMotionHandle(const QPointF& widgetPos,
                                                  int& outKfIdx, bool& outIsIn) const
{
    if (!m_motionX || !m_motionY) return false;
    const size_t n = m_motionX->keyframeCount();
    if (n < 2 || m_motionY->keyframeCount() != n) return false;
    const QRectF fr = computeFrameRect();
    constexpr double kHitRadius = 7.0;

    for (size_t i = 0; i < n; ++i) {
        const auto& kx = m_motionX->keyframe(i);
        const auto& ky = m_motionY->keyframe(i);
        if (kx.time != ky.time) continue;
        const bool hasManual =
            kx.spatialInterp == InterpMode::Bezier ||
            kx.spatialInterp == InterpMode::ContinuousBezier;
        if (!hasManual) continue;

        // Out-handle: only on segments that have one (not the last keyframe).
        if (i + 1 < n) {
            QPointF pt = refToWidget(kx.value + kx.spatialOutX,
                                     ky.value + ky.spatialOutY, fr);
            if (QLineF(widgetPos, pt).length() <= kHitRadius) {
                outKfIdx = static_cast<int>(i);
                outIsIn  = false;
                return true;
            }
        }
        // In-handle: only on segments that have one (not the first keyframe).
        if (i > 0) {
            QPointF pt = refToWidget(kx.value + kx.spatialInX,
                                     ky.value + ky.spatialInY, fr);
            if (QLineF(widgetPos, pt).length() <= kHitRadius) {
                outKfIdx = static_cast<int>(i);
                outIsIn  = true;
                return true;
            }
        }
    }
    return false;
}

int TransformOverlayWidget::hitTestMotionWaypoint(const QPointF& widgetPos) const
{
    if (!m_motionX || !m_motionY) return -1;
    const size_t nx = m_motionX->keyframeCount();
    const size_t ny = m_motionY->keyframeCount();
    if (nx < 2 || nx != ny) return -1;
    QRectF fr = computeFrameRect();
    constexpr double kHitRadius = 8.0;
    for (size_t i = 0; i < nx; ++i) {
        const auto& kx = m_motionX->keyframe(i);
        const auto& ky = m_motionY->keyframe(i);
        QPointF pt = refToWidget(kx.value, ky.value, fr);
        if (QLineF(widgetPos, pt).length() <= kHitRadius)
            return static_cast<int>(i);
    }
    return -1;
}

void TransformOverlayWidget::applyCursor(Qt::CursorShape shape)
{
    applyCursor(QCursor(shape));
}

void TransformOverlayWidget::applyCursor(const QCursor& cursor)
{
    // Per-window setCursor on a createWindowContainer'd Vulkan QWindow
    // races with Qt's own cursor handling on every mouse-move, making the
    // cursor flicker.  The application OVERRIDE cursor is authoritative —
    // Qt forces it regardless of native-window quirks, with no race.
    //
    // "Plain arrow" means: no special affordance → drop any override.
    // Anything else (scale / rotate / zoom / pen) → install or swap a
    // single override (stack depth stays 0 or 1, so it's always balanced).
    const bool wantSpecial =
        !(cursor.shape() == Qt::ArrowCursor && cursor.pixmap().isNull());

    if (wantSpecial) {
        if (m_haveCursorOverride)
            QGuiApplication::changeOverrideCursor(cursor);
        else {
            QGuiApplication::setOverrideCursor(cursor);
            m_haveCursorOverride = true;
        }
    } else {
        clearCursorOverride();
    }
}

void TransformOverlayWidget::clearCursorOverride()
{
    if (m_haveCursorOverride) {
        QGuiApplication::restoreOverrideCursor();
        m_haveCursorOverride = false;
    }
}

QCursor TransformOverlayWidget::rotateCursor()
{
    // Premiere-style curved arrow rotation cursor.
    // 32×32 pixmap, arc with a single filled arrowhead at one end.
    constexpr int SZ = 32;
    constexpr float CX = SZ / 2.0f;
    constexpr float CY = SZ / 2.0f;
    constexpr float R  = 7.0f;  // arc radius (shrunk 30% from 10)

    QPixmap pix(SZ, SZ);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Arc spanning ~270° (from 45° to 315°)
    QRectF arcRect(CX - R, CY - R, R * 2, R * 2);
    int startAngle16 = 45 * 16;
    int spanAngle16  = 270 * 16;

    // Arrowhead at the start of the arc (45° position) — single filled triangle
    constexpr float PI = 3.14159265f;
    float a = 45.0f * PI / 180.0f;
    float ex = CX + R * std::cos(a);
    float ey = CY - R * std::sin(a);
    // Tangent direction (CCW arc at this point)
    float tx =  std::sin(a);
    float ty =  std::cos(a);
    float arrLen = 5.0f;
    float arrHalf = 2.8f; // half-width of arrowhead base
    // Tip of arrowhead along tangent
    QPointF tip(ex + arrLen * tx, ey - arrLen * ty);
    // Perpendicular to tangent
    float px2 = -ty, py2 = -tx; // rotated 90° to get perpendicular in Qt coords
    QPointF base1(ex + arrHalf * px2, ey - arrHalf * py2);
    QPointF base2(ex - arrHalf * px2, ey + arrHalf * py2);

    // Lambda to draw the full shape with a given pen + brush style
    auto drawShape = [&](const QPen& pen, const QBrush& brush) {
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(arcRect, startAngle16, spanAngle16);

        // Filled arrowhead triangle
        p.setBrush(brush);
        QPolygonF arrow;
        arrow << tip << base1 << base2;
        p.drawPolygon(arrow);
    };

    // Black outline pass
    drawShape(QPen(Qt::black, 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin),
              QBrush(Qt::black));

    // White inner pass
    drawShape(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin),
              QBrush(Qt::white));

    p.end();
    return QCursor(pix, SZ / 2, SZ / 2);
}

QCursor TransformOverlayWidget::penCursor()
{
    // A plain crosshair makes the exact point placement unambiguous. The old
    // diagonal nib obscured its hot spot during tight Bezier work.
    return QCursor(Qt::CrossCursor);
}

QCursor TransformOverlayWidget::zoomCursor()
{
    // Magnifying glass zoom cursor matching Premiere Pro's zoom tool.
    // 32×32 pixmap, hot-spot at the lens center (12, 12).
    constexpr int SZ = 32;
    QPixmap pix(SZ, SZ);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Lens circle: center (12, 12), radius 7
    constexpr float CX = 12.0f;
    constexpr float CY = 12.0f;
    constexpr float R  = 7.0f;

    // Handle: line from (18, 18) to (28, 28)
    QPointF handleStart(CX + R * 0.85f, CY + R * 0.85f);
    QPointF handleEnd(28.0f, 28.0f);

    // Black outline
    p.setPen(QPen(Qt::black, 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(CX, CY), R, R);
    p.drawLine(handleStart, handleEnd);

    // White inner
    p.setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(CX, CY), R, R);
    p.drawLine(handleStart, handleEnd);

    // Plus cross in center of lens
    constexpr float CROSS = 4.0f;
    p.setPen(QPen(Qt::black, 2.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(CX - CROSS, CY), QPointF(CX + CROSS, CY));
    p.drawLine(QPointF(CX, CY - CROSS), QPointF(CX, CY + CROSS));
    p.setPen(QPen(Qt::white, 1.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(CX - CROSS, CY), QPointF(CX + CROSS, CY));
    p.drawLine(QPointF(CX, CY - CROSS), QPointF(CX, CY + CROSS));

    p.end();
    return QCursor(pix, static_cast<int>(CX), static_cast<int>(CY));
}

void TransformOverlayWidget::setSafeAreasVisible(bool visible)
{
    if (m_showSafeAreas == visible) return;
    m_showSafeAreas = visible;
    update();
}

void TransformOverlayWidget::setGridVisible(bool visible)
{
    if (m_showGrid == visible) return;
    m_showGrid = visible;
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Frame rect computation.
//  Primary: use the GPU's normalized VkViewport rect (accounts for
//  swapchain/widget size mismatch).  Fallback: widget-based calc.
// ═════════════════════════════════════════════════════════════════════════════

QRectF TransformOverlayWidget::computeFrameRect() const
{
    if (!m_vulkanVp) return {};

    // The composite is drawn into the swapchain at the SURFACE (HWND)
    // size — and on this code path the HWND can be positioned and sized
    // differently from the QWidget container.  (Observed: HWND extends
    // 39 px UPWARD past the QWidget; that part is hidden behind the
    // panel header but the composite's gpuFrameRect is still relative
    // to the full HWND.)  Query the HWND's actual screen rect and shift
    // the gpuFrameRect mapping by (HWND.topLeft − widget.topLeft) so
    // the box lands in the overlay's coordinate space and aligns with
    // the actually-displayed composite.
    QSize surface = m_vulkanVp->nativeSurfaceSize();
    double surfW = static_cast<double>(surface.width());
    double surfH = static_cast<double>(surface.height());
    if (surfW <= 0 || surfH <= 0) return {};

    QPoint widgetGlobal = m_vulkanVp->mapToGlobal(QPoint(0, 0));
    double hwndOffX = 0.0;
    double hwndOffY = 0.0;
#ifdef _WIN32
    if (auto* nw = m_vulkanVp->nativeWindow()) {
        HWND hwnd = reinterpret_cast<HWND>(nw->winId());
        if (hwnd) {
            RECT wr;
            if (GetWindowRect(hwnd, &wr)) {
                hwndOffX = static_cast<double>(wr.left - widgetGlobal.x());
                hwndOffY = static_cast<double>(wr.top  - widgetGlobal.y());
            }
        }
    }
#endif

    // Primary: GPU normalized rect (0-1 range from VkViewport / swapchain).
    QRectF norm = m_vulkanVp->gpuFrameRect();
    if (!norm.isEmpty()) {
        double ox = static_cast<double>(m_vpOffset.x());
        double oy = static_cast<double>(m_vpOffset.y());
        return QRectF(norm.x()     * surfW + hwndOffX - ox,
                      norm.y()     * surfH + hwndOffY - oy,
                      norm.width() * surfW,
                      norm.height()* surfH);
    }

    // Fallback (before first present): surface-based aspect fit.
    double srcW = static_cast<double>(m_vulkanVp->srcWidth());
    double srcH = static_cast<double>(m_vulkanVp->srcHeight());
    if (srcW <= 0.0 || srcH <= 0.0) {
        // OverlayController supplies the sequence dimensions independently
        // of frame presentation. This keeps mask geometry interactive during
        // the first/late frame instead of collapsing all hit targets to zero.
        srcW = static_cast<double>(m_seqW);
        srcH = static_cast<double>(m_seqH);
    }
    if (srcW <= 0 || srcH <= 0) return {};

    double imgAspect = srcW / srcH;
    double winAspect = surfW / surfH;
    double baseW, baseH, baseX, baseY;
    if (winAspect > imgAspect) {
        baseH = surfH; baseW = surfH * imgAspect;
        baseX = (surfW - baseW) * 0.5; baseY = 0.0;
    } else {
        baseW = surfW; baseH = surfW / imgAspect;
        baseX = 0.0; baseY = (surfH - baseH) * 0.5;
    }

    double ox = static_cast<double>(m_vpOffset.x());
    double oy = static_cast<double>(m_vpOffset.y());
    return QRectF(baseX + hwndOffX - ox,
                  baseY + hwndOffY - oy,
                  baseW, baseH);
}

QPointF TransformOverlayWidget::frameToWidget(const QPointF& fp) const
{
    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) return QPointF(-1, -1);

    float srcW = static_cast<float>(m_vulkanVp->srcWidth());
    float srcH = static_cast<float>(m_vulkanVp->srcHeight());
    if (srcW <= 0.0f || srcH <= 0.0f) {
        srcW = static_cast<float>(m_seqW);
        srcH = static_cast<float>(m_seqH);
    }
    if (srcW <= 0.0f || srcH <= 0.0f) return QPointF(-1, -1);

    double wx = fr.x() + (fp.x() / srcW) * fr.width();
    double wy = fr.y() + (fp.y() / srcH) * fr.height();
    return QPointF(wx, wy);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Overlay geometry helpers (same logic as Viewport)
// ═════════════════════════════════════════════════════════════════════════════

void TransformOverlayWidget::computeOverlayCorners(QPointF corners[4]) const
{
    computeOverlayCornersFor(m_overlay, corners);
}

TransformOverlayInfo TransformOverlayWidget::inlineTextLayoutOverlay() const
{
    TransformOverlayInfo layout = m_savedOverlayBeforeEdit;
    if (layout.useContentRect && layout.useTextLayoutRect
        && layout.textLayoutR > layout.textLayoutL
        && layout.textLayoutB > layout.textLayoutT) {
        layout.contentL = layout.textLayoutL;
        layout.contentT = layout.textLayoutT;
        layout.contentR = layout.textLayoutR;
        layout.contentB = layout.textLayoutB;
    }
    return layout;
}

void TransformOverlayWidget::computeOverlayCornersFor(
    const TransformOverlayInfo& ov, QPointF corners[4]) const
{
    constexpr float REF_W = 1920.0f;
    constexpr float REF_H = 1080.0f;

    // ── Per-layer content-rect mode ─────────────────────────────────────
    if (ov.useContentRect && m_vulkanVp) {
        QRectF fr = computeFrameRect();
        // Use the canvas dimensions that the content bounds were computed for
        // (outputWidth × outputHeight), NOT srcWidth × srcHeight which can
        // differ (e.g. half-res preview).
        float canvasW = ov.contentCanvasW;
        float canvasH = ov.contentCanvasH;
        if (canvasW > 0.0f && canvasH > 0.0f && !fr.isEmpty()) {
            float cx = canvasW * 0.5f;
            float cy = canvasH * 0.5f;
            float radians = ov.rotation * 3.14159265358979f / 180.0f;
            float cosR = std::cos(radians);
            float sinR = std::sin(radians);

            // fwd: apply the same QPainter transform as renderGraphicClip,
            // then the clip-level compositor transform, then map to widget space.
            float clipRadians = ov.clipRotation * 3.14159265358979f / 180.0f;
            float clipCosR = std::cos(clipRadians);
            float clipSinR = std::sin(clipRadians);
            // Clip-level position is in REF-1920 px; scale into canvas space.
            float clipPxX = ov.clipPosX * (canvasW / 1920.0f);
            float clipPxY = ov.clipPosY * (canvasH / 1080.0f);
            float clipAnchorPxX = ov.clipAnchorX * (canvasW / 1920.0f);
            float clipAnchorPxY = ov.clipAnchorY * (canvasH / 1080.0f);

            auto fwd = [&](float x, float y) -> QPointF {
                // Layer transform (inner): same as renderGraphicClip QPainter.
                // Pivot rotation/scale around the anchor (cx+anchor) and
                // re-add the anchor + layer position afterwards. Defaults
                // (anchor=0) reduce to the legacy un-anchored form.
                const float lax = ov.anchorX;
                const float lay = ov.anchorY;
                float dx = ov.scaleX * (x - cx - lax);
                float dy = ov.scaleY * (y - cy - lay);
                float ox = dx * cosR - dy * sinR + cx + lax + ov.posX;
                float oy = dx * sinR + dy * cosR + cy + lay + ov.posY;

                // Clip-level transform (outer): compositor blitLayerWithTransform
                float rx = (ox - cx - clipAnchorPxX) * ov.clipScaleX;
                float ry = (oy - cy - clipAnchorPxY) * ov.clipScaleY;
                float fx = rx * clipCosR - ry * clipSinR
                         + cx + clipPxX + clipAnchorPxX;
                float fy = rx * clipSinR + ry * clipCosR
                         + cy + clipPxY + clipAnchorPxY;

                // Map using canvas dimensions (not srcWidth) so coordinates
                // in 1920x1080 space map correctly even if displayed at 960x540.
                double wx = fr.x() + (static_cast<double>(fx) / canvasW) * fr.width();
                double wy = fr.y() + (static_cast<double>(fy) / canvasH) * fr.height();
                return QPointF(wx, wy);
            };

            corners[0] = fwd(ov.contentL, ov.contentT);
            corners[1] = fwd(ov.contentR, ov.contentT);
            corners[2] = fwd(ov.contentR, ov.contentB);
            corners[3] = fwd(ov.contentL, ov.contentB);
            return;
        }
    }

    if (!m_vulkanVp || ov.srcW == 0 || ov.srcH == 0) {
        for (int i = 0; i < 4; ++i) corners[i] = QPointF(0, 0);
        return;
    }

    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) {
        for (int i = 0; i < 4; ++i) corners[i] = QPointF(0, 0);
        return;
    }

    float outW = static_cast<float>(m_vulkanVp->srcWidth());
    float outH = static_cast<float>(m_vulkanVp->srcHeight());
    if (outW <= 0.0f || outH <= 0.0f) {
        outW = static_cast<float>(m_seqW);
        outH = static_cast<float>(m_seqH);
    }
    if (outW <= 0.0f || outH <= 0.0f) {
        for (int i = 0; i < 4; ++i) corners[i] = QPointF(0, 0);
        return;
    }

    // Scale positions from reference (1920×1080) to output.
    float posXPx    = ov.posX    * (outW / REF_W);
    float posYPx    = ov.posY    * (outH / REF_H);
    float anchorXPx = ov.anchorX * (outW / REF_W);
    float anchorYPx = ov.anchorY * (outH / REF_H);

    float cx = outW * 0.5f;
    float cy = outH * 0.5f;

    float fittedW, fittedH, baseOffX, baseOffY;

    if (ov.directSize) {
        // directSize: srcW/srcH are pixel dims in reference space (e.g., text bounding box).
        // Scale from reference to output without fill-scale.
        fittedW = static_cast<float>(ov.srcW) * (outW / REF_W);
        fittedH = static_cast<float>(ov.srcH) * (outH / REF_H);
        baseOffX = (outW - fittedW) * 0.5f;
        baseOffY = (outH - fittedH) * 0.5f;
    } else {
        float srcW = static_cast<float>(ov.srcW);
        float srcH = static_cast<float>(ov.srcH);
        float scaleToFitW = outW / srcW;
        float scaleToFitH = outH / srcH;
        float fitScale = ov.containFit
            ? std::min(scaleToFitW, scaleToFitH)
            : std::max(scaleToFitW, scaleToFitH);

        fittedW = static_cast<float>(ov.srcW) * fitScale;
        fittedH = static_cast<float>(ov.srcH) * fitScale;
        baseOffX = (outW - fittedW) * 0.5f;
        baseOffY = (outH - fittedH) * 0.5f;
    }

    float radians = ov.rotation * 3.14159265358979f / 180.0f;
    float cosR = std::cos(radians);
    float sinR = std::sin(radians);

    auto forwardXY = [&](float fitX, float fitY) -> QPointF {
        // Pivot rotation/scale around the anchor (cx + anchorPx); add
        // anchor + posPx back afterwards. anchor=(0,0) → identical to
        // legacy un-anchored math.
        float rx = (fitX - cx + baseOffX - anchorXPx) * ov.scaleX;
        float ry = (fitY - cy + baseOffY - anchorYPx) * ov.scaleY;
        float ox = rx * cosR - ry * sinR + cx + posXPx + anchorXPx;
        float oy = rx * sinR + ry * cosR + cy + posYPx + anchorYPx;
        return frameToWidget(QPointF(ox, oy));
    };

    corners[0] = forwardXY(0.0f,    0.0f);
    corners[1] = forwardXY(fittedW, 0.0f);
    corners[2] = forwardXY(fittedW, fittedH);
    corners[3] = forwardXY(0.0f,    fittedH);
}

int TransformOverlayWidget::hitTestHandle(const QPointF& widgetPos) const
{
    constexpr double HANDLE_RADIUS = 18.0;  // generous, easy to hit
    QPointF corners[4];
    computeOverlayCorners(corners);

    // Centroid — same robustness pattern as hitTestRotate. For a small
    // item the 4 corner-handle circles overlap the body's centre; without
    // this check, clicking the centre matches a corner handle and starts
    // a SCALE drag instead of a body MOVE, which the user perceives as a
    // dead zone in the middle.
    const QPointF center(
        (corners[0].x() + corners[1].x() + corners[2].x() + corners[3].x()) * 0.25,
        (corners[0].y() + corners[1].y() + corners[2].y() + corners[3].y()) * 0.25);

    for (int i = 0; i < 4; ++i) {
        double dx = widgetPos.x() - corners[i].x();
        double dy = widgetPos.y() - corners[i].y();
        if (dx * dx + dy * dy > HANDLE_RADIUS * HANDLE_RADIUS) continue;

        // Must be on the corner's OUTSIDE half (radially beyond the
        // corner from the centroid), so the centre/body never qualifies.
        const double pcDx = widgetPos.x() - center.x();
        const double pcDy = widgetPos.y() - center.y();
        const double kcDx = corners[i].x() - center.x();
        const double kcDy = corners[i].y() - center.y();
        if ((pcDx * pcDx + pcDy * pcDy) < (kcDx * kcDx + kcDy * kcDy)) continue;

        return i;
    }
    return -1;
}

int TransformOverlayWidget::hitTestRotate(const QPointF& widgetPos) const
{
    constexpr double INNER_RADIUS = 18.0;  // == handle radius: zones abut, no overlap
    constexpr double OUTER_RADIUS = 50.0;  // rotation zone extends this far from corner
    QPointF corners[4];
    computeOverlayCorners(corners);

    // Box centroid. The rotation zone is the OUTSIDE of each corner, so a
    // valid point must sit farther from the centroid than the corner does
    // (radially beyond it). This is robust for small items where the fixed
    // 50px corner rings would otherwise blanket the whole body — and where
    // the winding-based hitTestBody() is numerically unreliable on a tiny
    // or near-degenerate quad. The centroid itself can never qualify, so
    // the rotate cursor no longer appears over a small item's centre.
    const QPointF center(
        (corners[0].x() + corners[1].x() + corners[2].x() + corners[3].x()) * 0.25,
        (corners[0].y() + corners[1].y() + corners[2].y() + corners[3].y()) * 0.25);

    for (int i = 0; i < 4; ++i) {
        double dx = widgetPos.x() - corners[i].x();
        double dy = widgetPos.y() - corners[i].y();
        double distSq = dx * dx + dy * dy;
        if (distSq > INNER_RADIUS * INNER_RADIUS &&
            distSq <= OUTER_RADIUS * OUTER_RADIUS)
        {
            const double pcDx = widgetPos.x() - center.x();
            const double pcDy = widgetPos.y() - center.y();
            const double kcDx = corners[i].x() - center.x();
            const double kcDy = corners[i].y() - center.y();
            const bool beyondCorner =
                (pcDx * pcDx + pcDy * pcDy) >= (kcDx * kcDx + kcDy * kcDy);

            // Outside the body AND radially beyond the corner.
            if (beyondCorner && !hitTestBody(widgetPos))
                return i;
        }
    }
    return -1;
}

bool TransformOverlayWidget::hitTestBody(const QPointF& widgetPos) const
{
    QPointF corners[4];
    computeOverlayCorners(corners);

    auto cross = [](QPointF a, QPointF b, QPointF p) {
        return (b.x() - a.x()) * (p.y() - a.y()) -
               (b.y() - a.y()) * (p.x() - a.x());
    };

    double c0 = cross(corners[0], corners[1], widgetPos);
    double c1 = cross(corners[1], corners[2], widgetPos);
    double c2 = cross(corners[2], corners[3], widgetPos);
    double c3 = cross(corners[3], corners[0], widgetPos);

    bool allPos = (c0 >= 0) && (c1 >= 0) && (c2 >= 0) && (c3 >= 0);
    bool allNeg = (c0 <= 0) && (c1 <= 0) && (c2 <= 0) && (c3 <= 0);
    return allPos || allNeg;
}

bool TransformOverlayWidget::hitTestBodyMargin(const QPointF& widgetPos,
                                               double marginPx) const
{
    QPointF c[4];
    computeOverlayCorners(c);

    // Push each corner outward along the box's own edge axes, so the inflated
    // quad keeps the box's rotation/shear. U = TL→TR (the box's horizontal
    // axis), V = TL→BL (vertical).
    const QPointF U = c[1] - c[0];
    const QPointF V = c[3] - c[0];
    const double lenU = std::hypot(U.x(), U.y());
    const double lenV = std::hypot(V.x(), V.y());
    if (lenU < 1e-6 || lenV < 1e-6) return false;

    // A short axis additionally gets padded out toward a minimum grab
    // thickness. A thin text box can be only ~40 px tall on screen — and once
    // rotated it's nearly impossible to click along, because a few px of
    // vertical aim error drops you off the tilted edge. Guaranteeing a minimum
    // thickness on the short axis makes thin/rotated text a comfortable target
    // while leaving already-large boxes with just the base margin.
    constexpr double kMinGrabThicknessPx = 84.0;
    const double marU = marginPx + std::max(0.0, (kMinGrabThicknessPx - lenU) * 0.5);
    const double marV = marginPx + std::max(0.0, (kMinGrabThicknessPx - lenV) * 0.5);
    const QPointF mu = U / lenU * marU;   // along the box width
    const QPointF mv = V / lenV * marV;   // along the box height

    QPointF e[4];
    e[0] = c[0] - mu - mv;   // TL out (left + up)
    e[1] = c[1] + mu - mv;   // TR out (right + up)
    e[2] = c[2] + mu + mv;   // BR out (right + down)
    e[3] = c[3] - mu + mv;   // BL out (left + down)

    auto cross = [](QPointF a, QPointF b, QPointF p) {
        return (b.x() - a.x()) * (p.y() - a.y()) -
               (b.y() - a.y()) * (p.x() - a.x());
    };
    const double k0 = cross(e[0], e[1], widgetPos);
    const double k1 = cross(e[1], e[2], widgetPos);
    const double k2 = cross(e[2], e[3], widgetPos);
    const double k3 = cross(e[3], e[0], widgetPos);
    const bool allPos = (k0 >= 0) && (k1 >= 0) && (k2 >= 0) && (k3 >= 0);
    const bool allNeg = (k0 <= 0) && (k1 <= 0) && (k2 <= 0) && (k3 <= 0);
    return allPos || allNeg;
}

bool TransformOverlayWidget::cropHandlePositions(QPointF handles[4]) const
{
    if (!m_overlay.cropEnabled) return false;
    QPointF c[4];
    computeOverlayCorners(c);   // TL, TR, BR, BL of the uncropped box
    // The box is an affine parallelogram, so any inner point is
    // TL + a*(TR-TL) + b*(BL-TL) with a,b in [0,1] — correct even rotated.
    const QPointF U = c[1] - c[0];   // left→right axis
    const QPointF V = c[3] - c[0];   // top→bottom axis
    if (std::abs(U.x()) + std::abs(U.y()) < 1e-3 ||
        std::abs(V.x()) + std::abs(V.y()) < 1e-3) return false;

    const float fl = m_overlay.cropL / 100.0f;
    const float fr = m_overlay.cropR / 100.0f;
    const float ft = m_overlay.cropT / 100.0f;
    const float fb = m_overlay.cropB / 100.0f;
    const float midX = (fl + (1.0f - fr)) * 0.5f;
    const float midY = (ft + (1.0f - fb)) * 0.5f;
    auto pt = [&](float a, float b) -> QPointF {
        return QPointF(c[0].x() + a * U.x() + b * V.x(),
                       c[0].y() + a * U.y() + b * V.y());
    };
    handles[0] = pt(fl,        midY);      // Left edge mid
    handles[1] = pt(1.0f - fr, midY);      // Right edge mid
    handles[2] = pt(midX,      ft);        // Top edge mid
    handles[3] = pt(midX,      1.0f - fb); // Bottom edge mid
    return true;
}

int TransformOverlayWidget::hitTestCropHandle(const QPointF& widgetPos) const
{
    QPointF h[4];
    if (!cropHandlePositions(h)) return -1;
    constexpr double R = 14.0;  // generous grab radius
    for (int i = 0; i < 4; ++i) {
        const double dx = widgetPos.x() - h[i].x();
        const double dy = widgetPos.y() - h[i].y();
        if (dx * dx + dy * dy <= R * R) return i;
    }
    return -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Paint
// ═════════════════════════════════════════════════════════════════════════════


} // namespace rt
