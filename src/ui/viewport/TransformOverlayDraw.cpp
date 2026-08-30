/*
 * TransformOverlayDraw.cpp - Painting & drawing for TransformOverlayWidget.
 */

#include "viewport/TransformOverlayWidget.h"
#include "viewport/VulkanViewport.h"
#include "timeline/OpacityMask.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/Keyframe.h"
#include "timeline/Position2D.h"
#include "Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPen>
#include <QPolygonF>
#include <QGuiApplication>

#include <cmath>

#include <spdlog/spdlog.h>

namespace rt {

void TransformOverlayWidget::paintEvent(QPaintEvent* /*event*/)
{
    // Skip painting entirely when there's nothing to draw.
    // On a WA_TranslucentBackground window, even a no-op QPainter triggers
    // DWM per-pixel alpha composition, so avoid it when possible.
    bool hasMasks = (m_masks && !m_masks->empty()) || m_penDrawing;
    const bool inlineEditing = m_inlineTextEdit && m_inlineTextEdit->isVisible();
    if (!m_overlay.visible && !m_showSafeAreas && !m_showGrid && !hasMasks
        && !inlineEditing)
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Draw rule-of-thirds grid (behind safe areas)
    if (m_showGrid)
        drawGrid(painter);

    // Draw safe area guides (always if enabled, even without selected clip)
    if (m_showSafeAreas)
        drawSafeAreas(painter);

    // Draw mask overlays (blue outlines + control points)
    if (hasMasks)
        drawMaskOverlay(painter);

    // Draw transform overlay (bounding box + handles)
    if (m_overlay.visible)
        drawTransformOverlay(painter);

    // Draw the motion path (Premiere-style curve through Position keyframes)
    // — visible whenever a selected clip has 2+ Position keyframes.
    if (m_motionX && m_motionY)
        drawMotionPath(painter);

    // ── Compositor-aligned inline-edit bounds and caret ─────────────────
    if (inlineEditing) {
        const QTextCursor cursor = m_inlineTextEdit->textCursor();
        const auto& tc = Theme::colors();

        // The editor owns selection semantics, but its independently hinted
        // highlight is hidden. Paint the visible selection from the same
        // compositor insertion boundaries used by the custom caret.
        if (cursor.hasSelection()) {
            QColor selectionColor = tc.accent;
            selectionColor.setAlpha(165);
            painter.setPen(Qt::NoPen);
            painter.setBrush(selectionColor);
            painter.drawPath(inlineTextSelectionPath());
        }

        // Use the compositor's measured glyph bounds. The invisible editor
        // rectangle contains caret slack and rounded line cells, so it cannot
        // be used as a visual transform/text box.
        QPointF corners[4];
        computeOverlayCornersFor(inlineTextLayoutOverlay(), corners);
        QColor c = tc.accent; c.setAlpha(200);
        QPen pen(c, 1.5, Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        QPolygonF bounds;
        for (const QPointF& corner : corners) bounds << corner;
        bounds << corners[0];
        painter.drawPolyline(bounds);

        if (!cursor.hasSelection() && m_inlineTextEdit->hasFocus()
            && m_inlineCaretVisible) {
            const QLineF caret = inlineTextCaretLine();
            painter.setPen(QPen(tc.textBright, 1.5, Qt::SolidLine,
                                Qt::SquareCap));
            if (!caret.isNull()) painter.drawLine(caret);
        }
    }
}

void TransformOverlayWidget::drawMotionPath(QPainter& painter)
{
    if (!m_motionX || !m_motionY) return;
    const size_t n = m_motionX->keyframeCount();
    if (n < 2 || m_motionY->keyframeCount() != n) return;

    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) return;

    const auto& tc = Theme::colors();

    // ── The path itself (sampled cubic-Bezier per segment) ───────────
    // We use evaluatePosition2D directly because the spatial-interpolation
    // rules (Linear / Auto / Continuous / Manual) live there and we want
    // the drawn path to be exactly what the compositor will render.
    QPainterPath path;
    bool started = false;
    for (size_t i = 0; i + 1 < n; ++i) {
        const auto& kx0 = m_motionX->keyframe(i);
        const auto& ky0 = m_motionY->keyframe(i);
        const auto& kx1 = m_motionX->keyframe(i + 1);
        const auto& ky1 = m_motionY->keyframe(i + 1);
        if (kx0.time != ky0.time || kx1.time != ky1.time) continue;

        if (!started) {
            path.moveTo(refToWidget(kx0.value, ky0.value, fr));
            started = true;
        }

        // Sample 24 points along the segment via the joint evaluator so the
        // drawn path always matches what the renderer produces.
        constexpr int kSamples = 24;
        const int64_t dt = kx1.time - kx0.time;
        for (int s = 1; s <= kSamples; ++s) {
            const int64_t t = kx0.time + (dt * s) / kSamples;
            auto p = evaluatePosition2D(*m_motionX, *m_motionY, t);
            path.lineTo(refToWidget(p.first, p.second, fr));
        }
    }

    QColor pathColor = tc.warning; pathColor.setAlpha(220);
    painter.setPen(QPen(pathColor, 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);

    // ── Waypoint markers (small filled squares at each Position keyframe) ─
    constexpr double kMarker = 4.0;
    QColor mkBorder = tc.textBright; mkBorder.setAlpha(220);
    QColor mkFill   = tc.warning;    mkFill.setAlpha(220);
    painter.setPen(QPen(mkBorder, 1));
    painter.setBrush(mkFill);
    for (size_t i = 0; i < n; ++i) {
        const auto& kx = m_motionX->keyframe(i);
        const auto& ky = m_motionY->keyframe(i);
        if (kx.time != ky.time) continue;
        QPointF pt = refToWidget(kx.value, ky.value, fr);
        painter.drawRect(QRectF(pt.x() - kMarker, pt.y() - kMarker,
                                kMarker * 2.0, kMarker * 2.0));
    }

    // ── Spatial Bezier handles for Bezier / ContinuousBezier waypoints ───
    constexpr double kHandleR = 4.5;
    QColor hStem  = tc.textBright; hStem.setAlpha(180);
    QColor hFill  = tc.accent;     hFill.setAlpha(220);
    QColor hBord  = tc.textBright; hBord.setAlpha(230);
    for (size_t i = 0; i < n; ++i) {
        const auto& kx = m_motionX->keyframe(i);
        const auto& ky = m_motionY->keyframe(i);
        if (kx.time != ky.time) continue;
        const bool hasManual =
            kx.spatialInterp == InterpMode::Bezier ||
            kx.spatialInterp == InterpMode::ContinuousBezier;
        if (!hasManual) continue;

        QPointF wpPx = refToWidget(kx.value, ky.value, fr);

        // Out handle (only meaningful when a segment leaves this keyframe).
        if (i + 1 < n) {
            QPointF outPx = refToWidget(kx.value + kx.spatialOutX,
                                        ky.value + ky.spatialOutY, fr);
            painter.setPen(QPen(hStem, 1));
            painter.drawLine(wpPx, outPx);
            painter.setBrush(hFill);
            painter.setPen(QPen(hBord, 1));
            painter.drawEllipse(outPx, kHandleR, kHandleR);
        }
        // In handle (only meaningful when a segment arrives at this keyframe).
        if (i > 0) {
            QPointF inPx = refToWidget(kx.value + kx.spatialInX,
                                       ky.value + ky.spatialInY, fr);
            painter.setPen(QPen(hStem, 1));
            painter.drawLine(wpPx, inPx);
            painter.setBrush(hFill);
            painter.setPen(QPen(hBord, 1));
            painter.drawEllipse(inPx, kHandleR, kHandleR);
        }
    }
}

void TransformOverlayWidget::drawTransformOverlay(QPainter& painter)
{
    const auto& tc = Theme::colors();

    // ── Secondary (sibling) boxes for multi-selection ───────────────────
    // Drawn FIRST so they sit under the primary box. Each sibling gets
    // the full transform-handle treatment (dashed outline + four corner
    // squares) so the user can see at a glance which layers are selected.
    // Only the focused-layer handles are hit-tested — clicking a sibling
    // handle currently falls through to the body / empty-area path; the
    // sibling boxes travel with the focused one during a group-move drag.
    if (!m_secondaryOverlays.empty()) {
        constexpr double SEC_HANDLE_SIZE = 8.0;
        QColor secOutline = tc.accent;     secOutline.setAlpha(160);
        QColor secHandleBorder = tc.textBright; secHandleBorder.setAlpha(200);
        QColor secHandleFill   = tc.accent;     secHandleFill.setAlpha(160);
        for (const auto& sov : m_secondaryOverlays) {
            if (!sov.visible) continue;
            QPointF sc[4];
            computeOverlayCornersFor(sov, sc);

            // Dashed outline.
            painter.setPen(QPen(secOutline, 1.0, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            QPolygonF spoly;
            for (int i = 0; i < 4; ++i) spoly << sc[i];
            spoly << sc[0];
            painter.drawPolyline(spoly);

            // Corner handle squares — same shape as the focused box's
            // handles, slightly dimmer alpha so the focused box still
            // reads as the "active" one.
            painter.setPen(QPen(secHandleBorder, 1));
            painter.setBrush(secHandleFill);
            for (int i = 0; i < 4; ++i) {
                QRectF h(sc[i].x() - SEC_HANDLE_SIZE / 2,
                         sc[i].y() - SEC_HANDLE_SIZE / 2,
                         SEC_HANDLE_SIZE, SEC_HANDLE_SIZE);
                painter.drawRect(h);
            }
        }
    }

    QPointF corners[4];
    computeOverlayCorners(corners);

    // ── Bounding box (dashed cyan line) ─────────────────────────────────
    QColor boxColor = tc.accent; boxColor.setAlpha(200);
    QPen boxPen(boxColor, 1.5, Qt::DashLine);
    painter.setPen(boxPen);
    painter.setBrush(Qt::NoBrush);

    QPolygonF poly;
    for (int i = 0; i < 4; ++i)
        poly << corners[i];
    poly << corners[0];
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

    // ── Anchor point handle ─────────────────────────────────────────────
    // Premiere/AE-style target marker at the rotation/scale pivot. Two
    // coordinate conventions are in play:
    //   • Content-rect mode (graphic layers): posX/anchorX are in canvas
    //     (project) pixels, stored in info.contentCanvasW.
    //   • Standard mode (video / image / spine / etc.): posX/anchorX are
    //     in REF-1920 pixels, mapped to the viewport's srcWidth via the
    //     compositor's buildViewportTransform scaling.
    if (m_vulkanVp) {
        QRectF fr = computeFrameRect();
        if (!fr.isEmpty()) {
            float canvasW = 0.0f, canvasH = 0.0f;
            float refAnchorPxX = 0.0f, refAnchorPxY = 0.0f;
            float refPosPxX    = 0.0f, refPosPxY    = 0.0f;
            bool  drawAnchor   = false;
            if (m_overlay.useContentRect &&
                m_overlay.contentCanvasW > 0.0f && m_overlay.contentCanvasH > 0.0f)
            {
                canvasW = m_overlay.contentCanvasW;
                canvasH = m_overlay.contentCanvasH;
                refAnchorPxX = m_overlay.anchorX;  // already canvas-px
                refAnchorPxY = m_overlay.anchorY;
                refPosPxX    = m_overlay.posX;
                refPosPxY    = m_overlay.posY;
                drawAnchor = true;
            } else if (m_vulkanVp->srcWidth() > 0 && m_vulkanVp->srcHeight() > 0) {
                canvasW = static_cast<float>(m_vulkanVp->srcWidth());
                canvasH = static_cast<float>(m_vulkanVp->srcHeight());
                constexpr float REF_W = 1920.0f;
                constexpr float REF_H = 1080.0f;
                refAnchorPxX = m_overlay.anchorX * (canvasW / REF_W);
                refAnchorPxY = m_overlay.anchorY * (canvasH / REF_H);
                refPosPxX    = m_overlay.posX    * (canvasW / REF_W);
                refPosPxY    = m_overlay.posY    * (canvasH / REF_H);
                drawAnchor = true;
            }
            if (drawAnchor) {
                const float ax = canvasW * 0.5f + refPosPxX + refAnchorPxX;
                const float ay = canvasH * 0.5f + refPosPxY + refAnchorPxY;
                const QPointF anchorPt(
                    fr.x() + (static_cast<double>(ax) / canvasW) * fr.width(),
                    fr.y() + (static_cast<double>(ay) / canvasH) * fr.height());

                constexpr double ANCHOR_RADIUS = 7.0;
                constexpr double ANCHOR_CROSS  = 5.0;
                QColor anchorOuter = tc.warning; anchorOuter.setAlpha(220);
                QColor anchorInner = QColor(255, 255, 255, 230);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(anchorOuter, 1.5));
                painter.drawEllipse(anchorPt, ANCHOR_RADIUS, ANCHOR_RADIUS);
                painter.setPen(QPen(anchorInner, 1.0));
                painter.drawLine(QPointF(anchorPt.x() - ANCHOR_CROSS, anchorPt.y()),
                                 QPointF(anchorPt.x() + ANCHOR_CROSS, anchorPt.y()));
                painter.drawLine(QPointF(anchorPt.x(), anchorPt.y() - ANCHOR_CROSS),
                                 QPointF(anchorPt.x(), anchorPt.y() + ANCHOR_CROSS));
            }
        }
    }

    // Crop visualization, drawn last so it sits above the box. Existing crop
    // remains visible; interactive handles are Ctrl-only.
    drawCropOverlay(painter);
}

void TransformOverlayWidget::drawCropOverlay(QPainter& painter)
{
    if (!m_overlay.cropEnabled) return;

    // Existing crop remains visible as a dimmed region, but the green crop
    // controls themselves only appear while Ctrl is held (or for the duration
    // of an already-started crop drag). This keeps ordinary transform handles
    // visually and interactively unambiguous.
    const bool showCropControls = (m_dragMode == DragMode::CropEdge)
        || cropGestureRequested(QGuiApplication::keyboardModifiers());

    QPointF c[4];
    computeOverlayCorners(c);   // TL, TR, BR, BL of the uncropped box
    const QPointF U = c[1] - c[0];   // left→right axis
    const QPointF V = c[3] - c[0];   // top→bottom axis
    if (std::abs(U.x()) + std::abs(U.y()) < 1e-3 ||
        std::abs(V.x()) + std::abs(V.y()) < 1e-3) return;

    const float fl = m_overlay.cropL / 100.0f;
    const float fr = m_overlay.cropR / 100.0f;
    const float ft = m_overlay.cropT / 100.0f;
    const float fb = m_overlay.cropB / 100.0f;
    auto pt = [&](float a, float b) -> QPointF {
        return QPointF(c[0].x() + a * U.x() + b * V.x(),
                       c[0].y() + a * U.y() + b * V.y());
    };
    auto quad = [&](float a0, float b0, float a1, float b1,
                    float a2, float b2, float a3, float b3) {
        QPolygonF p; p << pt(a0, b0) << pt(a1, b1) << pt(a2, b2) << pt(a3, b3);
        return p;
    };

    const auto& tc = Theme::colors();

    // Dim the cropped-out strips (tiled so corners don't double-darken).
    const bool cropped = (fl > 1e-4f || fr > 1e-4f || ft > 1e-4f || fb > 1e-4f);
    if (cropped) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 110));
        if (ft > 1e-4f) painter.drawPolygon(quad(0, 0,        1, 0,        1, ft,      0, ft));
        if (fb > 1e-4f) painter.drawPolygon(quad(0, 1 - fb,   1, 1 - fb,   1, 1,       0, 1));
        if (fl > 1e-4f) painter.drawPolygon(quad(0, ft,       fl, ft,      fl, 1 - fb, 0, 1 - fb));
        if (fr > 1e-4f) painter.drawPolygon(quad(1 - fr, ft,  1, ft,       1, 1 - fb,  1 - fr, 1 - fb));
    }

    if (!showCropControls) return;

    // Inner crop rectangle outline — GREEN, matching the SHOT COMPOSE crop box
    // (distinct from the cyan transform box so crop reads as its own gizmo).
    const QColor kCropGreen(64, 220, 96);
    QColor line = kCropGreen; line.setAlpha(230);
    painter.setPen(QPen(line, 1.5));
    painter.setBrush(Qt::NoBrush);
    QPolygonF inner;
    inner << pt(fl, ft) << pt(1 - fr, ft) << pt(1 - fr, 1 - fb)
          << pt(fl, 1 - fb) << pt(fl, ft);
    painter.drawPolyline(inner);

    // Edge-mid handles (filled green squares).
    QPointF h[4];
    if (cropHandlePositions(h)) {
        constexpr double HS = 9.0;
        QColor border = tc.textBright; border.setAlpha(235);
        QColor fill   = kCropGreen;    fill.setAlpha(235);
        painter.setPen(QPen(border, 1));
        painter.setBrush(fill);
        for (int i = 0; i < 4; ++i)
            painter.drawRect(QRectF(h[i].x() - HS / 2, h[i].y() - HS / 2, HS, HS));
    }
}

void TransformOverlayWidget::drawSafeAreas(QPainter& painter)
{
    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) return;

    QColor safeColor = Theme::colors().textPrimary; safeColor.setAlpha(80);
    QPen safePen(safeColor, 1, Qt::DashLine);
    painter.setPen(safePen);
    painter.setBrush(Qt::NoBrush);

    // Action safe (90% — 5% inset on each side)
    QRectF actionSafe = fr;
    actionSafe.adjust(fr.width() * 0.05, fr.height() * 0.05,
                     -fr.width() * 0.05, -fr.height() * 0.05);
    painter.drawRect(actionSafe);

    // Title safe (80% — 10% inset on each side)
    QRectF titleSafe = fr;
    titleSafe.adjust(fr.width() * 0.1, fr.height() * 0.1,
                    -fr.width() * 0.1, -fr.height() * 0.1);
    painter.drawRect(titleSafe);
}

void TransformOverlayWidget::drawGrid(QPainter& painter)
{
    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) return;

    QColor gridColor = Theme::colors().textPrimary; gridColor.setAlpha(60);
    QPen gridPen(gridColor, 1, Qt::SolidLine);
    painter.setPen(gridPen);
    painter.setBrush(Qt::NoBrush);

    // Rule-of-thirds: 2 vertical + 2 horizontal lines
    double x1 = fr.left() + fr.width() / 3.0;
    double x2 = fr.left() + fr.width() * 2.0 / 3.0;
    double y1 = fr.top() + fr.height() / 3.0;
    double y2 = fr.top() + fr.height() * 2.0 / 3.0;

    painter.drawLine(QPointF(x1, fr.top()), QPointF(x1, fr.bottom()));
    painter.drawLine(QPointF(x2, fr.top()), QPointF(x2, fr.bottom()));
    painter.drawLine(QPointF(fr.left(), y1), QPointF(fr.right(), y1));
    painter.drawLine(QPointF(fr.left(), y2), QPointF(fr.right(), y2));

    // Center crosshair
    double cx = fr.center().x();
    double cy = fr.center().y();
    constexpr double CROSS = 8.0;
    painter.drawLine(QPointF(cx - CROSS, cy), QPointF(cx + CROSS, cy));
    painter.drawLine(QPointF(cx, cy - CROSS), QPointF(cx, cy + CROSS));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Mask overlay
// ═════════════════════════════════════════════════════════════════════════════

void TransformOverlayWidget::drawMaskOverlay(QPainter& painter)
{
    if ((!m_masks || m_masks->empty()) && !m_penDrawing) return;

    const QSizeF sourceSize = maskSourceSize();

    const QColor activeMaskColor(0, 120, 255, 220);
    const QColor dimMaskColor(0, 120, 255, 85);
    const QColor featherColor(0, 120, 255, 125);
    const QColor handleColor(255, 255, 255, 230);
    const QColor glowColor(100, 180, 255, 90);
    constexpr double CTRL_SIZE = 12.0;
    constexpr double GLOW_EXTRA = 4.0;
    constexpr int kTangentHandleBase = 10000;

    auto cosmeticPen = [](const QColor& color, double width,
                          Qt::PenStyle style = Qt::SolidLine) {
        QPen pen(color, width, style);
        pen.setCosmetic(true);
        return pen;
    };

    auto rotatePoint = [&](const MaskGeometry& geo, float dxNorm,
                           float dyNorm, const QSizeF& coordinateSize,
                           MaskCoordinateSpace coordinateSpace) {
        const double radians = static_cast<double>(geo.rotation)
            * 3.14159265358979323846 / 180.0;
        const double c = std::cos(radians);
        const double s = std::sin(radians);
        const double px = static_cast<double>(dxNorm) * coordinateSize.width();
        const double py = static_cast<double>(dyNorm) * coordinateSize.height();
        return maskLocalToWidget(
            geo.centerX + static_cast<float>((px * c - py * s)
                                             / coordinateSize.width()),
            geo.centerY + static_cast<float>((px * s + py * c)
                                             / coordinateSize.height()),
            coordinateSpace);
    };

    auto drawControl = [&](const QPointF& pt, const QColor& maskColor,
                           bool maskHovered, int handleIndex, bool square,
                           double size = 12.0) {
        const bool hovered = maskHovered && m_hoverMaskHandle == handleIndex;
        if (hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(glowColor);
            const double glowSize = size + GLOW_EXTRA * 2.0;
            if (square)
                painter.drawRect(QRectF(pt.x() - glowSize * 0.5,
                                        pt.y() - glowSize * 0.5,
                                        glowSize, glowSize));
            else
                painter.drawEllipse(pt, glowSize * 0.5, glowSize * 0.5);
        }
        painter.setPen(cosmeticPen(handleColor, hovered ? 2.0 : 1.0));
        painter.setBrush(maskColor);
        if (square)
            painter.drawRect(QRectF(pt.x() - size * 0.5, pt.y() - size * 0.5,
                                    size, size));
        else
            painter.drawEllipse(pt, size * 0.5, size * 0.5);
    };

    if (m_masks) {
        for (size_t maskLoopIdx = 0; maskLoopIdx < m_masks->size();
             ++maskLoopIdx) {
            const auto& mask = (*m_masks)[maskLoopIdx];
            const MaskGeometry geo = mask.geometryAt(m_maskTime);
            const float feather = mask.feather.evaluate(m_maskTime);
            const float expansion = mask.expansion.evaluate(m_maskTime);
            const QSizeF coordinateSize =
                mask.coordinateSpace == MaskCoordinateSpace::LegacySequenceFrame
                ? QSizeF(std::max<uint32_t>(
                             1u, m_vulkanVp ? m_vulkanVp->srcWidth() : 1u),
                         std::max<uint32_t>(
                             1u, m_vulkanVp ? m_vulkanVp->srcHeight() : 1u))
                : sourceSize;
            const QPointF pxX = maskVectorToWidget(
                1.0f / static_cast<float>(coordinateSize.width()), 0.0f,
                mask.coordinateSpace);
            const QPointF pxY = maskVectorToWidget(
                0.0f, 1.0f / static_cast<float>(coordinateSize.height()),
                mask.coordinateSpace);
            const double pixelScale = 0.5 * (
                std::hypot(pxX.x(), pxX.y()) +
                std::hypot(pxY.x(), pxY.y()));
            const int mi = static_cast<int>(maskLoopIdx);
            const bool active = (m_activeMaskIndex == mi);
            const bool maskHovered = (m_hoverMaskIndex == mi);
            const QColor maskColor = active ? activeMaskColor : dimMaskColor;

            const QPainterPath path =
                buildMaskWidgetPath(mask, geo, expansion, true);
            painter.setPen(cosmeticPen(maskColor, active ? 1.5 : 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);

            if (feather > 0.01f) {
                painter.setPen(cosmeticPen(active ? featherColor
                                                  : QColor(0, 120, 255, 50),
                                             1.0, Qt::DashLine));
                if (mask.shape == MaskShape::FreeDrawBezier) {
                    QPainterPathStroker stroker;
                    stroker.setWidth(std::max(
                        1.0, static_cast<double>(feather) * pixelScale * 2.0));
                    painter.drawPath(stroker.createStroke(path));
                } else {
                    painter.drawPath(buildMaskWidgetPath(
                        mask, geo, expansion + feather, true));
                }
            }

            if (!active) continue;

            const float expX = expansion
                / static_cast<float>(coordinateSize.width());
            const float expY = expansion
                / static_cast<float>(coordinateSize.height());

            if (mask.shape == MaskShape::Ellipse) {
                const float rw = std::max(0.0f, geo.width * 0.5f + expX);
                const float rh = std::max(0.0f, geo.height * 0.5f + expY);
                const QPointF handles[5] = {
                    rotatePoint(geo, rw, 0.0f, coordinateSize, mask.coordinateSpace),
                    rotatePoint(geo, -rw, 0.0f, coordinateSize, mask.coordinateSpace),
                    rotatePoint(geo, 0.0f, rh, coordinateSize, mask.coordinateSpace),
                    rotatePoint(geo, 0.0f, -rh, coordinateSize, mask.coordinateSpace),
                    maskLocalToWidget(geo.centerX, geo.centerY,
                                      mask.coordinateSpace)
                };
                for (int i = 0; i < 5; ++i)
                    drawControl(handles[i], maskColor, maskHovered, i, false);
            } else if (mask.shape == MaskShape::Rectangle) {
                const float hw = std::max(0.0f, geo.width * 0.5f + expX);
                const float hh = std::max(0.0f, geo.height * 0.5f + expY);
                const QPointF handles[9] = {
                    rotatePoint(geo, -hw, -hh, coordinateSize, mask.coordinateSpace),
                    rotatePoint(geo,  hw, -hh, coordinateSize, mask.coordinateSpace),
                    rotatePoint(geo,  hw,  hh, coordinateSize, mask.coordinateSpace),
                    rotatePoint(geo, -hw,  hh, coordinateSize, mask.coordinateSpace),
                    maskLocalToWidget(geo.centerX, geo.centerY,
                                      mask.coordinateSpace),
                    rotatePoint(geo, 0.0f, -hh, coordinateSize, mask.coordinateSpace),
                    rotatePoint(geo, hw, 0.0f, coordinateSize, mask.coordinateSpace),
                    rotatePoint(geo, 0.0f, hh, coordinateSize, mask.coordinateSpace),
                    rotatePoint(geo, -hw, 0.0f, coordinateSize, mask.coordinateSpace)
                };
                for (int i = 0; i < 9; ++i)
                    drawControl(handles[i], maskColor, maskHovered, i, i < 5);
            } else {
                QPen tangentPen(maskColor, 1.0, Qt::DashLine);
                tangentPen.setCosmetic(true);
                for (size_t vi = 0; vi < geo.vertices.size(); ++vi) {
                    const auto& v = geo.vertices[vi];
                    const QPointF anchor = maskLocalToWidget(
                        v.x, v.y, mask.coordinateSpace);
                    const float tangent[2][2] = {
                        {v.inTanX, v.inTanY},
                        {v.outTanX, v.outTanY}
                    };
                    for (int which = 0; which < 2; ++which) {
                        if (std::hypot(tangent[which][0], tangent[which][1])
                            < 1.0e-6f)
                            continue;
                        const QPointF hp = maskLocalToWidget(
                            v.x + tangent[which][0],
                            v.y + tangent[which][1], mask.coordinateSpace);
                        painter.setPen(tangentPen);
                        painter.setBrush(Qt::NoBrush);
                        painter.drawLine(anchor, hp);
                        drawControl(
                            hp, maskColor, maskHovered,
                            kTangentHandleBase + static_cast<int>(vi) * 2
                                + which,
                            true, 8.0);
                    }
                    drawControl(anchor, maskColor, maskHovered,
                                static_cast<int>(vi), false);
                }
            }
        }
    }

    // A Pen Mask is preview-only until closed. Incomplete paths therefore
    // never blank the clip, and creation becomes one undo step.
    if (m_penDrawing && !m_penDraft.base.vertices.empty()) {
        const QPainterPath draftPath =
            buildMaskWidgetPath(m_penDraft, m_penDraft.base, 0.0f, false);
        painter.setPen(cosmeticPen(activeMaskColor, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(draftPath);

        const auto& verts = m_penDraft.base.vertices;
        const QPointF last = maskLocalToWidget(verts.back().x, verts.back().y);
        if (m_dragMode != DragMode::DrawMaskPoint)
            painter.drawLine(last, m_penHoverWidget);

        for (size_t i = 0; i < verts.size(); ++i) {
            const QPointF pt = maskLocalToWidget(verts[i].x, verts[i].y);
            painter.setPen(cosmeticPen(handleColor, i == 0 ? 2.0 : 1.0));
            painter.setBrush(i == 0 ? QColor(0, 190, 255, 230)
                                    : activeMaskColor);
            painter.drawEllipse(pt, CTRL_SIZE * 0.5, CTRL_SIZE * 0.5);

            const auto& v = verts[i];
            if (std::hypot(v.inTanX, v.inTanY) > 1.0e-6f
                || std::hypot(v.outTanX, v.outTanY) > 1.0e-6f) {
                const QPointF inPt =
                    maskLocalToWidget(v.x + v.inTanX, v.y + v.inTanY);
                const QPointF outPt =
                    maskLocalToWidget(v.x + v.outTanX, v.y + v.outTanY);
                painter.setPen(cosmeticPen(activeMaskColor, 1.0,
                                           Qt::DashLine));
                painter.drawLine(inPt, outPt);
                painter.setBrush(handleColor);
                painter.drawRect(QRectF(inPt.x() - 3.0, inPt.y() - 3.0,
                                        6.0, 6.0));
                painter.drawRect(QRectF(outPt.x() - 3.0, outPt.y() - 3.0,
                                        6.0, 6.0));
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Mask hit-testing
// ═════════════════════════════════════════════════════════════════════════════


} // namespace rt
