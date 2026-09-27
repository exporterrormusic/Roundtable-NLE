/*
 * FramePresenter.cpp — See FramePresenter.h for architecture.
 */

#include "playback/FramePresenter.h"
#include "playback/FrameProducer.h"
#include "playback/PlaybackController.h"
#include "playback/AVSyncClock.h"
#include "cache/FrameCache.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

FramePresenter::FramePresenter() = default;

FramePresenter::~FramePresenter()
{
    stop();
}

void FramePresenter::start()
{
    if (m_running.load()) return;
    m_running.store(true);
    m_destroying.store(false);
    m_thread = std::thread(&FramePresenter::presentLoop, this);
    spdlog::info("[FramePresenter] Started");
}

void FramePresenter::stop()
{
    if (!m_running.load()) return;
    m_destroying.store(true);
    m_running.store(false);
    if (m_producer) m_producer->notifyPresenter();  // wake if blocked
    if (m_thread.joinable()) m_thread.join();
    spdlog::info("[FramePresenter] Stopped");
}

void FramePresenter::reset() noexcept
{
    if (m_running.load(std::memory_order_acquire))
        return;
    std::lock_guard lock(m_displayMtx);
    m_lastDisplayed.reset();
}

void FramePresenter::wake()
{
    if (m_producer) m_producer->notifyPresenter();
}

std::shared_ptr<CachedFrame> FramePresenter::lastDisplayedFrame() const
{
    std::lock_guard lock(m_displayMtx);
    return m_lastDisplayed;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Present thread
// ═════════════════════════════════════════════════════════════════════════════

void FramePresenter::presentLoop()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"RT-Presenter");
#endif

    spdlog::info("[FramePresenter] Thread started (deadline-driven)");

    using SteadyClock = std::chrono::steady_clock;
    using Duration    = SteadyClock::duration;

    // Frame hold: keep the last displayed frame for re-present
    std::shared_ptr<CachedFrame> holdFrame;
    int64_t holdTick = 0;

    // Deadline-driven pacing: the presenter sleeps until a fixed
    // deadline, then non-blocking consumes whatever frame the producer
    // has ready.  This decouples display timing from compositor timing
    // for smooth cadence regardless of compositor variability.
    SteadyClock::time_point nextDeadline{};
    bool pacingActive = false;

    // Playback-only presentation telemetry. Keep this local to the presenter
    // thread so gathering it requires no locks or cross-thread coordination.
    constexpr uint64_t kPresentTelemetryWindow = 120;
    uint64_t presentSamples = 0;
    uint64_t freshPresents = 0;
    uint64_t heldPresents = 0;
    uint64_t failedPresents = 0;
    double presentTotalMs = 0.0;
    double presentMaxMs = 0.0;

    while (m_running.load(std::memory_order_relaxed) && !m_destroying.load(std::memory_order_acquire)) {

        const bool playing = m_controller && m_controller->isPlaying();

        std::shared_ptr<CachedFrame> frame;
        int64_t tick = 0;
        bool consumedNew = false;

        if (playing) {
            // ── DEADLINE-DRIVEN (playback) ───────────────────────────
            // Sleep until the next fixed-interval deadline, then
            // non-blocking consume whatever frame is available.
            const double fps = m_controller->frameRate();
            if (fps <= 0.0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            const auto frameDur = std::chrono::duration_cast<Duration>(
                std::chrono::duration<double>(1.0 / fps));

            if (!pacingActive) {
                nextDeadline = SteadyClock::now() + frameDur;
                pacingActive = true;
            } else {
                std::this_thread::sleep_until(nextDeadline);
                if (!m_running.load(std::memory_order_relaxed)) break;
            }

            if (m_producer) {
                // Show the frame that is due at the audio clock position,
                // not merely the newest one: FrameClock renders ahead, so
                // producer timing jitter no longer turns into held/dropped
                // frames at this deadline.
                auto* sync = m_controller->syncClock();
                const double speed = sync ? sync->speed()
                                          : m_controller->shuttleSpeed();
                const int64_t ticksPerFrame =
                    std::max<int64_t>(1, static_cast<int64_t>(48000.0 / fps));
                const int64_t displayTick = m_controller->currentTick();
                consumedNew = m_producer->consumeFrameForTick(
                    displayTick, ticksPerFrame,
                    /*reverse=*/speed < 0.0, frame, tick);

                // Phase-lock deadlines to MID-frame of the audio clock.  A
                // free-running deadline that sits near a frame boundary maps
                // two consecutive deadlines to the same frame under clock
                // jitter (a repeat) and then skips one.  Nudge the next
                // deadline toward the frame middle, ≤2 ms per frame, at 1x
                // only (shuttle speeds cross frames at other rates).
                if (std::abs(std::abs(speed) - 1.0) < 0.01) {
                    int64_t phase = displayTick % ticksPerFrame;
                    if (phase < 0) phase += ticksPerFrame;
                    // Fraction of the frame already elapsed in the playback
                    // direction, minus one half: > 0 means we are late.
                    double lateness = static_cast<double>(phase) /
                        static_cast<double>(ticksPerFrame) - 0.5;
                    if (speed < 0.0) lateness = -lateness;
                    constexpr double kGain = 0.25;
                    const auto maxStep = std::chrono::duration_cast<Duration>(
                        std::chrono::milliseconds(2));
                    auto step = std::chrono::duration_cast<Duration>(
                        frameDur * (lateness * kGain));
                    step = std::clamp(step, -maxStep, maxStep);
                    nextDeadline -= step;  // late → present earlier next time
                }
            }

            nextDeadline += frameDur;

            auto now = SteadyClock::now();
            if (nextDeadline < now - frameDur * 2) {
                nextDeadline = now + frameDur;
            }

        } else {
            // ── EVENT-DRIVEN (paused / scrub) ────────────────────────
            pacingActive = false;
            // Block until the producer publishes — gives immediate
            // response to scrub / seek with no wasted CPU.

            if (m_producer) {
                m_producer->waitForFrame(std::chrono::milliseconds(5));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!m_running.load(std::memory_order_relaxed)) break;

            if (m_producer) {
                consumedNew = m_producer->consumeFrame(frame, tick);
            }
        }

        // Fall back to held frame if no new frame available
        if (!frame && holdFrame) {
            frame = holdFrame;
            tick  = holdTick;
        }

        if (!frame || !m_presentCB) continue;

        // Update hold buffer
        bool isNewFrame = (frame != holdFrame);
        holdFrame = frame;
        holdTick  = tick;

        // Skip re-presenting the same held frame when paused — avoids
        // hammering the Vulkan upload + present path at ~200fps with
        // identical data, which can trigger VK_ERROR_DEVICE_LOST.
        if (!isNewFrame && !playing) continue;

        // ── Present via callback ─────────────────────────────────────
        auto presentStart = SteadyClock::now();
        bool presented = m_presentCB(frame);
        auto presentEnd = SteadyClock::now();
        double presentMs = std::chrono::duration<double, std::milli>(presentEnd - presentStart).count();

        if (playing) {
            ++presentSamples;
            if (consumedNew) ++freshPresents;
            else ++heldPresents;
            if (!presented) ++failedPresents;
            presentTotalMs += presentMs;
            presentMaxMs = std::max(presentMaxMs, presentMs);

            if (presentSamples >= kPresentTelemetryWindow) {
                const double avgMs = presentTotalMs /
                    static_cast<double>(presentSamples);
                const double fps = m_controller ? m_controller->frameRate() : 30.0;
                const double budgetMs = 1000.0 / std::max(fps, 1.0);
                const bool unhealthy = failedPresents != 0 ||
                    heldPresents > presentSamples / 20 || avgMs > budgetMs;
                spdlog::log(
                    unhealthy ? spdlog::level::warn : spdlog::level::info,
                    "[PLAYBACK-PRESENT] n={} fresh={} held={} failed={} "
                    "presentMs[avg={:.2f} max={:.2f}] budget={:.2f}",
                    presentSamples, freshPresents, heldPresents,
                    failedPresents, avgMs, presentMaxMs, budgetMs);
                presentSamples = 0;
                freshPresents = 0;
                heldPresents = 0;
                failedPresents = 0;
                presentTotalMs = 0.0;
                presentMaxMs = 0.0;
            }
        }

        if (presented) {
            // Update last-displayed for scopes / export
            {
                std::lock_guard lock(m_displayMtx);
                m_lastDisplayed = frame;
            }

            // Notify listeners
            if (m_presentNotify) {
                m_presentNotify(tick);
            }

            // Report A/V drift to sync clock
            if (m_controller && m_controller->isPlaying()) {
                if (auto* clk = m_controller->syncClock())
                    clk->reportVideoPresentation(tick);
            }
        }
    }

    spdlog::info("[FramePresenter] Thread exiting");
}

} // namespace rt
