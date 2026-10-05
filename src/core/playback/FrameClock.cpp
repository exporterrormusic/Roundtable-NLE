/*
 * FrameClock.cpp — High-priority self-driven frame clock.
 * See FrameClock.h for architecture.
 */

#include "playback/FrameClock.h"
#include "playback/PlaybackController.h"
#include "playback/AVSyncClock.h"
#include "FrameTime.h"

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
    // Render-ahead: request frames kRenderAheadFrames ahead of the audio
    // position so FramePresenter (which picks the frame DUE at the audio
    // position) always has one ready.  Ramped from 0 at play start so the
    // first ticks still render the current frame immediately.
    constexpr int64_t kRenderAheadFrames = 2;
    int64_t ticksSincePlay = 0;
    // Sequential requests: at 1x the clock asks for lastRequested+1 rather
    // than re-deriving the frame from the audio position every tick.  A
    // tick landing near a frame boundary plus audio-clock jitter otherwise
    // rounds to N, N+2 — frame N+1 is never rendered and the presenter
    // repeats a frame (~15-20% held frames in perf_log 2026-09-26).
    bool    haveLastRequest = false;
    int64_t lastRequestedFrame = 0;
    int64_t lastRequestDir = 1;
    uint64_t lastResetGeneration = 0;

    while (m_running.load(std::memory_order_relaxed)) {

        const bool playing = m_controller && m_controller->isPlaying();

        // ── State transition detection ───────────────────────────────
        if (playing != wasPlaying) {
            wasPlaying = playing;
            if (playing) {
                // Reset deadline on play start — first frame fires immediately
                nextDeadline = clock::now();
                m_diagJumpDetector.reset();
                ticksSincePlay = 0;
                haveLastRequest = false;
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

            auto* sync = m_controller ? m_controller->syncClock() : nullptr;
            const uint64_t resetGeneration = sync
                ? sync->resetGeneration() : 0;
            const double speedNow = sync
                ? sync->speed() : m_controller->shuttleSpeed();

            // Read current audio clock position
            const int64_t tick = m_controller->currentTick();

            // Snap to the canonical frame grid (FrameTime.h) — the same ticks
            // export and "Render In to Out" key on. A truncated 48000/fps
            // (1601 at 29.97) drifted a frame every ~100 s: a repeated frame
            // each time, and pre-rendered segments never hit.
            const RationalFrameRate rate = canonicalFrameRate(fps);
            const int64_t frameNum = tickToFrameIndex(tick, rate);
            const int64_t snappedTick = frameIndexToTick(frameNum, rate);

            // Fire callback
            if (m_frameCB) {
                // DIAG: log clock ticks to verify audio clock stability
                {
                    const auto observation = m_diagJumpDetector.observe(
                        frameNum, speedNow, resetGeneration);
                    if (observation.intentionalReset) {
                        spdlog::debug("[DIAG-CLOCK] RESET tick={} frame={} "
                                      "speed={:.2f} resetGen={}",
                                      snappedTick, frameNum, speedNow,
                                      resetGeneration);
                    } else if (observation.unexpectedJump) {
                        // Attach sync-clock state so we can tell apart the
                        // three known causes of a JUMP:
                        //   (a) speed != 1.0          → speed field shows it
                        //   (b) audio thread stalled  → msSinceAdvance large
                        //                                + extrapGap grows
                        //   (c) FrameClock thread was preempted → masterTick
                        //                                 jumped by ~delta
                        //                                 frames AND extrap-
                        //                                 olation matches.
                        if (sync) {
                            const int64_t master = sync->masterTick();
                            const int64_t extrapGap = sync->currentTick() - master;
                            const double  msSinceAdv = sync->msSinceLastAdvance();
                            spdlog::warn("[DIAG-CLOCK] JUMP tick={} frame={} "
                                         "delta={} fps={:.1f} speed={:.2f} "
                                         "masterTick={} extrapGap={} "
                                         "msSinceAdvance={:.1f}",
                                         snappedTick, frameNum, observation.delta, fps,
                                         speedNow, master, extrapGap, msSinceAdv);
                        } else {
                            spdlog::warn("[DIAG-CLOCK] JUMP tick={} frame={} "
                                         "delta={} fps={:.1f} (no syncClock)",
                                         snappedTick, frameNum,
                                         observation.delta, fps);
                        }
                    }
                }
                // Lead in the playback direction (reverse shuttle renders
                // behind).  |speed| > 1 shuttles skip frames anyway; keep
                // the lead in frames of timeline time.
                const int64_t dir = speedNow < 0.0 ? -1 : 1;
                const int64_t lead = std::min(ticksSincePlay, kRenderAheadFrames);
                const int64_t target = std::max<int64_t>(0, frameNum + dir * lead);
                ++ticksSincePlay;

                const bool normalSpeed = std::abs(std::abs(speedNow) - 1.0) < 0.01;
                const bool resync = !haveLastRequest || !normalSpeed ||
                    dir != lastRequestDir ||
                    resetGeneration != lastResetGeneration;
                int64_t requestFrameNum = target;
                if (!resync) {
                    const int64_t next = lastRequestedFrame + dir;
                    // > 0: `next` is further along than the lead target.
                    const int64_t aheadOfTarget = (next - target) * dir;
                    if (aheadOfTarget > 1 && aheadOfTarget <= 3) {
                        // Clock slightly outran audio: skip this tick, the
                        // queue already holds the upcoming frames.
                        continue;
                    }
                    if (aheadOfTarget >= -2 && aheadOfTarget <= 1)
                        requestFrameNum = std::max<int64_t>(0, next);
                    // else: seek / loop wrap / fell behind → resync to target
                }
                haveLastRequest     = true;
                lastRequestedFrame  = requestFrameNum;
                lastRequestDir      = dir;
                lastResetGeneration = resetGeneration;
                m_frameCB(frameIndexToTick(requestFrameNum, rate), requestFrameNum);
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
