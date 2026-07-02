/*
 * FullWaveformWidget.cpp — full-file waveform with region selection and
 * grey-out overlays for the ManualMatchDialog.
 */

#include "widgets/FullWaveformWidget.h"
#include "Theme.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace rt {

FullWaveformWidget::FullWaveformWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(120);
    setCursor(Qt::ArrowCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void FullWaveformWidget::setAudio(const std::vector<float>& samples, uint32_t sampleRate)
{
    m_samples    = samples;
    m_sampleRate = sampleRate;
    m_viewStart  = 0.0;
    m_viewEnd    = duration();
    m_hasSelection = false;
    update();
}

void FullWaveformWidget::setConfirmedRegions(const std::vector<std::pair<double,double>>& regions)
{
    m_confirmedRegions = regions;
    update();
}

void FullWaveformWidget::setTentativeRegions(const std::vector<std::pair<double,double>>& regions)
{
    m_tentativeRegions = regions;
    update();
}

void FullWaveformWidget::setSelection(double start, double end)
{
    m_selStart = std::min(start, end);
    m_selEnd   = std::max(start, end);
    m_hasSelection = (m_selEnd - m_selStart > 0.01);
    update();
}

void FullWaveformWidget::clearSelection()
{
    m_hasSelection = false;
    m_selStart = m_selEnd = 0.0;
    update();
}

void FullWaveformWidget::setPlayhead(double timeSec)
{
    m_playhead = timeSec;
    update();
}

double FullWaveformWidget::duration() const noexcept
{
    return (m_sampleRate > 0) ? static_cast<double>(m_samples.size()) / m_sampleRate : 0.0;
}

// ── Coordinate conversion ────────────────────────────────────────────────

double FullWaveformWidget::pixelToTime(int x) const
{
    double viewDur = m_viewEnd - m_viewStart;
    if (viewDur <= 0.0 || width() <= 0) return m_viewStart;
    return m_viewStart + static_cast<double>(x) / width() * viewDur;
}

int FullWaveformWidget::timeToPixel(double t) const
{
    double viewDur = m_viewEnd - m_viewStart;
    if (viewDur <= 0.0) return 0;
    return static_cast<int>((t - m_viewStart) / viewDur * width());
}

// ── Paint ────────────────────────────────────────────────────────────────

void FullWaveformWidget::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const auto& tc = Theme::colors();

    int w = width();
    int h = height();

    // Background
    p.fillRect(rect(), tc.waveformBg);

    if (m_samples.empty() || m_sampleRate == 0 || w <= 0) return;

    double viewDur = m_viewEnd - m_viewStart;
    if (viewDur <= 0.0) return;

    int viewStartSample = static_cast<int>(m_viewStart * m_sampleRate);
    int viewEndSample   = static_cast<int>(m_viewEnd   * m_sampleRate);
    viewStartSample = std::clamp(viewStartSample, 0, static_cast<int>(m_samples.size()));
    viewEndSample   = std::clamp(viewEndSample,   0, static_cast<int>(m_samples.size()));
    int totalViewSamples = viewEndSample - viewStartSample;
    if (totalViewSamples <= 0) return;

    double samplesPerPixel = static_cast<double>(totalViewSamples) / w;

    // Find peak for normalization
    float maxAmp = 0.001f;
    for (int i = viewStartSample; i < viewEndSample; ++i)
        maxAmp = std::max(maxAmp, std::abs(m_samples[static_cast<size_t>(i)]));

    int centerY = h / 2;

    // ── Draw waveform ────────────────────────────────────────────────
    for (int px = 0; px < w; ++px) {
        int blockStart = viewStartSample + static_cast<int>(px * samplesPerPixel);
        int blockEnd   = viewStartSample + static_cast<int>((px + 1) * samplesPerPixel);
        blockStart = std::clamp(blockStart, 0, static_cast<int>(m_samples.size()));
        blockEnd   = std::clamp(blockEnd,   blockStart, static_cast<int>(m_samples.size()));
        if (blockStart >= blockEnd) continue;

        float minVal = 0.0f, maxVal = 0.0f;
        for (int j = blockStart; j < blockEnd; ++j) {
            float s = m_samples[static_cast<size_t>(j)];
            minVal = std::min(minVal, s);
            maxVal = std::max(maxVal, s);
        }

        float normMin = minVal / maxAmp;
        float normMax = maxVal / maxAmp;
        int y1 = centerY - static_cast<int>(normMax * (centerY - 4));
        int y2 = centerY - static_cast<int>(normMin * (centerY - 4));

        // Base waveform color: muted blue-gray
        p.setPen(tc.waveformFg);
        p.drawLine(px, y1, px, y2);
    }

    // ── Confirmed region overlays (dark grey + diagonal stripes) ─────
    for (const auto& [rs, re] : m_confirmedRegions) {
        if (re <= m_viewStart || rs >= m_viewEnd) continue;
        int x1 = std::max(0, timeToPixel(rs));
        int x2 = std::min(w, timeToPixel(re));
        if (x2 <= x1) continue;

        p.fillRect(x1, 0, x2 - x1, h, QColor(0, 0, 0, 160));
        // Diagonal stripes
        p.setPen(QPen(tc.border.darker(120), 1));
        for (int sx = x1; sx < x2; sx += 10)
            p.drawLine(sx, 0, sx + h, h);
    }

    // ── Tentative region overlays (lighter) ──────────────────────────
    for (const auto& [rs, re] : m_tentativeRegions) {
        if (re <= m_viewStart || rs >= m_viewEnd) continue;
        int x1 = std::max(0, timeToPixel(rs));
        int x2 = std::min(w, timeToPixel(re));
        if (x2 <= x1) continue;

        p.fillRect(x1, 0, x2 - x1, h, QColor(0, 0, 0, 80));
        // Lighter dashed outline
        QColor tentOutline = tc.warning; tentOutline.setAlpha(120);
        p.setPen(QPen(tentOutline, 1, Qt::DashLine));
        p.drawRect(x1, 0, x2 - x1 - 1, h - 1);
    }

    // ── In/Out point markers ────────────────────────────────────────
    if (m_hasSelection) {
        int inPx  = std::max(0, timeToPixel(m_selStart));
        int outPx = std::min(w, timeToPixel(m_selEnd));

        // Dim regions outside in/out range
        if (inPx > 0)
            p.fillRect(0, 0, inPx, h - 20, QColor(0, 0, 0, 100));
        if (outPx < w)
            p.fillRect(outPx, 0, w - outPx, h - 20, QColor(0, 0, 0, 100));

        // Green IN marker bar
        constexpr int handleW = 4;
        if (inPx >= 0 && inPx < w) {
            QColor inCol = tc.success; inCol.setAlpha(220);
            p.fillRect(inPx, 0, handleW, h - 20, inCol);
            // Small "I" label
            p.setPen(tc.success);
            p.setFont(QFont("Segoe UI"));
            {
                auto fn = p.font();
                fn.setPixelSize(Theme::typography().sizeXxs);
                fn.setWeight(QFont::Bold);
                p.setFont(fn);
            }
            p.drawText(inPx + handleW + 2, 12, "I");
        }

        // Red OUT marker bar
        if (outPx > 0 && outPx <= w) {
            QColor outCol = tc.error; outCol.setAlpha(220);
            p.fillRect(outPx - handleW, 0, handleW, h - 20, outCol);
            // Small "O" label
            p.setPen(tc.error);
            p.setFont(QFont("Segoe UI"));
            {
                auto fn = p.font();
                fn.setPixelSize(Theme::typography().sizeXxs);
                fn.setWeight(QFont::Bold);
                p.setFont(fn);
            }
            p.drawText(outPx - handleW - 12, 12, "O");
        }
    }

    // ── Time ruler at bottom ─────────────────────────────────────────
    p.fillRect(0, h - 20, w, 20, tc.surface0);
    p.setPen(tc.textDisabled);
    {
        QFont rulerFont("Segoe UI");
        rulerFont.setPixelSize(Theme::typography().sizeXxs);
        p.setFont(rulerFont);
    }

    // Decide step size based on zoom level
    double stepSec = 1.0;
    if (viewDur > 120.0) stepSec = 10.0;
    else if (viewDur > 30.0) stepSec = 5.0;
    else if (viewDur > 10.0) stepSec = 2.0;
    else if (viewDur < 2.0) stepSec = 0.5;

    double t = std::ceil(m_viewStart / stepSec) * stepSec;
    while (t < m_viewEnd) {
        int px = timeToPixel(t);
        p.drawLine(px, h - 20, px, h - 16);
        int mins = static_cast<int>(t) / 60;
        double secs = t - mins * 60;
        QString label = (mins > 0)
            ? QString("%1:%2").arg(mins).arg(secs, 04, 'f', 1, QChar('0'))
            : QString("%1s").arg(secs, 0, 'f', 1);
        p.drawText(px + 3, h - 5, label);
        t += stepSec;
    }

    // ── Playhead ─────────────────────────────────────────────────────
    if (m_playheadVisible) {
        int phPx = timeToPixel(m_playhead);
        if (phPx >= 0 && phPx < w) {
            p.setPen(QPen(tc.playhead, 2));
            p.drawLine(phPx, 0, phPx, h - 20);
            QPolygon tri;
            tri << QPoint(phPx - 5, 0) << QPoint(phPx + 5, 0) << QPoint(phPx, 8);
            p.setBrush(tc.playhead);
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
        }
    }

    // ── Center line ──────────────────────────────────────────────────
    p.setPen(QPen(tc.border, 1, Qt::DotLine));
    p.drawLine(0, centerY, w, centerY);

    --s_paintDepth;
}

// ── Mouse interaction ────────────────────────────────────────────────────

void FullWaveformWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::RightButton) {
        // Right-click: start panning
        m_dragMode = Panning;
        m_panStartX = event->pos().x();
        m_panStartView = m_viewStart;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        double clickTime = pixelToTime(event->pos().x());

        // Scrub mode: set playhead and start dragging
        m_dragMode = DragPlayhead;
        m_playhead = std::clamp(clickTime, 0.0, duration());
        m_playheadVisible = true;
        emit seekRequested(m_playhead);
        update();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void FullWaveformWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragMode == DragPlayhead) {
        // Scrub: continuously update playhead as user drags
        double t = std::clamp(pixelToTime(event->pos().x()), 0.0, duration());
        m_playhead = t;
        m_playheadVisible = true;
        emit seekRequested(m_playhead);
        update();
        event->accept();
        return;
    }

    if (m_dragMode == Panning) {
        int dx = event->pos().x() - m_panStartX;
        double viewDur = m_viewEnd - m_viewStart;
        double timeDelta = -dx * viewDur / width();
        double newStart = m_panStartView + timeDelta;
        double dur = duration();
        newStart = std::clamp(newStart, 0.0, std::max(0.0, dur - viewDur));
        m_viewStart = newStart;
        m_viewEnd   = newStart + viewDur;
        update();
        event->accept();
        return;
    }

    QWidget::mouseMoveEvent(event);
}

void FullWaveformWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragMode == DragPlayhead && event->button() == Qt::LeftButton) {
        m_dragMode = None;
        event->accept();
        return;
    }

    if (m_dragMode == Panning && event->button() == Qt::RightButton) {
        m_dragMode = None;
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void FullWaveformWidget::wheelEvent(QWheelEvent* event)
{
    double viewDur = m_viewEnd - m_viewStart;
    double dur     = duration();

    if (event->modifiers() & Qt::ControlModifier) {
        // Ctrl + wheel: zoom centered on cursor position
        double zoomFactor = (event->angleDelta().y() > 0) ? 0.8 : 1.25;
        double cursorTime = pixelToTime(static_cast<int>(event->position().x()));

        double newDur = viewDur * zoomFactor;
        newDur = std::clamp(newDur, 0.5, dur);

        double ratio = (cursorTime - m_viewStart) / viewDur;
        m_viewStart = cursorTime - ratio * newDur;
        m_viewEnd   = m_viewStart + newDur;
    } else if (event->modifiers() & Qt::ShiftModifier) {
        // Shift + wheel: horizontal scroll (pan left/right)
        double panAmount = viewDur * 0.15;
        if (event->angleDelta().y() > 0)
            panAmount = -panAmount;

        m_viewStart += panAmount;
        m_viewEnd   += panAmount;
    } else {
        // Plain wheel: vertical pan (scroll up = earlier, scroll down = later)
        double panAmount = viewDur * 0.15;
        if (event->angleDelta().y() > 0)
            panAmount = -panAmount;

        m_viewStart += panAmount;
        m_viewEnd   += panAmount;
    }

    // Clamp to bounds
    if (m_viewStart < 0.0) { m_viewEnd -= m_viewStart; m_viewStart = 0.0; }
    if (m_viewEnd > dur) { m_viewStart -= (m_viewEnd - dur); m_viewEnd = dur; }
    m_viewStart = std::max(0.0, m_viewStart);

    update();
    event->accept();
}

} // namespace rt
