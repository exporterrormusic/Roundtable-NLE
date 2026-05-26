/*
 * WaveformDisplayWidget.h -- Internal waveform display for SourceMonitor.
 *
 * Q_OBJECT widget that renders audio waveform peaks with a playhead overlay.
 * Used by both SourceMonitor.cpp and SourceMonitorUI.cpp.
 */

#pragma once

#include "Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace rt {

class WaveformDisplayWidget : public QWidget
{
public:
    explicit WaveformDisplayWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setStyleSheet(QStringLiteral("background: %1;")
            .arg(Theme::hex(Theme::colors().surface0)));
        setCursor(Qt::PointingHandCursor);
    }

    void setPeaks(std::vector<float> peaks, uint16_t channels)
    {
        m_peaks = std::move(peaks);
        m_channels = channels;
        update();
    }

    static double clampRatio(double value)
    {
        return std::max(0.0, std::min(1.0, value));
    }

    void setPlayheadRatio(double ratio)
    {
        m_playheadRatio = clampRatio(ratio);
        update();
    }

    void setSelectionRange(double startRatio, double endRatio)
    {
        m_selectionStartRatio = clampRatio(startRatio);
        m_selectionEndRatio = clampRatio(endRatio);
        if (m_selectionEndRatio < m_selectionStartRatio)
            std::swap(m_selectionStartRatio, m_selectionEndRatio);
        update();
    }

    void clearSelection()
    {
        m_selectionStartRatio = 0.0;
        m_selectionEndRatio = 0.0;
        update();
    }

    void clear()
    {
        m_peaks.clear();
        m_channels = 1;
        m_playheadRatio = 0.0;
        clearSelection();
        update();
    }

    void setScrubCallback(std::function<void(double)> cb) { m_scrubCallback = std::move(cb); }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && width() > 0) {
            m_dragging = true;
            double ratio = clampRatio(static_cast<double>(event->pos().x()) / width());
            if (m_scrubCallback) m_scrubCallback(ratio);
            event->accept();
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_dragging && width() > 0) {
            double ratio = clampRatio(static_cast<double>(event->pos().x()) / width());
            if (m_scrubCallback) m_scrubCallback(ratio);
            event->accept();
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton)
            m_dragging = false;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);

        const int w = width();
        const int h = height();

        // Background
        p.fillRect(rect(), Theme::colors().surface0);

        if (m_selectionEndRatio > m_selectionStartRatio && w > 0 && h > 0) {
            int selX0 = static_cast<int>(std::floor(m_selectionStartRatio * w));
            int selX1 = static_cast<int>(std::ceil(m_selectionEndRatio * w));
            selX0 = std::clamp(selX0, 0, w);
            selX1 = std::clamp(selX1, 0, w);
            if (selX1 > selX0) {
                QColor fillColor = Theme::colors().waveformSelection;
                fillColor.setAlpha(60);
                p.fillRect(QRect(selX0, 0, selX1 - selX0, h), fillColor);
            }
        }

        if (m_peaks.empty() || w < 2 || h < 2) {
            // "Loading waveform..." text
            p.setPen(Theme::colors().textSecondary);
            p.drawText(rect(), Qt::AlignCenter, tr("Loading waveform..."));
            return;
        }

        const int numPeaks = static_cast<int>(m_peaks.size());
        const float centerY = h * 0.5f;
        const float halfH = h * 0.45f; // leave some margin

        // Map peaks to pixel columns
        const float peaksPerPixel = static_cast<float>(numPeaks) / static_cast<float>(w);

        QColor waveColor(0x3d, 0xa0, 0xf5);    // blue
        QColor waveDimColor(0x26, 0x6a, 0xb0);  // darker blue for low amplitude

        for (int x = 0; x < w; ++x) {
            int p0 = static_cast<int>(x * peaksPerPixel);
            int p1 = static_cast<int>((x + 1) * peaksPerPixel);
            p0 = std::clamp(p0, 0, numPeaks - 1);
            p1 = std::clamp(p1, p0 + 1, numPeaks);

            float maxVal = 0.0f;
            for (int i = p0; i < p1; ++i)
                maxVal = std::max(maxVal, m_peaks[i]);

            float barH = maxVal * halfH;
            if (barH < 0.5f) barH = 0.5f;

            // Gradient based on amplitude
            QColor col = (maxVal > 0.5f) ? waveColor : waveDimColor;
            p.setPen(col);
            p.drawLine(x, static_cast<int>(centerY - barH),
                       x, static_cast<int>(centerY + barH));
        }

        // Center line
        p.setPen(QPen(Theme::colors().textSecondary, 1));
        p.drawLine(0, static_cast<int>(centerY), w, static_cast<int>(centerY));

        // Playhead
        if (w > 0 && m_playheadRatio >= 0.0 && m_playheadRatio <= 1.0) {
            int px = static_cast<int>(std::round(m_playheadRatio * static_cast<double>(std::max(0, w - 1))));
            px = std::clamp(px, 0, std::max(0, w - 1));
            p.setPen(QPen(QColor(0xff, 0xff, 0xff), 2));
            p.drawLine(px, 0, px, h);
        }
    }

private:
    std::vector<float> m_peaks;
    uint16_t m_channels{1};
    double m_playheadRatio{0.0};
    double m_selectionStartRatio{0.0};
    double m_selectionEndRatio{0.0};
    bool m_dragging{false};
    std::function<void(double)> m_scrubCallback;
};

} // namespace rt
