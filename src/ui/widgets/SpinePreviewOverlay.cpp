// SpinePreviewOverlay.cpp - Transform overlay + input (extracted from SpinePreviewWidget.cpp).

#ifdef ROUNDTABLE_HAS_SPINE

#include "widgets/SpinePreviewWidget.h"
#include "Theme.h"
#include "spine/SpineEngine.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QCursor>
#include <cmath>
#include <spdlog/spdlog.h>

namespace rt {

// ─────────────────────────────────────────────────────────────────────────
// Canvas transform — the ONE place that maps the 16:9 logical canvas to
// screen pixels.  Content rendering, the overlay frame, hit-testing and
// dragging all call this so they can never diverge (which previously caused
// zoom to drift the shot contents away from the shot frame).
// ─────────────────────────────────────────────────────────────────────────
void SpinePreviewWidget::computeCanvasTransform(float& originX, float& originY,
                                                float& w, float& h) const
{
    const float ww = static_cast<float>(width());
    const float wh = static_cast<float>(height());

    constexpr float kLogicalW = 1920.0f;
    constexpr float kLogicalH = 1080.0f;
    const float widgetAspect  = ww / std::max(1.0f, wh);
    const float logicalAspect = kLogicalW / kLogicalH;

    // Fit the 16:9 canvas inside the widget (letter-/pillar-box), then zoom.
    const float scaleToWidget = (widgetAspect > logicalAspect)
        ? wh / kLogicalH     // widget wider than 16:9 → fit to height
        : ww / kLogicalW;    // widget taller/equal     → fit to width

    w = kLogicalW * scaleToWidget * m_viewZoom;
    h = kLogicalH * scaleToWidget * m_viewZoom;

    // Zoom about the widget centre: (ww - w)/2 keeps the canvas centred at
    // every zoom level, then pan offsets it.  Pan is added directly so it
    // stays consistent with the cursor-anchored wheel-zoom math.
    originX = (ww - w) * 0.5f + m_viewPanX;
    originY = (wh - h) * 0.5f + m_viewPanY;
}

QRect SpinePreviewWidget::layerScreenRect(const PreviewCharLayer& layer) const
{
    int wh = height();

    float canvasOriginX, canvasOriginY, canvasW, canvasH;
    computeCanvasTransform(canvasOriginX, canvasOriginY, canvasW, canvasH);

    // â”€â”€ Background / video image layer â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (layer.isBackground) {
        if (layer.backgroundImage.isNull())
            return QRect(0, 0, 0, 0);

        float imgW = static_cast<float>(layer.backgroundImage.width());
        float imgH = static_cast<float>(layer.backgroundImage.height());

        float displayW, displayH;
        if (layer.isVideoCharacter) {
            // Character-style sizing: fit to ~85% of canvas height
            float fitScale = canvasH / imgH * 0.85f;
            float charScale = fitScale * layer.scale;
            displayW = imgW * charScale;
            displayH = imgH * charScale;
        } else {
            float scaleToFill = std::max(canvasW / imgW, canvasH / imgH);
            displayW = imgW * scaleToFill * layer.scale;
            displayH = imgH * scaleToFill * layer.scale;
        }

        float centerX = canvasOriginX + layer.posX * canvasW;
        float centerY = canvasOriginY + layer.posY * canvasH;
        return QRect(
            static_cast<int>(centerX - displayW * 0.5f),
            static_cast<int>(centerY - displayH * 0.5f),
            static_cast<int>(displayW),
            static_cast<int>(displayH)
        );
    }

    // â”€â”€ Character (Spine) layer â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    float bw = layer.boundsW;
    float bh = layer.boundsH;
    if (bw < 1.0f) bw = 400.0f;
    if (bh < 1.0f) bh = 700.0f;

    // Character visual width/height in screen pixels
    float fitScale = static_cast<float>(wh) / bh * 0.85f;
    float charScale = fitScale * layer.scale * m_viewZoom;
    float screenW = bw * charScale;
    float screenH = bh * charScale;

    // Visual center on screen â€” same as renderMultiLayer
    float screenCenterX = canvasOriginX + layer.posX * canvasW;
    float screenCenterY = canvasOriginY + layer.posY * canvasH;

    return QRect(
        static_cast<int>(screenCenterX - screenW * 0.5f),
        static_cast<int>(screenCenterY - screenH * 0.5f),
        static_cast<int>(screenW),
        static_cast<int>(screenH)
    );
}

int SpinePreviewWidget::hitTestLayer(const QPoint& pos) const
{
    // Prefer the currently selected layer â€” if the click is within its bounds,
    // keep it selected rather than switching to whichever layer is on top.
    for (size_t i = 0; i < m_layers.size(); ++i) {
        if (m_layers[i].layerIndex == m_selectedLayerIdx && m_layers[i].visible) {
            QRect r = layerScreenRect(m_layers[i]);
            if (r.contains(pos))
                return static_cast<int>(i);
            break;
        }
    }

    // Fall back: test in reverse order (front layers first)
    for (int i = static_cast<int>(m_layers.size()) - 1; i >= 0; --i) {
        const auto& layer = m_layers[static_cast<size_t>(i)];
        if (!layer.visible) continue;
        QRect r = layerScreenRect(layer);
        if (r.contains(pos))
            return i;
    }
    return -1;
}

QString SpinePreviewWidget::hitTestHandle(const QPoint& pos) const
{
    // Find the selected layer
    for (size_t i = 0; i < m_layers.size(); ++i) {
        if (m_layers[i].layerIndex == m_selectedLayerIdx) {
            QRect r = layerScreenRect(m_layers[i]);
            int s = HANDLE_SIZE;
            QRect tl(r.left() - s/2, r.top() - s/2, s, s);
            QRect tr(r.right() - s/2, r.top() - s/2, s, s);
            QRect bl(r.left() - s/2, r.bottom() - s/2, s, s);
            QRect br(r.right() - s/2, r.bottom() - s/2, s, s);
            if (tl.contains(pos)) return QStringLiteral("tl");
            if (tr.contains(pos)) return QStringLiteral("tr");
            if (bl.contains(pos)) return QStringLiteral("bl");
            if (br.contains(pos)) return QStringLiteral("br");
            break;
        }
    }
    return {};
}

QString SpinePreviewWidget::hitTestEdge(const QPoint& pos) const
{
    // Test edges of the selected layer's bounding box for Ctrl+drag crop.
    constexpr int EDGE_ZONE = 8; // pixels from edge to trigger
    for (size_t i = 0; i < m_layers.size(); ++i) {
        if (m_layers[i].layerIndex == m_selectedLayerIdx) {
            QRect r = layerScreenRect(m_layers[i]);
            if (r.width() < 10 || r.height() < 10) break;

            // Inflate slightly for the hit area
            bool inVert = (pos.y() >= r.top() - EDGE_ZONE && pos.y() <= r.bottom() + EDGE_ZONE);
            bool inHoriz = (pos.x() >= r.left() - EDGE_ZONE && pos.x() <= r.right() + EDGE_ZONE);

            // Left edge
            if (inVert && std::abs(pos.x() - r.left()) <= EDGE_ZONE)
                return QStringLiteral("left");
            // Right edge
            if (inVert && std::abs(pos.x() - r.right()) <= EDGE_ZONE)
                return QStringLiteral("right");
            // Top edge
            if (inHoriz && std::abs(pos.y() - r.top()) <= EDGE_ZONE)
                return QStringLiteral("top");
            // Bottom edge
            if (inHoriz && std::abs(pos.y() - r.bottom()) <= EDGE_ZONE)
                return QStringLiteral("bottom");
            break;
        }
    }
    return {};
}

void SpinePreviewWidget::drawTransformOverlay(QPainter& painter)
{
    painter.setRenderHint(QPainter::Antialiasing);

    // â”€â”€ Draw 16:9 canvas border â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    {
        float canvasOriginX, canvasOriginY, canvasW, canvasH;
        computeCanvasTransform(canvasOriginX, canvasOriginY, canvasW, canvasH);
        QRectF canvasRect(canvasOriginX, canvasOriginY, canvasW, canvasH);

        // Semi-transparent darkening outside the canvas area
        QPainterPath fullPath;
        fullPath.addRect(rect());
        QPainterPath canvasPath;
        canvasPath.addRect(canvasRect);
        QPainterPath outsidePath = fullPath.subtracted(canvasPath);
        painter.fillPath(outsidePath, QColor(0, 0, 0, 200));

        // Border around the canvas
        QColor borderColor = Theme::colors().textSecondary; borderColor.setAlpha(200);
        QPen borderPen(borderColor, 2);
        borderPen.setStyle(Qt::SolidLine);
        painter.setPen(borderPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(canvasRect);

        // Safe area guides (action-safe 90% + title-safe 80%)
        if (m_showSafeAreas) {
            QColor safeColor = Theme::colors().textPrimary; safeColor.setAlpha(80);
            QPen safePen(safeColor, 1, Qt::DashLine);
            painter.setPen(safePen);
            painter.setBrush(Qt::NoBrush);

            // Action safe (90% â€” 5% inset on each side)
            QRectF actionSafe = canvasRect;
            actionSafe.adjust(canvasRect.width() * 0.05, canvasRect.height() * 0.05,
                             -canvasRect.width() * 0.05, -canvasRect.height() * 0.05);
            painter.drawRect(actionSafe);

            // Title safe (80% â€” 10% inset on each side)
            QRectF titleSafe = canvasRect;
            titleSafe.adjust(canvasRect.width() * 0.1, canvasRect.height() * 0.1,
                            -canvasRect.width() * 0.1, -canvasRect.height() * 0.1);
            painter.drawRect(titleSafe);
        }
    }

    for (size_t i = 0; i < m_layers.size(); ++i) {
        const auto& layer = m_layers[i];
        if (!layer.visible) continue;

        bool isSelected = (layer.layerIndex == m_selectedLayerIdx);
        bool isMultiSelected = m_selectedLayerIndices.contains(layer.layerIndex);
        QRect r = layerScreenRect(layer);

        if (isSelected || isMultiSelected) {
            // Selected: bright accent border + corner handles (primary gets handles)
            QColor selColor = isSelected ? Theme::colors().accent
                                         : Theme::colors().accent;
            selColor.setAlpha(isSelected ? 255 : 180);
            QPen pen(selColor, 2);
            pen.setStyle(Qt::SolidLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(r);

            // Corner handles (only for primary selection)
            if (isSelected) {
                int s = HANDLE_SIZE;
                QColor handleColor = Theme::colors().accent;
                painter.setBrush(handleColor);
                painter.setPen(QPen(Qt::white, 1));
                QRect handles[4] = {
                    {r.left() - s/2, r.top() - s/2, s, s},
                    {r.right() - s/2, r.top() - s/2, s, s},
                    {r.left() - s/2, r.bottom() - s/2, s, s},
                    {r.right() - s/2, r.bottom() - s/2, s, s},
                };
                for (const auto& h : handles)
                    painter.drawRect(h);
            }

            // â”€â”€ Green crop overlay lines â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            bool hasCrop = (layer.cropLeft > 0.01f || layer.cropRight > 0.01f ||
                            layer.cropTop > 0.01f || layer.cropBottom > 0.01f);
            if (hasCrop) {
                // Crop insets in pixels
                float cL = r.width()  * (layer.cropLeft / 100.0f);
                float cR = r.width()  * (layer.cropRight / 100.0f);
                float cT = r.height() * (layer.cropTop / 100.0f);
                float cB = r.height() * (layer.cropBottom / 100.0f);

                QRectF cropRect(r.left() + cL, r.top() + cT,
                                r.width() - cL - cR, r.height() - cT - cB);

                // Semi-transparent darkened region outside the crop
                QPainterPath layerPath;
                layerPath.addRect(QRectF(r));
                QPainterPath cropPath;
                cropPath.addRect(cropRect);
                QPainterPath cropMask = layerPath.subtracted(cropPath);
                painter.fillPath(cropMask, QColor(0, 0, 0, 100));

                // Green crop boundary lines
                QPen cropPen(Theme::colors().success, 2, Qt::SolidLine);
                painter.setPen(cropPen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(cropRect);

                // Small green triangular indicators on each active crop edge
                painter.setBrush(Theme::colors().success);
                painter.setPen(Qt::NoPen);
                if (layer.cropLeft > 0.01f) {
                    float cy = cropRect.center().y();
                    QPolygonF tri;
                    tri << QPointF(cropRect.left(), cy - 5)
                        << QPointF(cropRect.left() + 6, cy)
                        << QPointF(cropRect.left(), cy + 5);
                    painter.drawPolygon(tri);
                }
                if (layer.cropRight > 0.01f) {
                    float cy = cropRect.center().y();
                    QPolygonF tri;
                    tri << QPointF(cropRect.right(), cy - 5)
                        << QPointF(cropRect.right() - 6, cy)
                        << QPointF(cropRect.right(), cy + 5);
                    painter.drawPolygon(tri);
                }
                if (layer.cropTop > 0.01f) {
                    float cx = cropRect.center().x();
                    QPolygonF tri;
                    tri << QPointF(cx - 5, cropRect.top())
                        << QPointF(cx, cropRect.top() + 6)
                        << QPointF(cx + 5, cropRect.top());
                    painter.drawPolygon(tri);
                }
                if (layer.cropBottom > 0.01f) {
                    float cx = cropRect.center().x();
                    QPolygonF tri;
                    tri << QPointF(cx - 5, cropRect.bottom())
                        << QPointF(cx, cropRect.bottom() - 6)
                        << QPointF(cx + 5, cropRect.bottom());
                    painter.drawPolygon(tri);
                }
            }
        } else {
            // Non-selected: no border (previously drew a dashed outline
            // that was visible as a 1px rectangular artefact).
        }
    }
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Mouse wheel zoom
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void SpinePreviewWidget::wheelEvent(QWheelEvent* event)
{
    if (m_layers.empty()) {
        event->ignore();
        return;
    }

    float delta = event->angleDelta().y();
    float factor = (delta > 0) ? 1.1f : 0.9f;
    float newZoom = m_viewZoom * factor;
    newZoom = std::clamp(newZoom, 0.1f, 10.0f);

    // Zoom toward mouse position â€” pan is relative to widget center
    // (computeCanvasTransform uses center-based origin: canvasOriginX = (ww - canvasW)/2 + panX)
    QPointF mousePos = event->position();
    float mx = static_cast<float>(mousePos.x()) - width()  * 0.5f;
    float my = static_cast<float>(mousePos.y()) - height() * 0.5f;

    // Adjust pan so the point under the cursor stays fixed
    float zoomRatio = newZoom / m_viewZoom;
    m_viewPanX += (1.0f - zoomRatio) * (mx - m_viewPanX);
    m_viewPanY += (1.0f - zoomRatio) * (my - m_viewPanY);

    m_viewZoom = newZoom;
    event->accept();
    update();
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Mouse press / move / release â€” drag to move, corner to scale, middle to pan
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void SpinePreviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (m_layers.empty()) {
        event->ignore();
        return;
    }

    QPoint pos = event->pos();

    // Middle button â†’ pan
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_dragStartPos = pos;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    // Ctrl + left click on edge starts a crop drag. Plain edge/corner drags
    // remain transform resizing, matching the Program Monitor.
    if (event->modifiers() & Qt::ControlModifier) {
        QString edge = hitTestEdge(pos);
        if (!edge.isEmpty()) {
            for (size_t i = 0; i < m_layers.size(); ++i) {
                if (m_layers[i].layerIndex == m_selectedLayerIdx) {
                    m_cropping = true;
                    m_cropEdge = edge;
                    m_dragLayerIdx = static_cast<int>(i);
                    m_dragStartPos = pos;
                    m_cropInitLeft   = m_layers[i].cropLeft;
                    m_cropInitRight  = m_layers[i].cropRight;
                    m_cropInitTop    = m_layers[i].cropTop;
                    m_cropInitBottom = m_layers[i].cropBottom;
                    if (edge == "left" || edge == "right")
                        setCursor(Qt::SizeHorCursor);
                    else
                        setCursor(Qt::SizeVerCursor);
                    emit dragStarted();
                    event->accept();
                    return;
                }
            }
        }
    }

    // Check for scale handle on selected layer first
    QString handle = hitTestHandle(pos);
    if (!handle.isEmpty()) {
        // Find the selected layer in m_layers
        for (size_t i = 0; i < m_layers.size(); ++i) {
            if (m_layers[i].layerIndex == m_selectedLayerIdx) {
                m_scaling = true;
                m_scaleHandle = handle;
                m_dragLayerIdx = static_cast<int>(i);
                m_dragStartPos = pos;
                m_dragInitScale = m_layers[i].scale;
                // Store initial scales for all selected layers
                m_dragInitStates.resize(m_layers.size());
                for (size_t j = 0; j < m_layers.size(); ++j) {
                    m_dragInitStates[j] = {m_layers[j].posX, m_layers[j].posY, m_layers[j].scale};
                }
                // Compute group centre (average of selected layers' positions)
                {
                    float sumX = 0, sumY = 0;
                    int count = 0;
                    for (size_t j = 0; j < m_layers.size(); ++j) {
                        if (m_selectedLayerIndices.contains(m_layers[j].layerIndex)) {
                            sumX += m_layers[j].posX;
                            sumY += m_layers[j].posY;
                            ++count;
                        }
                    }
                    if (count > 0) {
                        m_groupCenterX = sumX / static_cast<float>(count);
                        m_groupCenterY = sumY / static_cast<float>(count);
                    }
                }
                setCursor(Qt::SizeFDiagCursor);
                emit dragStarted();
                event->accept();
                return;
            }
        }
    }

    // Check for layer hit (for drag or selection change)
    int hitIdx = hitTestLayer(pos);
    if (hitIdx >= 0) {
        auto& hitLayer = m_layers[static_cast<size_t>(hitIdx)];
        m_selectedLayerIdx = hitLayer.layerIndex;

        // Photoshop-style: if the clicked layer is already part of a
        // multi-selection, keep the whole selection so the user can drag/
        // scale the group.  Only reset to single-select when clicking a
        // layer that isn't in the current selection.
        if (!m_selectedLayerIndices.contains(hitLayer.layerIndex)) {
            m_selectedLayerIndices = { hitLayer.layerIndex };
        }

        m_dragging = true;
        m_dragLayerIdx = hitIdx;
        m_dragStartPos = pos;
        m_dragInitPosX = hitLayer.posX;
        m_dragInitPosY = hitLayer.posY;
        // Store initial positions for all selected layers
        m_dragInitStates.resize(m_layers.size());
        for (size_t j = 0; j < m_layers.size(); ++j) {
            m_dragInitStates[j] = {m_layers[j].posX, m_layers[j].posY, m_layers[j].scale};
        }
        setCursor(Qt::SizeAllCursor);
        emit dragStarted();

        // Emit signal so ShotComposer updates its selection
        emit layerClicked(hitLayer.layerIndex);
        emit layerTransformChanged(hitLayer.layerIndex, hitLayer.posX, hitLayer.posY, hitLayer.scale);
        event->accept();
        update();
        return;
    }

    event->ignore();
}

void SpinePreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    QPoint pos = event->pos();

    // Pan
    if (m_panning) {
        QPoint delta = pos - m_dragStartPos;
        m_viewPanX += static_cast<float>(delta.x());
        m_viewPanY += static_cast<float>(delta.y());
        m_dragStartPos = pos;
        update();
        event->accept();
        return;
    }

    // Scale
    if (m_scaling && m_dragLayerIdx >= 0 && m_dragLayerIdx < static_cast<int>(m_layers.size())) {
        auto& layer = m_layers[static_cast<size_t>(m_dragLayerIdx)];
        QRect r = layerScreenRect(layer);

        // Distance from center of rect to current mouse vs distance at drag start
        float cx = r.center().x();
        float cy = r.center().y();
        float distNow = std::sqrt(
            (pos.x() - cx) * (pos.x() - cx) +
            (pos.y() - cy) * (pos.y() - cy));
        float distStart = std::sqrt(
            (m_dragStartPos.x() - cx) * (m_dragStartPos.x() - cx) +
            (m_dragStartPos.y() - cy) * (m_dragStartPos.y() - cy));
        if (distStart > 1.0f) {
            float ratio = distNow / distStart;
            layer.scale = std::clamp(m_dragInitScale * ratio, 0.05f, 10.0f);

            // Photoshop-style group transform: move position toward/away
            // from the group centre proportionally to the scale ratio.
            if (m_selectedLayerIndices.size() > 1 &&
                m_dragLayerIdx < static_cast<int>(m_dragInitStates.size())) {
                const auto& init = m_dragInitStates[static_cast<size_t>(m_dragLayerIdx)];
                layer.posX = m_groupCenterX + (init.posX - m_groupCenterX) * ratio;
                layer.posY = m_groupCenterY + (init.posY - m_groupCenterY) * ratio;
            }

            emit layerTransformChanged(layer.layerIndex, layer.posX, layer.posY, layer.scale);

            // Apply same scale ratio to all other selected layers
            for (size_t j = 0; j < m_layers.size(); ++j) {
                if (static_cast<int>(j) == m_dragLayerIdx) continue;
                if (!m_selectedLayerIndices.contains(m_layers[j].layerIndex)) continue;
                if (j < m_dragInitStates.size()) {
                    m_layers[j].scale = std::clamp(m_dragInitStates[j].scale * ratio, 0.05f, 10.0f);
                    // Adjust position around group centre
                    m_layers[j].posX = m_groupCenterX + (m_dragInitStates[j].posX - m_groupCenterX) * ratio;
                    m_layers[j].posY = m_groupCenterY + (m_dragInitStates[j].posY - m_groupCenterY) * ratio;
                    emit layerTransformChanged(m_layers[j].layerIndex,
                                               m_layers[j].posX, m_layers[j].posY, m_layers[j].scale);
                }
            }

            update();
        }
        event->accept();
        return;
    }

    // Crop (Ctrl+drag edge)
    if (m_cropping && m_dragLayerIdx >= 0 && m_dragLayerIdx < static_cast<int>(m_layers.size())) {
        auto& layer = m_layers[static_cast<size_t>(m_dragLayerIdx)];
        QRect r = layerScreenRect(layer);
        float rw = static_cast<float>(r.width());
        float rh = static_cast<float>(r.height());

        float dxPx = static_cast<float>(pos.x() - m_dragStartPos.x());
        float dyPx = static_cast<float>(pos.y() - m_dragStartPos.y());

        if (m_cropEdge == "left") {
            float pct = (dxPx / rw) * 100.0f;
            layer.cropLeft = std::clamp(m_cropInitLeft + pct, 0.0f, 100.0f - layer.cropRight);
        } else if (m_cropEdge == "right") {
            float pct = (-dxPx / rw) * 100.0f;
            layer.cropRight = std::clamp(m_cropInitRight + pct, 0.0f, 100.0f - layer.cropLeft);
        } else if (m_cropEdge == "top") {
            float pct = (dyPx / rh) * 100.0f;
            layer.cropTop = std::clamp(m_cropInitTop + pct, 0.0f, 100.0f - layer.cropBottom);
        } else if (m_cropEdge == "bottom") {
            float pct = (-dyPx / rh) * 100.0f;
            layer.cropBottom = std::clamp(m_cropInitBottom + pct, 0.0f, 100.0f - layer.cropTop);
        }

        emit layerCropChanged(layer.layerIndex, layer.cropLeft, layer.cropRight, layer.cropTop, layer.cropBottom);
        update();
        event->accept();
        return;
    }

    // Drag
    if (m_dragging && m_dragLayerIdx >= 0 && m_dragLayerIdx < static_cast<int>(m_layers.size())) {
        auto& layer = m_layers[static_cast<size_t>(m_dragLayerIdx)];

        // Convert pixel delta to normalized coordinate delta relative to canvas
        float canvasOriginX, canvasOriginY, canvasW, canvasH;
        computeCanvasTransform(canvasOriginX, canvasOriginY, canvasW, canvasH);
        float dxPx = static_cast<float>(pos.x() - m_dragStartPos.x());
        float dyPx = static_cast<float>(pos.y() - m_dragStartPos.y());
        float dxNorm = dxPx / canvasW;
        float dyNorm = dyPx / canvasH;

        layer.posX = std::clamp(m_dragInitPosX + dxNorm, -10.0f, 10.0f);
        layer.posY = std::clamp(m_dragInitPosY + dyNorm, -10.0f, 10.0f);

        emit layerTransformChanged(layer.layerIndex, layer.posX, layer.posY, layer.scale);

        // Apply same position delta to all other selected layers
        for (size_t j = 0; j < m_layers.size(); ++j) {
            if (static_cast<int>(j) == m_dragLayerIdx) continue;
            if (!m_selectedLayerIndices.contains(m_layers[j].layerIndex)) continue;
            if (j < m_dragInitStates.size()) {
                m_layers[j].posX = std::clamp(m_dragInitStates[j].posX + dxNorm, -10.0f, 10.0f);
                m_layers[j].posY = std::clamp(m_dragInitStates[j].posY + dyNorm, -10.0f, 10.0f);
                emit layerTransformChanged(m_layers[j].layerIndex,
                                           m_layers[j].posX, m_layers[j].posY, m_layers[j].scale);
            }
        }

        update();
        event->accept();
        return;
    }

    // Hover cursor
    if (!m_layers.empty()) {
        QString handle = hitTestHandle(pos);
        if (!handle.isEmpty()) {
            setCursor(Qt::SizeFDiagCursor);
        } else if (event->modifiers() & Qt::ControlModifier) {
            // Show the edge crop cursor only while Ctrl is held.
            QString edge = hitTestEdge(pos);
            if (edge == "left" || edge == "right")
                setCursor(Qt::SizeHorCursor);
            else if (edge == "top" || edge == "bottom")
                setCursor(Qt::SizeVerCursor);
            else if (hitTestLayer(pos) >= 0)
                setCursor(Qt::SizeAllCursor);
            else
                setCursor(Qt::ArrowCursor);
        } else if (hitTestLayer(pos) >= 0) {
            setCursor(Qt::SizeAllCursor);
        } else {
            setCursor(Qt::ArrowCursor);
        }
    }

    event->accept();
}

void SpinePreviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_panning || m_dragging || m_scaling || m_cropping) {
        m_panning = false;
        m_dragging = false;
        m_scaling = false;
        m_cropping = false;
        m_cropEdge.clear();
        m_dragLayerIdx = -1;
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    event->ignore();
}

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
