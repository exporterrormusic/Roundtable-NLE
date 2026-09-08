/*
 * FrameProducer.cpp — See FrameProducer.h for architecture.
 */

#include "playback/FrameProducer.h"
#include "cache/FrameCache.h"  // CachedFrame
#include "playback/PlaybackController.h"
#include "GpuContext.h"

#include <spdlog/spdlog.h>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

std::atomic<FrameProducer*> FrameProducer::s_active{nullptr};

FrameProducer::FrameProducer() = default;

FrameProducer::~FrameProducer()
{
    stop();
}

void FrameProducer::setCompositeCallback(CompositeCallback cb)
{
    if (!cb) {
        m_compositeResultCB = nullptr;
        return;
    }

    m_compositeResultCB = [cb = std::move(cb)](
        int64_t tick, uint32_t w, uint32_t h, bool scrub, bool still) {
        RenderResult result;
        result.timelineTick = tick;
        result.frame = cb(tick, w, h, scrub, still);
        if (!result.frame) {
            result.status = RenderResultStatus::Pending;
        } else if (result.frame->width == 0 || result.frame->height == 0) {
            result.status = RenderResultStatus::Blank;
        } else {
            result.status = RenderResultStatus::Ready;
            result.diagnostics.cacheHit = result.frame->segmentCacheHit;
        }
        result.diagnostics.status = result.status;
        return result;
    };
}

void FrameProducer::setOutputResolution(uint32_t w, uint32_t h, int divisor)
{
    m_outputW.store(w, std::memory_order_relaxed);
    m_outputH.store(h, std::memory_order_relaxed);
    m_resDivisor.store(divisor, std::memory_order_relaxed);
}

void FrameProducer::setAdaptiveEnabled(bool enabled) noexcept
{
    const bool prev = m_adaptiveEnabled.exchange(enabled, std::memory_order_acq_rel);
    if (prev == enabled) return;
    // Reset the rolling window so cold-start frames after toggling don't
    // bias an immediate tier decision in either direction.
    m_latencyIdx = 0;
    m_latencyCount = 0;
    m_promoteStreak = 0;
    m_changeCooldown = 0;
    m_dropNextComposite = false;
    spdlog::info("[FrameProducer] adaptive playback resolution {}",
                 enabled ? "ENABLED" : "DISABLED");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void FrameProducer::start()
{
    if (m_running.load()) return;
    m_running.store(true);
    m_destroying.store(false);
    s_active.store(this, std::memory_order_release);
    m_thread = std::thread(&FrameProducer::producerLoop, this);
    spdlog::info("[FrameProducer] Started");
}

void FrameProducer::stop()
{
    if (!m_running.load()) return;
    spdlog::info("[FP-TRACE] FrameProducer::stop() called (running=true, destroying={})",
                 m_destroying.load());
    m_destroying.store(true);
    m_running.store(false);
    m_reqCV.notify_one();       // wake worker if blocked
    m_exchangeCV.notify_one();  // wake presenter if blocked
    if (m_thread.joinable()) m_thread.join();
    // Clear the diag pointer last so any in-flight CACHE-DUMP read on
    // another thread cannot observe a producer that has joined.
    FrameProducer* expected = this;
    s_active.compare_exchange_strong(expected, nullptr,
                                     std::memory_order_acq_rel);
    spdlog::info("[FrameProducer] Stopped");
}

void FrameProducer::reset() noexcept
{
    // m_lastGoodFrame and the adaptive fields are producer-thread-owned.
    // PlaybackScheduler stops and joins that thread before resetting them.
    if (m_running.load(std::memory_order_acquire))
        return;

    {
        std::lock_guard lock(m_reqMtx);
        m_pendingTicks.clear();
        m_pendingScrub.reset();
        m_consecutiveReplacements = 0;
    }
    m_backpressure.store(false, std::memory_order_release);

    {
        std::lock_guard lock(m_exchangeMtx);
        m_exchange = {};
    }
    m_lastGoodFrame.reset();
    m_dropNextComposite = false;
    m_latencyIdx = 0;
    m_latencyCount = 0;
    m_promoteStreak = 0;
    m_changeCooldown = 0;
    m_playbackTelemetry.reset();
    m_backpressureSkippedTelemetry.store(0, std::memory_order_relaxed);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Request queue (FrameClock → producer thread)
// ═════════════════════════════════════════════════════════════════════════════

void FrameProducer::requestFrame(int64_t tick)
{
    {
        std::lock_guard lock(m_reqMtx);
        // Coalesce: a non-empty queue means the producer hasn't picked up
        // the previous request yet, so we're a frame behind.  Replace the
        // back entry (always work on the latest tick) and count the
        // replacement.  When we've replaced kBackpressureLagFrames times
        // in a row without the producer dequeuing, set backpressure so
        // FrameClock skips the next tick instead of piling more on.
        if (!m_pendingTicks.empty()) {
            m_pendingTicks.back() = tick;
            ++m_consecutiveReplacements;
            if (m_consecutiveReplacements >= kBackpressureLagFrames) {
                m_backpressure.store(true, std::memory_order_release);
            }
        } else {
            m_pendingTicks.push_back(tick);
            // Keep the hard ceiling as a sanity bound — the new lag-based
            // logic above should already have engaged backpressure, but
            // this is a belt-and-suspenders cap.
            if (m_pendingTicks.size() >= kMaxPendingFrames)
                m_backpressure.store(true, std::memory_order_release);
        }
    }
    m_reqCV.notify_one();
}

void FrameProducer::requestScrubFrame(int64_t tick, uint32_t w, uint32_t h, bool scrub)
{
    {
        std::lock_guard lock(m_reqMtx);
        // Replace any pending scrub request and invalidate an older request
        // that may already be compositing. Only the latest seek matters.
        const uint64_t generation =
            m_scrubGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        m_pendingScrub = ScrubRequest{tick, w, h, scrub, generation};
    }
    m_reqCV.notify_one();
}

void FrameProducer::cancelPendingScrub() noexcept
{
    {
        std::lock_guard lock(m_reqMtx);
        m_scrubGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_pendingScrub.reset();
    }
    m_reqCV.notify_one();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Producer thread
// ═════════════════════════════════════════════════════════════════════════════

void FrameProducer::producerLoop()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"RT-Producer");
#endif

    spdlog::info("[FrameProducer] Thread started");

    while (m_running.load(std::memory_order_relaxed) && !m_destroying.load(std::memory_order_acquire)) {
        int64_t tick = 0;
        std::optional<ScrubRequest> scrubReq;

        // Wait for a request (scrub requests or clock ticks)
        {
            std::unique_lock lock(m_reqMtx);
            m_reqCV.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return m_pendingScrub.has_value()
                    || !m_pendingTicks.empty()
                    || !m_running.load(std::memory_order_relaxed);
            });
            if (!m_running.load(std::memory_order_relaxed)) break;

            // Clear backpressure — the producer is about to process a frame.
            m_backpressure.store(false, std::memory_order_release);
            m_consecutiveReplacements = 0;

            // Once transport is playing, a clock tick must win over a late
            // paused request. Otherwise exact seek work can hold the viewport
            // on the seek frame while the playback clock advances.
            const bool playing = m_controller && m_controller->isPlaying();
            if (playing && !m_pendingTicks.empty()) {
                tick = m_pendingTicks.front();
                m_pendingTicks.pop_front();
            } else if (m_pendingScrub.has_value()) {
                scrubReq = *m_pendingScrub;
                m_pendingScrub.reset();
            } else if (!m_pendingTicks.empty()) {
                tick = m_pendingTicks.front();
                m_pendingTicks.pop_front();
            } else {
                continue;
            }
        }

        // Wrap each frame production in try/catch so an exception from
        // compositing doesn't kill the producer thread (which would
        // leave the presenter and clock running with no data source).
        try {
            if (scrubReq) {
                produceScrubFrameImpl(*scrubReq);
            } else {
                produceFrameImpl(tick);
            }
        } catch (const std::exception& e) {
            spdlog::error("[FrameProducer] Exception in produceFrame: {}", e.what());
            // Publish the last good frame instead of nullptr so the presenter
            // keeps displaying a valid frame.  Publishing nullptr causes the
            // presenter to skip presenting entirely — the viewport freezes on
            // whatever was last displayed while the playhead continues advancing,
            // creating an unrecoverable freeze-in-playback state.
            //
            // CRITICAL: Clear the inter-queue semaphore on the cached frame.
            // The old gpuSemaphore was already consumed (waited on) when this
            // frame was first presented.  Re-using it would be Vulkan UB and
            // would crash the NVIDIA driver.
            if (m_lastGoodFrame) {
                m_lastGoodFrame->gpuSemaphore = 0;
                publishFrame(m_lastGoodFrame, tick);
            }
            // If there is no last good frame yet (first frame failed), skip
            // publishing — the presenter will hold its current (null) state
            // and retry on the next deadline.
        } catch (...) {
            spdlog::error("[FrameProducer] Unknown exception in produceFrame");
            if (m_lastGoodFrame) {
                m_lastGoodFrame->gpuSemaphore = 0;
                publishFrame(m_lastGoodFrame, tick);
            }
        }
    }

    spdlog::info("[FrameProducer] Thread exiting");
}



void FrameProducer::produceFrameImpl(int64_t tick)
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    if (!m_compositeResultCB) return;

    const int div = m_resDivisor.load(std::memory_order_relaxed);
    uint32_t w = m_outputW.load(std::memory_order_relaxed) / static_cast<uint32_t>(div);
    uint32_t h = m_outputH.load(std::memory_order_relaxed) / static_cast<uint32_t>(div);
    w = std::clamp(w, 64u, 3840u);
    h = std::clamp(h, 36u, 2160u);

    // UPGRADE_PLAN item 1 STEP 2: time-bound producer submit.  If the
    // previous composite ran more than 2× the frame budget, skip the
    // composite call this tick and re-publish the last good frame so the
    // presenter clock never stalls.  Subsequent ticks composite again —
    // the drop is single-shot, and the rolling-window logic below will
    // pick up persistent overruns by dropping the tier.
    if (m_dropNextComposite && m_lastGoodFrame) {
        m_dropNextComposite = false;
        m_lastGoodFrame->gpuSemaphore = 0;
        RenderResult dropped;
        dropped.status = RenderResultStatus::HeldPrevious;
        dropped.diagnostics.heldFrame = true;
        dropped.diagnostics.droppedFrame = true;
        recordPlaybackTelemetry(dropped);
        publishFrame(m_lastGoodFrame, tick);
        return;
    }
    m_dropNextComposite = false;

    auto compStart = std::chrono::steady_clock::now();
    auto renderResult = m_compositeResultCB(
        tick, w, h, /*scrub=*/false, /*still=*/false);
    auto frame = renderResult.frame;
    auto compEnd = std::chrono::steady_clock::now();
    double compMs = std::chrono::duration<double, std::milli>(compEnd - compStart).count();
    renderResult.diagnostics.producerMs = compMs;
    recordPlaybackTelemetry(renderResult);

    // ── Adaptive tier feedback (UPGRADE_PLAN item 1 STEP 1) ───────────
    // Sample composite latency and let the controller bump the tier
    // up/down when sustained latency drifts above or below budget.  The
    // single-spike drop policy above complements this; here we react to
    // the average.
    if (m_adaptiveEnabled.load(std::memory_order_acquire)) {
        const double budget = currentFrameBudgetMs();
        recordCompositeMs(compMs);
        if (budget > 0.0 && compMs > 2.0 * budget)
            m_dropNextComposite = true;
        maybeAdjustTier(budget);
    }

    // Result-aware handling keeps an intentional blank distinct from a
    // temporary miss and prevents a held preview frame from being mistaken
    // for freshly-rendered output.
    if (renderResult.status == RenderResultStatus::Ready &&
        frame && frame->width > 0 && frame->height > 0) {
        m_lastGoodFrame = frame;
        publishFrame(std::move(frame), tick);
    } else if (renderResult.status == RenderResultStatus::Blank) {
        // Empty sentinel: timeline has no content at this tick.
        // Clear the hold-frame and publish the empty sentinel so the
        // presenter clears the viewport instead of showing stale content.
        m_lastGoodFrame.reset();
        publishFrame(std::move(frame), tick);
    } else if ((renderResult.status == RenderResultStatus::MissingMedia ||
                renderResult.status == RenderResultStatus::Failed) &&
               frame && frame->width > 0 && frame->height > 0) {
        // Resource-specific failures may still carry a valid composite of the
        // unaffected layers. Preview that honest partial result instead of
        // holding an unrelated shot. Export still rejects the Failed status.
        m_lastGoodFrame = frame;
        publishFrame(std::move(frame), tick);
    } else if (renderResult.status == RenderResultStatus::MissingMedia) {
        // A confirmed offline source will not become ready on the next clock
        // tick. With no partial composite, clear the viewport instead of
        // holding an unrelated prior shot.
        m_lastGoodFrame.reset();
        publishFrame(std::make_shared<CachedFrame>(), tick);
    } else if (renderResult.status == RenderResultStatus::HeldPrevious && frame) {
        frame->gpuSemaphore = 0;
        publishFrame(std::move(frame), tick);
    } else if (m_lastGoodFrame) {
        // Clear the inter-queue semaphore on the cached frame — the old
        // gpuSemaphore was already consumed (waited on) when this frame
        // was first presented.  Re-using it as a wait semaphore would be
        // undefined behaviour (binary semaphore cannot be waited on twice)
        // and would crash the NVIDIA Vulkan driver.
        m_lastGoodFrame->gpuSemaphore = 0;

        // If the GPU has entered Failed state (device-lost), the cached
        // gpuImageView / gpuSampler are stale handles into a destroyed
        // VkDevice.  Clear them and gpuReady so downstream consumers fall
        // through to the CPU display path (which uses lazyReadback / pixels).
        if (!GpuContext::get().isOperational()) {
            m_lastGoodFrame->gpuImageView = 0;
            m_lastGoodFrame->gpuSampler   = 0;
            m_lastGoodFrame->gpuReady     = false;
        }

        publishFrame(m_lastGoodFrame, tick);
    }
    // else: no frame at all (first frame of playback, cache cold) — skip
}

// ═════════════════════════════════════════════════════════════════════════════
//  Scrub (runs on producer thread — off the UI thread)
// ═════════════════════════════════════════════════════════════════════════════

void FrameProducer::produceScrubFrameImpl(const ScrubRequest& req)
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    if (!m_compositeResultCB) return;

    // User scrubs are intentional — never skip the composite for a scrub
    // even if the previous frame tripped the over-budget drop.
    m_dropNextComposite = false;

    // Requests through the paused/scrub slot are still-frame renders.  The
    // compositor uses this bit to require the exact selected decode tier once
    // active scrubbing has stopped, instead of leaving a lower-tier fallback
    // stretched across a full-size output canvas.
    auto renderResult = m_compositeResultCB(
        req.tick, req.w, req.h, req.scrub, /*still=*/true);

    // A newer seek or a Paused->Playing transition invalidated this exact
    // request while it was rendering. Never publish its stale result over the
    // playback frame that follows it.
    if (req.generation != m_scrubGeneration.load(std::memory_order_acquire))
        return;

    auto frame = renderResult.frame;
    if (renderResult.status == RenderResultStatus::Ready &&
        frame && frame->width > 0 && frame->height > 0) {
        m_lastGoodFrame = frame;
    } else if ((renderResult.status == RenderResultStatus::MissingMedia ||
                renderResult.status == RenderResultStatus::Failed) &&
               frame && frame->width > 0 && frame->height > 0) {
        m_lastGoodFrame = frame;
    } else if (renderResult.status == RenderResultStatus::MissingMedia) {
        m_lastGoodFrame.reset();
    } else if (renderResult.status == RenderResultStatus::Blank) {
        // Empty sentinel: timeline has no content at this tick (e.g., clip
        // was deleted).  Clear the hold-frame so we don't keep re-publishing
        // a stale composite from before the edit.
        m_lastGoodFrame.reset();
    }
    // Always publish whatever we got (even if empty) so the
    // presenter shows the correct state for that tick.
    // If falling back to m_lastGoodFrame, clear its stale semaphore
    // (already consumed by the previous present — re-use is Vulkan UB).
    if ((renderResult.status == RenderResultStatus::Pending ||
         renderResult.status == RenderResultStatus::Failed ||
         renderResult.status == RenderResultStatus::MissingMedia ||
         renderResult.status == RenderResultStatus::Canceled) &&
        !frame && m_lastGoodFrame)
        m_lastGoodFrame->gpuSemaphore = 0;
    if (renderResult.status == RenderResultStatus::Blank)
        publishFrame(std::move(frame), req.tick);
    else if (renderResult.status == RenderResultStatus::MissingMedia && !frame)
        publishFrame(std::make_shared<CachedFrame>(), req.tick);
    else
        publishFrame(frame ? std::move(frame) : m_lastGoodFrame, req.tick);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Frame exchange
// ═════════════════════════════════════════════════════════════════════════════

void FrameProducer::publishFrame(std::shared_ptr<CachedFrame> frame, int64_t tick)
{
    {
        std::lock_guard lock(m_exchangeMtx);
        m_exchange.frame  = std::move(frame);
        m_exchange.tick   = tick;
        m_exchange.hasNew = true;
    }
    m_exchangeCV.notify_one();
}

bool FrameProducer::consumeFrame(std::shared_ptr<CachedFrame>& outFrame, int64_t& outTick)
{
    std::lock_guard lock(m_exchangeMtx);
    if (!m_exchange.hasNew) return false;
    outFrame = m_exchange.frame;
    outTick  = m_exchange.tick;
    m_exchange.hasNew = false;
    return true;
}

bool FrameProducer::waitForFrame(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(m_exchangeMtx);
    return m_exchangeCV.wait_for(lock, timeout, [this] {
        return m_exchange.hasNew;
    });
}

std::shared_ptr<CachedFrame> FrameProducer::lastProducedFrame() const
{
    std::lock_guard lock(m_exchangeMtx);
    return m_exchange.frame;
}

void FrameProducer::recordPlaybackTelemetry(const RenderResult& result)
{
    m_playbackTelemetry.addDropped(
        m_backpressureSkippedTelemetry.exchange(0, std::memory_order_relaxed));
    m_playbackTelemetry.add(result.diagnostics, result.status);
    if (!m_playbackTelemetry.ready())
        return;

    const auto summary = m_playbackTelemetry.take();
    const double budgetMs = currentFrameBudgetMs();
    const bool unhealthy = summary.avgProducerMs > budgetMs ||
        summary.held + summary.pending + summary.dropped > summary.samples / 20;
    const auto level = unhealthy ? spdlog::level::warn : spdlog::level::info;

    spdlog::log(
        level,
        "[PLAYBACK-PERF] n={} status[ready={} held={} pending={} blank={} "
        "failed={} dropped={}] cache[any={} lru={} segment={}] "
        "cpuAvgMs[producer={:.2f} render={:.2f} cache={:.2f} prewarm={:.2f} "
        "shot={:.2f} media={:.2f} recordSubmit={:.2f} readback={:.2f}] "
        "gpuAvgMs[n={} frame={:.2f} upload={:.2f} fx={:.2f} composite={:.2f}] "
        "maxMs[producer={:.2f} render={:.2f}] budget={:.2f} divisor={}",
        summary.samples, summary.ready, summary.held, summary.pending,
        summary.blank, summary.failed, summary.dropped, summary.cacheHits,
        summary.compositeCacheHits, summary.segmentCacheHits,
        summary.avgProducerMs, summary.avgRenderMs, summary.avgCacheLookupMs,
        summary.avgPrewarmMs, summary.avgShotBoundaryMs,
        summary.avgMediaResolveMs, summary.avgGpuRecordSubmitMs,
        summary.avgReadbackMs, summary.gpuSamples, summary.avgGpuFrameMs,
        summary.avgGpuUploadMs, summary.avgGpuEffectMs,
        summary.avgGpuCompositeMs, summary.maxProducerMs,
        summary.maxRenderMs, budgetMs,
        m_resDivisor.load(std::memory_order_relaxed));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Adaptive tier feedback loop
// ═════════════════════════════════════════════════════════════════════════════

void FrameProducer::recordCompositeMs(double ms)
{
    m_latencyWindow[m_latencyIdx] = ms;
    m_latencyIdx = (m_latencyIdx + 1) % kLatencyWindow;
    if (m_latencyCount < kLatencyWindow) ++m_latencyCount;
}

double FrameProducer::averageLatencyMs() const noexcept
{
    if (m_latencyCount == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < m_latencyCount; ++i) sum += m_latencyWindow[i];
    return sum / static_cast<double>(m_latencyCount);
}

double FrameProducer::currentFrameBudgetMs() const noexcept
{
    double fps = 30.0;
    if (m_controller) {
        const double f = m_controller->frameRate();
        if (f > 0.0) fps = f;
    }
    return 1000.0 / std::max(fps, 1.0);
}

void FrameProducer::maybeAdjustTier(double frameBudgetMs)
{
    if (frameBudgetMs <= 0.0) return;
    if (m_changeCooldown > 0) { --m_changeCooldown; return; }

    // Wait for a full window so cold-start outliers can't shift tier.
    if (m_latencyCount < kLatencyWindow) return;

    const double avg = averageLatencyMs();
    const int curDiv = m_resDivisor.load(std::memory_order_acquire);

    // Drop tier on sustained over-budget (Full→Half→Quarter).  Skip
    // divisor 8 — Quarter is the lowest tier MediaPool/CompositeService
    // distinguishes today, and the dropdown's 1/8 only shrinks composite
    // output without a matching decode tier.
    if (avg > 1.4 * frameBudgetMs) {
        int next = curDiv;
        if      (curDiv <= 1) next = 2;
        else if (curDiv <= 2) next = 4;
        if (next != curDiv) {
            spdlog::warn("[ADAPTIVE-TIER] drop {}→{} avg={:.1f}ms budget={:.1f}ms",
                         curDiv, next, avg, frameBudgetMs);
            applyAdaptiveDivisor(next);
            return;
        }
    }

    // Promote tier on a long run of comfortably-under-budget frames.
    if (avg < 0.6 * frameBudgetMs) {
        ++m_promoteStreak;
        if (m_promoteStreak >= kPromoteStreak) {
            int next = curDiv;
            if      (curDiv >= 4) next = 2;
            else if (curDiv >= 2) next = 1;
            if (next != curDiv) {
                spdlog::warn("[ADAPTIVE-TIER] raise {}→{} avg={:.1f}ms budget={:.1f}ms",
                             curDiv, next, avg, frameBudgetMs);
                applyAdaptiveDivisor(next);
            } else {
                m_promoteStreak = 0;
            }
        }
    } else {
        m_promoteStreak = 0;
    }
}

void FrameProducer::applyAdaptiveDivisor(int newDivisor)
{
    m_resDivisor.store(newDivisor, std::memory_order_release);
    m_changeCooldown = kChangeCooldown;
    m_promoteStreak  = 0;
    m_latencyIdx     = 0;
    m_latencyCount   = 0;
    if (m_adaptiveTierCB) {
        try { m_adaptiveTierCB(newDivisor); }
        catch (const std::exception& e) {
            spdlog::error("[FrameProducer] adaptive tier callback threw: {}",
                          e.what());
        } catch (...) {
            spdlog::error("[FrameProducer] adaptive tier callback threw");
        }
    }
}

} // namespace rt
