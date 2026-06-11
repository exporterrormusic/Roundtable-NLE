/*
 * WaveformWidget — Qt widget for displaying audio waveforms.
 *
 * Step 19: GPU Waveform Renderer
 *
 * Renders waveform peak data from WaveformCache using QPainter.
 * Supports:
 *   - Multi-channel display (mono/stereo)
 *   - Zoom (scroll wheel) and pan
 *   - Playhead with glow effect
 *   - Segment markers (highlighted active segment)
 *   - Time grid overlay
 *   - Click/drag scrubbing to set playhead position
 *   - Gradient coloring (green → yellow → red by amplitude)
 *   - Automatic LOD via WaveformCache mip levels
 *
 * Layout:
 *   ┌──────────────────────────────────────────────────┐
 *   │  ▏                                              │
 *   │  ▏   ▃▅▇█▇▅▃▁   ▃▅▇▅▃▁▃▅▇█████▇▅▃▁            │
 *   │  ▏—————————————————————————————————————————     │
 *   │  ▏   ▃▅▇█▇▅▃▁   ▃▅▇▅▃▁▃▅▇█████▇▅▃▁            │
 *   │  ▏                                              │
 *   │  0:00      0:05      0:10      0:15             │
 *   └──────────────────────────────────────────────────┘
 */

#pragma once

#include "audio/WaveformCache.h"

#include <QColor>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace rt {

/// Segment marker for waveform display
struct WaveformSegment
{
    int64_t startFrame{0};
    int64_t endFrame{0};
    QString label;
    bool    active{false};
};

class WaveformWidget : public QWidget
{
    Q_OBJECT

public:
    explicit WaveformWidget(QWidget* parent = nullptr);
    ~WaveformWidget() override;

    // ── Data source ─────────────────────────────────────────────────────

    /// Set the waveform data to display.
    void setWaveformData(const WaveformData* data);

    /// Get the currently bound waveform data.
    [[nodiscard]] const WaveformData* waveformData() const noexcept { return m_waveformData; }

    // ── View control ────────────────────────────────────────────────────

    /// Set the visible range in sample frames.
    void setVisibleRange(int64_t startFrame, int64_t endFrame);

    /// Get the visible start frame.
    [[nodiscard]] int64_t visibleStartFrame() const noexcept { return m_visibleStart; }

    /// Get the visible end frame.
    [[nodiscard]] int64_t visibleEndFrame() const noexcept { return m_visibleEnd; }

    /// Zoom in/out centered on the current view.
    void zoomIn();
    void zoomOut();

    /// Zoom to show the entire waveform.
    void zoomToFit();

    /// Get the zoom level (samples per pixel).
    [[nodiscard]] double samplesPerPixel() const noexcept { return m_samplesPerPixel; }

    // ── Playhead ────────────────────────────────────────────────────────

    /// Set the playhead position in sample frames.
    void setPlayheadFrame(int64_t frame);

    /// Get the playhead position.
    [[nodiscard]] int64_t playheadFrame() const noexcept { return m_playheadFrame; }

    /// Show/hide the playhead.
    void setPlayheadVisible(bool visible);
    [[nodiscard]] bool isPlayheadVisible() const noexcept { return m_playheadVisible; }

    // ── Segments ────────────────────────────────────────────────────────

    /// Set segment markers.
    void setSegments(const std::vector<WaveformSegment>& segments);

    /// Get segments.
    [[nodiscard]] const std::vector<WaveformSegment>& segments() const noexcept
    {
        return m_segments;
    }

    /// Set the active segment index (-1 for none).
    void setActiveSegment(int index);
    [[nodiscard]] int activeSegment() const noexcept { return m_activeSegment; }

    // ── Appearance ──────────────────────────────────────────────────────

    /// Set the background color.
    void setBackgroundColor(const QColor& color);
    [[nodiscard]] QColor backgroundColor() const noexcept { return m_bgColor; }

    /// Set the waveform gradient colors.
    void setWaveformColors(const QColor& low, const QColor& mid, const QColor& high);

    /// Set the playhead color.
    void setPlayheadColor(const QColor& color);
    [[nodiscard]] QColor playheadColor() const noexcept { return m_playheadColor; }

    /// Show/hide time grid.
    void setTimeGridVisible(bool visible);
    [[nodiscard]] bool isTimeGridVisible() const noexcept { return m_timeGridVisible; }

    /// Set number of channels to display (0 = auto from data).
    void setChannelCount(int channels);
    [[nodiscard]] int channelCount() const noexcept { return m_channelCount; }

    // ── Size hints ──────────────────────────────────────────────────────
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // ── Coordinate conversion ───────────────────────────────────────────

    /// Convert a sample frame to a pixel X coordinate.
    [[nodiscard]] double frameToPixelX(int64_t frame) const;

    /// Convert a pixel X coordinate to a sample frame.
    [[nodiscard]] int64_t pixelXToFrame(double x) const;

signals:
    /// Emitted when the user clicks/drags to set a position.
    void positionChanged(int64_t frame);

    /// Emitted when a segment is clicked.
    void segmentClicked(int index);

    /// Emitted when the visible range changes (zoom/pan).
    void visibleRangeChanged(int64_t startFrame, int64_t endFrame);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void paintWaveform(QPainter& painter);
    void paintPlayhead(QPainter& painter);
    void paintSegments(QPainter& painter);
    void paintTimeGrid(QPainter& painter);
    void updateSamplesPerPixel();

    QColor gradientColor(float amplitude) const;

    // ── Data ────────────────────────────────────────────────────────────
    const WaveformData* m_waveformData = nullptr;
    std::vector<WaveformSegment> m_segments;

    // ── View state ──────────────────────────────────────────────────────
    int64_t  m_visibleStart{0};
    int64_t  m_visibleEnd{0};
    double   m_samplesPerPixel{256.0};
    int64_t  m_playheadFrame{0};
    bool     m_playheadVisible{true};
    int      m_activeSegment{-1};
    int      m_channelCount{0};           ///< 0 = auto from data

    // ── Interaction ─────────────────────────────────────────────────────
    bool     m_scrubbing{false};
    QPoint   m_lastMousePos;

    // ── Appearance ──────────────────────────────────────────────────────
    QColor   m_bgColor{20, 20, 25};
    QColor   m_colorLow{0, 200, 0};       ///< Green
    QColor   m_colorMid{230, 230, 0};     ///< Yellow
    QColor   m_colorHigh{255, 50, 0};     ///< Red
    QColor   m_playheadColor{255, 60, 60};
    bool     m_timeGridVisible{true};
};

} // namespace rt
