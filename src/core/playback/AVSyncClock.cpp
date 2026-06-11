/*
 * AVSyncClock.cpp — audio-driven master synchronization clock.
 */

#include "playback/AVSyncClock.h"
#include <cmath>

#include <spdlog/spdlog.h>

namespace rt {

AVSyncClock::AVSyncClock()
{
    // Initialize wall clock timestamp
    const auto now = std::chrono::steady_clock::now();
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    ).count();
    m_lastAdvanceNs.store(ns);
    m_anchorNs.store(ns);
    m_anchorTick.store(0);
}

// ── Audio thread interface ──────────────────────────────────────────────

void AVSyncClock::advance(int64_t frames, uint32_t sampleRate) noexcept
{
    if (sampleRate == 0 || frames <= 0) return;

    // Convert audio frames to ticks: frames * (48000 / sampleRate)
    // Using integer math to avoid floating point in audio thread
    const int64_t speedFixed = m_speedFixed.load(std::memory_order_relaxed);
    const int64_t tickDelta = (frames * kTicksPerSecond * speedFixed)
                            / (static_cast<int64_t>(sampleRate) * 1000);

    const int64_t newTick = m_tick.fetch_add(tickDelta, std::memory_order_release) + tickDelta;

    // Update wall clock
    const auto now = std::chrono::steady_clock::now();
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    ).count();
    m_lastAdvanceNs.store(ns, std::memory_order_relaxed);

    // Update anchor for wall-clock extrapolation (seqlock write)
    m_anchorSeq.fetch_add(1, std::memory_order_release);   // odd → writing
    m_anchorTick.store(newTick, std::memory_order_relaxed);
    m_anchorNs.store(ns, std::memory_order_relaxed);
    m_anchorSeq.fetch_add(1, std::memory_order_release);   // even → done
}

void AVSyncClock::reset(int64_t tick) noexcept
{
    // Diagnostic: log every backwards reset, whether or not the clock
    // is currently flagged as "running".  Earlier the gate required
    // m_running=true, but observation (2026-05-23) showed that some
    // reset() callers fire during play/shuttle transitions when
    // m_running is briefly false, AND the resulting backward tick is
    // still visible to the FrameClock once running resumes — producing
    // a DIAG-CLOCK JUMP delta=-N without a matching DIAG-CLOCK BACK.
    // The relaxed condition catches every reset that LOWERS the tick.
    // The `caller` field still pins down which of play / shuttleForward
    // / shuttleReverse / seekInternal / AudioEngine::stop did it.
    // We additionally suppress logs where prevTick == 0 (legitimate
    // initial reset on startup / seek-to-zero) to avoid spam.
    const int64_t prevTick = m_tick.load(std::memory_order_acquire);
    if (prevTick > 0 && tick < prevTick) {
#if defined(_MSC_VER)
        void* caller = _ReturnAddress();
#elif defined(__GNUC__) || defined(__clang__)
        void* caller = __builtin_return_address(0);
#else
        void* caller = nullptr;
#endif
        // Backwards while RUNNING is a genuine sync anomaly — keep it at
        // warn so it survives the warn+ filter.  Backwards while paused is
        // just the user scrubbing left; debug-level so it doesn't flood
        // perf_log.txt (it produced dozens of lines per scrub session).
        // Single-line to stay parseable next to [DIAG-CLOCK] JUMP entries.
        if (m_running.load(std::memory_order_relaxed)) {
            spdlog::warn("[DIAG-CLOCK BACK] prev={} new={} delta={} running=1 caller=0x{:X}",
                         prevTick, tick, tick - prevTick,
                         reinterpret_cast<uintptr_t>(caller));
        } else {
            spdlog::debug("[DIAG-CLOCK BACK] prev={} new={} delta={} running=0 caller=0x{:X}",
                          prevTick, tick, tick - prevTick,
                          reinterpret_cast<uintptr_t>(caller));
        }
    }

    m_tick.store(tick, std::memory_order_release);

    // Update anchor so extrapolation starts from the new position (seqlock write)
    const auto now = std::chrono::steady_clock::now();
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    ).count();
    m_anchorSeq.fetch_add(1, std::memory_order_release);   // odd → writing
    m_anchorTick.store(tick, std::memory_order_relaxed);
    m_anchorNs.store(ns, std::memory_order_relaxed);
    m_anchorSeq.fetch_add(1, std::memory_order_release);   // even → done
}

// ── Query interface ─────────────────────────────────────────────────────

int64_t AVSyncClock::currentTick() const noexcept
{
    const int64_t baseTick = m_tick.load(std::memory_order_acquire);

    if (!m_running.load(std::memory_order_relaxed))
        return baseTick;

    // Seqlock read: retry if the audio thread is mid-write or if the
    // version changed between our two loads (torn anchor pair).
    int64_t anchorTick, anchorNs;
    for (;;) {
        const unsigned v0 = m_anchorSeq.load(std::memory_order_acquire);
        anchorTick = m_anchorTick.load(std::memory_order_relaxed);
        anchorNs   = m_anchorNs.load(std::memory_order_relaxed);
        const unsigned v1 = m_anchorSeq.load(std::memory_order_acquire);
        if (v0 == v1 && (v0 & 1u) == 0)
            break;  // consistent read
        // Audio thread was writing — spin (costs ~ns; audio write is ~ns too)
    }
    if (anchorNs <= 0) return baseTick;

    const auto now = std::chrono::steady_clock::now();
    const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    ).count();

    const int64_t elapsedNs = nowNs - anchorNs;
    if (elapsedNs <= 0) return baseTick;

    const int64_t speedFixed = m_speedFixed.load(std::memory_order_relaxed);
    const int64_t extraTicks = (elapsedNs * kTicksPerSecond * speedFixed)
                             / (1'000'000'000LL * 1000LL);

    return anchorTick + extraTicks;
}

double AVSyncClock::currentSeconds() const noexcept
{
    return static_cast<double>(currentTick()) / kTicksPerSecond;
}

int64_t AVSyncClock::currentFrame(uint32_t sampleRate) const noexcept
{
    if (sampleRate == 0) return 0;
    return (currentTick() * static_cast<int64_t>(sampleRate)) / kTicksPerSecond;
}

// ── Playback speed ──────────────────────────────────────────────────────

void AVSyncClock::setSpeed(double speed) noexcept
{
    // Store as fixed-point * 1000 to avoid float in audio thread
    m_speedFixed.store(static_cast<int64_t>(speed * 1000.0), std::memory_order_relaxed);
}

double AVSyncClock::speed() const noexcept
{
    return static_cast<double>(m_speedFixed.load(std::memory_order_relaxed)) / 1000.0;
}

// ── AV drift monitoring ─────────────────────────────────────────────────

void AVSyncClock::reportVideoPresentation(int64_t videoTick) noexcept
{
    m_lastVideoTick.store(videoTick, std::memory_order_relaxed);
}

int64_t AVSyncClock::drift() const noexcept
{
    return currentTick() - m_lastVideoTick.load(std::memory_order_relaxed);
}

double AVSyncClock::driftMs() const noexcept
{
    return static_cast<double>(drift()) / kTicksPerSecond * 1000.0;
}

// ── State ───────────────────────────────────────────────────────────────

bool AVSyncClock::isRunning() const noexcept
{
    return m_running.load(std::memory_order_relaxed);
}

void AVSyncClock::setRunning(bool running) noexcept
{
    m_running.store(running, std::memory_order_relaxed);
}

std::chrono::steady_clock::time_point AVSyncClock::lastAdvanceTime() const noexcept
{
    const auto ns = m_lastAdvanceNs.load(std::memory_order_relaxed);
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns));
}

double AVSyncClock::msSinceLastAdvance() const noexcept
{
    const int64_t lastNs = m_lastAdvanceNs.load(std::memory_order_relaxed);
    if (lastNs <= 0) return -1.0;
    const auto now = std::chrono::steady_clock::now();
    const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    return static_cast<double>(nowNs - lastNs) / 1.0e6;
}

} // namespace rt

