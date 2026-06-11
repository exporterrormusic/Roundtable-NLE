/*
 * Viewport.cpp — GPU viewport widget implementation.
 * Step 11 (basic) / Step 15 (used by monitors)
 *
 * Current implementation uses QPainter software blitting.
 * Vulkan backend will be added in a future step.
 */

#include "viewport/Viewport.h"
#include "viewport/OverlayMath.h"
#include "Theme.h"

#include "cache/FrameCache.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QPixmap>
#include <QPolygonF>
#include <QCursor>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════════

Viewport::Viewport(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(160, 90);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);

    // Dark background
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Theme::colors().surface0);
    setPalette(pal);
}

Viewport::~Viewport() = default;

// ═════════════════════════════════════════════════════════════════════════════
//  Frame display
// ═════════════════════════════════════════════════════════════════════════════

void Viewport::displayFrame(const CachedFrame& frame)
{
    if (frame.pixels.empty() || frame.width == 0 || frame.height == 0)
    {
        clearFrame();
        return;
    }

    displayRaw(frame.pixels.data(), frame.width, frame.height, frame.stride);
}

void Viewport::displayFrame(std::shared_ptr<CachedFrame> frame)
{
    if (!frame || !frame->ensurePixels() || frame->width == 0 || frame->height == 0)
    {
        clearFrame();
        return;
    }

    // Zero-copy: wrap the CachedFrame's pixel data directly in a QImage.
    // The shared_ptr m_frameRef keeps the data alive until the next frame.
    uint32_t w = frame->width;
    uint32_t h = frame->height;
    uint32_t stride = frame->stride > 0 ? frame->stride : w * 4;

    m_frameRef = std::move(frame);
    m_image = QImage(m_frameRef->pixels.data(), static_cast<int>(w),
                     static_cast<int>(h), static_cast<int>(stride),
                     QImage::Format_ARGB32);

    m_frameWidth  = w;
    m_frameHeight = h;
    m_hasFrame    = true;

    recalcLayout();
    update();
}

void Viewport::displayRaw(const uint8_t* bgraData, uint32_t w, uint32_t h, uint32_t stride)
{
    if (!bgraData || w == 0 || h == 0)
    {
        clearFrame();
        return;
    }

    uint32_t rowBytes = w * 4;
    uint32_t effectiveStride = stride > 0 ? stride : rowBytes;

    // Allocate owned buffer (reuses capacity when size is unchanged).
    // Qt's ARGB32 is stored as BGRA in memory on little-endian, so no conversion needed.
    if (m_rawBuffer.size() != h * rowBytes)
        m_rawBuffer.assign(h * rowBytes, 0);

    // Single contiguous copy when stride matches; otherwise copy row-by-row.
    if (effectiveStride == rowBytes)
    {
        std::memcpy(m_rawBuffer.data(), bgraData, h * rowBytes);
    }
    else
    {
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t* srcRow  = bgraData + y * effectiveStride;
            uint8_t*      dstRow  = m_rawBuffer.data() + y * rowBytes;
            std::memcpy(dstRow, srcRow, rowBytes);
        }
    }

    m_image = QImage(m_rawBuffer.data(), static_cast<int>(w), static_cast<int>(h),
                     static_cast<int>(rowBytes), QImage::Format_ARGB32);

    m_frameWidth  = w;
    m_frameHeight = h;
    m_hasFrame    = true;

    recalcLayout();
    update();
}

void Viewport::clearFrame()
{
    m_image      = QImage();
    m_frameRef.reset();
    m_hasFrame   = false;
    m_frameWidth = 0;
    m_frameHeight = 0;
    m_drawRect   = QRectF();
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Fit mode
// ═════════════════════════════════════════════════════════════════════════════

void Viewport::setFitMode(ViewportFitMode mode)
{
    if (m_fitMode == mode) return;
    m_fitMode = mode;
    recalcLayout();
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Overlay
// ═════════════════════════════════════════════════════════════════════════════

void Viewport::setOverlayText(const QString& text)
{
    m_overlayText = text;
    update();
}

void Viewport::setSafeAreasVisible(bool visible)
{
    m_showSafeAreas = visible;
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Viewport zoom & pan
// ═════════════════════════════════════════════════════════════════════════════

void Viewport::resetZoomPan()
{
    m_viewZoom = 1.0f;
    m_viewPanX = 0.0f;
    m_viewPanY = 0.0f;
    recalcLayout();
    update();
    emit viewZoomChanged(m_viewZoom);
}

void Viewport::setViewZoom(float zoom)
{
    m_viewZoom = std::clamp(zoom, 0.1f, 16.0f);
    m_viewPanX = 0.0f;
    m_viewPanY = 0.0f;
    recalcLayout();
    update();
    emit viewZoomChanged(m_viewZoom);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Transform overlay
// ═════════════════════════════════════════════════════════════════════════════

void Viewport::setTransformOverlay(const TransformOverlayInfo& info)
{
    m_transformOverlay = info;
    m_cornerCacheDirty = true;
    update();
}

void Viewport::clearTransformOverlay()
{
    m_transformOverlay.visible = false;
    m_dragMode = DragMode::None;
    m_cornerCacheDirty = true;
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Coordinate mapping
// ═════════════════════════════════════════════════════════════════════════════

QPointF Viewport::widgetToFrame(const QPointF& widgetPos) const
{
    if (!m_hasFrame || m_drawRect.isEmpty())
        return QPointF(-1.0, -1.0);

    // m_drawRect already includes zoom/pan, so mapping is direct
    double fx = (widgetPos.x() - m_drawRect.x()) / m_drawRect.width()  * m_frameWidth;
    double fy = (widgetPos.y() - m_drawRect.y()) / m_drawRect.height() * m_frameHeight;

    // When zoomed in, allow coords outside the visible area but within the frame
    if (fx < 0 || fy < 0 || fx >= m_frameWidth || fy >= m_frameHeight)
        return QPointF(-1.0, -1.0);

    return QPointF(fx, fy);
}

QPointF Viewport::frameToWidget(const QPointF& framePos) const
{
    if (!m_hasFrame || m_drawRect.isEmpty())
        return QPointF(-1.0, -1.0);

    double wx = m_drawRect.x() + (framePos.x() / m_frameWidth)  * m_drawRect.width();
    double wy = m_drawRect.y() + (framePos.y() / m_frameHeight) * m_drawRect.height();

    return QPointF(wx, wy);
}

QRectF Viewport::frameRect() const
{
    return m_drawRect;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Size hints
// ═════════════════════════════════════════════════════════════════════════════

QSize Viewport::sizeHint() const
{
    return QSize(640, 360);
}

QSize Viewport::minimumSizeHint() const
{
    return QSize(160, 90);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Painting
// ═════════════════════════════════════════════════════════════════════════════

void Viewport::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Background: dark grey outside the frame box, BLACK inside the
    // frame box — matches Premiere Pro's monitor styling where the
    // letterbox/empty area is grey but the video frame itself is black
    // (visible behind any transparent areas of the rendered image).
    painter.fillRect(rect(), Theme::colors().surface0);
    if (!m_drawRect.isEmpty())
    {
        painter.fillRect(m_drawRect, Qt::black);
        if (m_hasFrame && !m_image.isNull())
            painter.drawImage(m_drawRect, m_image);
    }

    // Safe areas
    if (m_showSafeAreas && !m_drawRect.isEmpty())
    {
        QColor safeColor = Theme::colors().textPrimary; safeColor.setAlpha(80);
        QPen safePen(safeColor, 1, Qt::DashLine);
        painter.setPen(safePen);
        painter.setBrush(Qt::NoBrush);

        // Action safe (90%)
        QRectF actionSafe = m_drawRect;
        actionSafe.adjust(m_drawRect.width() * 0.05, m_drawRect.height() * 0.05,
                         -m_drawRect.width() * 0.05, -m_drawRect.height() * 0.05);
        painter.drawRect(actionSafe);

        // Title safe (80%)
        QRectF titleSafe = m_drawRect;
        titleSafe.adjust(m_drawRect.width() * 0.1, m_drawRect.height() * 0.1,
                        -m_drawRect.width() * 0.1, -m_drawRect.height() * 0.1);
        painter.drawRect(titleSafe);
    }

    // Transform overlay (bounding box + handles for selected clip)
    if (m_transformOverlay.visible && m_hasFrame && !m_drawRect.isEmpty())
    {
        drawTransformOverlay(painter);
    }

    // Overlay text
    if (!m_overlayText.isEmpty())
    {
        painter.setPen(Theme::colors().textPrimary);
        QFont font = painter.font();
        font.setPixelSize(13);
        font.setFamily(QStringLiteral("Consolas"));
        painter.setFont(font);
        painter.drawText(10, 20, m_overlayText);
    }

    --s_paintDepth;
}

void Viewport::resizeEvent(QResizeEvent* event)
{
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;
    recalcLayout();
    QWidget::resizeEvent(event);
    s_inResize = false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Mouse events
// ═════════════════════════════════════════════════════════════════════════════

void Viewport::mousePressEvent(QMouseEvent* event)
{    // ── Middle button → pan ────────────────────────────────────────────
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_panStartPos = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    // ── Transform overlay interaction (left-click only) ─────────────────
    if (event->button() == Qt::LeftButton && m_transformOverlay.visible)
    {
        QPointF wPos = event->position();

        // Check corner handles first (higher priority than body)
        int handle = hitTestHandle(wPos);
        if (handle >= 0) {
            m_dragMode = DragMode::ScaleCorner;
            m_dragHandle = handle;
            m_dragStartWidget = wPos;
            m_dragStartPosX = m_transformOverlay.posX;
            m_dragStartPosY = m_transformOverlay.posY;
            m_dragStartScX  = m_transformOverlay.scaleX;
            m_dragStartScY  = m_transformOverlay.scaleY;
            m_dragStartRot  = m_transformOverlay.rotation;
            setCursor(Qt::SizeFDiagCursor);
            return;
        }

        // Rotation zone — just OUTSIDE a corner (Premiere-style)
        int rotHandle = hitTestRotate(wPos);
        if (rotHandle >= 0) {
            m_dragMode = DragMode::RotateCorner;
            m_dragHandle = rotHandle;
            m_dragStartWidget = wPos;
            m_dragStartPosX = m_transformOverlay.posX;
            m_dragStartPosY = m_transformOverlay.posY;
            m_dragStartScX  = m_transformOverlay.scaleX;
            m_dragStartScY  = m_transformOverlay.scaleY;
            m_dragStartRot  = m_transformOverlay.rotation;
            QPointF corners[4];
            computeOverlayCorners(corners);
            QPointF center = (corners[0] + corners[2]) * 0.5;
            m_dragStartAngle = static_cast<float>(
                std::atan2(wPos.y() - center.y(), wPos.x() - center.x())
                * 180.0 / 3.14159265358979);
            setCursor(rotateCursor());
            return;
        }

        // Check body (inside bounding box)
        if (hitTestBody(wPos)) {
            m_dragMode = DragMode::MoveBody;
            m_dragStartWidget = wPos;
            m_dragStartPosX = m_transformOverlay.posX;
            m_dragStartPosY = m_transformOverlay.posY;
            m_dragStartScX  = m_transformOverlay.scaleX;
            m_dragStartScY  = m_transformOverlay.scaleY;
            m_dragStartRot  = m_transformOverlay.rotation;
            setCursor(Qt::ArrowCursor);   // no special move cursor
            return;
        }
    }

    QPointF fp = widgetToFrame(event->position());
    if (fp.x() >= 0)
        emit frameClicked(fp, event->button());
}

void Viewport::mouseMoveEvent(QMouseEvent* event)
{
    QPointF wPos = event->position();

    // ── Pan drag ─────────────────────────────────────────────────────────
    if (m_panning) {
        float dx = static_cast<float>(wPos.x() - m_panStartPos.x());
        float dy = static_cast<float>(wPos.y() - m_panStartPos.y());
        m_viewPanX += dx;
        m_viewPanY += dy;
        m_panStartPos = wPos;
        recalcLayout();
        update();
        event->accept();
        return;
    }

    // ── Active drag ─────────────────────────────────────────────────────
    if (m_dragMode == DragMode::MoveBody && (event->buttons() & Qt::LeftButton))
    {
        // Convert widget-space delta to reference-space delta.
        // Widget draw-rect maps reference resolution to widget pixels.
        constexpr float REF_W = 1920.0f;
        constexpr float REF_H = 1080.0f;
        float pxPerRefX = static_cast<float>(m_drawRect.width())  / REF_W;
        float pxPerRefY = static_cast<float>(m_drawRect.height()) / REF_H;
        if (pxPerRefX < 0.001f || pxPerRefY < 0.001f) return;

        float dx = static_cast<float>(wPos.x() - m_dragStartWidget.x()) / pxPerRefX;
        float dy = static_cast<float>(wPos.y() - m_dragStartWidget.y()) / pxPerRefY;

        m_transformOverlay.posX = m_dragStartPosX + dx;
        m_transformOverlay.posY = m_dragStartPosY + dy;

        // Premiere-style Ctrl-snap: drag with Ctrl held magnetises the
        // overlay's AABB to the frame's edges and centre lines. Implemented
        // in widget pixels and converted back to ref-units so the snap
        // distance feels consistent across zoom levels.
        if (event->modifiers() & Qt::ControlModifier) {
            // Corner cache holds stale corners; force recompute so the snap
            // box reflects the just-applied posX/posY.
            m_cornerCacheDirty = true;
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
                static_cast<double>(m_drawRect.left()),
                static_cast<double>(m_drawRect.right()),
                static_cast<double>(m_drawRect.center().x())
            };
            const double tgtsY[3] = {
                static_cast<double>(m_drawRect.top()),
                static_cast<double>(m_drawRect.bottom()),
                static_cast<double>(m_drawRect.center().y())
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
                m_transformOverlay.posX += static_cast<float>(bestDx / pxPerRefX);
            if (bestDyAbs <= kSnapPx)
                m_transformOverlay.posY += static_cast<float>(bestDy / pxPerRefY);
            m_cornerCacheDirty = true;
        }

        emit transformPositionChanged(m_transformOverlay.posX, m_transformOverlay.posY);
        update();
        return;
    }

    if (m_dragMode == DragMode::ScaleCorner && (event->buttons() & Qt::LeftButton))
    {
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
                m_transformOverlay.scaleX = std::max(0.01f, m_dragStartScX * ratioX);
                m_transformOverlay.scaleY = std::max(0.01f, m_dragStartScY * ratioY);
            } else {
                // Uniform scale (default): both axes get the same value
                float ratio = curDist / startDist;
                float newScale = std::max(0.01f, m_dragStartScX * ratio);
                m_transformOverlay.scaleX = newScale;
                m_transformOverlay.scaleY = newScale;
            }
            emit transformScaleChanged(m_transformOverlay.scaleX, m_transformOverlay.scaleY);
            update();
        }
        return;
    }

    if (m_dragMode == DragMode::RotateCorner && (event->buttons() & Qt::LeftButton))
    {
        QPointF corners[4];
        computeOverlayCorners(corners);
        QPointF center = (corners[0] + corners[2]) * 0.5;

        float curAngle = static_cast<float>(
            std::atan2(wPos.y() - center.y(), wPos.x() - center.x())
            * 180.0 / 3.14159265358979);
        float deltaAngle = curAngle - m_dragStartAngle;
        while (deltaAngle >  180.0f) deltaAngle -= 360.0f;
        while (deltaAngle < -180.0f) deltaAngle += 360.0f;

        m_transformOverlay.rotation = m_dragStartRot + deltaAngle;
        emit transformRotationChanged(m_transformOverlay.rotation);
        update();
        return;
    }

    // ── Cursor hint when hovering ───────────────────────────────────────
    if (m_transformOverlay.visible && m_dragMode == DragMode::None)
    {
        if (hitTestHandle(wPos) >= 0)
            setCursor(Qt::SizeFDiagCursor);
        else if (hitTestRotate(wPos) >= 0)
            setCursor(rotateCursor());
        else
            setCursor(Qt::ArrowCursor);   // body = plain arrow (no hand)
    }

    QPointF fp = widgetToFrame(wPos);
    if (fp.x() >= 0)
    {
        emit frameHovered(fp);
        if (event->buttons() != Qt::NoButton)
            emit frameDragged(fp, event->buttons());
    }
}

void Viewport::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_panning) {
        m_panning = false;
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    if (m_dragMode != DragMode::None) {
        // Capture final values before resetting drag state
        float oldPX = m_dragStartPosX, oldPY = m_dragStartPosY;
        float oldSX = m_dragStartScX,  oldSY = m_dragStartScY;
        float newPX = m_transformOverlay.posX, newPY = m_transformOverlay.posY;
        float newSX = m_transformOverlay.scaleX, newSY = m_transformOverlay.scaleY;
        float oldRot = m_dragStartRot;
        float newRot = m_transformOverlay.rotation;
        m_dragMode = DragMode::None;
        setCursor(Qt::ArrowCursor);
        emit transformDragFinished(oldPX, oldPY, oldSX, oldSY, oldRot,
                                   newPX, newPY, newSX, newSY, newRot);
    }
}

void Viewport::wheelEvent(QWheelEvent* event)
{
    float delta = static_cast<float>(event->angleDelta().y());
    if (delta == 0.0f) {
        event->ignore();
        return;
    }

    float factor = (delta > 0) ? 1.1f : (1.0f / 1.1f);
    float newZoom = std::clamp(m_viewZoom * factor, 0.1f, 20.0f);

    // Zoom toward mouse position — pan is relative to the base-rect center
    // (rendering: drawRect.x = cx - zw/2 + panX, where cx = baseRect.center)
    QPointF mousePos = event->position();
    float mx = static_cast<float>(mousePos.x()) - width()  * 0.5f;
    float my = static_cast<float>(mousePos.y()) - height() * 0.5f;

    float zoomRatio = newZoom / m_viewZoom;
    m_viewPanX += (1.0f - zoomRatio) * (mx - m_viewPanX);
    m_viewPanY += (1.0f - zoomRatio) * (my - m_viewPanY);

    m_viewZoom = newZoom;
    recalcLayout();
    update();
    emit viewZoomChanged(m_viewZoom);
    event->accept();
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Double-click middle button resets zoom/pan
    if (event->button() == Qt::MiddleButton) {
        resetZoomPan();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Layout calculation
// ═════════════════════════════════════════════════════════════════════════════

void Viewport::invalidateCornerCache() { m_cornerCacheDirty = true; }

const QPointF* Viewport::getCachedCorners() const
{
    if (m_cornerCacheDirty) {
        computeOverlayCorners(m_cachedCorners);
        m_cornerCacheDirty = false;
    }
    return m_cachedCorners;
}

void Viewport::recalcLayout()
{
    if (!m_hasFrame || m_frameWidth == 0 || m_frameHeight == 0)
    {
        m_drawRect = QRectF();
        return;
    }

    double ww = width();
    double wh = height();
    double fw = static_cast<double>(m_frameWidth);
    double fh = static_cast<double>(m_frameHeight);

    // Compute the base draw rect (before zoom/pan)
    QRectF baseRect;
    switch (m_fitMode)
    {
    case ViewportFitMode::Fit:
    {
        // 5% padding on EVERY side, not just the limiting one.  Earlier
        // version multiplied scale by 0.95 which only guaranteed 2.5%
        // margin on the dimension chosen by aspect-fit — the other
        // dimension fell to whatever the letterbox/pillarbox gave (0%
        // in the worst case).  Fix: shrink the available area on both
        // axes first, then aspect-fit into the smaller box.
        constexpr double kFitPadding = 0.90;  // 5% margin per side
        const double availW = ww * kFitPadding;
        const double availH = wh * kFitPadding;
        const double scaleX = availW / fw;
        const double scaleY = availH / fh;
        const double scale  = std::min(scaleX, scaleY);
        const double dw     = fw * scale;
        const double dh     = fh * scale;
        baseRect = QRectF((ww - dw) * 0.5, (wh - dh) * 0.5, dw, dh);
        break;
    }
    case ViewportFitMode::Fill:
    {
        double scaleX = ww / fw;
        double scaleY = wh / fh;
        double scale  = std::max(scaleX, scaleY);
        double dw     = fw * scale;
        double dh     = fh * scale;
        baseRect = QRectF((ww - dw) * 0.5, (wh - dh) * 0.5, dw, dh);
        break;
    }
    case ViewportFitMode::Actual:
    {
        baseRect = QRectF((ww - fw) * 0.5, (wh - fh) * 0.5, fw, fh);
        break;
    }
    }

    // Apply viewport zoom & pan.
    // Zoom scales the draw rect around the widget center, then pan shifts it.
    double zw = baseRect.width()  * m_viewZoom;
    double zh = baseRect.height() * m_viewZoom;
    double cx = baseRect.center().x();
    double cy = baseRect.center().y();
    m_drawRect = QRectF(cx - zw * 0.5 + m_viewPanX,
                        cy - zh * 0.5 + m_viewPanY,
                        zw, zh);
    m_cornerCacheDirty = true; // drawRect changed
}

// ═════════════════════════════════════════════════════════════════════════════
//  Transform overlay helpers
// ═════════════════════════════════════════════════════════════════════════════

void Viewport::computeOverlayCorners(QPointF corners[4]) const
{
    // Delegate to shared free function. The toWidget callback maps
    // canvas-space coordinates to widget-space coordinates.
    rt::computeOverlayCorners(
        m_transformOverlay,
        static_cast<float>(m_frameWidth),
        static_cast<float>(m_frameHeight),
        m_drawRect,
        [this](float x, float y) { return frameToWidget(QPointF(x, y)); },
        corners);
}

int Viewport::hitTestHandle(const QPointF& widgetPos) const
{
    const QPointF* corners = getCachedCorners();
    return rt::hitTestHandle(widgetPos, corners);
}

bool Viewport::hitTestBody(const QPointF& widgetPos) const
{
    const QPointF* corners = getCachedCorners();
    return rt::hitTestBody(widgetPos, corners);
}

int Viewport::hitTestRotate(const QPointF& widgetPos) const
{
    // Mirrors TransformOverlayWidget::hitTestRotate (GPU path): a ring
    // just OUTSIDE each corner handle is the Premiere-style rotation zone.
    constexpr double INNER_RADIUS = 18.0;  // == corner-handle hit radius (zones abut)
    constexpr double OUTER_RADIUS = 50.0;  // rotation zone reach from corner
    const QPointF* corners = getCachedCorners();
    // Centroid: a rotate point must be radially beyond the corner (outside
    // the box). This keeps the rotate zone off the centre/body of small
    // items, where the fixed 50px rings would otherwise cover everything
    // and the winding-based hitTestBody() is unreliable on a tiny quad.
    const QPointF center(
        (corners[0].x() + corners[1].x() + corners[2].x() + corners[3].x()) * 0.25,
        (corners[0].y() + corners[1].y() + corners[2].y() + corners[3].y()) * 0.25);
    for (int i = 0; i < 4; ++i) {
        double dx = widgetPos.x() - corners[i].x();
        double dy = widgetPos.y() - corners[i].y();
        double distSq = dx * dx + dy * dy;
        const double pcDx = widgetPos.x() - center.x();
        const double pcDy = widgetPos.y() - center.y();
        const double kcDx = corners[i].x() - center.x();
        const double kcDy = corners[i].y() - center.y();
        const bool beyondCorner =
            (pcDx * pcDx + pcDy * pcDy) >= (kcDx * kcDx + kcDy * kcDy);
        if (distSq > INNER_RADIUS * INNER_RADIUS &&
            distSq <= OUTER_RADIUS * OUTER_RADIUS &&
            beyondCorner &&
            !rt::hitTestBody(widgetPos, corners))   // must be outside the box
            return i;
    }
    return -1;
}

QCursor Viewport::rotateCursor()
{
    // Premiere-style curved-arrow rotation cursor (same as the GPU path's
    // TransformOverlayWidget::rotateCursor).
    constexpr int SZ = 32;
    constexpr float CX = SZ / 2.0f;
    constexpr float CY = SZ / 2.0f;
    constexpr float R  = 7.0f;

    QPixmap pix(SZ, SZ);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);

    QRectF arcRect(CX - R, CY - R, R * 2, R * 2);
    int startAngle16 = 45 * 16;
    int spanAngle16  = 270 * 16;

    constexpr float PI = 3.14159265f;
    float a = 45.0f * PI / 180.0f;
    float ex = CX + R * std::cos(a);
    float ey = CY - R * std::sin(a);
    float tx =  std::sin(a);
    float ty =  std::cos(a);
    float arrLen = 5.0f;
    float arrHalf = 2.8f;
    QPointF tip(ex + arrLen * tx, ey - arrLen * ty);
    float px2 = -ty, py2 = -tx;
    QPointF base1(ex + arrHalf * px2, ey - arrHalf * py2);
    QPointF base2(ex - arrHalf * px2, ey + arrHalf * py2);

    auto drawShape = [&](const QPen& pen, const QBrush& brush) {
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(arcRect, startAngle16, spanAngle16);
        p.setBrush(brush);
        QPolygonF arrow;
        arrow << tip << base1 << base2;
        p.drawPolygon(arrow);
    };

    drawShape(QPen(Qt::black, 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin),
              QBrush(Qt::black));
    drawShape(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin),
              QBrush(Qt::white));

    p.end();
    return QCursor(pix, SZ / 2, SZ / 2);
}

void Viewport::drawTransformOverlay(QPainter& painter)
{
    const QPointF* corners = getCachedCorners();

    // ── Bounding box (dashed cyan line) ─────────────────────────────────
    const auto& tc = Theme::colors();
    QColor boxColor = tc.accent; boxColor.setAlpha(200);
    QPen boxPen(boxColor, 1.5, Qt::DashLine);
    painter.setPen(boxPen);
    painter.setBrush(Qt::NoBrush);

    QPolygonF poly;
    for (int i = 0; i < 4; ++i)
        poly << corners[i];
    poly << corners[0]; // close the polygon
    painter.drawPolyline(poly);

    // ── Corner handles (filled squares) ─────────────────────────────────
    constexpr double HANDLE_SIZE = 8.0;
    QColor handleBorder = tc.textBright; handleBorder.setAlpha(220);
    painter.setPen(QPen(handleBorder, 1));
    QColor handleFill = tc.accent; handleFill.setAlpha(180);
    painter.setBrush(handleFill);

    for (int i = 0; i < 4; ++i) {
        QRectF handle(corners[i].x() - HANDLE_SIZE / 2,
                      corners[i].y() - HANDLE_SIZE / 2,
                      HANDLE_SIZE, HANDLE_SIZE);
        painter.drawRect(handle);
    }

    // ── Center crosshair ────────────────────────────────────────────────
    QPointF center = (corners[0] + corners[2]) * 0.5;
    QColor crossColor = tc.accent; crossColor.setAlpha(160);
    painter.setPen(QPen(crossColor, 1));
    constexpr double CROSS_SIZE = 6.0;
    painter.drawLine(QPointF(center.x() - CROSS_SIZE, center.y()),
                     QPointF(center.x() + CROSS_SIZE, center.y()));
    painter.drawLine(QPointF(center.x(), center.y() - CROSS_SIZE),
                     QPointF(center.x(), center.y() + CROSS_SIZE));
}

} // namespace rt

