/*
 * SpinePreviewWidget.cpp — Software-rasterised animated Spine preview.
 *
 * Supports two modes:
 *   1. Single-engine: one SpineEngine rendered centered (Characters tab)
 *   2. Multi-layer: multiple SpineEngines composited with per-layer
 *      transforms (posX, posY, scale, flipX, opacity) for ShotComposer.
 *
 * Multi-layer mode also supports:
 *   - Photoshop-style transform overlay (bounding box + corner handles)
 *   - Mouse drag to reposition characters
 *   - Corner-drag to scale characters
 *   - Mouse-wheel zoom / middle-button pan
 */

#ifdef ROUNDTABLE_HAS_SPINE

#include "widgets/SpinePreviewWidget.h"
#include "Theme.h"

#include <QDir>
#include <QFileInfo>

#include "spine/SpineEngine.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QCursor>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <thread>

#include <spdlog/spdlog.h>

namespace rt {

// ─────────────────────────────────────────────────────────────────────────────
// Box blur helper (separable, applied 3× for Gaussian approximation)
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────

SpinePreviewWidget::SpinePreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(200, 200);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    connect(&m_timer, &QTimer::timeout, this, &SpinePreviewWidget::onTimerTick);
}

SpinePreviewWidget::~SpinePreviewWidget() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Single-engine API
// ─────────────────────────────────────────────────────────────────────────────

void SpinePreviewWidget::setSpineEngine(SpineEngine* engine)
{
    m_engine = engine;
    m_boundsCached = false;
    m_layers.clear();
}

void SpinePreviewWidget::loadTextures()
{
    m_textures.clear();
    m_boundsCached = false;
    if (!m_engine || !m_engine->isLoaded()) return;

    const auto& pages = m_engine->atlas().pages();
    const auto& atlasDir = m_engine->atlas().directory();

    for (const auto& page : pages) {
        std::string fullPath = atlasDir + "/" + page.texturePath;
        // Normalize to native separators for QImage
        QString qPath = QDir::toNativeSeparators(QString::fromStdString(fullPath));
        spdlog::info("SpinePreviewWidget: loading texture: '{}' (atlasDir='{}', texturePath='{}', exists={})",
                     qPath.toStdString(), atlasDir, page.texturePath,
                     QFileInfo(qPath).exists() ? "yes" : "no");
        QImage img(qPath);
        if (img.isNull()) {
            spdlog::warn("SpinePreviewWidget: failed to load texture: '{}' (absolute: '{}')",
                         qPath.toStdString(),
                         QFileInfo(qPath).absoluteFilePath().toStdString());
            m_textures.emplace_back();
        } else {
            img = img.convertToFormat(QImage::Format_ARGB32);
            // If the atlas page uses premultiplied alpha, the PNG pixel
            // data has RGB values pre-multiplied by A.  The rasteriser
            // expects straight-alpha textures and premultiplies during
            // compositing, so un-premultiply here to avoid double-
            // darkening at semi-transparent edges (eyes, mouth, hair).
            if (page.pma) {
                uint8_t* px = img.bits();
                const int total = img.width() * img.height();
                for (int p = 0; p < total; ++p) {
                    const uint8_t a = px[p * 4 + 3];
                    if (a > 0 && a < 255) {
                        px[p * 4 + 0] = static_cast<uint8_t>(std::min(255, px[p * 4 + 0] * 255 / a));
                        px[p * 4 + 1] = static_cast<uint8_t>(std::min(255, px[p * 4 + 1] * 255 / a));
                        px[p * 4 + 2] = static_cast<uint8_t>(std::min(255, px[p * 4 + 2] * 255 / a));
                    } else if (a == 0) {
                        px[p * 4 + 0] = 0; px[p * 4 + 1] = 0; px[p * 4 + 2] = 0;
                    }
                }
            }
            m_textures.push_back(std::move(img));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-layer API
// ─────────────────────────────────────────────────────────────────────────────

void SpinePreviewWidget::setCharacterLayers(std::vector<PreviewCharLayer> layers)
{
    m_layers = std::move(layers);
    m_engine = nullptr;
    m_textures.clear();
    m_boundsCached = false;
}

void SpinePreviewWidget::clearCharacterLayers()
{
    m_layers.clear();
}

void SpinePreviewWidget::setSelectedLayer(int layerIndex)
{
    m_selectedLayerIdx = layerIndex;
    update();
}

void SpinePreviewWidget::setSelectedLayers(const QSet<int>& layerIndices)
{
    m_selectedLayerIndices = layerIndices;
}

void SpinePreviewWidget::setCameraTransform(float zoom, float panX, float panY)
{
    m_viewZoom = zoom;
    // Convert normalized panX/panY (-1..+1) to pixel offsets
    m_viewPanX = panX * static_cast<float>(width());
    m_viewPanY = panY * static_cast<float>(height());
    update();
}

void SpinePreviewWidget::resetViewport()
{
    m_viewZoom = 1.0f;
    m_viewPanX = 0.0f;
    m_viewPanY = 0.0f;
    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// Common
// ─────────────────────────────────────────────────────────────────────────────

void SpinePreviewWidget::setBackgroundColor(const QColor& color)
{
    m_bgColor = color;
    update();
}

void SpinePreviewWidget::setBackgroundImage(const QImage& image)
{
    if (image.isNull())
        m_bgImage = QImage();
    else
        m_bgImage = image.convertToFormat(QImage::Format_ARGB32);
    update();
}

void SpinePreviewWidget::clearBackgroundImage()
{
    m_bgImage = QImage();
    update();
}

void SpinePreviewWidget::setSafeAreasVisible(bool visible)
{
    if (m_showSafeAreas == visible) return;
    m_showSafeAreas = visible;
    update();
}

void SpinePreviewWidget::startAnimation()
{
    bool hasContent = false;
    if (m_engine && m_engine->isLoaded())
        hasContent = true;
    for (auto& layer : m_layers) {
        if ((layer.engine && layer.engine->isLoaded()) || layer.videoFrameProvider)
            hasContent = true;
        if (hasContent) break;
    }
    if (!hasContent) return;

    m_elapsed.start();
    m_timer.start(16);
}

void SpinePreviewWidget::stopAnimation()
{
    m_timer.stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer
// ─────────────────────────────────────────────────────────────────────────────

void SpinePreviewWidget::onTimerTick()
{
    float dt = static_cast<float>(m_elapsed.nsecsElapsed()) / 1.0e9f;
    m_elapsed.restart();
    if (dt > 0.1f) dt = 0.016f;

    if (m_engine && m_engine->isLoaded())
        m_engine->update(dt);

    for (auto& layer : m_layers) {
        if (layer.engine && layer.engine->isLoaded())
            layer.engine->update(dt);

        // Advance video layers
        if (layer.videoFrameProvider) {
            QImage frame = layer.videoFrameProvider(dt);
            if (!frame.isNull()) {
                layer.backgroundImage = frame;
                layer.scaledBgCache = QImage();  // Invalidate scaled cache
                layer.blurredBgCache = QImage(); // Invalidate blur cache
            }
        }
    }

    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// Paint
// ─────────────────────────────────────────────────────────────────────────────

// Drag overlay

void SpinePreviewWidget::setDragOverlay(const QPixmap& pixmap, const QPoint& pos)
{
    m_dragOverlayPixmap  = pixmap;
    m_dragOverlayPos     = pos;
    m_dragOverlayVisible = true;
    update();
}

void SpinePreviewWidget::clearDragOverlay()
{
    m_dragOverlayVisible = false;
    m_dragOverlayPixmap  = QPixmap();
    update();
}

void SpinePreviewWidget::resizeEvent(QResizeEvent* event)
{
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;
    QWidget::resizeEvent(event);
    update();
    s_inResize = false;
}

void SpinePreviewWidget::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }
    QPainter painter(this);

    if (!m_layers.empty() || !m_bgImage.isNull()) {
        renderMultiLayer(painter);
    } else if (m_engine && m_engine->isLoaded()) {
        renderSingleEngine(painter);
    } else {
        painter.fillRect(rect(), m_bgColor);
        if (!m_bgImage.isNull()) {
            QImage scaled = m_bgImage.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            int dx = (width() - scaled.width()) / 2;
            int dy = (height() - scaled.height()) / 2;
            painter.drawImage(dx, dy, scaled);
        } else {
            painter.setPen(Theme::colors().textDisabled);
            painter.drawText(rect(), Qt::AlignCenter, "No character loaded");
        }
    }

    // Draw drag-and-drop overlay on top of everything
    if (m_dragOverlayVisible && !m_dragOverlayPixmap.isNull()) {
        painter.setOpacity(0.55);
        int dx = m_dragOverlayPos.x() - m_dragOverlayPixmap.width() / 2;
        int dy = m_dragOverlayPos.y() - m_dragOverlayPixmap.height() / 2;
        painter.drawPixmap(dx, dy, m_dragOverlayPixmap);
        painter.setOpacity(1.0);
    }

    --s_paintDepth;
}
} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
