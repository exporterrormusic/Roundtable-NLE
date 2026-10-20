/*
 * CompositeServiceFrame.cpp - Frame compositing orchestrator.
 * Extracted from CompositeService.cpp; prewarm methods moved to
 * CompositeServiceFramePrewarm.cpp (Step 3.1 of modularization plan).
 *
 * Contains:
 *   - compositeFrame()   — main compositing entry point
 *   - renderTitleClip()  — TitleClip rendering wrapper
 *   (GraphicClip rendering is rt::renderGraphicClip, called from layer build)
 */

#include "CompositeService.h"
#include "CompositeServiceFrameState.h"
#include "ClipRenderers.h"
#include "effects/CpuComposite.h"
#include "CompositeServiceLayerBuild.h"

// Media / timeline
#include "cache/FrameCache.h"
#include "cache/SegmentRenderCache.h"
#include "playback/FrameFallbackPolicy.h"
#include "playback/MediaPool.h"
#include "playback/SnapshotPublisher.h"
#include "decode/VideoFrameMapping.h"
#include "cache/CachePolicy.h"
#include "project/RenderStateHash.h"
#include "Constants.h"
#include "FrameTime.h"
#include "timeline/AudioClip.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"
#include "timeline/OpacityMask.h"

#include "project/Project.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/AnimationVideoCache.h"
#include "spine/ModelManager.h"
#include "spine/ShotPreset.h"
#include "stb_image.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>

// GPU compositing
#include "CompositeEngine.h"
#include "GpuContext.h"
#include "Compositor.h"
#include "EffectProcessor.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

namespace {

std::optional<ResolutionTier> resolutionTierFor(RenderQuality quality)
{
    switch (quality) {
    case RenderQuality::Full:
    case RenderQuality::Native:
        return ResolutionTier::Full;
    case RenderQuality::Half:
        return ResolutionTier::Half;
    case RenderQuality::Quarter:
    case RenderQuality::Proxy:
        return ResolutionTier::Quarter;
    case RenderQuality::Auto:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

CompositeService::RenderExecutionContext CompositeService::makeLiveContext(
    const RenderExecutionPolicy& policy) const
{
    RenderExecutionContext context;
    context.policy = policy;
    if (m_snapshotPublisher) {
        // Never the live model: only the UI thread may read m_timeline.
        context.sequenceSnapshot = m_snapshotPublisher->latest();
        if (context.sequenceSnapshot) {
            context.timeline =
                const_cast<Timeline*>(context.sequenceSnapshot->timeline.get());
            context.editVersion = context.sequenceSnapshot->editVersion;
        }
        return context;
    }
    context.timeline = m_timeline;
    context.project = m_project;
    return context;
}

Timeline* CompositeService::resolveSequence(const RenderExecutionContext& context,
                                            size_t sequenceIndex)
{
    if (context.sequenceSnapshot)
        return const_cast<Timeline*>(context.sequenceSnapshot->sequenceAt(sequenceIndex));
    if (context.project && sequenceIndex < context.project->sequenceCount())
        return context.project->sequence(sequenceIndex);
    return nullptr;
}

uint64_t CompositeService::configHashFor(const RenderExecutionContext& context,
                                         const Timeline& timeline, int64_t tick,
                                         uint32_t renderFlags)
{
    if (context.sequenceSnapshot) {
        const SequenceLookup lookup = [&context](size_t index) -> const Timeline* {
            return context.sequenceSnapshot->sequenceAt(index);
        };
        return hashCompositeConfigAt(timeline, tick, lookup, renderFlags);
    }
    return hashCompositeConfigAt(timeline, tick, context.project, renderFlags);
}

uint64_t CompositeService::boundConfigHash(int64_t tick, uint32_t renderFlags) const
{
    // A project-less export snapshot resolves nested sequences itself.
    if (m_boundSnapshot && !m_boundSnapshot->project
            && m_boundSnapshot->timeline.get() == m_timeline) {
        RenderExecutionContext context;
        context.sequenceSnapshot = m_boundSnapshot;
        return configHashFor(context, *m_timeline, tick, renderFlags);
    }
    return hashCompositeConfigAt(*m_timeline, tick, m_project, renderFlags);
}

std::shared_ptr<CachedFrame> CompositeService::compositeFrame(
    const RenderRequest& request, bool isNestedRecursion)
{
    return renderFrame(request, isNestedRecursion).frame;
}

RenderResult CompositeService::renderFrame(
    const RenderRequest& request, bool isNestedRecursion)
{
    RenderResult result;
    result.timelineTick = request.timelineTick;
    result.diagnostics.requestId = request.requestId;

    const auto fail = [&](RenderResultStatus status, std::string warning) {
        result.status = status;
        result.diagnostics.status = status;
        result.diagnostics.warning = std::move(warning);
        return result;
    };

    if (m_shutdown.load(std::memory_order_acquire))
        return fail(RenderResultStatus::Canceled,
                    "compositor is shutting down");
    if (request.outputWidth == 0 || request.outputHeight == 0)
        return fail(RenderResultStatus::Failed,
                    "render output dimensions must be non-zero");

    RenderExecutionOutcome executionOutcome;
    RenderExecutionContext context = makeLiveContext({
        request.preferGpuOutput,
        request.forceFullResolution,
        request.preserveAlpha,
        request.isRealtimePlaybackPass(),
        request.outputFormat == RenderOutputFormat::Nv12 &&
            request.forceFullResolution && !request.preserveAlpha,
        request.gpuSourceConvert && request.forceFullResolution,
    });
    context.outcome = &executionOutcome;
    context.diagnostics = &result.diagnostics;

    if (request.snapshot && request.targetSequenceIndex) {
        spdlog::error("CompositeService: render request cannot select both a "
                      "snapshot and a live sequence index");
        return fail(RenderResultStatus::Failed,
                    "render request selected both snapshot and live sequence");
    }

    // Snapshot services stay bound to one immutable graph so their service-
    // owned caches cannot mix projects. The graph is still captured in the
    // execution context rather than reread during evaluation.
    if (request.snapshot) {
        if (!request.snapshot->timeline ||
            request.snapshot->timeline.get() != m_timeline) {
            spdlog::error("CompositeService: render request timeline snapshot "
                          "does not match the bound timeline");
            return fail(RenderResultStatus::Failed,
                        "render snapshot timeline does not match compositor");
        }
        // A published snapshot carries its sequences instead of a Project;
        // the service is then bound without one.
        if (request.snapshot->project.get() != m_project) {
            spdlog::error("CompositeService: render request project snapshot "
                          "does not match the bound project");
            return fail(RenderResultStatus::Failed,
                        "render snapshot project does not match compositor");
        }
        context.timeline = const_cast<Timeline*>(request.snapshot->timeline.get());
        context.project = const_cast<Project*>(request.snapshot->project.get());
        context.editVersion = request.snapshot->editVersion;
        context.immutableSnapshot = true;
        // Nested sequences resolve through the snapshot's own sequences when
        // it has no Project (resolveSequence / configHashFor).
        context.sequenceSnapshot = request.snapshot->project ? nullptr : request.snapshot;
    } else if (request.targetSequenceIndex && context.sequenceSnapshot) {
        context.timeline = resolveSequence(context, *request.targetSequenceIndex);
        if (!context.timeline)
            return fail(RenderResultStatus::Failed,
                        "render request selected an unavailable sequence");
    } else if (request.targetSequenceIndex) {
        if (!m_project || *request.targetSequenceIndex >= m_project->sequenceCount()) {
            spdlog::error("CompositeService: live sequence target {} is invalid",
                          *request.targetSequenceIndex);
            return fail(RenderResultStatus::Failed,
                        "render request selected an invalid live sequence");
        }
        context.timeline = m_project->sequence(*request.targetSequenceIndex);
        if (!context.timeline)
            return fail(RenderResultStatus::Failed,
                        "render request selected an unavailable live sequence");
    }

    if (!context.timeline)
        return fail(RenderResultStatus::Failed,
                    "no timeline is bound to the compositor");

    RenderResultStatus exceptionStatus = RenderResultStatus::Pending;
    const auto renderStart = std::chrono::steady_clock::now();
    result.frame = compositeFrameImpl(request.timelineTick,
                                      request.outputWidth,
                                      request.outputHeight,
                                      request.scrubMode,
                                      isNestedRecursion,
                                      request.stillFrame,
                                      resolutionTierFor(request.quality),
                                      context,
                                      &exceptionStatus);
    result.diagnostics.renderMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - renderStart).count();
    if (exceptionStatus == RenderResultStatus::Failed) {
        result.frame.reset();
        return fail(RenderResultStatus::Failed,
                    "exception while compositing the requested frame");
    }
    if (executionOutcome.status == RenderResultStatus::Failed ||
        executionOutcome.status == RenderResultStatus::MissingMedia) {
        result.status = executionOutcome.status;
        result.diagnostics.status = result.status;
        result.diagnostics.warning = executionOutcome.warning;
        return result;
    }
    if (!result.frame) {
        // A null produced by an otherwise valid request is transient in the
        // existing compositor (decoder/GPU resource not ready).  Export may
        // retry the same tick; preview may explicitly hold its prior frame.
        result.status = RenderResultStatus::Pending;
    } else if (result.frame->width == 0 || result.frame->height == 0) {
        result.status = RenderResultStatus::Blank;
    } else {
        result.status = RenderResultStatus::Ready;
        result.diagnostics.segmentCacheHit |= result.frame->segmentCacheHit;
        result.diagnostics.cacheHit |= result.frame->segmentCacheHit;

        // Best-effort playback is allowed to receive the compositor's
        // previous good frame while a new source arrives.  Surface that fact
        // instead of making the scheduler infer it from pointer identity.
        if (request.exactness == RenderExactness::BestEffortAllowed &&
            !isNestedRecursion &&
            m_hold.isHeldFromAnotherTick(result.frame, request.timelineTick)) {
            result.status = RenderResultStatus::HeldPrevious;
            result.diagnostics.heldFrame = true;
        }
    }
    // Also for an empty sentinel (Blank) answered while a source decodes.
    result.diagnostics.provisionalSource = executionOutcome.provisional ||
        (result.frame && result.frame->provisionalSource);
    result.diagnostics.status = result.status;
    return result;
}

std::shared_ptr<CachedFrame> CompositeService::compositeFrame(int64_t tick, uint32_t outW, uint32_t outH,
                                                                bool scrubMode,
                                                                bool isNestedRecursion,
                                                                bool stillMode,
                                                                std::optional<ResolutionTier> tierOverride)
{
    const RenderExecutionContext context = makeLiveContext({
        m_gpuDisplayMode,
        m_forceFullResolution.load(std::memory_order_relaxed),
        m_exportAlpha.load(std::memory_order_relaxed),
        !scrubMode && !stillMode &&
            !m_forceFullResolution.load(std::memory_order_relaxed),
    });
    return compositeFrameImpl(tick, outW, outH, scrubMode,
                              isNestedRecursion, stillMode, tierOverride,
                              context);
}

std::shared_ptr<CachedFrame> CompositeService::compositeFrameImpl(
    int64_t tick, uint32_t outW, uint32_t outH,
    bool scrubMode, bool isNestedRecursion, bool stillMode,
    std::optional<ResolutionTier> tierOverride,
    const RenderExecutionContext& context,
    RenderResultStatus* exceptionStatus)
try
{
    if (m_shutdown.load(std::memory_order_acquire))
        return nullptr;
    auto* const renderTimeline = context.timeline;
    auto* const renderProject = context.project;
    const auto& policy = context.policy;
    if (!renderTimeline) return nullptr;
    FrameDiagnostics* const realtimeDiagnostics =
        policy.measureRealtimeCost && !isNestedRecursion
            ? context.diagnostics : nullptr;

    // Suppress GPU compositing when a modal dialog is active (QDialog::exec
    // nested event loop).  Paint events still fire for widgets behind the
    // dialog, and invoking the Vulkan compositor from a paint event cascade
    // triggers deep driver call stacks that overflow the NVIDIA driver stack
    // (STACK_OVERFLOW in nvoglv64.dll).  Return the last good cached frame
    // instead, keeping the display valid without touching the GPU.
    if (s_modalDialogActive.load(std::memory_order_acquire)) {
        // Nested recursion: don't fall back to OUTER's last-good frame
        // for an INNER layer (would render outer content inside itself).
        if (isNestedRecursion || stillMode) return nullptr;
        return m_hold.lastGoodWithoutGpu();
    }

    // Re-entrancy guard — safety net against unexpected recursive calls.
    // FrameProducer is the sole compositor thread, so this should never
    // trigger in normal operation.  Kept as defense-in-depth against
    // signal re-entrancy or callback chains.
    //
    // A2: Allow exactly ONE level of legitimate recursion (nested
    // SequenceClip composition). The child context selects the inner sequence
    // and recursively calls
    // compositeFrame to render the inner timeline.  Without this
    // allowance, the previous depth>0 → return-lastGood logic caused
    // nested sequences to silently show stale frames.  Two levels of
    // nesting is enough for almost any real project; deeper recursion
    // is treated as runaway re-entry and short-circuits.
    auto& depth = compositeDepth();
    constexpr int kMaxCompositeDepth = 2;
    if (depth >= kMaxCompositeDepth) {
        if (isNestedRecursion || stillMode) return nullptr;
        return m_hold.lastGood();
    }
    ++depth;
    struct DepthGuard {
        ~DepthGuard() { --compositeDepth(); }
    } depthGuard;

    // Persistent GPU failure transitions to Failed and fires the fatal
    // recovery callback. There is no per-frame CPU compositor.

    // Blocking lock — FrameProducer is now the sole compositor thread.
    // The old try_to_lock pattern (which returned the stale last good composite
    // on contention) is removed.  compositeFrame() now ALWAYS produces the
    // correct frame for the requested tick.  Never returns stale frames.
    // A recursive mutex keeps synchronous child-sequence evaluation inside
    // the same exclusive GPU transaction without releasing service state.
    std::lock_guard lock(m_compositeMutex);

    const ResolutionTier requestTier = policy.forceFullResolution
        ? ResolutionTier::Full
        : tierOverride.value_or(playbackTier());
    // stillMode is also used by the export callback.  Export requests use
    // scrubMode to force exact source decoding, but they must still bypass
    // composite LRUs and every last-good substitution: a stale picture is a
    // valid-looking payload that output validation cannot identify later.
    const bool exactStillMode = stillMode;

    // Phase B: advance the CachePolicy generation counter and run its
    // throttled eviction + adaptive-budget passes.  These are very cheap
    // — eviction is gated by a 3s throttle, rebalance is gated by a
    // 30-frame throttle — but having the call site here ensures every
    // composite tick contributes to its LRU view.
    if (!isNestedRecursion && m_cachePolicy) {
        m_cachePolicy->onFrameStart();
        m_cachePolicy->runEvictionPass();
        m_cachePolicy->rebalanceBudgets();
    }

    FrameState r{
        .tick = tick,
        .outW = outW,
        .outH = outH,
        .scrubMode = scrubMode,
        .isNestedRecursion = isNestedRecursion,
        .stillMode = stillMode,
        .exactStillMode = exactStillMode,
        .requestTier = requestTier,
        .context = context,
        .policy = policy,
        .renderTimeline = renderTimeline,
        .renderProject = renderProject,
        .realtimeDiagnostics = realtimeDiagnostics,
    };
    consumeCacheInvalidation(r);
    if (auto answer = serveSegmentCache(r))
        return *answer;
    warmNewShotClips(r);
    logPipelineConfigOnce(r);

    // ── PERF TIMING ─────────────────────────────────────────────────
    static int s_perfCounter = 0;
    r.perfLog = (++s_perfCounter % 10 == 0);
    r.perfT0 = std::chrono::high_resolution_clock::now();
    r.perfTlayers = r.perfT0;

    if (auto answer = serveCompositeLru(r))
        return *answer;
    if (r.outW < 64) r.outW = 64;
    if (r.outH < 36) r.outH = 36;
    resolveLayers(r);
    if (auto answer = answerIncompleteFrame(r))
        return *answer;
    reportLayers(r);
    if (auto answer = serveStaticComposite(r))
        return *answer;
    if (auto answer = servePassthroughLayer(r))
        return *answer;
    if (auto frame = compositeOnGpu(r))
        return frame;

    // P2 (corrected): GPU composite returned nullptr.  This is NOT
    // necessarily device-lost — it can be a transient miss (first
    // frame, no decoded inputs, allocation hiccup).  The actual
    // device-lost signal is fired inside CompositeEngine::composite-
    // ViaRenderGraph on submit failure (see GpuContext.signalDeviceLost
    // call there).  Here we just return the last good frame and log at
    // debug level so the path stays quiet during normal warm-up.
    static thread_local int s_quietLogCounter = 0;
    if (++s_quietLogCounter % 30 == 1) {
        spdlog::debug("[COMPOSITE] tryCompositeOnGpu returned nullptr — "
                      "returning last-good (counter={})", s_quietLogCounter);
    }
    // Nested recursion: never fall back to the outer composite's last
    // good frame -- that frame represents OUTER content. Using it as the
    // inner-sequence layer paints the outer's last frame INSIDE itself,
    // which is the visual feedback loop the user sees. Returning nullptr
    // cleanly skips the inner layer for this frame; the next composite
    // tick will retry.
    if (isNestedRecursion || exactStillMode)
        return nullptr;
    return m_hold.lastGoodWithoutGpu();
}
catch (const std::exception& ex)
{
    spdlog::error("compositeFrame: exception: {}", ex.what());
    if (exceptionStatus) *exceptionStatus = RenderResultStatus::Failed;
    if (isNestedRecursion || stillMode) return nullptr;
    return m_hold.lastGood();
}
catch (...)
{
    spdlog::error("compositeFrame: unknown exception");
    if (exceptionStatus) *exceptionStatus = RenderResultStatus::Failed;
    if (isNestedRecursion || stillMode) return nullptr;
    return m_hold.lastGood();
}


std::shared_ptr<CachedFrame> CompositeService::renderTitleClip(
    TitleClip* clip, int64_t tick, uint32_t outW, uint32_t outH)
{
    return rt::renderTitleClip(clip, tick, outW, outH);
}


namespace {
// Clean CPU-only snapshot of a composite output frame.  compositeFrame returns
// the SHARED last good composite which the next tick reuses, so the cache must
// own an independent copy (the same aliasing the nested-sequence builder guards
// against).  GPU handles are deliberately dropped — this is a CPU frame.
std::shared_ptr<CachedFrame> snapshotForCache(const CachedFrame& frame,
                                              ResolutionTier tier)
{
    auto snap = std::make_shared<CachedFrame>();
    snap->width              = frame.width;
    snap->height             = frame.height;
    snap->stride             = frame.stride ? frame.stride : frame.width * 4;
    snap->pixels             = frame.pixels;
    snap->tier               = tier;
    snap->origin             = frame.origin;
    snap->unpackedAlpha      = frame.unpackedAlpha;
    snap->premultipliedAlpha = frame.premultipliedAlpha;
    snap->preservesAlpha     = frame.preservesAlpha;
    snap->segmentCacheHit    = false;
    return snap;
}
} // namespace

void CompositeService::cacheExportFrame(
    int64_t tick, const std::shared_ptr<CachedFrame>& frame)
{
    if (!frame || !m_segmentRenderCache || !m_timeline) return;
    if (frame->pixels.empty()) return;   // caller hands us pixel-ready frames

    // Export is always full-resolution → key at the Full tier (what the
    // export consult and full-res playback look up).
    constexpr ResolutionTier tier = ResolutionTier::Full;
    const uint32_t renderFlags = frame->preservesAlpha
        ? RenderStatePreserveAlpha : RenderStateNone;
    const uint64_t cfgHash = boundConfigHash(tick, renderFlags);
    if (m_segmentRenderCache->hasFresh(tick, tier, cfgHash)) return;  // already cached

    m_segmentRenderCache->put(tick, tier, cfgHash,
                              snapshotForCache(*frame, tier));

    const uint64_t n = ++m_segCacheWrites;
    if (n == 1 || (n % 120) == 0)
        spdlog::debug("[SEGCACHE] export write-through stored {} frame(s) "
                      "(tick={} {}x{})", n, tick, frame->width, frame->height);
}

void CompositeService::recordFrameCost(int64_t tick, float ms)
{
    std::lock_guard lk(m_frameCostMtx);
    // Bound: drop everything if we somehow accumulate a very long session's
    // worth (rolling estimate — replaying repopulates it cheaply).
    if (m_frameCostMs.size() >= kMaxFrameCostEntries)
        m_frameCostMs.clear();
    m_frameCostMs[tick] = ms;
}

float CompositeService::measuredFrameCostMs(int64_t tick) const
{
    std::lock_guard lk(m_frameCostMtx);
    auto it = m_frameCostMs.find(tick);
    return it != m_frameCostMs.end() ? it->second : -1.0f;
}

bool CompositeService::isTickCached(int64_t tick) const
{
    if (!m_segmentRenderCache || !m_timeline) return false;
    const uint64_t cfgHash = boundConfigHash(tick, RenderStateNone);
    // "Render In to Out" pre-renders at Full (it serves export + full-res
    // playback), so the bar must probe Full to light green from the editing
    // (reduced-tier) context too.  Also accept the current effective tier so a
    // reduced-tier pre-render still shows.
    return m_segmentRenderCache->hasFresh(tick, ResolutionTier::Full, cfgHash)
        || m_segmentRenderCache->hasFresh(tick, effectiveCacheTier(), cfgHash);
}

// ── §4.6 slice 2c: "Render In to Out" foreground driver ─────────────────────
int CompositeService::renderRangeToCache(
    int64_t fromTick, int64_t toTick, uint32_t outW, uint32_t outH,
    const std::function<bool()>& shouldCancel,
    const std::function<void(int, int)>& onProgress,
    bool forceFullResolution)
{
    if (!m_timeline || !m_segmentRenderCache || outW == 0 || outH == 0)
        return 0;
    if (toTick < fromTick) std::swap(fromTick, toTick);

    // Ticks are 48kHz sample-accurate; the frame grid comes from the
    // sequence fps.  Render exactly the frame-aligned ticks the playback
    // clock would land on (see RenderQueue's frame->tick mapping), so the
    // 2c-read consult later keys identically.
    const RationalFrameRate fps = canonicalFrameRate(
        m_timeline->settings().frameRate());
    const double ticksPerFrame = static_cast<double>(kTicksPerSecond)
                               * fps.denominator / fps.numerator;

    // Frame indices whose tick falls within [fromTick, toTick] (inclusive).
    const int64_t firstFrame = static_cast<int64_t>(
        std::ceil(static_cast<double>(fromTick) / ticksPerFrame));
    const int64_t lastFrame = static_cast<int64_t>(
        std::floor(static_cast<double>(toTick) / ticksPerFrame));
    if (lastFrame < firstFrame) return 0;

    const ResolutionTier tier = forceFullResolution
        ? ResolutionTier::Full : playbackTier();
    const int            total = static_cast<int>(lastFrame - firstFrame + 1);

    // Force CPU readback so the composited pixels are materialised before we
    // snapshot them — GPU display mode leaves CachedFrame::pixels empty (the
    // same convention as the thumbnail grab in ProjectControllerMisc).
    const RenderExecutionContext rangeContext{
        m_timeline,
        m_project,
        {
            /*preferGpuOutput=*/false,
            forceFullResolution,
            /*preserveAlpha=*/false,
            /*measureRealtimeCost=*/false,
        },
    };

    int rendered  = 0;
    int processed = 0;
    for (int64_t f = firstFrame; f <= lastFrame; ++f) {
        if (shouldCancel && shouldCancel()) break;

        const int64_t tick = frameIndexToTick(f, fps);
        const uint64_t cfgHash = hashCompositeConfigAt(*m_timeline, tick, m_project);

        // Skip frames already cached under the current config (idempotent /
        // resumable — re-running over an unchanged range is nearly free).
        if (!m_segmentRenderCache->hasFresh(tick, tier, cfgHash)) {
            auto frame = compositeFrameImpl(
                tick, outW, outH, /*scrubMode=*/true,
                /*isNestedRecursion=*/false, /*stillMode=*/false,
                tier, rangeContext);
            if (frame && frame->ensurePixels() && !frame->pixels.empty()) {
                m_segmentRenderCache->put(tick, tier, cfgHash,
                                          snapshotForCache(*frame, tier));
                ++rendered;
            }
        }

        ++processed;
        if (onProgress) onProgress(processed, total);
    }

    spdlog::debug("renderRangeToCache: [{}, {}] tier={} -> {} of {} frames "
                  "rendered ({} MB cached)",
                  fromTick, toTick, static_cast<int>(tier), rendered, total,
                  m_segmentRenderCache->sizeBytes() / (1024 * 1024));
    return rendered;
}


} // namespace rt
