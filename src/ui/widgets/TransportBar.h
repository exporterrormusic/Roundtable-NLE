/*
 * TransportBar — Qt transport control bar widget.
 *
 * Step 14: Transport & Playback
 *
 * Layout:
 *   ⏮  ⏪  ⏸  ▶  ⏹  ⏩  ⏭  |  🔁 Loop  |  HH:MM:SS:FF  |  Speed indicator
 *
 * Keyboard:
 *   Space     = Play/Pause
 *   S         = Stop
 *   J         = Shuttle reverse (repeat = 2x/4x/8x)
 *   K         = Shuttle pause
 *   L         = Shuttle forward (repeat = 2x/4x/8x)
 *   Left      = Frame back
 *   Right     = Frame forward
 *   Up        = Previous edit point
 *   Down      = Next edit point
 *   Home      = Go to start
 *   End       = Go to end
 *
 * The TransportBar wraps a PlaybackController for the actual logic.
 */

#pragma once

#include <QLabel>
#include <atomic>
#include <functional>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

namespace rt {

class PlaybackController;

/// Transport control bar — play/pause/stop, JKL shuttle, timecode, loop.
class TransportBar : public QWidget
{
    Q_OBJECT

public:
    explicit TransportBar(QWidget* parent = nullptr);
    ~TransportBar() override;

    /// Attach the playback controller (required).
    void setController(PlaybackController* controller);

    /// Get the attached controller.
    [[nodiscard]] PlaybackController* controller() const noexcept { return m_controller; }

    /// Set a provider that returns the count of dropped frames.
    /// Called on each poll tick to update the drop indicator light.
    /// Return -1 to hide the indicator entirely.
    using FrameDropProvider = std::function<int()>;
    void setFrameDropProvider(FrameDropProvider provider) { m_dropProvider = std::move(provider); }

    /// Start the UI polling timer.
    void startPolling(int intervalMs = 16);

    /// Stop the UI polling timer.
    void stopPolling();

    /// Update the display (timecode, button states, speed indicator).
    void updateDisplay();

    /// Preferred height for the transport bar.
    static constexpr int kBarHeight = 32;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// Emitted when the user interacts with transport controls.
    void playheadChanged(int64_t tick);

    /// Emitted on state change.
    void stateChanged(int state);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onPollTimer();

private:
    PlaybackController* m_controller{nullptr};
    QTimer*             m_pollTimer{nullptr};

    // Buttons
    QPushButton* m_btnGoStart{nullptr};
    QPushButton* m_btnStepBack{nullptr};
    QPushButton* m_btnStop{nullptr};
    QPushButton* m_btnPlayPause{nullptr};
    QPushButton* m_btnStepForward{nullptr};
    QPushButton* m_btnGoEnd{nullptr};
    QPushButton* m_btnLoop{nullptr};

    // Timecode display
    QLabel* m_timecodeLabel{nullptr};

    // Duration / remaining display
    QLabel* m_durationLabel{nullptr};
    bool    m_showRemaining{false};  // false = total duration, true = remaining

    // Speed indicator
    QLabel* m_speedLabel{nullptr};

    // Frame drop indicator (green/yellow/red dot beside timecode)
    QLabel*  m_dropIndicator{nullptr};
    int      m_lastDropCount{-1};
    FrameDropProvider m_dropProvider;

    std::atomic<bool> m_destroying{false};

    void setupUI();
    void updateButtonStates();
    void styleButton(QPushButton* btn, const QString& text, const QString& tooltip);
};

} // namespace rt
