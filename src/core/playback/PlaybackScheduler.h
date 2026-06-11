#pragma once

#include "playback/EngineContracts.h"
#include "playback/FrameClock.h"
#include "playback/FrameProducer.h"
#include "playback/FramePresenter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace rt {

enum class ScheduledFrameAction : uint8_t
{
    Render,
    HoldPrevious,
    DropLate,
    Canceled
};

struct ScheduledFrameRequest
{
    RenderRequest request;
    FrameDiagnostics diagnostics;
    ScheduledFrameAction action{ScheduledFrameAction::Render};
    uint64_t generation{0};
};

struct PlaybackSchedulerStats
{
    uint64_t requestedFrames{0};
    uint64_t renderedFrames{0};
    uint64_t heldFrames{0};
    uint64_t droppedFrames{0};
    uint64_t canceledFrames{0};
};

class PlaybackController;
struct CachedFrame;

/// PlaybackScheduler — Central playback engine coordinator.
///
/// Owns and wires three independent modules:
///   FrameClock      — High-priority timing thread (replaces QTimer).
///   FrameProducer   — Compositor wrapper, called by FrameClock at each tick.
///   FramePresenter  — Display thread, consumes from FrameProducer.
///
/// Also provides frame-level scheduling intelligence: evaluating whether
/// each frame should be rendered, held (re-presented), or dropped based
/// on deadlines and generation tracking.
///
/// Data flow:
///   AVSyncClock → FrameClock → FrameProducer → FramePresenter → Swapchain
class PlaybackScheduler
{
public:
    using Clock = RenderRequest::Clock;

    /// Compositor function signature.
    using CompositeCallback = FrameProducer::CompositeCallback;

    /// Present callback (provided by UI layer).
    using PresentCallback = FramePresenter::PresentCallback;

    /// Post-present notification.
    using PresentNotify = FramePresenter::PresentNotify;

    PlaybackScheduler();
    ~PlaybackScheduler();

    PlaybackScheduler(const PlaybackScheduler&) = delete;
    PlaybackScheduler& operator=(const PlaybackScheduler&) = delete;

    // ── Configuration (set before start) ─────────────────────────────

    void setController(PlaybackController* c);
    void setCompositeCallback(CompositeCallback cb);
    void setPresentCallback(PresentCallback cb);
    void setPresentNotify(PresentNotify cb);

    /// Thread-safe resolution update.
    void setOutputResolution(uint32_t w, uint32_t h, int divisor = 1);

    // ── Adaptive playback resolution (UPGRADE_PLAN item 1) ──────────
    using AdaptiveTierCallback = FrameProducer::AdaptiveTierCallback;
    void setAdaptiveEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isAdaptiveEnabled() const noexcept;
    void setAdaptiveTierCallback(AdaptiveTierCallback cb);
    [[nodiscard]] int currentResDivisor() const noexcept;

    /// External suspend — the FrameClock keeps running (so audio sync
    /// state stays current) but its tick callback stops dispatching
    /// requestFrame() to the producer.  Used by the Export panel preview:
    /// the Export QTimer composites synchronously on the UI thread, and
    /// without this gate the main Program Monitor's producer thread
    /// would race the UI thread on m_compositeMutex — doubling effective
    /// composite work and producing the choppy-video / smooth-audio
    /// symptom users report during 60 fps ProRes preview playback.
    void setExternallySuspended(bool suspended) noexcept;
    [[nodiscard]] bool isExternallySuspended() const noexcept;

    // ── Lifecycle ────────────────────────────────────────────────────

    void start();
    void stop();
    [[nodiscard]] bool isRunning() const noexcept;

    // ── Scrub / seek (paused mode) ───────────────────────────────────

    void requestFrame(int64_t tick, uint32_t w, uint32_t h, bool scrub);
    void notifyStateChange();

    // ── Queries ──────────────────────────────────────────────────────

    [[nodiscard]] std::shared_ptr<CachedFrame> lastDisplayedFrame() const;
    [[nodiscard]] int droppedFrames() const noexcept;
    void resetDroppedFrames() noexcept;

    // ── Scheduling (intelligent frame dispatch) ──────────────────────

    [[nodiscard]] ScheduledFrameRequest schedulePlaybackFrame(
        int64_t tick,
        uint32_t width,
        uint32_t height,
        double fps,
        Clock::time_point now = Clock::now());

    [[nodiscard]] ScheduledFrameRequest scheduleStillFrame(
        int64_t tick,
        uint32_t width,
        uint32_t height,
        bool scrub,
        Clock::time_point now = Clock::now());

    void noteFramePresented(uint64_t requestId) noexcept;
    void noteHeldFrame(uint64_t requestId) noexcept;
    void noteDroppedFrame(uint64_t requestId) noexcept;
    void cancelPendingScrub() noexcept;

    [[nodiscard]] PlaybackSchedulerStats stats() const noexcept { return m_stats; }
    [[nodiscard]] uint64_t latestGeneration() const noexcept { return m_generation; }
    void reset() noexcept;

private:
    [[nodiscard]] ScheduledFrameRequest makeRequest(
        RenderRequestType type,
        int64_t tick,
        uint32_t width,
        uint32_t height,
        Clock::time_point deadline,
        const char* caller);

    // ── Pipeline modules ─────────────────────────────────────────────
    FrameClock     m_clock;
    FrameProducer  m_producer;
    FramePresenter m_presenter;

    // ── Scheduling state ─────────────────────────────────────────────
    uint64_t m_nextRequestId{0};
    uint64_t m_generation{0};
    int64_t m_lastPlaybackTick{-1};
    int m_playbackDirection{0};
    uint64_t m_latestScrubRequestId{0};
    PlaybackSchedulerStats m_stats;

    /// Frames skipped by backpressure (compositor < clock speed).
    std::atomic<int64_t> m_backpressureSkippedFrames{0};

    /// True while the Export panel (or another external owner) is the
    /// sole compositor.  Read by the FrameClock tick callback to skip
    /// requestFrame() so the producer thread doesn't compete with the
    /// external compositor.
    std::atomic<bool> m_externallySuspended{false};
};

[[nodiscard]] const char* toString(ScheduledFrameAction action) noexcept;

} // namespace rt
