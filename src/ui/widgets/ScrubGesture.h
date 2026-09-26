/*
 * ScrubGesture — the pointer half of a Premiere-style "hot text" scrub,
 * shared by ScrubbySpinBox and SpinBoxScrubber.
 *
 *   press()   arm on mouse press (nothing happens yet)
 *   move()    returns INCREMENTAL horizontal pixels since the last move.
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

namespace rt {

class ScrubGesture
{
public:
    ScrubGesture() = default;
    ~ScrubGesture() { cancel(); }
    ScrubGesture(const ScrubGesture&) = delete;
    ScrubGesture& operator=(const ScrubGesture&) = delete;

    void press(QPoint globalPos);
    [[nodiscard]] int move(QPoint globalPos);
    bool release();
    /// Abort without moving the cursor back (widget going away, etc.).
    void cancel();

    [[nodiscard]] bool isArmed()  const noexcept { return m_state == State::Armed; }
    [[nodiscard]] bool isActive() const noexcept { return m_state == State::Active; }

    static constexpr int kDragThreshold = 3;

private:
    enum class State { Idle, Armed, Active };

    void endActive();

    State  m_state{State::Idle};
    QPoint m_pressPos;
    QPoint m_lastPos;
    /// Set after a cursor warp: mouse events queued before the warp still
    /// carry pre-warp coordinates, so one huge jump is dropped.
    bool   m_justWarped{false};
};

} // namespace rt
