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
#include <QPen>
#include <QPolygonF>

#include <cmath>

#include <spdlog/spdlog.h>

namespace rt {

void TransformOverlayWidget::paintEvent(QPaintEvent* /*event*/)
{
    // Skip painting entirely when there's nothing to draw.
    // On a WA_TranslucentBackground window, even a no-op QPainter triggers
    // DWM per-pixel alpha composition, so avoid it when possible.
    bool hasMasks = (m_masks && !m_masks->empty());
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

    // ── Live bounding box around the inline text editor ─────────────────
    // The editor is a top-level borderless window so the user has no
    // visual cue for the text bounds while typing. Mirror Premiere by
    // drawing a thin dashed rectangle that hugs the editor's current
    // screen geometry — the editor's textChanged handler resizes the
    // widget as text grows, so this box automatically tracks size.
    if (inlineEditing) {
        const QRect g = m_inlineTextEdit->geometry();        // screen coords
        const QPoint tl = mapFromGlobal(g.topLeft());        // overlay-local
        const QRect local(tl, g.size());
        const auto& tc = Theme::colors();
        QColor c = tc.accent; c.setAlpha(200);
        QPen pen(c, 1.5, Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        // Small inset so the line doesn't clip glyph descenders.
        painter.drawRect(local.adjusted(-2, -2, 2, 2));
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
    if (!m_masks || !m_vulkanVp) return;

    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) return;

    float srcW = static_cast<float>(m_vulkanVp->srcWidth());
    float srcH = static_cast<float>(m_vulkanVp->srcHeight());
    if (srcW <= 0.0f || srcH <= 0.0f) return;

    // Source-pixel to widget-pixel conversion factors
    double pxToWX = fr.width()  / static_cast<double>(srcW);
    double pxToWY = fr.height() / static_cast<double>(srcH);

    // Mask outline: Premiere-style blue
    QColor activeMaskColor(0, 120, 255, 200);
    QColor dimMaskColor(0, 120, 255, 80);
    QColor featherColor(0, 120, 255, 120);
    QColor dimFeatherColor(0, 120, 255, 50);
    QColor handleColor(255, 255, 255, 220);
    QColor glowColor(100, 180, 255, 90);
    constexpr double CTRL_SIZE = 12.0;
    constexpr double GLOW_EXTRA = 4.0; // extra radius for hover glow

    for (size_t maskLoopIdx = 0; maskLoopIdx < m_masks->size(); ++maskLoopIdx) {
        const auto& mask = (*m_masks)[maskLoopIdx];
        int mi = static_cast<int>(maskLoopIdx);
        bool isActive = (m_activeMaskIndex < 0 || m_activeMaskIndex == mi);
        bool isMaskHovered = (mi == m_hoverMaskIndex);

        QColor maskColor = isActive ? activeMaskColor : dimMaskColor;
        QPen maskPen(maskColor, isActive ? 1.5 : 1.0, Qt::SolidLine);
        QPen featherPen(isActive ? featherColor : dimFeatherColor, 1.0, Qt::DashLine);

        painter.setPen(maskPen);
        painter.setBrush(Qt::NoBrush);

        // Map normalized mask coords to widget space
        auto toWidget = [&](float nx, float ny) -> QPointF {
            return QPointF(fr.x() + static_cast<double>(nx) * fr.width(),
                           fr.y() + static_cast<double>(ny) * fr.height());
        };

        // Expansion offset in widget pixels (averaged for non-square pixels)
        double expW = static_cast<double>(mask.expansion) * pxToWX;
        double expH = static_cast<double>(mask.expansion) * pxToWY;
        // Feather offset (additional, outside the expanded boundary)
        double feathW = static_cast<double>(mask.feather) * pxToWX;
        double feathH = static_cast<double>(mask.feather) * pxToWY;

        if (mask.shape == MaskShape::Ellipse) {
            QPointF center = toWidget(mask.centerX, mask.centerY);
            double rw = static_cast<double>(mask.width)  * fr.width()  * 0.5 + expW;
            double rh = static_cast<double>(mask.height) * fr.height() * 0.5 + expH;
            rw = std::max(rw, 0.0);
            rh = std::max(rh, 0.0);

            painter.save();
            painter.translate(center);
            painter.rotate(static_cast<double>(mask.rotation));

            // Main expanded boundary
            painter.setPen(maskPen);
            painter.drawEllipse(QPointF(0, 0), rw, rh);

            // Feather boundary (dotted, outside the main boundary)
            if (mask.feather > 0.01f) {
                double frw = rw + feathW;
                double frh = rh + feathH;
                painter.setPen(featherPen);
                painter.drawEllipse(QPointF(0, 0), std::max(frw, 0.0), std::max(frh, 0.0));
            }

            // Control handles at expanded cardinal positions + center
            if (isActive) {
                int hIdx = 0;
                for (auto [dx, dy] : {std::pair{rw, 0.0}, {-rw, 0.0}, {0.0, rh}, {0.0, -rh}, {0.0, 0.0}}) {
                    bool hovered = isMaskHovered && (m_hoverMaskHandle == hIdx);
                    if (hovered) {
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(glowColor);
                        painter.drawEllipse(QPointF(dx, dy), CTRL_SIZE / 2 + GLOW_EXTRA, CTRL_SIZE / 2 + GLOW_EXTRA);
                    }
                    painter.setPen(QPen(handleColor, hovered ? 2 : 1));
                    painter.setBrush(maskColor);
                    painter.drawEllipse(QPointF(dx, dy), CTRL_SIZE / 2, CTRL_SIZE / 2);
                    ++hIdx;
                }
            }
            painter.restore();
        }
        else if (mask.shape == MaskShape::Rectangle) {
            QPointF center = toWidget(mask.centerX, mask.centerY);
            double hw = static_cast<double>(mask.width)  * fr.width()  * 0.5 + expW;
            double hh = static_cast<double>(mask.height) * fr.height() * 0.5 + expH;
            hw = std::max(hw, 0.0);
            hh = std::max(hh, 0.0);

            painter.save();
            painter.translate(center);
            painter.rotate(static_cast<double>(mask.rotation));

            // Main expanded boundary
            painter.setPen(maskPen);
            QRectF rect(-hw, -hh, hw * 2, hh * 2);
            painter.drawRect(rect);

            // Feather boundary (dotted)
            if (mask.feather > 0.01f) {
                double fhw = hw + feathW;
                double fhh = hh + feathH;
                painter.setPen(featherPen);
                QRectF feathRect(-fhw, -fhh, fhw * 2, fhh * 2);
                painter.drawRect(feathRect);
            }

            // Corner handles + center + mid-edge handles (at expanded dims)
            if (isActive) {
                int hIdx = 0;
                // Corners (square handles) + center
                for (auto [cx2, cy2] : {std::pair{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}, {0.0, 0.0}}) {
                    bool hovered = isMaskHovered && (m_hoverMaskHandle == hIdx);
                    if (hovered) {
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(glowColor);
                        painter.drawRect(QRectF(cx2 - CTRL_SIZE / 2 - GLOW_EXTRA, cy2 - CTRL_SIZE / 2 - GLOW_EXTRA,
                                                CTRL_SIZE + GLOW_EXTRA * 2, CTRL_SIZE + GLOW_EXTRA * 2));
                    }
                    painter.setPen(QPen(handleColor, hovered ? 2 : 1));
                    painter.setBrush(maskColor);
                    painter.drawRect(QRectF(cx2 - CTRL_SIZE / 2, cy2 - CTRL_SIZE / 2,
                                            CTRL_SIZE, CTRL_SIZE));
                    ++hIdx;
                }
                // Mid-edge handles (round handles for distinction) indices 5-8
                for (auto [ex, ey] : {std::pair{0.0, -hh}, {hw, 0.0}, {0.0, hh}, {-hw, 0.0}}) {
                    bool hovered = isMaskHovered && (m_hoverMaskHandle == hIdx);
                    if (hovered) {
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(glowColor);
                        painter.drawEllipse(QPointF(ex, ey), CTRL_SIZE / 2 + GLOW_EXTRA, CTRL_SIZE / 2 + GLOW_EXTRA);
                    }
                    painter.setPen(QPen(handleColor, hovered ? 2 : 1));
                    painter.setBrush(maskColor);
                    painter.drawEllipse(QPointF(ex, ey), CTRL_SIZE / 2, CTRL_SIZE / 2);
                    ++hIdx;
                }
            }
            painter.restore();
        }
        else if (mask.shape == MaskShape::FreeDrawBezier && mask.vertices.size() >= 2) {
            // Draw bezier path
            QPainterPath path;
            const auto& verts = mask.vertices;
            QPointF first = toWidget(verts[0].x, verts[0].y);
            path.moveTo(first);
            for (size_t vi = 0; vi < verts.size(); ++vi) {
                size_t ni = (vi + 1) % verts.size();
                QPointF p0 = toWidget(verts[vi].x, verts[vi].y);
                QPointF cp1(p0.x() + static_cast<double>(verts[vi].outTanX) * fr.width(),
                            p0.y() + static_cast<double>(verts[vi].outTanY) * fr.height());
                QPointF p1 = toWidget(verts[ni].x, verts[ni].y);
                QPointF cp2(p1.x() + static_cast<double>(verts[ni].inTanX) * fr.width(),
                            p1.y() + static_cast<double>(verts[ni].inTanY) * fr.height());
                path.cubicTo(cp1, cp2, p1);
            }
            painter.drawPath(path);

            // Feather outline for bezier (stroked expansion of path)
            if (mask.feather > 0.01f) {
                QPainterPathStroker stroker;
                double avgFeath = (feathW + feathH) * 0.5;
                stroker.setWidth(avgFeath * 2.0);
                QPainterPath feathOutline = stroker.createStroke(path);
                painter.setPen(featherPen);
                painter.drawPath(feathOutline);
            }

            // Vertex control points
            if (isActive) {
                int hIdx = 0;
                for (const auto& v : verts) {
                    QPointF pt = toWidget(v.x, v.y);
                    bool hovered = isMaskHovered && (m_hoverMaskHandle == hIdx);
                    if (hovered) {
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(glowColor);
                        painter.drawEllipse(pt, CTRL_SIZE / 2 + GLOW_EXTRA, CTRL_SIZE / 2 + GLOW_EXTRA);
                    }
                    painter.setPen(QPen(handleColor, hovered ? 2 : 1));
                    painter.setBrush(maskColor);
                    painter.drawEllipse(pt, CTRL_SIZE / 2, CTRL_SIZE / 2);
                    ++hIdx;
                }
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Mask hit-testing
// ═════════════════════════════════════════════════════════════════════════════


} // namespace rt
