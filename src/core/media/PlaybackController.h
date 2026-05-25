/*
 * PlaybackController — Master transport/playback controller.
 *
 * Orchestrates the AVSyncClock, AudioEngine, and Timeline to provide
 * a unified transport interface:
 *   • Play / pause / stop
 *   • JKL shuttle (J=reverse, K=pause, L=forward; repeat = 2x/4x)
 *   • Frame step forward / backward
 *   • Go to next/previous edit point
 *   • Go to start / end
 *   • Loop playback (between in/out points or full timeline)
 *   • Variable speed playback
 *   • Timecode formatting (HH:MM:SS:FF)
 *
 * No Qt dependency — this is pure logic. The UI (TransportBar) wraps this.
 *
 * Thread model:
 *   All public methods are called from the main/UI thread.
 *   The internal timer-based position poll uses the same thread affinity.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace rt {

// Forward declarations
class Timeline;
class AudioEngine;
class AVSyncClock;

/// Transport / playback state.
enum class PlayState : uint8_t
{
    Stopped,    ///< Position at 0, clock not running
    Playing,    ///< Clock running forward
    Paused,     ///< Position held, clock not running
    Shuttling   ///< JKL variable speed (can be reverse)
};

/// Timecode components for display.
struct Timecode
{
    int hours{0};
    int minutes{0};
    int seconds{0};
    int frames{0};

    /// Format as "HH:MM:SS:FF"
    [[nodiscard]] std::string toString() const;
};

/// Converts a tick + framerate into timecode.
[[nodiscard]] Timecode tickToTimecode(int64_t tick, double fps) noexcept;

/// Converts timecode back to a tick.
[[nodiscard]] int64_t timecodeToTick(const Timecode& tc, double fps) noexcept;

class PlaybackController
{
public:
    PlaybackController();
    ~PlaybackController();

    // Non-copyable
    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    // ── Dependencies ────────────────────────────────────────────────────

    /// Attach the timeline data model.
    void setTimeline(Timeline* timeline) noexcept;

    /// Attach the audio engine (optional — can work without audio).
    void setAudioEngine(AudioEngine* engine) noexcept;

    /// Attach the AV sync clock (optional — can work with internal timer).
    void setSyncClock(AVSyncClock* clock) noexcept;

    /// Set the project framerate (for timecode, frame stepping, grid snap).
    void setFrameRate(double fps) noexcept;

    /// Get the project framerate.
    [[nodiscard]] double frameRate() const noexcept { return m_fps; }

    /// Set standalone duration (for Source Monitor — no Timeline attached).
    void setStandaloneDuration(int64_t ticks) noexcept;

    // ── Transport controls ──────────────────────────────────────────────

    /// Play from the current position.
    void play();

    /// Pause (hold position).
    void pause();

    /// Toggle play/pause.
    void togglePlayPause();

    /// Stop (pause + return to in-point or start).
    void stop();

    /// Seek to a specific tick position.
    void seekTo(int64_t tick);

    // ── Frame stepping ──────────────────────────────────────────────────

    /// Step forward by one frame.
    void stepForward();

    /// Step backward by one frame.
    void stepBackward();

    // ── JKL Shuttle ─────────────────────────────────────────────────────

    /// Press J: reverse shuttle. Each press doubles speed: -1x, -2x, -4x.
    void shuttleReverse();

    /// Press K: pause shuttle / stop.
    void shuttlePause();

    /// Press L: forward shuttle. Each press doubles speed: 1x, 2x, 4x.
    void shuttleForward();

    /// Begin shuttling at an exact speed (e.g. 0.5 for K+L slow scrub).
    /// Bypasses the J/L press-counter ramp.  Speed 0 pauses.
    void setShuttleSpeed(double speed);

    /// Get current shuttle speed (negative = reverse).
    [[nodiscard]] double shuttleSpeed() const noexcept { return m_shuttleSpeed; }

    // ── Navigation ──────────────────────────────────────────────────────

    /// Go to the start of the timeline.
    void goToStart();

    /// Go to the end of the timeline.
    void goToEnd();

    /// Go to the next edit point (clip boundary).
    void goToNextEditPoint();

    /// Go to the previous edit point (clip boundary).
    void goToPrevEditPoint();

    /// Go to the in-point (if set).
    void goToInPoint();

    /// Go to the out-point (if set).
    void goToOutPoint();

    // ── Loop ────────────────────────────────────────────────────────────

    /// Enable/disable loop playback.
    void setLoopEnabled(bool enabled) noexcept;

    /// Is loop playback enabled?
    [[nodiscard]] bool isLoopEnabled() const noexcept { return m_loopEnabled; }

    // ── State queries ───────────────────────────────────────────────────

    /// Current transport state.
    [[nodiscard]] PlayState state() const noexcept { return m_state; }

    /// Is playback active (playing or shuttling)?
    [[nodiscard]] bool isPlaying() const noexcept;

    /// Current playhead position in ticks.
    [[nodiscard]] int64_t currentTick() const noexcept;

    /// Current position as timecode.
    [[nodiscard]] Timecode currentTimecode() const noexcept;

    /// Current position as "HH:MM:SS:FF" string.
    [[nodiscard]] std::string currentTimecodeString() const;

    /// Duration of the timeline in ticks.
    [[nodiscard]] int64_t durationTicks() const noexcept;

    /// Access the AV sync clock (for drift monitoring).
    [[nodiscard]] AVSyncClock* syncClock() const noexcept { return m_syncClock; }

    // ── Polling ─────────────────────────────────────────────────────────

    /// Called periodically (e.g. from a QTimer at 60fps) to sync
    /// the playhead, check for loop boundaries, and handle adaptive
    /// frame skipping. Returns the current tick for the UI to render.
    [[nodiscard]] int64_t pollPosition();

    // ── Callbacks ───────────────────────────────────────────────────────

    /// Called whenever the playhead position changes (for UI updates).
    std::function<void(int64_t tick)> onPositionChanged;

    /// Called when transport state changes.
    std::function<void(PlayState state)> onStateChanged;

    /// Called when shuttle speed changes.
    std::function<void(double speed)> onSpeedChanged;

    /// Called just before playback starts, with the start tick.
    /// Use this to trigger pre-buffering (prefetch) before the clock begins.
    std::function<void(int64_t startTick)> onPlayStarting;

    // ── Audio safety (Phase 2.C) ────────────────────────────────────────
    // When true, the audio callback NEVER blocks waiting for GPU compositing.
    // If the next audio buffer isn't ready, the last buffer is repeated
    // instead. This prevents audio glitches when the GPU compositor is slow.
    // Users may see a repeated video frame instead of hearing audio crackle.
    std::atomic<bool> m_audioNeverBlock{true};

private:
    Timeline*      m_timeline{nullptr};
    AudioEngine*   m_audioEngine{nullptr};
    AVSyncClock*   m_syncClock{nullptr};

    PlayState      m_state{PlayState::Stopped};
    double         m_fps{60.0};
    double         m_shuttleSpeed{0.0};
    int            m_jShuttleLevel{0};   ///< 0, -1, -2, -3 (press count)
    int            m_lShuttleLevel{0};   ///< 0,  1,  2,  3 (press count)
    bool           m_loopEnabled{false};
    int64_t        m_lastPollTick{0};
    int64_t        m_standaloneTick{0};     ///< Position when no timeline/clock
    int64_t        m_standaloneDuration{0}; ///< Duration when no timeline
    int64_t        m_lastPollTimeNs{0};     ///< For standalone time advancement

    /// Use-after-free guard
    std::atomic<bool> m_destroying{false};

    /// Ticks per frame at current FPS.
    [[nodiscard]] int64_t ticksPerFrame() const noexcept;

    /// Apply loop boundaries if applicable.
    [[nodiscard]] int64_t applyLoopBounds(int64_t tick) const;

    /// Internal state change + callback dispatch.
    void setState(PlayState newState);

    /// Internal seek (updates clock + audio engine + timeline).
    void seekInternal(int64_t tick);
};

} // namespace rt
