/*
 * FrameClock.cpp — High-priority self-driven frame clock.
 * See FrameClock.h for architecture.
 */

#include "playback/FrameClock.h"
#include "playback/PlaybackController.h"
#include "playback/AVSyncClock.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

FrameClock::FrameClock() = default;

FrameClock::~FrameClock()
{
    stop();
}

void FrameClock::start()
{
    if (m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread(&FrameClock::clockLoop, this);
    spdlog::info("[FrameClock] Started");
}

void FrameClock::stop()
{
    if (!m_running.load()) return;
    m_running.store(false);
    m_wakeCV.notify_all();
    if (m_thread.joinable()) m_thread.join();
    spdlog::info("[FrameClock] Stopped");
}

void FrameClock::wake()
{
    m_wakeCV.notify_all();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Clock thread
// ═════════════════════════════════════════════════════════════════════════════

void FrameClock::clockLoop()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(GetCurrentThread(), L"RT-FrameClock");
#endif

    using clock = std::chrono::steady_clock;
    auto nextDeadline = clock::now();
    bool wasPlaying = false;

    while (m_running.load(std::memory_order_relaxed)) {

        const bool playing = m_controller && m_controller->isPlaying();

        // ── State transition detection ───────────────────────────────
        if (playing != wasPlaying) {
            wasPlaying = playing;
            if (playing) {
                // Reset deadline on play start — first frame fires immediately
                nextDeadline = clock::now();
            }
            if (m_stateCB) m_stateCB(playing);
        }

        if (playing) {
            // ── PLAYBACK: accumulator-timed frame ticks ──────────────

            const double fps = m_controller->frameRate();
            const auto frameDuration = std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(1.0 / std::max(fps, 1.0)));

            // Sleep until deadline
            auto now = clock::now();
            if (nextDeadline > now) {
                std::this_thread::sleep_until(nextDeadline);
            }

            // Advance deadline (accumulator — no drift)
            nextDeadline += frameDuration;

            // If we fell behind by 2+ frames, reset to prevent burst
            now = clock::now();
            if (nextDeadline < now - frameDuration * 2) {
                nextDeadline = now + frameDuration;
                m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
            }

            // Read current audio clock position
            const int64_t tick = m_controller->currentTick();

            // Snap to frame boundary
            const int64_t ticksPerFrame = (fps > 0.0)
                ? static_cast<int64_t>(48000.0 / fps) : 1600;
            const int64_t frameNum = tick / ticksPerFrame;
            const int64_t snappedTick = frameNum * ticksPerFrame;

            // Fire callback
            if (m_frameCB) {
                // DIAG: log clock ticks to verify audio clock stability
                {
                    static int s_clockLog = 0;
                    static int64_t s_lastFrame = -1;
                    int64_t frameDelta = (s_lastFrame >= 0) ? (frameNum - s_lastFrame) : 0;
                    if (++s_clockLog % 15 == 0) {
                        spdlog::info("[DIAG-CLOCK] tick={} frame={} delta={} fps={:.1f}",
                                     snappedTick, frameNum, frameDelta, fps);
                    } else if (frameDelta > 2 || frameDelta < 0) {
                        // Attach sync-clock state so we can tell apart the
                        // three known causes of a JUMP:
                        //   (a) speed != 1.0          → speed field shows it
                        //   (b) audio thread stalled  → msSinceAdvance large
                        //                                + extrapGap grows
                        //   (c) FrameClock thread was preempted → masterTick
                        //                                 jumped by ~delta
                        //                                 frames AND extrap-
                        //                                 olation matches.
                        auto* sync = m_controller ? m_controller->syncClock() : nullptr;
                        if (sync) {
                            const int64_t master = sync->masterTick();
                            const int64_t extrapGap = sync->currentTick() - master;
                            const double  msSinceAdv = sync->msSinceLastAdvance();
                            const double  speedNow = sync->speed();
                            spdlog::warn("[DIAG-CLOCK] JUMP tick={} frame={} "
                                         "delta={} fps={:.1f} speed={:.2f} "
                                         "masterTick={} extrapGap={} "
                                         "msSinceAdvance={:.1f}",
                                         snappedTick, frameNum, frameDelta, fps,
                                         speedNow, master, extrapGap, msSinceAdv);
                        } else {
                            spdlog::warn("[DIAG-CLOCK] JUMP tick={} frame={} "
                                         "delta={} fps={:.1f} (no syncClock)",
                                         snappedTick, frameNum, frameDelta, fps);
                        }
                    }
                    s_lastFrame = frameNum;
                }
                m_frameCB(snappedTick, frameNum);
            }

        } else {
            // ── PAUSED: block until woken ────────────────────────────
            // Use a short timeout (5ms) so the clock reacts quickly
            // to play-state changes.  notifyStateChange() can also
            // wake us immediately via m_wakeCV.notify_all().
            std::unique_lock lock(m_wakeMtx);
            m_wakeCV.wait_for(lock, std::chrono::milliseconds(5), [this] {
                return !m_running.load(std::memory_order_relaxed)
                    || (m_controller && m_controller->isPlaying());
            });
        }
    }

    spdlog::info("[FrameClock] Thread exiting");
}

} // namespace rt
