/*
 * ScopesPanel.cpp — Async scope analysis & rendering (MLT/Shotcut pattern).
 *
 * All O(w×h) pixel analysis and scope visualization rendering runs on a
 * dedicated worker thread.  The UI thread's paintEvent() simply blits a
 * pre-rendered QImage, taking < 1ms.  Scope updates are throttled to ~15fps.
 *
 * Architecture mirrors how MLT's consumer-frame-show event works:
 *   1. Frame data arrives (from any thread) → copied to pending buffer
 *   2. Worker thread wakes, runs analysis, renders QImage
 *   3. QImage delivered to UI via QueuedConnection → paintEvent blits it
 */

#include "panels/monitors/ScopesPanel.h"
#include "Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace rt {

// ─── helpers ─────────────────────────────────────────────────────────────────

static inline uint8_t lumaFromBGRA(const uint8_t* px)
{
    // BT.709 luma — px is B,G,R,A
    return static_cast<uint8_t>(0.0722 * px[0] + 0.7152 * px[1] + 0.2126 * px[2] + 0.5);
}

// ─── constructor / destructor ────────────────────────────────────────────────

ScopesPanel::ScopesPanel(QWidget* parent)
    : QWidget(parent)
{
    const auto& tc = Theme::colors();
    const auto& m  = Theme::metrics();

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ── Toolbar ──────────────────────────────────────────────────────────
    auto* toolbar = new QWidget(this);
    toolbar->setFixedHeight(28);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* topBar = new QHBoxLayout(toolbar);
    topBar->setContentsMargins(m.spacingXs, m.spacingXxs, m.spacingXs, m.spacingXxs);
    topBar->setSpacing(m.spacingXs);

    auto* label = new QLabel(tr("Video Scopes"), toolbar);
    label->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; font-size: 12px; background: transparent; border: none; }")
        .arg(Theme::hex(tc.textPrimary)));
    topBar->addWidget(label);
    topBar->addStretch();

    m_modeCombo = new QComboBox(toolbar);
    m_modeCombo->addItem(tr("Waveform"));
    m_modeCombo->addItem(tr("Vectorscope"));
    m_modeCombo->addItem(tr("Histogram"));
    m_modeCombo->setMinimumWidth(140);
    m_modeCombo->setCurrentIndex(static_cast<int>(m_mode));
    m_modeCombo->setStyleSheet(
        QString("QComboBox { background: %1; color: %2; border: 1px solid %3; "
                "border-radius: %4px; padding: 2px 6px; font-size: 12px; }"
                "QComboBox:focus { border: 1px solid %5; }"
                "QComboBox::drop-down { border: none; }"
                "QComboBox QAbstractItemView { background: %1; color: %2; "
                "selection-background-color: %6; border: 1px solid %3; }")
            .arg(Theme::hex(tc.inputBg), Theme::hex(tc.text),
                 Theme::hex(tc.controlBorder),
                 QString::number(m.radiusSm),
                 Theme::hex(tc.accent),
                 Theme::hex(tc.accentSubtle)));
    topBar->addWidget(m_modeCombo);

    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        m_mode = static_cast<ScopeMode>(idx);
        updateWorkerConfiguration(m_mode, size());
        emit scopeModeChanged(idx);
        if (m_statusLabel)
            m_statusLabel->setText(m_modeCombo->currentText());
        update();
    });

    mainLayout->addWidget(toolbar);
    mainLayout->addStretch();

    // ── Status bar ───────────────────────────────────────────────────────
    auto* statusBar = new QWidget(this);
    statusBar->setFixedHeight(22);
    statusBar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-top: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(m.spacingMd, 0, m.spacingMd, 0);
    statusLayout->setSpacing(0);

    m_statusLabel = new QLabel(tr("Waveform"), statusBar);
    m_statusLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 12px; background: transparent; border: none; }")
            .arg(Theme::hex(tc.textSecondary)));
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addStretch();
    mainLayout->addWidget(statusBar);

    setMinimumSize(200, 160);
    updateWorkerConfiguration(m_mode, size());

    // Connect the worker's output signal to the UI thread via QueuedConnection.
    // This is the core of the MLT pattern: heavy work on background thread,
    // trivial blit on UI thread.
    connect(this, &ScopesPanel::scopeImageReady,
            this, &ScopesPanel::onScopeImageReady,
            Qt::QueuedConnection);

    startWorker();
}

ScopesPanel::~ScopesPanel()
{
    stopWorker();
}

// ─── public API ──────────────────────────────────────────────────────────────

void ScopesPanel::setScopeMode(ScopeMode mode)
{
    if (mode < Waveform || mode >= ModeCount || mode == m_mode) return;
    m_modeCombo->setCurrentIndex(static_cast<int>(mode));
}

QSize ScopesPanel::sizeHint() const { return {320, 260}; }

// ─── feedFrame (called from UI thread — just copies data, never blocks) ─────

void ScopesPanel::feedFrame(const uint8_t* pixels, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0) return;

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixelCount > std::numeric_limits<size_t>::max() / 4) return;
    const size_t dataSize = pixelCount * 4;

    // The throttle timestamp shares the worker lock because callers may feed
    // frames concurrently. Capture the complete render request with the frame.
    {
        std::lock_guard lock(m_workerMtx);
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastFeedTime).count() < kMinFeedIntervalMs) {
            return;
        }
        m_lastFeedTime = now;

        m_pendingPixels.resize(dataSize);
        std::memcpy(m_pendingPixels.data(), pixels, dataSize);
        m_pendingWidth  = width;
        m_pendingHeight = height;
        m_pendingMode = m_requestedMode;
        m_pendingRenderWidth = m_requestedRenderWidth;
        m_pendingRenderHeight = m_requestedRenderHeight;
        m_pendingRevision = m_configRevision;
        m_hasPending    = true;
    }
    m_workerCv.notify_one();
}

// ─── Worker thread ───────────────────────────────────────────────────────────

void ScopesPanel::startWorker()
{
    if (m_workerRunning.load()) return;
    m_workerRunning.store(true);
    m_worker = std::thread(&ScopesPanel::workerLoop, this);
}

void ScopesPanel::stopWorker()
{
    if (!m_workerRunning.load()) return;
    m_workerRunning.store(false);
    m_workerCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void ScopesPanel::workerLoop()
{
    spdlog::info("[SCOPES] Worker thread started");

    // Local buffer to avoid holding the lock during analysis
    std::vector<uint8_t> localPixels;
    int localW = 0, localH = 0;
    ScopeMode localMode = Waveform;
    int localRenderW = 200, localRenderH = 160;
    quint64 localRevision = 0;

    while (m_workerRunning.load()) {
        // Wait for new frame data
        {
            std::unique_lock lock(m_workerMtx);
            m_workerCv.wait(lock, [this] {
                return m_hasPending || !m_workerRunning.load();
            });
            if (!m_workerRunning.load()) break;

            // Swap pending data to local (fast, lock held briefly)
            localPixels.swap(m_pendingPixels);
            localW = m_pendingWidth;
            localH = m_pendingHeight;
            localMode = m_pendingMode;
            localRenderW = m_pendingRenderWidth;
            localRenderH = m_pendingRenderHeight;
            localRevision = m_pendingRevision;
            m_hasPending = false;
        }

        if (localPixels.empty() || localW <= 0 || localH <= 0)
            continue;

        // ── Phase 1: Analyze pixels (O(w×h) — runs on worker thread) ────
        switch (localMode) {
        case Waveform:    analyzeWaveform(localPixels.data(), localW, localH);    break;
        case Vectorscope: analyzeVectorscope(localPixels.data(), localW, localH); break;
        case Histogram:   analyzeHistogram(localPixels.data(), localW, localH);   break;
        default: break;
        }

        // ── Phase 2: Render scope visualization to QImage ────────────────
        // This replaces the per-pixel QPainter::drawPoint() calls that were
        // taking 10-20ms on the UI thread.  We render to a QImage here on
        // the worker thread, then the UI thread's paintEvent() just blits it.
        const int imgW = std::clamp(localRenderW, 200, 1024);
        const int imgH = std::clamp(localRenderH, 160, 800);

        QImage scopeImg;
        switch (localMode) {
        case Waveform:    scopeImg = renderWaveform(imgW, imgH);    break;
        case Vectorscope: scopeImg = renderVectorscope(imgW, imgH); break;
        case Histogram:   scopeImg = renderHistogram(imgW, imgH);   break;
        default: break;
        }

        if (!scopeImg.isNull()) {
            // Deliver to UI thread via QueuedConnection (< 0.1ms)
            emit scopeImageReady(std::move(scopeImg), localRevision);
        }
    }

    spdlog::info("[SCOPES] Worker thread stopped");
}

// ─── UI thread: receive pre-rendered image ───────────────────────────────────

void ScopesPanel::onScopeImageReady(QImage image, quint64 revision)
{
    {
        std::lock_guard lock(m_workerMtx);
        if (revision != m_configRevision)
            return;
    }
    m_renderedScope = std::move(image);
    update(); // schedule repaint — paintEvent will just blit
}

void ScopesPanel::updateWorkerConfiguration(ScopeMode mode, const QSize& renderSize)
{
    const int renderWidth = std::max(1, renderSize.width());
    const int renderHeight = std::max(1, renderSize.height());

    std::lock_guard lock(m_workerMtx);
    if (m_requestedMode == mode
        && m_requestedRenderWidth == renderWidth
        && m_requestedRenderHeight == renderHeight) {
        return;
    }

    m_requestedMode = mode;
    m_requestedRenderWidth = renderWidth;
    m_requestedRenderHeight = renderHeight;
    ++m_configRevision;
    m_renderedScope = {};
}

// ─── paintEvent (UI thread — just blits the pre-rendered image, < 1ms) ──────

void ScopesPanel::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }
    const auto& tc = Theme::colors();
    QPainter p(this);
    p.fillRect(rect(), tc.surface0);

    if (m_renderedScope.isNull()) {
        const int topMargin = 30;
        QRect area(4, topMargin, width() - 8, height() - topMargin - 26);
        p.setPen(tc.textDisabled);
        p.drawText(area, Qt::AlignCenter, tr("No frame data"));
        --s_paintDepth;
        return;
    }

    // Blit the pre-rendered scope image (scaled to current widget size)
    const int topMargin = 30;
    QRect area(4, topMargin, width() - 8, height() - topMargin - 26);
    p.drawImage(area, m_renderedScope);

    --s_paintDepth;
}

void ScopesPanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateWorkerConfiguration(m_mode, event->size());
}

// ═════════════════════════════════════════════════════════════════════════════
//  Analysis (runs on worker thread)
// ═════════════════════════════════════════════════════════════════════════════

void ScopesPanel::analyzeWaveform(const uint8_t* pixels, int w, int h)
{
    std::memset(m_waveformData.data(), 0, sizeof(m_waveformData));
    m_waveformMax = 1;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int col = x * kWaveformCols / w;
            col = std::clamp(col, 0, kWaveformCols - 1);
            const uint8_t* px = pixels + (y * w + x) * 4;
            uint8_t lum = lumaFromBGRA(px);
            auto& val = m_waveformData[col][lum];
            if (val < 0xFFFF) ++val;
            if (val > m_waveformMax) m_waveformMax = val;
        }
    }
}

void ScopesPanel::analyzeVectorscope(const uint8_t* pixels, int w, int h)
{
    std::memset(m_vectorscopeData.data(), 0, sizeof(m_vectorscopeData));
    m_vectorscopeMax = 1;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* px = pixels + (y * w + x) * 4;
            float r = px[2] / 255.0f;
            float g = px[1] / 255.0f;
            float b = px[0] / 255.0f;

            float cb = -0.1687f * r - 0.3313f * g + 0.5f * b;
            float cr =  0.5f    * r - 0.4187f * g - 0.0813f * b;

            int ix = static_cast<int>((cb + 0.5f) * 255.0f);
            int iy = static_cast<int>((0.5f - cr) * 255.0f);
            ix = std::clamp(ix, 0, kVectorscopeSize - 1);
            iy = std::clamp(iy, 0, kVectorscopeSize - 1);

            auto& val = m_vectorscopeData[iy][ix];
            if (val < 0xFFFF) ++val;
            if (val > m_vectorscopeMax) m_vectorscopeMax = val;
        }
    }
}

void ScopesPanel::analyzeHistogram(const uint8_t* pixels, int w, int h)
{
    std::memset(m_histR.data(), 0, sizeof(m_histR));
    std::memset(m_histG.data(), 0, sizeof(m_histG));
    std::memset(m_histB.data(), 0, sizeof(m_histB));
    std::memset(m_histLuma.data(), 0, sizeof(m_histLuma));
    m_histMax = 1;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* px = pixels + (y * w + x) * 4;
            m_histR[px[2]]++;
            m_histG[px[1]]++;
            m_histB[px[0]]++;
            m_histLuma[lumaFromBGRA(px)]++;
        }
    }

    for (int i = 0; i < 256; ++i) {
        m_histMax = std::max(m_histMax, m_histR[i]);
        m_histMax = std::max(m_histMax, m_histG[i]);
        m_histMax = std::max(m_histMax, m_histB[i]);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Rendering to QImage (runs on worker thread — replaces per-pixel drawPoint)
// ═════════════════════════════════════════════════════════════════════════════

QImage ScopesPanel::renderWaveform(int imgW, int imgH)
{
    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(0x0a, 0x0a, 0x0a));

    QPainter p(&img);

    // Graticule lines
    p.setPen(QPen(QColor(0x30, 0x30, 0x30), 1, Qt::DashLine));
    for (float pct : {0.1f, 0.5f, 0.9f}) {
        int y = imgH - 1 - static_cast<int>(pct * imgH);
        p.drawLine(0, y, imgW - 1, y);
    }

    // Use direct pixel writing instead of per-pixel drawPoint (10-100x faster)
    const float logMax = std::log2(static_cast<float>(m_waveformMax) + 1.0f);

    for (int col = 0; col < kWaveformCols; ++col) {
        int px = col * imgW / kWaveformCols;
        if (px < 0 || px >= imgW) continue;

        for (int lev = 0; lev < kWaveformLevels; ++lev) {
            uint16_t count = m_waveformData[col][lev];
            if (count == 0) continue;

            float intensity = std::log2(static_cast<float>(count) + 1.0f) / logMax;
            intensity = std::clamp(intensity, 0.0f, 1.0f);

            int py = imgH - 1 - lev * imgH / kWaveformLevels;
            if (py < 0 || py >= imgH) continue;

            int alpha = static_cast<int>(intensity * 220) + 35;
            int g = static_cast<int>(220 * intensity);
            img.setPixelColor(px, py, QColor(static_cast<int>(80 * intensity),
                                              g, static_cast<int>(80 * intensity),
                                              alpha));
        }
    }

    return img;
}

QImage ScopesPanel::renderVectorscope(int imgW, int imgH)
{
    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(0x0a, 0x0a, 0x0a));

    QPainter p(&img);
    int side = std::min(imgW, imgH);
    int ox = (imgW - side) / 2;
    int oy = (imgH - side) / 2;
    QRect square(ox, oy, side, side);

    // Graticule
    p.setPen(QPen(QColor(0x33, 0x33, 0x33), 1));
    p.drawEllipse(square);
    for (float s : {0.25f, 0.5f, 0.75f}) {
        int r = static_cast<int>(side * s / 2);
        p.drawEllipse(square.center(), r, r);
    }
    p.drawLine(square.center().x(), square.top(), square.center().x(), square.bottom());
    p.drawLine(square.left(), square.center().y(), square.right(), square.center().y());

    // Skin tone line
    {
        float angle = 123.0f * 3.14159265f / 180.0f;
        int dx = static_cast<int>(std::cos(angle) * side / 2);
        int dy = static_cast<int>(-std::sin(angle) * side / 2);
        p.setPen(QPen(QColor(0x80, 0x60, 0x40), 1, Qt::DashLine));
        p.drawLine(square.center(), QPoint(square.center().x() + dx, square.center().y() + dy));
    }

    p.end(); // End QPainter before direct pixel writes

    // Plot data via direct pixel access (avoids per-pixel setPen+drawPoint)
    const float logMax = std::log2(static_cast<float>(m_vectorscopeMax) + 1.0f);
    for (int iy = 0; iy < kVectorscopeSize; ++iy) {
        for (int ix = 0; ix < kVectorscopeSize; ++ix) {
            uint16_t count = m_vectorscopeData[iy][ix];
            if (count == 0) continue;

            float intensity = std::log2(static_cast<float>(count) + 1.0f) / logMax;
            intensity = std::clamp(intensity, 0.0f, 1.0f);

            int px = ox + ix * side / kVectorscopeSize;
            int py = oy + iy * side / kVectorscopeSize;
            if (px < 0 || px >= imgW || py < 0 || py >= imgH) continue;

            int alpha = static_cast<int>(intensity * 220) + 35;
            float cb = (ix / 255.0f - 0.5f) * 2.0f;
            float cr = (0.5f - iy / 255.0f) * 2.0f;
            int rr = std::clamp(static_cast<int>(128 + cr * 127), 60, 255);
            int bb = std::clamp(static_cast<int>(128 + cb * 127), 60, 255);
            int gg = std::clamp(255 - (rr + bb) / 2, 40, 200);
            img.setPixelColor(px, py, QColor(rr, gg, bb, alpha));
        }
    }

    return img;
}

QImage ScopesPanel::renderHistogram(int imgW, int imgH)
{
    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(0x0a, 0x0a, 0x0a));

    QPainter p(&img);
    QRect area(0, 0, imgW, imgH);

    auto drawChannel = [&](const std::array<uint32_t, 256>& hist, const QColor& color) {
        QPainterPath path;
        path.moveTo(area.left(), area.bottom());
        for (int i = 0; i < 256; ++i) {
            int x = area.left() + i * area.width() / 256;
            float h = static_cast<float>(hist[i]) / static_cast<float>(m_histMax);
            int y = area.bottom() - static_cast<int>(h * area.height());
            path.lineTo(x, y);
        }
        path.lineTo(area.right(), area.bottom());
        path.closeSubpath();

        QColor fill = color;
        fill.setAlpha(50);
        p.fillPath(path, fill);

        QColor stroke = color;
        stroke.setAlpha(180);
        p.setPen(QPen(stroke, 1));
        p.drawPath(path);
    };

    drawChannel(m_histR, QColor(220, 50, 50));
    drawChannel(m_histG, QColor(50, 200, 50));
    drawChannel(m_histB, QColor(50, 80, 220));
    drawChannel(m_histLuma, QColor(200, 200, 200));

    return img;
}

} // namespace rt
