/*
 * AudioMixer — Per-track mixer panel with faders, pan, meters, solo/mute.
 *
 * Step 25: Audio Mixer Panel
 *
 * Horizontal strip of vertical channel strips — one per audio track, plus
 * a master bus. Each strip: track name, VU meter, volume fader, pan knob,
 * mute/solo buttons. Real-time metering from AudioEngine callbacks.
 *
 * Layout:
 * ┌─────────┬─────────┬─────────┬─────────┬─────────┐
 * │  A1     │  A2     │  A3     │  A4     │ MASTER  │
 * │         │         │         │         │         │
 * │ ┌─┐┌─┐ │ ┌─┐┌─┐ │ ┌─┐┌─┐ │ ┌─┐┌─┐ │ ┌─┐┌─┐ │
 * │ │█││█│ │ │█││█│ │ │░││░│ │ │▓││▓│ │ │▓││▓│ │
 * │ │█││█│ │ │█││█│ │ │░││░│ │ │▓││▓│ │ │▓││▓│ │
 * │ └─┘└─┘ │ └─┘└─┘ │ └─┘└─┘ │ └─┘└─┘ │ └─┘└─┘ │
 * │  ──●── │  ──●── │  ──●── │  ──●── │  ──●── │  ← fader
 * │  0.0dB │  0.0dB │  0.0dB │  0.0dB │  0.0dB │
 * │  ◐     │  ◐     │  ◐     │  ◐     │  ◐     │  ← pan
 * │ [M][S] │ [M][S] │ [M][S] │ [M][S] │ [M]    │
 * └─────────┴─────────┴─────────┴─────────┴─────────┘
 */

#pragma once

#include "widgets/VUMeter.h"
#include "command/Command.h"
#include "timeline/TimelineObserver.h"

#include <QLabel>
#include <QDial>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rt {

// Forward declarations
class AudioEngine;
class CommandStack;
class Timeline;
class Track;

// ═════════════════════════════════════════════════════════════════════════════

/// A single channel strip in the audio mixer.
struct ChannelStrip
{
    Track*       track{nullptr};     ///< Non-owning pointer to the track
    size_t       trackIndex{0};      ///< Index in the timeline
    bool         isMaster{false};    ///< Is this the master bus strip?

    // Widgets (owned by Qt parent)
    QLabel*      nameLabel{nullptr};
    VUMeter*     vuMeter{nullptr};
    QSlider*     fader{nullptr};
    QLabel*      faderLabel{nullptr}; ///< Shows current dB value
    QDial*       panDial{nullptr};
    QLabel*      panLabel{nullptr};   ///< Shows current pan value
    QPushButton* muteButton{nullptr};
    QPushButton* soloButton{nullptr};
    QWidget*     container{nullptr};  ///< The strip widget

    /// Convert fader slider position (0-1000) to linear volume.
    [[nodiscard]] static float faderToVolume(int faderValue) noexcept;

    /// Convert linear volume to fader slider position.
    [[nodiscard]] static int volumeToFader(float volume) noexcept;

    /// Convert linear volume to dB string.
    [[nodiscard]] static QString volumeToDbString(float volume);

    /// Convert pan dial value (-100..+100) to float (-1..+1).
    [[nodiscard]] static float dialToPan(int dialValue) noexcept;

    /// Convert float pan to dial value.
    [[nodiscard]] static int panToDial(float pan) noexcept;

    /// Format pan value as string (e.g. "C", "L50", "R50").
    [[nodiscard]] static QString panToString(float pan);
};

// ═════════════════════════════════════════════════════════════════════════════
// Mixer commands (exposed for testing and external use)
// ═════════════════════════════════════════════════════════════════════════════

/// Undoable command: change a track's volume.
class SetTrackVolumeCommand : public Command
{
public:
    SetTrackVolumeCommand(Track* track, float oldVol, float newVol)
        : m_track(track), m_oldVolume(oldVol), m_newVolume(newVol) {}

    void execute() override;
    void undo()    override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override;
    [[nodiscard]] bool mergeWith(const Command& next) override;

private:
    Track* m_track;
    float  m_oldVolume;
    float  m_newVolume;
};

/// Undoable command: change a track's pan.
class SetTrackPanCommand : public Command
{
public:
    SetTrackPanCommand(Track* track, float oldPan, float newPan)
        : m_track(track), m_oldPan(oldPan), m_newPan(newPan) {}

    void execute() override;
    void undo()    override;
    [[nodiscard]] std::string description() const override;
    [[nodiscard]] int typeId() const override;
    [[nodiscard]] bool mergeWith(const Command& next) override;

private:
    Track* m_track;
    float  m_oldPan;
    float  m_newPan;
};

/// Undoable command: toggle mute on a track.
class SetTrackMuteCommand : public Command
{
public:
    SetTrackMuteCommand(Track* track, bool muted)
        : m_track(track), m_muted(muted) {}

    void execute() override;
    void undo()    override;
    [[nodiscard]] std::string description() const override;

private:
    Track* m_track;
    bool   m_muted;
};

/// Undoable command: toggle solo on a track.
class SetTrackSoloCommand : public Command
{
public:
    SetTrackSoloCommand(Track* track, bool soloed)
        : m_track(track), m_soloed(soloed) {}

    void execute() override;
    void undo()    override;
    [[nodiscard]] std::string description() const override;

private:
    Track* m_track;
    bool   m_soloed;
};

// ═════════════════════════════════════════════════════════════════════════════

/// Audio Mixer panel — horizontal strip of channel faders + VU meters.
class AudioMixer : public QWidget, public TimelineObserver
{
    Q_OBJECT

public:
    explicit AudioMixer(QWidget* parent = nullptr);
    ~AudioMixer() override;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the timeline to display audio tracks from.
    void setTimeline(Timeline* timeline);

    // TimelineObserver: drop the cached timeline before it is destroyed so the
    // meter timer can never dereference a freed Timeline (project/seq close).
    void onTimelineDestroyed(Timeline* tl) override;

    /// Set the audio engine for metering.
    void setAudioEngine(AudioEngine* engine);

    /// Set the command stack for undo/redo integration.
    void setCommandStack(CommandStack* stack);

    // ── Track management ────────────────────────────────────────────────

    /// Rebuild all channel strips from the current timeline.
    void rebuildStrips();

    /// Update meter levels from the audio engine (call from timer).
    void updateMeters();

    /// Restart the meter timer (call when playback starts).
    void ensureMeterTimerRunning();

    /// Preferred size.
    QSize sizeHint() const override;

    /// Event filter for double-click reset on faders/dials.
    bool eventFilter(QObject* obj, QEvent* event) override;

    // ── Accessors (for testing) ─────────────────────────────────────────

    [[nodiscard]] size_t stripCount() const noexcept { return m_strips.size(); }
    [[nodiscard]] const ChannelStrip& strip(size_t index) const { return m_strips[index]; }
    [[nodiscard]] const ChannelStrip& masterStrip() const { return m_masterStrip; }

    /// Get the channel strip for a given track index, or nullptr.
    [[nodiscard]] ChannelStrip* stripForTrack(size_t trackIndex);

signals:
    /// Emitted when a fader value changes.
    void volumeChanged(size_t trackIndex, float volume);

    /// Emitted when a pan value changes.
    void panChanged(size_t trackIndex, float pan);

    /// Emitted when mute/solo state changes.
    void muteChanged(size_t trackIndex, bool muted);
    void soloChanged(size_t trackIndex, bool soloed);

private slots:
    void onFaderChanged(int value);
    void onPanChanged(int value);
    void onMuteToggled();
    void onSoloToggled();

private:
    void setupUI();
    QWidget* createChannelStrip(ChannelStrip& strip);
    QWidget* createMasterStrip();
    void syncStripToTrack(ChannelStrip& strip);
    void showStripContextMenu(const QPoint& pos, ChannelStrip& strip);
    void resetFader(ChannelStrip& strip);
    void resetPan(ChannelStrip& strip);

    // ── Data ────────────────────────────────────────────────────────────
    Timeline*                m_timeline{nullptr};
    AudioEngine*             m_audioEngine{nullptr};
    CommandStack*            m_commandStack{nullptr};

    std::vector<ChannelStrip> m_strips;
    ChannelStrip              m_masterStrip;

    // ── Widgets ─────────────────────────────────────────────────────────
    QHBoxLayout*             m_stripLayout{nullptr};
    QScrollArea*             m_scrollArea{nullptr};
    QWidget*                 m_stripContainer{nullptr};
    QTimer*                  m_meterTimer{nullptr};
    QLabel*                  m_statusLabel{nullptr};
    QLabel*                  m_emptyLabel{nullptr};

    static constexpr int kFaderMin     = 0;
    static constexpr int kFaderMax     = 1000;   ///< Slider range
    static constexpr int kFaderDefault = 800;    ///< ~0 dB (unity)
    static constexpr int kPanMin       = -100;
    static constexpr int kPanMax       = 100;
    static constexpr int kMeterRefreshMs = 30;   ///< ~33 Hz meter updates

    std::atomic<bool> m_destroying{false};
};

} // namespace rt
