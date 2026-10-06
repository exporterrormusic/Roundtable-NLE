/*
 * MiniWaveformWidget — compact waveform display with trim handles for clip cards.
 *
 * Features:
 *   - Renders waveform from raw float samples
 *   - Green in-handle, red out-handle (draggable)
 *   - Dimmed regions outside trim range
 *   - Playhead (yellow line)
 *   - Silence indicator bars (thin red)
 *   - Click to seek
 *   - Shift+drag to mark delete/crop regions (gray striped)
 *   - Double-click on delete region to restore
 *   - Drag-past-edge auto-expands view
 */

#pragma once

#include "audio/SampleBuffer.h"

#include <QWidget>
#include <QColor>
#include <QKeyEvent>

#include <cstdint>
#include <vector>
#include <utility>

namespace rt {

class MiniWaveformWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MiniWaveformWidget(QWidget* parent = nullptr);
    ~MiniWaveformWidget() override = default;

    /// Set the audio samples for this clip region.
    void setAudio(const std::vector<float>& allSamples, uint32_t sampleRate,
                  double clipStart, double clipEnd);

    /// Like setAudio() without copying: the widget shares `samples`.
    void setAudioShared(const SampleBuffer& samples, uint32_t sampleRate,
                        double clipStart, double clipEnd);

    /// Set trim in/out points (in seconds, absolute time).
    void setTrimRange(double inPoint, double outPoint);
    [[nodiscard]] double trimIn()  const noexcept { return m_trimIn; }
    [[nodiscard]] double trimOut() const noexcept { return m_trimOut; }

    /// Show/hide trim in/out handles and dimmed regions.
    void setTrimHandlesVisible(bool v) { m_trimHandlesVisible = v; update(); }
    [[nodiscard]] bool trimHandlesVisible() const noexcept { return m_trimHandlesVisible; }

    /// Set playhead position (seconds, absolute time).
    void setPlayhead(double timeSec);
    [[nodiscard]] double playhead() const noexcept { return m_playhead; }
    void setPlayheadVisible(bool v) { m_playheadVisible = v; update(); }
    [[nodiscard]] bool isPlayheadVisible() const noexcept { return m_playheadVisible; }

    /// Accessors for view range and duration
    [[nodiscard]] double viewStart() const noexcept { return m_viewStart; }
    [[nodiscard]] double viewEnd()   const noexcept { return m_viewEnd; }
    [[nodiscard]] double clipStart() const noexcept { return m_clipStart; }
    [[nodiscard]] double clipEnd()   const noexcept { return m_clipEnd; }

    /// Deleted (cropped) regions — each is (start, end) in seconds.
    [[nodiscard]] const std::vector<std::pair<double,double>>& deletedRegions() const noexcept
        { return m_deletedRegions; }
    void setDeletedRegions(const std::vector<std::pair<double,double>>& regions);

    /// Effective playback ranges (clip minus deleted regions), sorted by time.
    [[nodiscard]] std::vector<std::pair<double,double>> effectiveRanges() const;

    /// Size hints
    QSize sizeHint() const override { return {300, 50}; }
    QSize minimumSizeHint() const override { return {100, 30}; }

signals:
    void trimChanged(double inPoint, double outPoint);
    void trimChanging(double inPoint, double outPoint);  ///< Live updates during drag
    void seekRequested(double timeSec);
    void playToggleRequested();
    void shuttleSpeedChanged(double speed);  ///< JKL shuttle: +1/2/4 fwd, -1/2/4 rev, 0 stop
    void inPointSet(double timeSec);
    void outPointSet(double timeSec);
    void deleteRegionRequested(double start, double end);
    void deletedRegionsChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    double pixelToTime(int x) const;
    int    timeToPixel(double t) const;
    void   detectSilences();
    void   mergeDeletedRegions();
    void   expandViewIfNeeded(double timeSec);
    double fileDuration() const;

    // Audio data (a copy from setAudio, or shared via setAudioShared)
    SampleBuffer m_samples;
    const SampleBuffer& sam() const { return m_samples; }
    uint32_t           m_sampleRate{44100};
    double             m_clipStart{0.0};
    double             m_clipEnd{1.0};

    // View window (with padding)
    double m_viewStart{0.0};
    double m_viewEnd{1.0};

    // Trim
    double m_trimIn{0.0};
    double m_trimOut{1.0};
    bool   m_trimHandlesVisible{true};

    // Playhead
    double m_playhead{0.0};
    bool   m_playheadVisible{false};

    // Deleted / cropped regions
    std::vector<std::pair<double,double>> m_deletedRegions;
    bool   m_deleteSelecting{false};
    double m_deleteStart{0.0};
    double m_deleteEnd{0.0};

    // Interaction
    enum DragMode { None, DragIn, DragOut, DragPlayhead, DragDeleteEdge };
    DragMode m_dragMode{None};
    int      m_dragDeleteRegionIdx{-1};  // which deleted region edge
    bool     m_dragDeleteIsStart{false}; // dragging start vs end edge

    // JKL shuttle state
    int m_jShuttleLevel{0};   // reverse shuttle level (0 = off, 1..3)
    int m_lShuttleLevel{0};   // forward shuttle level (0 = off, 1..3)

    // Silence regions
    struct SilenceRegion { double start; double end; };
    std::vector<SilenceRegion> m_silences;
    bool m_silencesComputed{false};

    // Constants
    static constexpr double kContextPadding = 1.5;   // seconds of padding around clip
    static constexpr double kEdgeExpand     = 0.3;    // seconds — expand threshold near edge
    static constexpr int    kHandleWidth    = 6;
    static constexpr int    kHandleHitZone  = 12;
    static constexpr float  kSilenceThreshold = 0.02f;
};

} // namespace rt
