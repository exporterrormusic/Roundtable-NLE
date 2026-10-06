/*
 * FullWaveformWidget — full-file waveform display for manual matching.
 *
 * Shows the entire audio file with:
 *   - Grey-out overlay on regions already confirmed to other lines
 *   - Lighter overlay on tentatively matched regions
 *   - Click to set playhead, drag to scrub
 *   - I/O keys set in/out points (green/red markers)
 *   - Scroll-wheel to zoom, right-drag to pan
 *   - Playhead for preview playback
 */

#pragma once

#include "audio/SampleBuffer.h"

#include <QWidget>
#include <QColor>
#include <vector>
#include <utility>

namespace rt {

class FullWaveformWidget : public QWidget
{
    Q_OBJECT

public:
    explicit FullWaveformWidget(QWidget* parent = nullptr);
    ~FullWaveformWidget() override = default;

    /// Set the full audio file samples.
    void setAudio(const std::vector<float>& samples, uint32_t sampleRate);
    /// Like setAudio() without copying: the widget shares `samples`
    /// (a whole file is ~700 MB per hour).
    void setAudioShared(const SampleBuffer& samples, uint32_t sampleRate);

    /// Set regions that are already confirmed (drawn as dark grey overlay).
    void setConfirmedRegions(const std::vector<std::pair<double,double>>& regions);

    /// Set regions that are tentatively matched (drawn as lighter overlay).
    void setTentativeRegions(const std::vector<std::pair<double,double>>& regions);

    /// Get/set the currently selected region.
    void setSelection(double start, double end);
    void clearSelection();
    [[nodiscard]] bool hasSelection() const noexcept { return m_hasSelection; }
    [[nodiscard]] double selectionStart() const noexcept { return m_selStart; }
    [[nodiscard]] double selectionEnd()   const noexcept { return m_selEnd; }

    /// Playhead
    /// Zoom/pan to show [start, end] seconds (clamped to the file).
    void setViewRange(double start, double end);

    void setPlayhead(double timeSec);
    void setPlayheadVisible(bool v) { m_playheadVisible = v; update(); }
    [[nodiscard]] double playhead() const noexcept { return m_playhead; }
    [[nodiscard]] bool playheadVisible() const noexcept { return m_playheadVisible; }

    /// Duration
    [[nodiscard]] double duration() const noexcept;

    QSize sizeHint() const override { return {800, 200}; }
    QSize minimumSizeHint() const override { return {300, 120}; }

signals:
    void selectionChanged(double start, double end);
    void seekRequested(double timeSec);
    void playToggleRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    double pixelToTime(int x) const;
    int    timeToPixel(double t) const;

    // Audio data (a copy from setAudio, or shared via setAudioShared)
    SampleBuffer m_samples;
    [[nodiscard]] const SampleBuffer& pcm() const noexcept { return m_samples; }
    uint32_t           m_sampleRate{44100};

    // View window (zoom/pan)
    double m_viewStart{0.0};
    double m_viewEnd{1.0};

    // Selection
    bool   m_hasSelection{false};
    double m_selStart{0.0};
    double m_selEnd{0.0};

    // Playhead
    double m_playhead{0.0};
    bool   m_playheadVisible{false};

    // Confirmed/tentative region overlays
    std::vector<std::pair<double,double>> m_confirmedRegions;
    std::vector<std::pair<double,double>> m_tentativeRegions;

    // Interaction state
    enum DragMode { None, DragPlayhead, Panning };
    DragMode m_dragMode{None};
    double   m_dragStartTime{0.0};
    int      m_panStartX{0};
    double   m_panStartView{0.0};
};

} // namespace rt
