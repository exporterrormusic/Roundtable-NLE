/*
 * FramePresenter — Event-driven display thread.
 *
 * Consumes frames from FrameProducer and presents them to the
 * window surface via a callback (VulkanViewport).  Runs on its own
 * high-priority thread.
 *
 * Key design:
 *   - DEADLINE-DRIVEN (playback): sleeps until a fixed-interval
 *     deadline, then non-blocking consumes whatever frame the producer
 *     has ready.  This decouples display timing from compositor timing
 *     so that frames reach the swapchain at rock-steady intervals
 *     matching the project framerate, regardless of compositor jitter.
 *   - EVENT-DRIVEN (paused): blocks on FrameProducer::waitForFrame()
 *     for immediate scrub / seek responsiveness.
 *   - Frame-hold: re-presents the last good frame if no new frame
 *     is available, preventing blank/flicker.
 *   - Present callback is provided by ProgramMonitor and calls
 *     through to VulkanViewport.
 *   - Reports A/V drift to AVSyncClock for monitoring.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace rt {

class PlaybackController;
class FrameProducer;
struct CachedFrame;

class FramePresenter
{
public:
    /// Returns true if the frame was actually displayed.
    using PresentCallback = std::function<
        bool(const std::shared_ptr<CachedFrame>& frame)>;

    /// Called after each successful present (for UI update signals).
    using PresentNotify = std::function<void(int64_t tick)>;

    FramePresenter();
    ~FramePresenter();

    FramePresenter(const FramePresenter&) = delete;
    FramePresenter& operator=(const FramePresenter&) = delete;

    // ── Configuration ────────────────────────────────────────────────

    void setController(PlaybackController* c)  { m_controller = c; }
    void setProducer(FrameProducer* p)         { m_producer = p; }
    void setPresentCallback(PresentCallback cb) { m_presentCB = std::move(cb); }
    void setPresentNotify(PresentNotify cb)     { m_presentNotify = std::move(cb); }

    // ── Lifecycle ────────────────────────────────────────────────────

    void start();
    void stop();
    [[nodiscard]] bool isRunning() const noexcept
    { return m_running.load(std::memory_order_relaxed); }

    /// Wake the present thread (play/pause transitions, new scrub frame).
    void wake();

    // ── Queries ──────────────────────────────────────────────────────

    [[nodiscard]] std::shared_ptr<CachedFrame> lastDisplayedFrame() const;

private:
    void presentLoop();

    PlaybackController* m_controller{nullptr};
    FrameProducer*      m_producer{nullptr};
    PresentCallback     m_presentCB;
    PresentNotify       m_presentNotify;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};

    // Use-after-free guard
    std::atomic<bool> m_destroying{false};

    // Last displayed frame (for scopes / export readback)
    mutable std::mutex           m_displayMtx;
    std::shared_ptr<CachedFrame> m_lastDisplayed;
};

} // namespace rt
