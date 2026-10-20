/*
 * CompositeEngineSubmit.cpp -- the end of one render-graph composite:
 * GPU timing end markers, the graphics-queue submit (waiting on the
 * prefetch producer timeline), the result frame (GPU-direct, lazy or
 * synchronous readback, LRU) and the slow-frame log lines. See
 * CompositeEngineFrame.h.
 */

// define VK_NO_PROTOTYPES first.
#include <volk.h>

#include "CompositeEngine.h"
#include "CompositeEngineFrame.h"
#include "ExportStageTimes.h"
#include "render_graph/GpuRenderGraph.h"
#include "cache/CachePolicy.h"
#include "StagingRing.h"
#include "CompositeServiceLayerBuild.h"  // rt::LayerInfo
#include "effects/CpuComposite.h"       // rasterizeMasks
#include "Compositor.h"
#include "diag/FrameSignatureLog.h"
#include "GpuContext.h"
#include "GpuTextureCache.h"
#include "GpuWorkSubmission.h"
#include "GpuUploadManager.h"
#include "vulkan/Texture.h"
#include "TransitionRenderer.h"
#include "EffectProcessor.h"
#include "cache/FrameCache.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/LUT.h"
#include "timeline/Transition.h"
#include "timeline/Clip.h"

#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace rt;

void CompositeEngine::submitComposite(FrameRecording& frame)
{
    auto& ctx = GpuContext::get();
    auto& slot = *frame.slot;
    VkCommandBuffer cmd = frame.cmd;
    const int timingSlot = frame.timingSlot;
    const auto& layers = frame.layers;
    const uint32_t outW = frame.outW;
    const uint32_t outH = frame.outH;
    const int64_t tick = frame.tick;
    const auto& compositor = frame.compositor;
    bool& uploadOk = frame.uploadOk;
    bool& compOk = frame.compOk;
    bool& readbackOk = frame.readbackOk;
    bool& gpuSubmitOk = frame.gpuSubmitOk;
    VkSemaphore& frameSem = frame.frameSem;
    const bool timingActive = frame.timingActive;
    bool& uploadTimestampWritten = frame.uploadTimestampWritten;
    bool& effectTimestampWritten = frame.effectTimestampWritten;

    // Final GPU-TIMING marker (frame end).  Backfill any markers we did
    // not reach (e.g. no effects this frame) so deltas remain monotonic.
    if (timingActive) {
        if (!uploadTimestampWritten) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                m_timingPools[timingSlot], 1);
        }
        if (!effectTimestampWritten) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                m_timingPools[timingSlot], 2);
        }
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            m_timingPools[timingSlot], 3);
        m_timingPoolUsed[timingSlot] = true;
    }

    // Per-pass fault isolation: a failed Upload pass disables only that
    // layer (see PassType::Upload case above, which sets gpuLayers[li].
    // enabled = false on failure).  The compositor renders the surviving
    // layers — the user sees one missing layer instead of a blank frame
    // or a CPU-fallback regression.  This matches RenderPass::optional=true.
    // The frame is only killed if the Composite pass itself failed below.
    if (!uploadOk) {
        spdlog::warn("[RENDER_GRAPH] one or more uploads failed — compositing "
                     "surviving layers (fault-isolated)");
    }

    // Composite and viewport presentation both submit to the graphics queue.
    // Queue order is sufficient; a one-shot binary semaphore was unsafe for
    // held frames and leaked whenever a produced frame was dropped.
    frame.splitWalk = FrameRecording::SplitClock::now();
    const bool endOk = slot.endRecording();
    frame.splitEnd = FrameRecording::SplitClock::now();
    if (endOk) {
        // Note: no manual queue-mutex lock here.  GpuWorkSubmission::
        // submit routes through GpuScheduler (P1.1), which owns the
        // queue mutex.  Locking it manually would re-enter a non-
        // recursive std::mutex on the same thread and throw
        // resource_deadlock_would_occur — the regression that left
        // the program monitor blank in an earlier revision.
        //
        // UPGRADE_PLAN Path C (2026-05-22): target the graphics queue.
        // The composite cmd was allocated from graphicsCmdPool above;
        // submitting it to the graphics queue runs the composite work
        // on the 3D engine, in parallel with whatever the async-compute
        // engine (still owned by prefetch convert+copy) is doing.  This
        // removes the 130-468 ms submit= stalls observed in
        // logs/perf_log.txt at 2026-05-22 13:40:50, 13:41:01, etc.
        // graphicsQueue() falls back to the compute queue on devices
        // where the families coincide, preserving the previous path.
        //
        // Cross-queue memory visibility (Path C optimisation): walk the
        // layers vector and find the max producer timeline value across
        // every CachedFrame the compositor is about to sample.  We
        // assume one shared producer timeline semaphore — all
        // prefetch-produced frames currently signal MediaPool's
        // m_prefetchTimelineSem.  Layers whose frame was CPU-decoded
        // or composite-output (no producer) contribute 0 and are
        // ignored.  The wait is added GPU-side via a
        // VkTimelineSemaphoreSubmitInfo so the prefetch worker thread
        // does NOT block on vkWaitForFences — the previous per-frame
        // ~5 ms CPU stall that limited prefetch throughput under
        // seek-recovery and high playback rates is gone.
        VkSemaphore producerSem      = VK_NULL_HANDLE;
        uint64_t    producerMaxValue = 0;
        for (const auto& layer : layers) {
            if (!layer.frame) continue;
            const uint64_t semHandle = layer.frame->producerTimelineSem;
            const uint64_t v         = layer.frame->producerTimelineValue;
            if (semHandle == 0 || v == 0) continue;
            // Currently all prefetch frames share one timeline sem.  If
            // that ever changes, the per-sem map can replace this
            // single-value tracker; for now we sanity-check the handle.
            VkSemaphore sem = reinterpret_cast<VkSemaphore>(semHandle);
            if (producerSem == VK_NULL_HANDLE) producerSem = sem;
            if (sem == producerSem && v > producerMaxValue)
                producerMaxValue = v;
        }
        if (producerSem != VK_NULL_HANDLE && producerMaxValue != 0) {
            gpuSubmitOk = slot.submitWithTimelineWait(
                ctx.graphicsQueue(), frameSem,
                producerSem, producerMaxValue, nullptr);
        } else {
            gpuSubmitOk = slot.submit(ctx.graphicsQueue(), frameSem, nullptr);
        }
    }
    frame.splitSubmit = FrameRecording::SplitClock::now();

    // Advance the composite epoch that drives PrefetchTexturePool's deferred-
    // reuse quarantine.  Bumped per composite submit (whether or not it
    // succeeded) so a recycled texture released during this frame is held out
    // of reuse until several more frames have been submitted — guaranteeing any
    // in-flight submit that sampled it has completed.
    ctx.bumpCompositeEpoch();

    if (!gpuSubmitOk) {
        // P2: no retry/backoff.  Submit failure means the device is lost
        // or wedged; signalling it propagates to the fatal-failure modal.
        spdlog::error("[RENDER_GRAPH] GPU submit failed — signalling device lost");
        GpuContext::get().signalDeviceLost();
        compOk = false;
        readbackOk = false;
    }

    // A/B harness (#18): GPU-signature the preview output. The composite was
    // submitted (async) just above; FrameSignature's own GpuScheduler submit
    // runs after it in graphics-queue submission order, and its acquire barrier
    // provides the cross-submission memory visibility. The Compositor output
    // rests in GENERAL (same as the export path). No-op unless ROUNDTABLE_FRAMEHASH
    // is set; when set, this serialises a per-frame signature submit (offline use).
    if (compOk && gpuSubmitOk && FrameSignatureLog::get().enabled()) {
        FrameSignatureLog::get().capture(
            "preview", tick,
            compositor->outputImageView(), compositor->outputSampler(),
            outW, outH, VK_IMAGE_LAYOUT_GENERAL);
    }

    m_uploadManager->endFrame();
}

std::shared_ptr<CachedFrame> CompositeEngine::buildCompositeResult(
    FrameRecording& frame, const CompositeCacheKey& cacheKey, bool allowLruInsert)
{
    const auto& layers = frame.layers;
    const uint32_t outW = frame.outW;
    const uint32_t outH = frame.outH;
    const bool gpuDisplayMode = frame.gpuDisplayMode;
    const bool forceSyncReadback = frame.forceSyncReadback;
    const auto& compositor = frame.compositor;
    bool& readbackOk = frame.readbackOk;
    bool& nv12ReadbackOk = frame.nv12ReadbackOk;
    bool& gpuSubmitOk = frame.gpuSubmitOk;
    VkSemaphore& frameSem = frame.frameSem;

    auto result = std::make_shared<CachedFrame>();
    result->width  = outW;
    result->height = outH;
    result->stride = outW * 4;

    // forceSyncReadback (export / Export Preview): the caller consumes
    // the CPU pixels on the SAME thread immediately after this returns,
    // with no fence wait of its own.  The lazy/GPU-direct path below
    // defers the readback AND leaves the EffectProcessor's single shared
    // ping-pong storage + descriptor sets live; with up to kRingSize
    // export frames overlapping on the GPU, frame N+1's effect dispatch
    // then overwrites the storage/descriptor that frame N's still-pending
    // blur + composite reads — the "blur flickers / pulls in other
    // composited layers after a few seconds of export" bug.  Routing
    // export through the synchronous wait+readback path (below) drains
    // each frame before the next reuses the EffectProcessor, and reads
    // fully-rendered (non-stale) pixels.
    if (gpuDisplayMode && !forceSyncReadback) {
        // GPU-direct display path: used for BOTH playback and scrub.
        //
        // Previously this was gated on `!scrubMode`, which forced
        // scrub composites through the synchronous CPU-readback
        // path below (vkWaitForFences with a 5 s timeout +
        // mapAndCopyReadback).  That wait blocked the FrameProducer
        // thread until ALL GPU work queued ahead — including the
        // prefetch convert+copy submissions saturating the compute
        // queue — drained.  Result: `readback=` times of 71-307 ms
        // during scrub bursts, exactly the symptom in
        // logs/perf_log.txt at 2026-05-22 13:11:51–13:12:18.
        //
        // The GPU-direct path works identically for scrub: the
        // compositor produces the same outputImageView, the
        // gpuSemaphore signals completion to the presenter, and
        // ProgramMonitor::presentFrame's gpuReady branch displays
        // directly without ever touching CPU memory.  No reason to
        // pay the readback cost just because the user is scrubbing.
        result->gpuReady     = true;
        result->gpuImageView = reinterpret_cast<uint64_t>(compositor->outputImageView());
        result->gpuSampler   = reinterpret_cast<uint64_t>(compositor->outputSampler());
        result->gpuSemaphore = reinterpret_cast<uint64_t>(frameSem);
        // Keep the compositor's output texture alive until all frames
        // referencing it are consumed by the viewport.
        result->gpuTextureOwner = compositor->outputTextureOwner();
    }

    if (readbackOk && gpuDisplayMode && !forceSyncReadback) {
        auto compPtr = compositor;
        uint32_t rW = outW, rH = outH;
        const uint32_t readbackSlot = compPtr->outputSlot();
        result->lazyReadback = [compPtr, rW, rH, readbackSlot](std::vector<uint8_t>& px) -> bool {
            const size_t imgBytes = static_cast<size_t>(rW) * rH * 4;
            px.resize(imgBytes);
            return compPtr->mapAndCopyReadbackSlot(readbackSlot, px);
        };
    } else if (readbackOk) {
        const size_t imgBytes = static_cast<size_t>(outW) * outH * 4;
        if (!nv12ReadbackOk && m_compositeLru.size() >= kCacheSize) {
            auto& victim = m_compositeLru[m_compositeLruIdx];
            if (victim.frame && victim.frame.use_count() == 1 &&
                victim.frame->pixels.size() == imgBytes)
            {
                result->pixels = std::move(victim.frame->pixels);
            }
        }
        // Wait for the just-submitted composite + recordReadback to
        // finish on the GPU before reading staging memory.  Without
        // this, mapAndCopyReadback races the DMA into staging and
        // returns a mix of the new frame's pixels and whatever the
        // previous frame left behind — visible at cuts as "top of the
        // previous frame leaking into the next one" because the GPU
        // hasn't yet fully overwritten the staging buffer.  The
        // lazyReadback path (GPU display mode) is unaffected — it runs
        // from the FrameProducer after its own fence wait.
        if (gpuSubmitOk && m_gpuSubmission) {
            // 5-second timeout — same as other waits in this file;
            // generous enough to survive a heavy frame, short enough to
            // surface a wedged device instead of hanging the export.
            constexpr uint64_t kReadbackWaitNs = 5'000'000'000ull;
            ExportStageTimer waitTimer(exportStageTimes().readbackWaitUs);
            if (!m_gpuSubmission->waitForCompletion(kReadbackWaitNs)) {
                spdlog::warn("[RENDER_GRAPH] readback fence wait timed out — "
                             "frame may contain partial/stale pixels");
            }
        }
        ExportStageTimer copyTimer(exportStageTimes().readbackCopyUs);
        if (nv12ReadbackOk) {
            result->stride = 0;
            result->nv12 = compositor->mappedNv12(result->nv12Size,
                                                  result->nv12Stride);
        } else {
            compositor->mapAndCopyReadback(result->pixels);
        }
    }

    // A nearest-sampled temporal fallback is intentionally provisional:
    // the next request must rebuild once both exact endpoints arrive.
    // Caching that output by tick would make the clip remain sampled for
    // the lifetime of the LRU entry even though optical flow is selected.
    const bool hasPendingSourceFallback = std::any_of(
        layers.begin(), layers.end(), [](const LayerInfo& layer) {
            return layer.sourceFallbackPending;
        });
    result->provisionalSource = hasPendingSourceFallback;

    // allowLruInsert is false for nested-sequence inner composites:
    // their CPU result is the inner timeline WITHOUT the SequenceClip
    // transform and would collide with the outer program tick in the
    // shared (tick,w,h)-keyed LRU, flickering the nested clip every
    // other frame.
    // An NV12 export frame has no BGRA pixels for any other consumer.
    if (allowLruInsert && !hasPendingSourceFallback && !result->gpuReady &&
        !result->hasNv12()) {
        insertLru(cacheKey, result);
    }

    return result;
}

void CompositeEngine::logCompositeTimings(
    const FrameRecording& frame, bool perfLog,
    std::chrono::high_resolution_clock::time_point perfT0,
    std::chrono::high_resolution_clock::time_point perfTlayers,
    std::chrono::high_resolution_clock::time_point perfTgpuUp,
    std::chrono::high_resolution_clock::time_point perfTcomp) const
{
    using SplitClock = FrameRecording::SplitClock;
    auto splitMs = [](SplitClock::time_point a, SplitClock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto splitT0 = frame.splitT0;
    const auto splitBegin = frame.splitBegin;
    const auto splitPrep = frame.splitPrep;
    const auto splitWalk = frame.splitWalk;
    const auto splitEnd = frame.splitEnd;
    const auto splitSubmit = frame.splitSubmit;
    const auto& layers = frame.layers;
    const int64_t tick = frame.tick;
    const bool scrubMode = frame.scrubMode;
    const bool gpuDisplayMode = frame.gpuDisplayMode;
    const int effectLayerCount = frame.effectLayerCount;
    const int effectPassCount = frame.effectPassCount;
    const int transitionCount = frame.transitionCount;
    const double passUploadMs = frame.passUploadMs;
    const double passEffectMs = frame.passEffectMs;
    const double passCompositeMs = frame.passCompositeMs;
    const double passOtherMs = frame.passOtherMs;

    auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const double totalMs   = ms(perfT0, perfTcomp);
    const double layersMs  = ms(perfT0, perfTlayers);
    const double submitMs  = ms(perfTlayers, perfTgpuUp);
    const double readbackMs = ms(perfTgpuUp, perfTcomp);

    // Always log when a frame breaches the 33ms budget (target 30 fps).
    // This is the most direct diagnostic for the clip-boundary cascade
    // and any other "FrameClock falls to ~10 fps" symptom: each slow
    // frame is one tick the FrameClock could not advance in real time,
    // so a flurry of these lines correlates exactly with the JUMP
    // stream in the same log.  Stage breakdown (layers / submit /
    // readback) is included so a slow frame can be attributed to
    // CPU-side layer build vs the GPU submit window vs CPU↔GPU
    // readback, without needing a second test run.  warn level: the
    // user runs warn+ filtered logging, and a >33 ms frame during
    // active playback is a real perf event worth surfacing every time.
    if (totalMs > 33.0) {
        // Preserve an attributable outlier without turning sustained
        // overload into synchronous warning-level file I/O every frame.
        // PLAYBACK-PERF carries the complete aggregate once per window.
        static std::atomic<int64_t> s_lastSlowLogNs{0};
        const int64_t nowNs = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t lastNs = s_lastSlowLogNs.load(std::memory_order_relaxed);
        if (nowNs - lastNs >= 1'000'000'000ll &&
            s_lastSlowLogNs.compare_exchange_strong(
                lastNs, nowNs, std::memory_order_relaxed)) {
            // Which effects are active: "L<index>[adj]:Name+Name".
            std::string fxList;
            for (size_t li = 0; li < layers.size(); ++li) {
                if (layers[li].effects.empty()) continue;
                if (!fxList.empty()) fxList += ' ';
                fxList += 'L' + std::to_string(li);
                if (layers[li].isAdjustmentLayer) fxList += "adj";
                fxList += ':';
                for (size_t fi = 0; fi < layers[li].effects.size(); ++fi) {
                    if (fi) fxList += '+';
                    fxList += effectTypeName(layers[li].effects[fi].type);
                }
            }
            spdlog::warn("[COMPOSITE-SLOW] tick={} TOTAL={:.1f}ms "
                         "layers={:.1f}ms submit={:.1f}ms readback={:.1f}ms "
                         "| scrub={} layerCount={} effectLayers={} effectPasses={} "
                         "transitions={} gpuDisplay={} fx=[{}]",
                         tick, totalMs, layersMs, submitMs, readbackMs,
                         scrubMode, layers.size(),
                         effectLayerCount, effectPassCount,
                         transitionCount, gpuDisplayMode, fxList);
            spdlog::warn("[SUBMIT-SPLIT] tick={} slotWait={:.1f} prep={:.1f} "
                         "passes[upload={:.1f} effect={:.1f} composite={:.1f} "
                         "other={:.1f}] endRec={:.1f} queueSubmit={:.1f} "
                         "post={:.1f} (ms)",
                         tick, splitMs(splitT0, splitBegin),
                         splitMs(splitBegin, splitPrep),
                         passUploadMs, passEffectMs, passCompositeMs,
                         passOtherMs, splitMs(splitWalk, splitEnd),
                         splitMs(splitEnd, splitSubmit),
                         splitMs(splitSubmit, SplitClock::now()));
        }
    } else if (perfLog) {
        spdlog::info("[RENDER_GRAPH] compositeFrame (DAG): layers={} | "
                     "gpu={:.1f}ms  TOTAL={:.1f}ms  "
                     "gpuDisplay={}  effectLayers={}  effectPasses={}  transitions={}",
                     layers.size(),
                     ms(perfTlayers, perfTcomp),
                     totalMs,
                     gpuDisplayMode,
                     effectLayerCount, effectPassCount, transitionCount);
    }
}
