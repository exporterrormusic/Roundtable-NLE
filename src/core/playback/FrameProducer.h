/*
 * FrameProducer — Asynchronous compositor wrapper.
 *
 * Runs on a dedicated thread.  Receives "produce frame" requests
 * from FrameClock (via requestFrame) and calls the compositor callback
 * on its own thread — keeping the TIME_CRITICAL clock thread unblocked.
 * Finished frames are published to a lock-free exchange slot for
 * FramePresenter to consume.
 *
 * Key improvements over the old monolithic renderThreadLoop:
 *   - Completely decoupled from both clock and presentation.
 *   - Holds the last good frame and republishes it on cache miss,
 *     so the presenter always has something to display.
 *   - Supports scrub/seek (paused) requests via a separate path.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace rt {

class PlaybackController;
struct CachedFrame;

class FrameProducer
{
public:
    /// Compositor function signature
    /// (wraps TimelineWorkspace::compositeFrame)
    using CompositeCallback = std::function<
        std::shared_ptr<CachedFrame>(int64_t tick, uint32_t w, uint32_t h, bool scrub)>;

    FrameProducer();
    ~FrameProducer();

    FrameProducer(const FrameProducer&) = delete;
    FrameProducer& operator=(const FrameProducer&) = delete;

    // ── Configuration ────────────────────────────────────────────────

    void setCompositeCallback(CompositeCallback cb) { m_compositeCB = std::move(cb); }
    void setController(PlaybackController* c) { m_controller = c; }

    /// Thread-safe resolution update.
    void setOutputResolution(uint32_t w, uint32_t h, int divisor = 1);

    // ── Lifecycle ────────────────────────────────────────────────────

    void start();
    void stop();
    [[nodiscard]] bool isRunning() const noexcept
    { return m_running.load(std::memory_order_relaxed); }

    // ── Frame production ─────────────────────────────────────────────

    /// Queue a frame request for the given tick (called from FrameClock).
    /// Non-blocking: pushes to request queue and returns immediately.
    void requestFrame(int64_t tick);

    /// Queue a scrub/seek request for the producer thread (paused mode).
    /// Non-blocking: pushes to the scrub request slot and returns
    /// immediately.  The producer thread picks it up and runs the
    /// compositor off the UI thread.
    void requestScrubFrame(int64_t tick, uint32_t w, uint32_t h, bool scrub);

    // ── Frame exchange (consumed by FramePresenter) ──────────────────

    struct FrameExchange {
        std::shared_ptr<CachedFrame> frame;
        int64_t tick{0};
        bool    hasNew{false};
    };

    /// Lock the exchange and read the latest frame.
    /// Returns true if a new frame was available.
    bool consumeFrame(std::shared_ptr<CachedFrame>& outFrame, int64_t& outTick);

    /// Wait for a new frame (for paused mode or presenter thread).
    /// Returns false if timed out.
    bool waitForFrame(std::chrono::milliseconds timeout);

    /// Notify waiters (wake presenter thread).
    void notifyPresenter() { m_exchangeCV.notify_one(); }

    // ── Queries ──────────────────────────────────────────────────────

    [[nodiscard]] std::shared_ptr<CachedFrame> lastProducedFrame() const;

    /// Returns true if the producer is backlogged — more frames are
    /// requested than the compositor can produce in real time.
    /// FrameClock calls this before requestFrame() to decide whether
    /// to skip a clock tick, preventing unbounded queue growth and
    /// keeping the compositor focused on the latest frame.
    [[nodiscard]] bool isBacklogged() const noexcept {
        return m_backpressure.load(std::memory_order_acquire);
    }

    /// Maximum pending frames before backpressure engages.
    /// Beyond this, requestFrame() will signal backpressure instead
    /// of enqueuing, and FrameClock should skip its composite tick.
    static constexpr size_t kMaxPendingFrames = 3;

    // ── Adaptive playback resolution (UPGRADE_PLAN item 1) ────────────
    //
    // When enabled, the producer measures rolling composite latency
    // against the project frame budget and shifts m_resDivisor between
    // Full (1), Half (2) and Quarter (4) automatically:
    //   - average latency > 1.4 × budget over the last kLatencyWindow
    //     frames → drop tier (Full→Half→Quarter)
    //   - average latency < 0.6 × budget for kPromoteStreak frames in a
    //     row → raise tier (Quarter→Half→Full)
    //   - composite > 2 × budget → skip the next composite call and
    //     re-publish the last good frame (frame-drop policy)
    //
    // The adaptive callback is invoked on the producer thread whenever
    // the divisor changes, so the UI layer can route the new tier to
    // CompositeService::setPlaybackTier and keep MediaPool decode size
    // in step with the composite size.
    using AdaptiveTierCallback = std::function<void(int divisor)>;
    void setAdaptiveEnabled(bool enabled) noexcept;
    [[nodiscard]] bool isAdaptiveEnabled() const noexcept
    { return m_adaptiveEnabled.load(std::memory_order_acquire); }
    void setAdaptiveTierCallback(AdaptiveTierCallback cb)
    { m_adaptiveTierCB = std::move(cb); }

    /// Current effective divisor (1 = Full, 2 = Half, 4 = Quarter,
    /// 8 = Eighth — Eighth never set by adaptive logic).
    [[nodiscard]] int currentDivisor() const noexcept
    { return m_resDivisor.load(std::memory_order_acquire); }

    /// Diagnostic accessor — points at the currently-active producer
    /// instance so MediaPool's CACHE-DUMP can query divisor + adaptive
    /// state without a hard dependency on PlaybackScheduler.  Set on
    /// start(), cleared on stop().  Tolerate nullptr.
    [[nodiscard]] static FrameProducer* activeForDiag() noexcept
    { return s_active.load(std::memory_order_acquire); }

private:
    struct ScrubRequest {
        int64_t  tick;
        uint32_t w, h;
        bool     scrub;
    };

    void producerLoop();
    void produceFrameImpl(int64_t tick);
    void produceScrubFrameImpl(const ScrubRequest& req);
    void publishFrame(std::shared_ptr<CachedFrame> frame, int64_t tick);

    CompositeCallback m_compositeCB;
    PlaybackController* m_controller{nullptr};

    // Resolution (atomics for lock-free cross-thread access)
    std::atomic<uint32_t> m_outputW{1920};
    std::atomic<uint32_t> m_outputH{1080};
    std::atomic<int>      m_resDivisor{2};

    // Worker thread
    std::thread       m_thread;
    std::atomic<bool> m_running{false};

    // Request queue (FrameClock → producer thread)
    std::mutex              m_reqMtx;
    std::condition_variable m_reqCV;
    std::deque<int64_t>     m_pendingTicks;
    std::optional<ScrubRequest> m_pendingScrub;  // latest scrub request (replaces prior)

    // Frame exchange slot (producer → presenter)
    mutable std::mutex       m_exchangeMtx;
    FrameExchange            m_exchange;
    std::condition_variable  m_exchangeCV;

    // Last good frame (for re-publish on cache miss)
    std::shared_ptr<CachedFrame> m_lastGoodFrame;

    // Backpressure: true when the compositor is too slow to keep up with
    // FrameClock.  Old logic only triggered when kMaxPendingFrames had
    // accumulated, but the queue is coalesced (newer ticks replace older
    // ones) so size rarely exceeds 1 — the flag almost never fired in
    // practice and the producer thrashed the compositor on every tick.
    //
    // New logic: count the number of consecutive requestFrame() calls that
    // arrive while a previous request is still pending.  Two or more
    // replacements in a row means the producer hasn't picked up the last
    // request before the clock issued a new one — i.e. we're a frame behind.
    std::atomic<bool>     m_backpressure{false};
    int                   m_consecutiveReplacements{0};
    static constexpr int  kBackpressureLagFrames = 2;

    // ── Adaptive playback resolution state (producer-thread only) ─────
    //
    // The window holds the last kLatencyWindow composite-latency samples
    // (in ms).  Tier decisions only fire once the window is full so the
    // controller doesn't act on cold-start outliers.  m_promoteStreak
    // counts consecutive sub-budget frames seen since the last drop or
    // promote — kPromoteStreak frames in a row at < 0.6 × budget bump
    // the tier up.  m_changeCooldown is set after every divisor change
    // to suppress flapping until the rolling window refills with the
    // post-change latency.
    static constexpr size_t kLatencyWindow  = 8;
    static constexpr int    kPromoteStreak  = 30;
    static constexpr int    kChangeCooldown = 8;
    std::atomic<bool>   m_adaptiveEnabled{false};
    AdaptiveTierCallback m_adaptiveTierCB;
    double              m_latencyWindow[kLatencyWindow]{};
    size_t              m_latencyIdx{0};
    size_t              m_latencyCount{0};
    int                 m_promoteStreak{0};
    int                 m_changeCooldown{0};
    bool                m_dropNextComposite{false};

    void recordCompositeMs(double ms);
    [[nodiscard]] double averageLatencyMs() const noexcept;
    [[nodiscard]] double currentFrameBudgetMs() const noexcept;
    void maybeAdjustTier(double frameBudgetMs);
    void applyAdaptiveDivisor(int newDivisor);

    static std::atomic<FrameProducer*> s_active;

    // Use-after-free guard
    std::atomic<bool> m_destroying{false};
};

} // namespace rt
