#include "playback/PlaybackScheduler.h"
#include "playback/PlaybackController.h"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace rt {
namespace {

[[nodiscard]] std::chrono::milliseconds playbackDeadlineFor(double fps)
{
    const double safeFps = std::max(fps, 1.0);
    const auto frameMs = static_cast<int64_t>((1000.0 / safeFps) * 1.5);
    return std::chrono::milliseconds(std::clamp<int64_t>(frameMs, 8, 50));
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
//  Construction / wiring
// ═════════════════════════════════════════════════════════════════════════════

PlaybackScheduler::PlaybackScheduler()
{
    // Wire FrameProducer as the data source for FramePresenter
    m_presenter.setProducer(&m_producer);

    // Wire FrameClock to push tick requests to FrameProducer's queue.
    // When the producer is backlogged (compositor slower than clock),
    // skip the tick to let the producer catch up.  Without this check,
    // the clock fires at 60fps regardless of compositor throughput,
    // and the producer wastes CPU replacing its queue entry with every
    // tick instead of compositing.  The skipped frame is counted so
    // diagnostics and A/V sync can react.
    m_clock.setFrameCallback([this](int64_t tick, int64_t /*frame*/) {
        // External owner (Export preview) has taken sole compositing.
        // Skip without counting as backpressure — this is intentional,
        // not a regression of throughput.
        if (m_externallySuspended.load(std::memory_order_acquire))
            return;
        if (m_producer.isBacklogged()) {
            m_backpressureSkippedFrames.fetch_add(1, std::memory_order_relaxed);
            return; // skip — compositor can't keep up
        }
        m_producer.requestFrame(tick);
    });

    // When playback state changes, wake the presenter
    m_clock.setStateCallback([this](bool playing) {
        if (playing) {
            m_presenter.wake();
        }
    });
}

PlaybackScheduler::~PlaybackScheduler()
{
    stop();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Configuration
// ═════════════════════════════════════════════════════════════════════════════

void PlaybackScheduler::setController(PlaybackController* c)
{
    m_clock.setController(c);
    m_producer.setController(c);
    m_presenter.setController(c);
}

void PlaybackScheduler::setCompositeCallback(CompositeCallback cb)
{
    m_producer.setCompositeCallback(std::move(cb));
}

void PlaybackScheduler::setPresentCallback(PresentCallback cb)
{
    m_presenter.setPresentCallback(std::move(cb));
}

void PlaybackScheduler::setPresentNotify(PresentNotify cb)
{
    m_presenter.setPresentNotify(std::move(cb));
}

void PlaybackScheduler::setOutputResolution(uint32_t w, uint32_t h, int divisor)
{
    m_producer.setOutputResolution(w, h, divisor);
}

// ── Adaptive playback resolution ───────────────────────────────────

void PlaybackScheduler::setAdaptiveEnabled(bool enabled) noexcept
{
    m_producer.setAdaptiveEnabled(enabled);
}

bool PlaybackScheduler::isAdaptiveEnabled() const noexcept
{
    return m_producer.isAdaptiveEnabled();
}

void PlaybackScheduler::setAdaptiveTierCallback(AdaptiveTierCallback cb)
{
    m_producer.setAdaptiveTierCallback(std::move(cb));
}

int PlaybackScheduler::currentResDivisor() const noexcept
{
    return m_producer.currentDivisor();
}

void PlaybackScheduler::setExternallySuspended(bool suspended) noexcept
{
    const bool prev = m_externallySuspended.exchange(
        suspended, std::memory_order_acq_rel);
    if (prev == suspended) return;
    spdlog::info("[PlaybackScheduler] external suspend {}",
                 suspended ? "ON" : "OFF");
}

bool PlaybackScheduler::isExternallySuspended() const noexcept
{
    return m_externallySuspended.load(std::memory_order_acquire);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void PlaybackScheduler::start()
{
    if (isRunning()) return;

    m_presenter.start();
    m_producer.start();
    m_clock.start();
    spdlog::info("[PlaybackScheduler] Started (clock + producer + presenter)");
}

void PlaybackScheduler::stop()
{
    if (isRunning()) {
        spdlog::info("[PlaybackScheduler] Stopping (clock={} prod={} pres={})",
                     m_clock.isRunning(), m_producer.isRunning(), m_presenter.isRunning());
        m_clock.stop();
        m_producer.stop();
        m_presenter.stop();
        spdlog::info("[PlaybackScheduler] Stopped");
    }

    // A stopped pipeline may later be restarted for a different sequence.
    // Do not let its producer exchange or either hold-frame cross that
    // sequence boundary.
    m_producer.reset();
    m_presenter.reset();
}

bool PlaybackScheduler::isRunning() const noexcept
{
    return m_clock.isRunning() || m_producer.isRunning() || m_presenter.isRunning();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Scrub / seek
// ═════════════════════════════════════════════════════════════════════════════

void PlaybackScheduler::requestFrame(int64_t tick, uint32_t w, uint32_t h, bool scrub)
{
    m_producer.requestScrubFrame(tick, w, h, scrub);
    m_presenter.wake();
}

void PlaybackScheduler::notifyStateChange()
{
    m_clock.wake();
    m_presenter.wake();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Queries
// ═════════════════════════════════════════════════════════════════════════════

std::shared_ptr<CachedFrame> PlaybackScheduler::lastDisplayedFrame() const
{
    return m_presenter.lastDisplayedFrame();
}

int PlaybackScheduler::droppedFrames() const noexcept
{
    return m_clock.droppedFrames()
         + static_cast<int>(m_backpressureSkippedFrames.load(std::memory_order_relaxed));
}

void PlaybackScheduler::resetDroppedFrames() noexcept
{
    m_clock.resetDroppedFrames();
    m_backpressureSkippedFrames.store(0, std::memory_order_relaxed);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Scheduling
// ═════════════════════════════════════════════════════════════════════════════

ScheduledFrameRequest PlaybackScheduler::schedulePlaybackFrame(
    int64_t tick,
    uint32_t width,
    uint32_t height,
    double fps,
    Clock::time_point now)
{
    auto scheduled = makeRequest(
        RenderRequestType::Playback,
        tick,
        width,
        height,
        now + playbackDeadlineFor(fps),
        "PlaybackScheduler::schedulePlaybackFrame");

    if (m_lastPlaybackTick >= 0 && tick == m_lastPlaybackTick) {
        scheduled.action = ScheduledFrameAction::DropLate;
        scheduled.diagnostics.status = RenderResultStatus::Canceled;
        scheduled.diagnostics.droppedFrame = true;
        ++m_stats.droppedFrames;
        ++m_stats.canceledFrames;
        return scheduled;
    }

    if (m_lastPlaybackTick >= 0) {
        m_playbackDirection = (tick > m_lastPlaybackTick) ? 1 : -1;
    }
    m_lastPlaybackTick = tick;
    scheduled.action = ScheduledFrameAction::Render;
    return scheduled;
}

ScheduledFrameRequest PlaybackScheduler::scheduleStillFrame(
    int64_t tick,
    uint32_t width,
    uint32_t height,
    bool scrub,
    Clock::time_point now)
{
    const auto type = scrub ? RenderRequestType::Scrub : RenderRequestType::Still;
    auto scheduled = makeRequest(
        type,
        tick,
        width,
        height,
        now + std::chrono::milliseconds(scrub ? 150 : 500),
        "PlaybackScheduler::scheduleStillFrame");

    if (scrub) {
        if (m_latestScrubRequestId != 0 && m_latestScrubRequestId != scheduled.request.requestId) {
            ++m_generation;
        }
        m_latestScrubRequestId = scheduled.request.requestId;
        scheduled.generation = m_generation;
    }

    scheduled.action = ScheduledFrameAction::Render;
    return scheduled;
}

void PlaybackScheduler::noteFramePresented(uint64_t requestId) noexcept
{
    if (requestId == 0) {
        return;
    }
    ++m_stats.renderedFrames;
}

void PlaybackScheduler::noteHeldFrame(uint64_t requestId) noexcept
{
    if (requestId == 0) {
        return;
    }
    ++m_stats.heldFrames;
}

void PlaybackScheduler::noteDroppedFrame(uint64_t requestId) noexcept
{
    if (requestId == 0) {
        return;
    }
    ++m_stats.droppedFrames;
}

void PlaybackScheduler::cancelPendingScrub() noexcept
{
    if (m_latestScrubRequestId == 0) {
        return;
    }
    m_latestScrubRequestId = 0;
    ++m_generation;
    ++m_stats.canceledFrames;
}

void PlaybackScheduler::reset() noexcept
{
    // Callers that reset scheduling state also expect a clean frame boundary.
    // These are no-ops while running; stop() performs the ordered join/reset.
    m_producer.reset();
    m_presenter.reset();
    m_nextRequestId = 0;
    m_generation = 0;
    m_lastPlaybackTick = -1;
    m_playbackDirection = 0;
    m_latestScrubRequestId = 0;
    m_stats = {};
}

ScheduledFrameRequest PlaybackScheduler::makeRequest(
    RenderRequestType type,
    int64_t tick,
    uint32_t width,
    uint32_t height,
    Clock::time_point deadline,
    const char* caller)
{
    ScheduledFrameRequest scheduled;
    scheduled.generation = m_generation;
    scheduled.request.requestId = ++m_nextRequestId;
    scheduled.request.type = type;
    scheduled.request.quality = defaultQualityFor(type);
    scheduled.request.exactness = defaultExactnessFor(type);
    scheduled.request.timelineTick = tick;
    scheduled.request.outputWidth = width;
    scheduled.request.outputHeight = height;
    scheduled.request.deadline = deadline;
    scheduled.request.caller = caller ? caller : "PlaybackScheduler";

    scheduled.diagnostics.requestId = scheduled.request.requestId;
    scheduled.diagnostics.status = RenderResultStatus::Pending;

    ++m_stats.requestedFrames;
    return scheduled;
}

const char* toString(ScheduledFrameAction action) noexcept
{
    switch (action) {
    case ScheduledFrameAction::Render:       return "Render";
    case ScheduledFrameAction::HoldPrevious: return "HoldPrevious";
    case ScheduledFrameAction::DropLate:     return "DropLate";
    case ScheduledFrameAction::Canceled:     return "Canceled";
    }
    return "Unknown";
}

} // namespace rt
