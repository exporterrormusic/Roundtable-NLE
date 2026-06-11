/*
 * AVSyncClock — master audio/video synchronization clock.
 *
 * The audio callback is the single source of truth for playback time.
 * Video rendering queries this clock to determine which frame to display.
 *
 * Design:
 *   • Audio thread calls advance() on every callback with the number of
 *     samples consumed. This atomically updates the master position.
 *   • Video/UI thread calls currentTick() or currentSeconds() to read
 *     the position without locking.
 *   • Supports variable speed playback (0.25x – 4.0x).
 *   • Computes drift between audio and video for monitoring.
 *
 * Lock-free: all state uses atomics for safe cross-thread access.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "Constants.h"

namespace rt {

class AVSyncClock
{
public:
    AVSyncClock();
    ~AVSyncClock() = default;

    // ── Audio thread interface ──────────────────────────────────────────

    /// Called by audio callback after consuming `frames` frames at `sampleRate`.
    /// Atomically advances the master clock.
    void advance(int64_t frames, uint32_t sampleRate) noexcept;

    /// Reset the clock to a specific position (used on seek/stop).
    void reset(int64_t tick = 0) noexcept;

    // ── Query interface (any thread) ────────────────────────────────────

    /// Current master position in ticks (48000 ticks/sec).
    [[nodiscard]] int64_t currentTick() const noexcept;

    /// Current master position in seconds.
    [[nodiscard]] double currentSeconds() const noexcept;

    /// Current position in sample frames at the given sample rate.
    [[nodiscard]] int64_t currentFrame(uint32_t sampleRate) const noexcept;

    // ── Playback speed ──────────────────────────────────────────────────

    /// Set playback speed multiplier (1.0 = normal).
    void setSpeed(double speed) noexcept;

    /// Get current speed.
    [[nodiscard]] double speed() const noexcept;

    // ── AV drift monitoring ─────────────────────────────────────────────

    /// Report the PTS of the last displayed video frame (in ticks).
    /// Called by the video renderer after displaying a frame.
    void reportVideoPresentation(int64_t videoTick) noexcept;

    /// Get drift = audio position - last video position (in ticks).
    /// Positive means video is behind audio, negative means ahead.
    [[nodiscard]] int64_t drift() const noexcept;

    /// Get drift in milliseconds.
    [[nodiscard]] double driftMs() const noexcept;

    // ── State ───────────────────────────────────────────────────────────

    /// Is the clock running (being advanced by audio callbacks)?
    [[nodiscard]] bool isRunning() const noexcept;

    /// Set running state.
    void setRunning(bool running) noexcept;

    /// Wall-clock timestamp of the last advance call.
    [[nodiscard]] std::chrono::steady_clock::time_point lastAdvanceTime() const noexcept;

    // ── Diagnostics ─────────────────────────────────────────────────────

    /// The raw audio-played position (m_tick) WITHOUT wall-clock
    /// extrapolation.  Reflects only positions actually consumed by
    /// audio callbacks.  Use alongside currentTick() to detect whether
    /// extrapolation is racing ahead of audio (e.g., audio thread
    /// stalled, or speed multiplier produced a runaway clock).
    [[nodiscard]] int64_t masterTick() const noexcept {
        return m_tick.load(std::memory_order_acquire);
    }

    /// Milliseconds since the most recent advance() call.  >100ms during
    /// active playback typically indicates the audio thread is blocked.
    [[nodiscard]] double msSinceLastAdvance() const noexcept;

private:
    std::atomic<int64_t>  m_tick{0};           // Master position in ticks
    std::atomic<int64_t>  m_lastVideoTick{0};  // Last displayed video PTS
    std::atomic<bool>     m_running{false};
    std::atomic<int64_t>  m_speedFixed{1000};  // speed * 1000 (fixed-point)

    // Wall clock for jitter estimation
    std::atomic<int64_t>  m_lastAdvanceNs{0};  // nanoseconds since epoch

    // Wall-clock extrapolation anchor — recorded each advance()/reset().
    // When currentTick() is called between audio callbacks, we extrapolate
    // from (m_anchorTick, m_anchorNs) using wall-clock time × speed.
    // Protected by a seqlock (m_anchorSeq) to prevent torn reads when the
    // audio callback updates both values between reader loads.
    std::atomic<unsigned> m_anchorSeq{0};      // seqlock version counter
    std::atomic<int64_t>  m_anchorTick{0};
    std::atomic<int64_t>  m_anchorNs{0};
};

} // namespace rt
