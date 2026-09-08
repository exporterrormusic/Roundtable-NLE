/*
 * FrameClock — High-priority self-driven frame clock.
 *
 * Replaces the Qt QTimer poll loop (which fires at best-effort 16ms
 * intervals on the UI thread, subject to event-loop stalls) with a
 * dedicated high-priority thread that uses accumulator-based timing
 * to fire callbacks at precise frame boundaries.
 *
 * Design:
 *   - Dedicated thread, THREAD_PRIORITY_TIME_CRITICAL on Windows.
 *   - Accumulator timing: nextDeadline += frameDuration (no drift).
 *   - Fires a "frame tick" callback at each deadline.
 *   - Separate from composite and present — FrameClock only keeps time.
 *   - When paused, blocks on a condition variable (zero CPU).
 *
 * Usage:
 *   FrameClock clock;
 *   clock.setController(ctrl);
 *   clock.setFrameCallback([](int64_t tick, int64_t frameNum) {
 *       // Called at each frame deadline on the clock thread
 *   });
 *   clock.start();  // spawns thread
 *   clock.stop();   // joins thread
 */

#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace rt {

namespace frame_clock_detail {

struct JumpObservation
{
    int64_t delta{0};
    bool intentionalReset{false};
    bool unexpectedJump{false};
};

/// Stateful diagnostic classifier kept separate from the timing loop so seek,
/// speed, and direction behavior can be tested without a real-time thread.
class JumpDetector
{
public:
    JumpObservation observe(int64_t frame, double speed,
                            uint64_t resetGeneration) noexcept
    {
        if (!m_hasBaseline || resetGeneration != m_resetGeneration) {
            const bool reset = m_hasBaseline;
            m_hasBaseline = true;
            m_lastFrame = frame;
            m_resetGeneration = resetGeneration;
            return {0, reset, false};
        }

        const int64_t delta = frame - m_lastFrame;
        m_lastFrame = frame;

        const int64_t expectedStride = std::max<int64_t>(
            1, static_cast<int64_t>(std::ceil(std::abs(speed))));
        const int64_t allowedStride = expectedStride + 1;

        bool jump = false;
        if (speed > 0.0)
            jump = delta < 0 || delta > allowedStride;
        else if (speed < 0.0)
            jump = delta > 0 || -delta > allowedStride;
        else
            jump = std::abs(delta) > 2;

        return {delta, false, jump};
    }

    void reset() noexcept
    {
        m_hasBaseline = false;
        m_lastFrame = -1;
        m_resetGeneration = 0;
    }

private:
    bool m_hasBaseline{false};
    int64_t m_lastFrame{-1};
    uint64_t m_resetGeneration{0};
};

} // namespace frame_clock_detail

class PlaybackController;

class FrameClock
{
public:
    /// Called on the clock thread at each frame deadline.
    /// tick   = audio-clock position (48kHz ticks)
    /// frame  = project frame number
    using FrameCallback = std::function<void(int64_t tick, int64_t frame)>;

    /// Called when playback state changes (play→pause or pause→play).
    /// Runs on the clock thread.
    using StateCallback = std::function<void(bool playing)>;

    FrameClock();
    ~FrameClock();

    FrameClock(const FrameClock&) = delete;
    FrameClock& operator=(const FrameClock&) = delete;

    // ── Configuration ────────────────────────────────────────────────

    void setController(PlaybackController* c) { m_controller = c; }

    /// Set the callback invoked at each frame boundary during playback.
    void setFrameCallback(FrameCallback cb) { m_frameCB = std::move(cb); }

    /// Set the callback invoked on state transitions.
    void setStateCallback(StateCallback cb) { m_stateCB = std::move(cb); }

    // ── Lifecycle ────────────────────────────────────────────────────

    void start();
    void stop();
    [[nodiscard]] bool isRunning() const noexcept
    { return m_running.load(std::memory_order_relaxed); }

    /// Wake the clock thread (e.g. when play/pause changes).
    void wake();

    // ── Stats ────────────────────────────────────────────────────────

    [[nodiscard]] int droppedFrames() const noexcept
    { return m_droppedFrames.load(std::memory_order_relaxed); }
    void resetDroppedFrames() noexcept
    { m_droppedFrames.store(0, std::memory_order_relaxed); }

private:
    void clockLoop();

    PlaybackController* m_controller{nullptr};
    FrameCallback       m_frameCB;
    StateCallback       m_stateCB;

    std::thread        m_thread;
    std::atomic<bool>  m_running{false};

    // Wake mechanism for play/pause transitions
    std::mutex              m_wakeMtx;
    std::condition_variable m_wakeCV;

    std::atomic<int> m_droppedFrames{0};
    frame_clock_detail::JumpDetector m_diagJumpDetector;
};

} // namespace rt
