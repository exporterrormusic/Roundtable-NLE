/*
 * ScrubGesture — the pointer half of a Premiere-style "hot text" scrub,
 * shared by ScrubbySpinBox and SpinBoxScrubber.
 *
 *   press()   arm on mouse press (nothing happens yet)
 *   move()    returns INCREMENTAL horizontal movement since the last move,
 *             in pixels scaled by the drag's speed (see speedGain): slow,
 *             careful movement counts a quarter, a normal flick in full.
 *             That gives scrubbing the "weight" of Premiere's hot text,
 *             where small slow nudges barely move the value, without its
 *             dead zone (slow movement still always moves it a little).
 *             Returns 0 until the drag passes the threshold; the threshold
 *             distance itself is discarded so the value doesn't jump when
 *             the scrub engages. Once active the cursor is hidden and is
 *             warped back to the screen centre near either edge, so a scrub
 *             never runs into the edge of the monitor.
 *   release() ends the gesture; returns true if a scrub actually happened.
 *             The cursor reappears where the press started.
 *
 * Callers accumulate dx * (step for the CURRENT modifiers), so pressing
 * Shift/Ctrl mid-drag changes the rate from that point on instead of
 * re-scaling the whole drag.
 */

#pragma once

#include <QPoint>
#include <QtGlobal>

namespace rt {

class ScrubGesture
{
public:
    ScrubGesture() = default;
    ~ScrubGesture() { cancel(); }
    ScrubGesture(const ScrubGesture&) = delete;
    ScrubGesture& operator=(const ScrubGesture&) = delete;

    void press(QPoint globalPos);
    /// `timestampMs` = the mouse event's timestamp(); 0 (unknown, e.g. a
    /// synthesized event) applies no speed scaling.
    [[nodiscard]] double move(QPoint globalPos, quint64 timestampMs = 0);
    bool release();
    /// Abort without moving the cursor back (widget going away, etc.).
    void cancel();

    [[nodiscard]] bool isArmed()  const noexcept { return m_state == State::Armed; }
    [[nodiscard]] bool isActive() const noexcept { return m_state == State::Active; }

    static constexpr int kDragThreshold = 3;

    /// Movement multiplier for a drag speed in pixels per second: kSlowGain
    /// up to kSlowSpeed, 1 from kFastSpeed, a smooth ramp in between.
    [[nodiscard]] static double speedGain(double pxPerSecond) noexcept;
    static constexpr double kSlowGain  = 0.25;
    static constexpr double kSlowSpeed = 100.0;   ///< px/s
    static constexpr double kFastSpeed = 800.0;   ///< px/s
    /// Smoothing of the speed estimate (mouse events are 1-8 ms apart and
    /// individually jittery).
    static constexpr double kSpeedTimeConstantMs = 40.0;

private:
    enum class State { Idle, Armed, Active };

    void endActive();

    State  m_state{State::Idle};
    QPoint m_pressPos;
    QPoint m_lastPos;
    /// Set after a cursor warp: mouse events queued before the warp still
    /// carry pre-warp coordinates, so one huge jump is dropped.
    bool   m_justWarped{false};
    quint64 m_lastTimeMs{0};   ///< 0 = no timestamp seen yet
    double  m_speed{0.0};      ///< smoothed |px/s| since engaging
};

} // namespace rt
