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

    while (m_running.load(std::memory_order_relaxed) && !m_destroying.load(std::memory_order_acquire)) {

        const bool playing = m_controller && m_controller->isPlaying();

        std::shared_ptr<CachedFrame> frame;
        int64_t tick = 0;

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
                m_producer->consumeFrame(frame, tick);
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
                m_producer->consumeFrame(frame, tick);
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

        // DIAG: log every 5th presented frame with timing
        {
            static SteadyClock::time_point s_lastPresent{};
            static int64_t s_lastPresentTick = 0;
            static int s_diagCount = 0;
            double sinceLastMs = 0.0;
            if (s_lastPresent != SteadyClock::time_point{}) {
                sinceLastMs = std::chrono::duration<double, std::milli>(presentEnd - s_lastPresent).count();
            }
            if (++s_diagCount % 5 == 0) {
                spdlog::info("[DIAG-PRESENTER] tick={} presented={} presentMs={:.1f} "
                             "sinceLastPresent={:.1f}ms tickDelta={} "
                             "gpuReady={} gpuView=0x{:X} hold={}",
                             tick, presented, presentMs, sinceLastMs,
                             tick - s_lastPresentTick,
                             frame->gpuReady, frame->gpuImageView,
                             isNewFrame ? "NEW" : "HELD");
            }
            s_lastPresent = presentEnd;
            s_lastPresentTick = tick;
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
