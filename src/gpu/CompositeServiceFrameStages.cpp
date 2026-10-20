/*
 * CompositeServiceFrameStages.cpp -- the stages of
 * CompositeService::compositeFrameImpl (CompositeServiceFrame.cpp), in the
 * order it runs them, and the helpers only they use. The request they share
 * is FrameState (CompositeServiceFrameState.h).
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

template <typename TimePoint>
double wallMs(TimePoint start, TimePoint end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::vector<uint64_t> activeVisualClipIds(const Timeline& timeline, int64_t tick)
{
    std::vector<uint64_t> ids;
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        auto* track = timeline.track(ti);
        if (!track || track->type() != TrackType::Video || track->isMuted())
            continue;
        for (auto* clip : track->clipsAtTime(tick)) {
            if (!clip || !clip->isEnabled() ||
                dynamic_cast<AdjustmentClip*>(clip)) {
                continue;
            }
            ids.push_back(clip->id());
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void staticStateHashBytes(uint64_t& hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
}

template <typename T>
void staticStateHashValue(uint64_t& hash, const T& value)
{
    staticStateHashBytes(hash, &value, sizeof(value));
}

/// Clips that produced a layer stack (adjustment boundaries excluded): the
/// shot identity the hold compares against.
std::vector<uint64_t> visualLayerClipIds(const std::vector<LayerInfo>& layers)
{
    std::vector<uint64_t> ids;
    ids.reserve(layers.size());
    for (const auto& layer : layers)
        if (layer.clipId != 0 && !layer.isAdjustmentLayer)
            ids.push_back(layer.clipId);
    return ids;
}

std::optional<uint64_t> staticCompositeStateKey(
    const std::vector<LayerInfo>& layers, uint32_t outW, uint32_t outH,
    ResolutionTier tier, bool preserveAlpha, bool nv12Output)
{
    if (layers.empty()) return std::nullopt;
    uint64_t hash = 1469598103934665603ull;
    staticStateHashValue(hash, outW);
    staticStateHashValue(hash, outH);
    const uint8_t tierValue = static_cast<uint8_t>(tier);
    staticStateHashValue(hash, tierValue);
    staticStateHashValue(hash, preserveAlpha);
    staticStateHashValue(hash, nv12Output);   // the result's pixel format
    const uint32_t count = static_cast<uint32_t>(layers.size());
    staticStateHashValue(hash, count);

    for (const LayerInfo& layer : layers) {
        if (!layer.contentStableForStateCache || layer.sourceFallbackPending
            || layer.temporalMode != 0 || layer.motionSampleCount != 1
            || !layer.effects.empty() || !layer.masks.empty()
            || layer.wipeProgress >= 0.0f) {
            return std::nullopt;
        }

        uint64_t mediaId = 0;
        int64_t frameNumber = 0;
        uint8_t sourceTier = 0;
        if (layer.gpuCacheBacked) {
            mediaId = layer.gpuCacheMediaId;
            frameNumber = layer.gpuCacheFrameNumber;
            sourceTier = layer.gpuCacheTier;
        } else if (layer.frame && layer.frame->mediaId != 0) {
            mediaId = layer.frame->mediaId;
            frameNumber = layer.frame->frameNumber;
            sourceTier = static_cast<uint8_t>(layer.frame->tier);
        } else {
            return std::nullopt;
        }

        staticStateHashValue(hash, mediaId);
        staticStateHashValue(hash, frameNumber);
        staticStateHashValue(hash, sourceTier);
        staticStateHashValue(hash, layer.clipId);
        staticStateHashValue(hash, layer.frameWidth);
        staticStateHashValue(hash, layer.frameHeight);
        staticStateHashValue(hash, layer.opacity);
        staticStateHashValue(hash, layer.posX);
        staticStateHashValue(hash, layer.posY);
        staticStateHashValue(hash, layer.scX);
        staticStateHashValue(hash, layer.scY);
        staticStateHashValue(hash, layer.rot);
        staticStateHashValue(hash, layer.anchorX);
        staticStateHashValue(hash, layer.anchorY);
        staticStateHashValue(hash, layer.cropL);
        staticStateHashValue(hash, layer.cropR);
        staticStateHashValue(hash, layer.cropT);
        staticStateHashValue(hash, layer.cropB);
        staticStateHashValue(hash, layer.srcRotation);
        staticStateHashValue(hash, layer.blendMode);
        staticStateHashValue(hash, layer.containFit);
        staticStateHashValue(hash, layer.isPacked);
        staticStateHashValue(hash, layer.isPMA);
        staticStateHashValue(hash, layer.needsSwapRB);
    }
    return hash;
}

std::shared_ptr<CachedFrame> makeExactBlankFrame(
    uint32_t outW, uint32_t outH, bool preserveAlpha)
{
    if (outW == 0 || outH == 0 ||
        outW > std::numeric_limits<uint32_t>::max() / 4u) {
        return nullptr;
    }
    const uint32_t stride = outW * 4u;
    const size_t rowBytes = static_cast<size_t>(stride);
    if (static_cast<size_t>(outH) >
        std::numeric_limits<size_t>::max() / rowBytes) {
        return nullptr;
    }

    auto frame = std::make_shared<CachedFrame>();
    frame->width = outW;
    frame->height = outH;
    frame->stride = stride;
    frame->preservesAlpha = preserveAlpha;
    frame->pixels.assign(rowBytes * static_cast<size_t>(outH), 0);
    if (!preserveAlpha) {
        for (size_t i = 3; i < frame->pixels.size(); i += 4)
            frame->pixels[i] = 0xFF;
    }
    return frame;
}

} // namespace

void CompositeService::consumeCacheInvalidation(FrameState& r)
{
    const int64_t tick = r.tick;
    const bool isNestedRecursion = r.isNestedRecursion;
    bool& postInvalidate = r.postInvalidate;

    // Check deferred cache invalidation request (set by requestCacheInvalidation
    // from the UI thread via atomic flag, avoiding deadlock).
    //
    // Skip during nested-sequence recursion: the OUTER frame already
    // consumed the flag at its own entry and reset its own state. If the
    // inner re-consumes the flag (it could read it while still set from a
    // late UI invalidation), inner would reset the outer's last good composite
    // and wipe the outer's LRU mid-frame -- producing the "every other frame
    // glitches to a stale frame" symptom on nested sequences.
    if (!isNestedRecursion
            && m_cacheInvalidateRequested.exchange(false, std::memory_order_acquire)) {
        postInvalidate = true;
        spdlog::debug("[LIVE-RELOAD] compositeFrame tick={}: invalidate flag "
                      "consumed — clearLru + reset lastGoodComposite", tick);
        if (m_engine)
            m_engine->clearLru();
        // Re-arms the settle window too, so post-invalidation views get the
        // same first-view grace.
        m_hold.clearLastGoodAndSettle();
    }
}

CompositeService::FrameAnswer CompositeService::serveSegmentCache(FrameState& r)
{
    const int64_t tick = r.tick;
    const uint32_t outW = r.outW;
    const uint32_t outH = r.outH;
    const bool isNestedRecursion = r.isNestedRecursion;
    const ResolutionTier requestTier = r.requestTier;
    const auto& context = r.context;
    const auto& policy = r.policy;
    auto* const renderTimeline = r.renderTimeline;
    FrameDiagnostics* const realtimeDiagnostics = r.realtimeDiagnostics;

    // ── §4.6 2c-read: serve a pre-rendered ("green") segment from cache ──
    // After invalidation handling (so a just-consumed edit can't serve stale)
    // and only for a top-level composite whose effective tier + output size
    // match the cached render.  configHash gates correctness: any edit changes
    // the hash, so a stale entry silently misses.  The cached frame is a CPU
    // snapshot (gpuReady=false) — a presentation path the modal fallback above
    // already exercises. Default ON now that it's verified. The candidate
    // probe is an O(1) (tick,tier) lookup, so an unrelated cached range does
    // not force full render-state hashing on every live playback frame.
    // The consult also fires during export (forceFullResolution): export reuses
    // pre-rendered FULL-tier segments, and effectiveCacheTier() keeps export
    // (Full) and reduced playback (Half/…) keyed apart.
    const ResolutionTier segmentTier = requestTier;
    const auto segmentLookupStart = std::chrono::steady_clock::now();
    // Segments are stored at canonical frame STARTS (renderRangeToCache).
    // A preview tick inside a frame shows that frame, so look up its start.
    // Export ticks are already exact on the EXPORT grid, which can differ
    // from the sequence rate — use them as-is.
    const int64_t segmentTick = (policy.forceFullResolution || !renderTimeline)
        ? tick
        : frameStartAtOrBefore(tick, renderTimeline->settings().frameGrid());
    if ((m_segmentCacheReadEnabled.load(std::memory_order_relaxed)
             || policy.forceFullResolution)
            && !isNestedRecursion && m_segmentRenderCache
            && m_segmentRenderCache->hasCandidate(segmentTick, segmentTier)) {
        const ResolutionTier tier = segmentTier;
        const uint32_t renderFlags = policy.preserveAlpha
            ? RenderStatePreserveAlpha : RenderStateNone;
        const uint64_t cfgHash = configHashFor(
            context, *renderTimeline, segmentTick, renderFlags);
        auto cached = m_segmentRenderCache->get(segmentTick, tier, cfgHash);
        if (!cached) {
            // The (tick,tier,hash) key isn't present — the common, expected
            // case for any not-yet-rendered frame.  Debug-only tally.
            const uint64_t n = ++m_segCacheKeyMiss;
            if (n == 1 || (n % 600) == 0)
                spdlog::debug("[SEGCACHE] miss #{}: tick={} tier={} cfgHash={:#x}",
                              n, segmentTick, static_cast<int>(tier), cfgHash);
        }
        if (cached) {
            if (cached->width == outW && cached->height == outH
                    && !cached->pixels.empty()) {
                // Keep the last-good fallback coherent with what we present.
                m_hold.adoptLastGood(cached, tick);
                cached->segmentCacheHit = true;
                cached->preservesAlpha = (renderFlags & RenderStatePreserveAlpha) != 0;
                const uint64_t n = ++m_segCacheHits;
                if (n == 1 || (n % 300) == 0)
                    spdlog::debug("[SEGCACHE] served {} frame(s) from cache "
                                  "(tick={} {}x{} tier={})",
                                  n, tick, outW, outH, static_cast<int>(tier));
                if (realtimeDiagnostics) {
                    realtimeDiagnostics->cacheLookupMs += wallMs(
                        segmentLookupStart, std::chrono::steady_clock::now());
                    realtimeDiagnostics->cacheHit = true;
                    realtimeDiagnostics->segmentCacheHit = true;
                }
                return cached;
            } else {
                // Entry exists but the size differs — a real misconfiguration
                // (e.g. exporting at a resolution other than what was pre-
                // rendered): the segment silently won't be reused.  Keep this
                // at warn, throttled, so it's discoverable without spamming.
                const uint64_t n = ++m_segCacheSizeMiss;
                if (n == 1 || (n % 60) == 0)
                    spdlog::warn("[SEGCACHE] {} cached frame(s) NOT reused — size "
                                 "mismatch: cached {}x{} vs requested {}x{} "
                                 "(re-render at the current playback resolution)",
                                 n, cached->width, cached->height, outW, outH);
            }
        }
    }

    if (realtimeDiagnostics) {
        realtimeDiagnostics->cacheLookupMs += wallMs(
            segmentLookupStart, std::chrono::steady_clock::now());
    }
    return std::nullopt;
}

void CompositeService::warmNewShotClips(FrameState& r)
{
    const int64_t tick = r.tick;
    const bool scrubMode = r.scrubMode;
    const bool isNestedRecursion = r.isNestedRecursion;
    const ResolutionTier requestTier = r.requestTier;
    const auto& policy = r.policy;
    auto* const renderTimeline = r.renderTimeline;
    FrameDiagnostics* const realtimeDiagnostics = r.realtimeDiagnostics;

    // ── Timeline lookahead prewarm ─────────────────────────────────────
    // Proactively open media + schedule first-frame prefetch for clips
    // coming up in the next ~2s.  Kills the reactive cold-decoder stall
    // at every shot boundary.  Also run during export (forceFullRes)
    // so the decode cache stays hot and we don't block on every frame.
    //
    // Scrub: SKIP during normal playback, but ALWAYS run when exporting
    // (forceFullResolution).  The Export Panel preview and RenderQueue
    // export are forward-sequential, not random scrub — prewarm is just
    // as beneficial as during normal playback.  During scrub the user's
    // direction/speed isn't known and the predictive work is mostly
    // wasted.  Worse, the scan can stall on MediaPool m_mutex (see
    // PRE-LAYER-SLOW > 6s on backward scrub across clip boundaries).
    auto preLayerT0 = std::chrono::high_resolution_clock::now();
    if (!isNestedRecursion && (!scrubMode || policy.forceFullResolution)) {
        prewarmUpcomingShots(
            tick, policy.forceFullResolution, renderTimeline);
    }
    auto prewarmT1 = std::chrono::high_resolution_clock::now();

    // Shot-boundary detection: when new clips appear (different from last
    // composite), force blocking decode so the correct character shows
    // immediately instead of flashing the previous shot's frame.
    // Also run during export (forceFullResolution) so cache isn't
    // polluted by stale clip IDs and upcoming clips decode promptly.
    //
    // Scrub: SKIP during normal playback for the same reason as prewarm
    // above, but ALWAYS run during export/Export Preview.
    // m_lastActiveClipIds is deliberately left untouched when skipped
    // so the first non-scrub composite after the user stops treats every
    // visible clip as "new" and fires the prefetch once, in the right place.
    if (!isNestedRecursion && (!scrubMode || policy.forceFullResolution)) {
        std::unordered_set<uint64_t> currentClipIds;
        for (size_t ti = renderTimeline->trackCount(); ti > 0; --ti) {
            auto* track = renderTimeline->track(ti - 1);
            if (!track || track->type() != TrackType::Video || track->isMuted())
                continue;
            for (auto* clip : track->clipsAtTime(tick)) {
                if (clip && clip->isEnabled())
                    currentClipIds.insert(clip->id());
            }
        }
        // Read m_lastActiveClipIds via a snapshot under its dedicated mutex —
        // the prewarm worker thread may move-assign it concurrently (see the
        // member declaration). All .find() checks below use the stable copy.
        std::unordered_set<uint64_t> lastActiveSnapshot;
        {
            std::lock_guard<std::mutex> g(m_lastActiveClipIdsMutex);
            lastActiveSnapshot = m_lastActiveClipIds;
        }
        bool hasNewClips = false;
        for (auto id : currentClipIds) {
            if (lastActiveSnapshot.find(id) == lastActiveSnapshot.end()) {
                hasNewClips = true;
                break;
            }
        }
        if (hasNewClips) {
            int unopenedCount = 0;
            int uncachedCount = 0;
            for (size_t ti = renderTimeline->trackCount(); ti > 0; --ti) {
                auto* track = renderTimeline->track(ti - 1);
                if (!track || track->type() != TrackType::Video || track->isMuted())
                    continue;
                for (auto* clip : track->clipsAtTime(tick)) {
                    if (!clip || !clip->isEnabled()) continue;
                    if (lastActiveSnapshot.find(clip->id()) != lastActiveSnapshot.end())
                        continue;
                    auto* videoClip = dynamic_cast<VideoClip*>(clip);
                    if (!videoClip) continue;
                    const auto& mp = videoClip->mediaPath();
                    if (mp.empty() || !m_mediaPool) continue;

                    if (!m_mediaPool->isPathOpen(mp)) {
                        m_mediaPool->openAsync(mp);
                        ++unopenedCount;
                        continue;
                    }
                    auto handle = m_mediaPool->open(mp);
                    if (handle != 0) {
                        const int64_t clipTickOffset = (tick - clip->timelineIn()) + clip->sourceIn();
                        const auto* info = m_mediaPool->getInfo(handle);
                        const double srcFps = (info && info->fps > 0.0)
                                              ? info->fps
                                              : (videoClip->sourceFps() > 0.0
                                                 ? videoClip->sourceFps() : 30.0);
                        const double secs = static_cast<double>(clipTickOffset)
                                            / static_cast<double>(rt::kTicksPerSecond);
                        const int64_t srcFrame = std::llround(secs * srcFps);
                        // Match the live composite path (charVideoTier):
                        // forceFullResolution wins, otherwise playbackTier().
                        // Previously characters were pinned to Half here,
                        // which filled the cache with Half-tier frames.
                        // When forceFullResolution was set (export/preview),
                        // getFrame() would then return the cached Half-tier
                        // frame via its alt-tier fallback — the export
                        // composited Half-tier characters at full viewport
                        // resolution, producing blurry output.
                        const auto warmTier = videoClip->isVideoCharacter()
                            ? (policy.forceFullResolution
                               ? ResolutionTier::Full
                               : requestTier)
                            : requestTier;
                        if (!m_mediaPool->isFrameCached(handle, srcFrame, warmTier)) {
                            // Two-stage prefetch on shot boundary: a small
                            // *urgent* batch (the current + previous frame)
                            // followed by a non-urgent fan-out for the next
                            // half-second.  The previous version requested
                            // count=8 urgent per new clip, which at a typical
                            // multi-clip boundary (2-3 new clips + Spine
                            // atlas upload happening in parallel) flooded
                            // NVDEC + the graphics queue with ~16-24
                            // simultaneous urgent decodes and tripped TDR.
                            // The urgent pair guarantees the frame we are
                            // about to composite is hot; the deferred batch
                            // covers the upcoming playhead motion without
                            // sharing a single contention window.
                            m_mediaPool->schedulePrefetch(handle, srcFrame - 1,
                                /*count=*/2, /*urgent=*/true, warmTier);
                            m_mediaPool->schedulePrefetch(handle, srcFrame + 1,
                                /*count=*/6, /*urgent=*/false, warmTier);
                            ++uncachedCount;
                        }
                    }
                }
            }

            if (unopenedCount > 0 || uncachedCount > 0) {
                spdlog::info("[COMPOSITE] Shot boundary: {} new clips ({} unopened-async, {} uncached-prefetch) — staying non-blocking",
                             currentClipIds.size(), unopenedCount, uncachedCount);
                if (unopenedCount > 0) {
                    spdlog::warn("[LOOKAHEAD-MISS] shot-boundary caught {} clips with UNOPENED media — prewarm didn't reach far enough ahead or timeline was modified",
                                 unopenedCount);
                }
            }
        }
        // Commit the new active set under the dedicated mutex. Copy (not
        // move) so currentClipIds stays valid for the spine cleanup below —
        // that lets the cleanup use the local set instead of touching the
        // shared member a second time.
        {
            std::lock_guard<std::mutex> g(m_lastActiveClipIdsMutex);
            m_lastActiveClipIds = currentClipIds;
        }
    #ifdef ROUNDTABLE_HAS_SPINE
        for (auto it = m_lastPreRenderedSpineFrame.begin();
             it != m_lastPreRenderedSpineFrame.end();) {
            if (currentClipIds.find(it->first) == currentClipIds.end())
            it = m_lastPreRenderedSpineFrame.erase(it);
            else
            ++it;
        }
    #endif
    }

    if (!isNestedRecursion && m_mediaPool) {
        m_mediaPool->scheduler().setPlayhead(tick);
    }

    auto shotBoundaryT1 = std::chrono::high_resolution_clock::now();
    if (realtimeDiagnostics) {
        realtimeDiagnostics->prewarmMs = wallMs(preLayerT0, prewarmT1);
        realtimeDiagnostics->shotBoundaryMs = wallMs(prewarmT1, shotBoundaryT1);
        realtimeDiagnostics->timelineEvalMs =
            realtimeDiagnostics->prewarmMs +
            realtimeDiagnostics->shotBoundaryMs;
    }

    // Warn-level breakdown of the pre-layer-build phase so we can tell
    // apart prewarmUpcomingShots vs the shot-boundary detection block
    // when COMPOSITE-SLOW reports a high "layers=" value.  Both can
    // block on MediaPool m_mutex under heavy churn (e.g., the 6.4 s
    // stall observed at 2026-05-22 12:16:40 tick=1811200 after a
    // backward scrub crossed clip boundaries).  Threshold 30 ms.
    {
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        const double prewarmMs = ms(preLayerT0, prewarmT1);
        const double shotMs    = ms(prewarmT1, shotBoundaryT1);
        if (prewarmMs > 30.0 || shotMs > 30.0) {
            spdlog::warn("[PRE-LAYER-SLOW] tick={} prewarm={:.1f}ms "
                         "shotBoundary={:.1f}ms",
                         tick, prewarmMs, shotMs);
        }
    }
}

void CompositeService::logPipelineConfigOnce(const FrameState& r)
{
    const uint32_t outW = r.outW;
    const uint32_t outH = r.outH;
    const auto& policy = r.policy;

    // ── ONE-TIME STARTUP DIAGNOSTIC ──────────────────────────────────
    {
        static bool s_startupLogged = false;
        if (!s_startupLogged) {
            s_startupLogged = true;
            auto& gpu = GpuContext::get();
            spdlog::info("========== COMPOSITE PIPELINE CONFIG ==========");
            spdlog::info("  gpuDisplayMode = {}", policy.preferGpuOutput);
            spdlog::info("  GpuContext initialized = {}", gpu.isInitialized());
            spdlog::info("  CudaVulkanInterop = {}", gpu.cudaVulkanInterop() ? "YES" : "NO");
            spdlog::info("  outW={} outH={}", outW, outH);
            if (m_mediaPool)
                spdlog::info("  open media count = {}", m_mediaPool->openCount());
            if (m_engine) {
                auto* texCache = m_engine->textureCache();
                if (texCache)
                    spdlog::info("  GpuTexCache budget = {:.0f} MB, entries = {}",
                                 texCache->budget() / 1048576.0,
                                 texCache->entryCount());
                else
                    spdlog::info("  GpuTexCache = NOT yet created");
            }
            spdlog::info("===============================================");
        }
    }
}

CompositeService::FrameAnswer CompositeService::serveCompositeLru(FrameState& r)
{
    const int64_t tick = r.tick;
    const uint32_t outW = r.outW;
    const uint32_t outH = r.outH;
    const bool isNestedRecursion = r.isNestedRecursion;
    const bool exactStillMode = r.exactStillMode;
    const ResolutionTier requestTier = r.requestTier;
    const auto& context = r.context;
    const auto& policy = r.policy;
    auto* const renderTimeline = r.renderTimeline;
    FrameDiagnostics* const realtimeDiagnostics = r.realtimeDiagnostics;
    const CompositeCacheKey& compositeCacheKey = r.cacheKey;

    // Composite result LRU cache.
    //
    // Skip entirely during nested-sequence recursion.  The LRU is keyed
    // only by (tick, w, h) and is SHARED between the outer program
    // composite and the inner nested-sequence composite (same engine,
    // child graph context). The inner composite runs in forced CPU mode
    // and writes its result (the inner timeline WITHOUT the SequenceClip
    // transform) into this same LRU.  When innerTick collides with the
    // outer tick, the outer's checkLru would return that inner-only frame
    // as the whole program output -- the "nested sequence flickers its
    // untransformed position every other frame" bug.  The inner must
    // always freshly composite its own content (it is snapshotted to a
    // CPU layer by the caller anyway).
    //
    // Scrub: ALLOW.  Letting scrub hit the LRU means an oscillating drag
    // around the same tick at the same divisor reuses the prior composite.
    // Exact paused stills deliberately bypass it because the key has no
    // decode-tier component: an older same-size composite may contain a
    // lower-tier fallback stretched to the requested canvas.
    // Live preview uses the editor's monotonic invalidation generation. This
    // makes the per-frame LRU key O(1); serializing every clip on every video
    // track here made playback cost grow with total project size. Immutable
    // render snapshots retain their captured editVersion. Persistent segment
    // cache entries still use the complete render-state hash above.
    // A published live snapshot contributes its editVersion too: an edit
    // bumps the generation immediately but publishes one event-loop turn
    // later, and a frame rendered from the older snapshot in between must
    // not be cached under the new generation.
    const uint64_t liveGeneration =
        m_liveCacheGeneration.load(std::memory_order_acquire);
    const uint64_t compositeVersion = context.immutableSnapshot
        ? context.editVersion
        : (context.sequenceSnapshot
               ? (context.editVersion * 0x9E3779B97F4A7C15ull) ^ liveGeneration
               : liveGeneration);
    r.cacheKey = CompositeCacheKey{
        renderTimeline,
        compositeVersion,
        tick,
        outW,
        outH,
        requestTier,
        policy.preserveAlpha,
    };
    const auto compositeCacheStart = std::chrono::steady_clock::now();
    if (m_engine && !isNestedRecursion && !exactStillMode) {
        auto cached = m_engine->checkLru(compositeCacheKey);
        if (cached) {
            if (realtimeDiagnostics) {
                realtimeDiagnostics->cacheLookupMs += wallMs(
                    compositeCacheStart, std::chrono::steady_clock::now());
                realtimeDiagnostics->cacheHit = true;
                realtimeDiagnostics->compositeCacheHit = true;
            }
            return cached;
        }
    }
    if (realtimeDiagnostics) {
        realtimeDiagnostics->cacheLookupMs += wallMs(
            compositeCacheStart, std::chrono::steady_clock::now());
    }
    return std::nullopt;
}

void CompositeService::resolveLayers(FrameState& r)
{
    const int64_t tick = r.tick;
    const uint32_t outW = r.outW;
    const uint32_t outH = r.outH;
    const bool scrubMode = r.scrubMode;
    const bool isNestedRecursion = r.isNestedRecursion;
    const bool exactStillMode = r.exactStillMode;
    const ResolutionTier requestTier = r.requestTier;
    const auto& context = r.context;
    auto* const renderTimeline = r.renderTimeline;
    auto* const renderProject = r.renderProject;
    FrameDiagnostics* const realtimeDiagnostics = r.realtimeDiagnostics;
    bool& playbackNonBlocking = r.playbackNonBlocking;
    bool& gpuSpineUsedThisFrame = r.gpuSpineUsedThisFrame;
    const bool perfLog = r.perfLog;
    auto& layers = r.layers;
    int& clipsAtTick = r.clipsAtTick;
    int& resolvedClipsAtTick = r.resolvedClipsAtTick;

    const auto mediaResolveStart = std::chrono::steady_clock::now();
    layers = buildLayersForFrame(tick, outW, outH, scrubMode, playbackNonBlocking,
                                 requestTier, exactStillMode, context,
                                 clipsAtTick, resolvedClipsAtTick, perfLog,
                                 gpuSpineUsedThisFrame);

    // ── Phase 4: boundary block-on-arrival ──────────────────────────────
    // When layers.empty() while clipsAtTick > 0, the prewarm prefetches
    // are in flight but the playhead arrived a few ms early.  Briefly
    // poll the active clips' frames and retry buildLayersForFrame once
    // if any arrive within ~16 ms (one 60 fps frame budget — better to
    // lose one frame than show a black flash).  The decoder workers
    // run on their own threads and don't need m_compositeMutex, so
    // holding the lock during the short poll is fine.
    if (!scrubMode && !isNestedRecursion &&
        layers.empty() && clipsAtTick > 0 && m_mediaPool)
    {
        struct PendingFrame {
            MediaHandle handle;
            int64_t frame;
            ResolutionTier tier;
        };
        std::vector<PendingFrame> pending;
        pending.reserve(8);

        const auto wantTier = requestTier;

        // Collect VideoClips in a Timeline at the given tick that aren't
        // cached yet.  Extracted as a lambda so we can call it for the
        // outer timeline AND for the inner timeline of each SequenceClip
        // — without descending, a nested sequence's first frame would
        // never trigger the block-on-arrival wait and the program monitor
        // would flash black when the only visible content is the nested
        // sequence and its inner decoders aren't warm.
        auto collectPending = [&](Timeline* tl, int64_t atTick) {
            if (!tl) return;
            for (size_t ti = 0; ti < tl->trackCount(); ++ti) {
                auto* trk = tl->track(ti);
                if (!trk || trk->type() != TrackType::Video || trk->isMuted())
                    continue;
                for (auto* c : trk->clipsAtTime(atTick)) {
                    if (!c || !c->isEnabled()) continue;
                    auto* vc = dynamic_cast<VideoClip*>(c);
                    if (!vc) continue;
                    const auto& mp = vc->mediaPath();
                    if (mp.empty()) continue;

                    const MediaHandle handle = findMediaHandle(mp);
                    if (handle == 0) continue;

                    const auto* info = m_mediaPool->getInfo(handle);
                    if (!info) continue;

                    // Shared mapping authority (VideoFrameMapping.h).  This
                    // collector previously resolved fps in a DIFFERENT
                    // priority order than the renderer (media fps first
                    // instead of clip fps first), so it could prefetch a
                    // different frame than the renderer would request —
                    // wasted prefetch + a cache miss on the real frame.
                    const auto mapped =
                        mapTickToSourceFrame(*vc, atTick, info);
                    const bool temporal =
                        vc->timeInterpolation() != TimeInterpolation::FrameSampling &&
                        mapped.blendPhase > 0.000001 &&
                        mapped.lowerFrame != mapped.upperFrame;
                    const int64_t primary = temporal
                        ? mapped.lowerFrame : mapped.frame;
                    if (!m_mediaPool->isFrameCached(handle, primary, wantTier))
                        pending.push_back({handle, primary, wantTier});
                    if (temporal &&
                        !m_mediaPool->isFrameCached(handle, mapped.upperFrame,
                                                    wantTier)) {
                        pending.push_back({handle, mapped.upperFrame, wantTier});
                    }
                }
            }
        };

        // Outer timeline.
        collectPending(renderTimeline, tick);

        // Inner timelines reachable through one level of SequenceClip
        // (matches the depth=2 cap on compositeFrame recursion).  Without
        // this the nested sequence's first frame on a fresh shot shows
        // black instead of waiting up to 16 ms for inner decoders.
        if (renderProject || context.sequenceSnapshot) {
            for (size_t ti = 0; ti < renderTimeline->trackCount(); ++ti) {
                auto* trk = renderTimeline->track(ti);
                if (!trk || trk->type() != TrackType::Video || trk->isMuted())
                    continue;
                for (auto* c : trk->clipsAtTime(tick)) {
                    if (!c || !c->isEnabled()) continue;
                    auto* seqClip = dynamic_cast<SequenceClip*>(c);
                    if (!seqClip) continue;
                    auto* innerTl = resolveSequence(context, seqClip->sequenceIndex());
                    if (!innerTl || innerTl == renderTimeline) continue;
                    // Mirror the render path's innerTick mapping
                    // (localTick + sourceIn, clamped non-negative).
                    int64_t innerTick = (tick - c->timelineIn()) + c->sourceIn();
                    if (innerTick < 0) innerTick = 0;
                    collectPending(innerTl, innerTick);
                }
            }
        }

        if (!pending.empty()) {
            using namespace std::chrono;
            const auto deadline = steady_clock::now() + milliseconds(16);
            bool anyArrived = false;
            do {
                std::this_thread::sleep_for(milliseconds(2));
                for (const auto& p : pending) {
                    if (m_mediaPool->isFrameCached(p.handle, p.frame, p.tier)) {
                        anyArrived = true;
                        break;
                    }
                }
            } while (!anyArrived && steady_clock::now() < deadline);

            if (anyArrived) {
                // Reset gpuSpineUsedThisFrame so the retry isn't blocked by
                // the prior call's side-effect.
                gpuSpineUsedThisFrame = false;
                layers = buildLayersForFrame(
                    tick, outW, outH, scrubMode, playbackNonBlocking,
                    requestTier, exactStillMode, context,
                    clipsAtTick, resolvedClipsAtTick, perfLog,
                    gpuSpineUsedThisFrame);
                spdlog::info("[BLOCK-ON-ARRIVAL] tick={}: retry yielded "
                             "{}/{} clips", tick, resolvedClipsAtTick,
                             clipsAtTick);
            } else {
                static std::atomic<int> s_missCount{0};
                int n = ++s_missCount;
                if (n <= 20 || n % 50 == 0) {
                    spdlog::warn("[BLOCK-ON-ARRIVAL] tick={}: 16ms deadline "
                                 "elapsed with no decoded frames ({} pending) "
                                 "— falling through to empty sentinel (miss #{}) ",
                                 tick, pending.size(), n);
                }
            }
        }
    }

    if (realtimeDiagnostics) {
        realtimeDiagnostics->mediaResolveMs = wallMs(
            mediaResolveStart, std::chrono::steady_clock::now());
        realtimeDiagnostics->layerCount = static_cast<uint32_t>(layers.size());
    }
}

CompositeService::FrameAnswer CompositeService::answerIncompleteFrame(FrameState& r)
{
    const int64_t tick = r.tick;
    const uint32_t outW = r.outW;
    const uint32_t outH = r.outH;
    const bool scrubMode = r.scrubMode;
    const bool isNestedRecursion = r.isNestedRecursion;
    const bool stillMode = r.stillMode;
    const bool exactStillMode = r.exactStillMode;
    const auto& context = r.context;
    const auto& policy = r.policy;
    auto* const renderTimeline = r.renderTimeline;
    bool& postInvalidate = r.postInvalidate;
    auto& layers = r.layers;
    int& clipsAtTick = r.clipsAtTick;
    int& resolvedClipsAtTick = r.resolvedClipsAtTick;

    // ── Nested-sequence recursion isolation ─────────────────────────────
    // When this call is an inner SequenceClip composite and it cannot
    // fully resolve its own layers this tick, return nullptr instead of
    // falling through to the settle-hold / empty-sentinel paths below.
    // Those paths return the OUTER's last good composite (outer program
    // content) or an empty width=0 sentinel; either one used as the
    // inner-sequence layer paints the program inside itself / blanks the
    // clip, flickering against correctly-composited frames.  Skipping the
    // inner layer for this tick (it retries next tick) is the documented
    // correct behavior — see the isNestedRecursion nullptr fallback after
    // tryCompositeOnGpu below.
    // A valid timeline gap is a deliberate blank canvas.  It is distinct from
    // a failed source/composite and must be represented by a complete payload
    // so export can encode the gap without substituting a failure frame.
    if (exactStillMode && clipsAtTick == 0 && layers.empty()) {
        return makeExactBlankFrame(outW, outH, policy.preserveAlpha);
    }

    // LayerInfo also contains adjustment boundaries, so layers.size() cannot
    // prove every active source clip resolved.  Exact export fails this tick
    // closed whenever even one real visual clip is absent.
    if (exactStillMode && resolvedClipsAtTick < clipsAtTick)
        return nullptr;

    if (isNestedRecursion &&
        (resolvedClipsAtTick < clipsAtTick || layers.empty())) {
        return nullptr;
    }

    // A decoder miss is pending work, not an empty timeline. In particular,
    // BAR overlays and adjustment boundaries can make layers.size() look
    // complete while a new shot or its background is still decoding. Never
    // publish/cache that partial composite as a ready playback frame. The
    // producer holds its last complete picture until the new sources arrive.
    // Confirmed resource failures retain the partial-result policy below.
    const bool resourceFailure = context.outcome &&
        (context.outcome->status == RenderResultStatus::MissingMedia ||
         context.outcome->status == RenderResultStatus::Failed);
    if (!scrubMode && !stillMode && !resourceFailure &&
        resolvedClipsAtTick < clipsAtTick) {
        return nullptr;
    }
    // Still and scrub requests answer anyway — with the held prior composite,
    // an empty sentinel or a partial composite. Say so, so the caller renders
    // this tick again instead of taking it as the exact frame.
    if (!resourceFailure && resolvedClipsAtTick < clipsAtTick && context.outcome)
        context.outcome->provisional = true;

    // ── A1: Settle-window fallback (Premiere-style hold) ─────────────────
    // If we resolved FEWER layers than the active clip count, the composite
    // would render with missing layers — the visible "layers build up over
    // several frames" symptom.  Hold the prior complete composite (or, on
    // first view when there's no prior, return an empty sentinel so the
    // viewport stays clear) for up to CompositeHold::kSettleWindowMs.  After that
    // timeout, accept the partial composite so we don't freeze indefinitely
    // if a clip is permanently unresolvable (e.g. missing media file).
    //
    // Skip during scrub (the user wants immediate feedback on every drag
    // step, partial frames are fine).  Previously also skipped when
    // forceFullResolution was set, but that left the Export Preview's
    // real-time playback path showing partial composites (missing
    // background / discolored character) because the actual export and
    // the Export Preview share the same forceFullResolution=true path —
    // export tolerates partial frames because it runs non-real-time and
    // blocks until full, but Export Preview runs on a QTimer and can't.
    // Export still bypasses this via scrub=true (set by RenderQueue).
    if (!scrubMode &&
        clipsAtTick > 0 && resolvedClipsAtTick < clipsAtTick)
    {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        // Adjustment clips are not in the id set: they produce no pixels.
        const auto settle = m_hold.settle(now, tick, [&] {
            return activeVisualClipIds(*renderTimeline, tick);
        });
        const std::shared_ptr<CachedFrame>& hold = settle.frame;
        const bool withinSettle = settle.withinWindow;
        const bool firstView = settle.firstView;
        if (withinSettle) {
            static int s_settleLog = 0;
            if (++s_settleLog % 30 == 0 || firstView) {
                spdlog::info("[SETTLE] tick={} resolved={}/{} — {}",
                             tick, layers.size(), clipsAtTick,
                             hold ? "holding prior composite"
                                  : "first-view, returning empty sentinel");
            }
            if (hold) return hold;
            // No prior composite — return an empty sentinel.  The pipeline
            // treats width==0 as "clear viewport" and the next tick will
            // re-composite (poll timer / settle counter forces re-render).
            // By CompositeHold::kSettleWindowMs the prefetch workers should have decoded
            // enough first frames that layers.size() == clipsAtTick.
            return std::make_shared<CachedFrame>();
        }
    }

    if (layers.empty()) {
        static int s_emptyLog = 0;
        if (++s_emptyLog <= 20 || s_emptyLog % 60 == 0) {
            spdlog::info("[FLICKER-DIAG] compositeFrame tick={}: layers empty "
                         "(clipsAtTick={}, all skipped)",
                         tick, clipsAtTick);
        }

        if (clipsAtTick > 0 && !exactStillMode) {
            // The stale fallback below masks only a short, mid-shot decoder
            // warm-up. A persistent overlay must not make two different clip
            // sets count as the same shot, and a distant seek within one long
            // clip must not borrow the old source position.
            const auto currentClipIds = activeVisualClipIds(*renderTimeline, tick);

            // At most CompositeHold::kMaxStaleTicks (1 frame at 24 fps,
            // ~2.5 at 60 fps), so the viewport doesn't freeze on the
            // previous picture while the new shot's first frame decodes.
            using Stale = CompositeHold::Stale;
            const auto stale = m_hold.staleFallback(tick, currentClipIds);
            switch (stale.kind) {
            case Stale::Kind::DifferentShot:
                // The prior composite showed another shot; it has been
                // dropped so later ticks here don't return it either.
                return std::make_shared<CachedFrame>();
            case Stale::Kind::Reuse:
                if (postInvalidate) {
                    // debug-level: routine stale-fallback during settle,
                    // not an anomaly (see LIVE-RELOAD FRESH note below).
                    spdlog::debug("[LIVE-RELOAD] compositeFrame tick={}: "
                                  "returning STALE last good composite "
                                  "(lastTick={}, clipsAtTick={}, delta={})",
                                  tick, stale.lastTick, clipsAtTick, stale.delta);
                }
                return stale.frame;
            case Stale::Kind::TooOld:
                spdlog::info("[COMPOSITE] Suppressing stale lastGoodComposite "
                             "(tick={} vs last={}, delta={})",
                             tick, stale.lastTick, stale.delta);
                break;
            case Stale::Kind::None:
                break;
            }
        }

        // ── Shot boundary: return empty sentinel ────────────────────────
        // The stale frame was shown for at most 1-2 frames (kMaxStaleTicks
        // above), then the new clip's first frame still isn't decoded.
        // Return an empty frame (width=0) which propagates through the
        // FrameProducer → FramePresenter → ProgramMonitor chain as a
        // "clear viewport" signal.  Meanwhile the async prefetch workers
        // (started by the shot-boundary detection above) continue decoding
        // the new clip's frames; when they arrive, the next composite tick
        // will produce the correct frame.
        spdlog::info("[COMPOSITE] Shot boundary tick={}: returning empty sentinel "
                     "({} clips present, no decoded frames available)",
                     tick, clipsAtTick);
        if (postInvalidate) {
            spdlog::debug("[LIVE-RELOAD] compositeFrame tick={}: returning "
                          "EMPTY SENTINEL after invalidate (clipsAtTick={}, "
                          "no decoded frames available — decode in flight)",
                          tick, clipsAtTick);
        }
        // Re-arm the settle window too: the next composite at this tick
        // (after prefetches land) gets the first-view grace instead of
        // accepting another partial right away.
        m_hold.clearLastGoodAndSettle();
        return std::make_shared<CachedFrame>();
    }
    return std::nullopt;
}

void CompositeService::reportLayers(FrameState& r)
{
    const int64_t tick = r.tick;
    const auto& context = r.context;
    auto& layers = r.layers;

    // Log layer details for packed-alpha diagnostics
    {
        static int s_layerDiag = 0;
        if (++s_layerDiag % 60 == 0) {
            for (size_t li = 0; li < layers.size(); ++li) {
                const auto& L = layers[li];
                spdlog::info("[FLICKER-DIAG] tick={} layer[{}]: {}x{} isPacked={} containFit={} "
                             "gpuTex={} opacity={:.2f} clipId={} unpackedAlpha={}",
                             tick, li, L.frameWidth, L.frameHeight,
                             L.isPacked, L.containFit, L.gpuTextureReady,
                             L.opacity, L.clipId,
                             (L.frame ? L.frame->unpackedAlpha : false));
            }
        }
    }

    // A layer drawn from a stand-in source makes the answer provisional on
    // every return path below — including the single-layer fast path, which
    // hands back the (nearby) source frame itself.
    if (context.outcome &&
        std::any_of(layers.begin(), layers.end(), [](const LayerInfo& layer) {
            return layer.sourceFallbackPending;
        }))
        context.outcome->provisional = true;
}

CompositeService::FrameAnswer CompositeService::serveStaticComposite(FrameState& r)
{
    const int64_t tick = r.tick;
    const uint32_t outW = r.outW;
    const uint32_t outH = r.outH;
    const bool isNestedRecursion = r.isNestedRecursion;
    const ResolutionTier requestTier = r.requestTier;
    const auto& policy = r.policy;
    auto& layers = r.layers;
    const auto& staticStateKey = r.staticStateKey;

    // If every source is invariant and the evaluated layer state is identical
    // to the preceding tick, reuse the completed composite. This is the common
    // stacked-PNG/tier-board case: layer lookup is cheap, while dispatching and
    // reading back the same full-frame blend again is pure duplicate work.
    r.staticStateKey = !isNestedRecursion
        ? staticCompositeStateKey(
              layers, outW, outH, requestTier, policy.preserveAlpha,
              policy.nv12Output)
        : std::optional<uint64_t>{};
    if (staticStateKey) {
        auto staticHit = m_hold.staticHit(*staticStateKey, tick,
                                          visualLayerClipIds(layers));
        if (staticHit) {
            const uint64_t hits = ++m_staticCompositeHits;
            if (hits == 1 || hits % 300 == 0)
                spdlog::debug("[STATIC-COMPOSITE] reused {} frame(s); "
                              "tick={} layers={} {}x{}",
                              hits, tick, layers.size(), outW, outH);
            return staticHit;
        }
    } else if (!isNestedRecursion) {
        m_hold.clearStatic();
    }
    return std::nullopt;
}

CompositeService::FrameAnswer CompositeService::servePassthroughLayer(FrameState& r)
{
    const uint32_t outW = r.outW;
    const uint32_t outH = r.outH;
    const auto& policy = r.policy;
    auto& layers = r.layers;

    // Single layer fast path
    if (!policy.preferGpuOutput && layers.size() == 1) {
        const auto& L = layers[0];
        bool isIdentity = !L.isAdjustmentLayer &&
                          L.opacity >= 0.999f &&
                          std::abs(L.posX) < 0.5f && std::abs(L.posY) < 0.5f &&
                          std::abs(L.scX - 1.0f) < 0.001f &&
                           std::abs(L.scY - 1.0f) < 0.001f &&
                           std::abs(L.rot) < 0.01f &&
                           L.motionSampleCount == 1 &&
                           L.cropL < 0.01f && L.cropR < 0.01f &&
                          L.cropT < 0.01f && L.cropB < 0.01f &&
                          L.effects.empty() &&
                          L.temporalMode == 0 &&
                          !L.gpuTextureReady &&
                          // Everything else the compositor would apply: a
                          // single-clip fade/wipe in progress, opacity masks,
                          // a blend mode against the black canvas, the pivot,
                          // source rotation and alpha packing. (A fade from
                          // black on a lone full-frame clip used to export
                          // with no fade at all.)
                          L.wipeProgress < 0.0f &&
                          L.masks.empty() &&
                          L.blendMode == 0 &&
                          std::abs(L.anchorX) < 0.5f && std::abs(L.anchorY) < 0.5f &&
                          L.srcRotation == 0 &&
                          !L.isPacked && !L.isPMA && !L.needsSwapRB;
        if (isIdentity && L.frame && L.frame->width == outW && L.frame->height == outH)
            return L.frame;
    }
    return std::nullopt;
}

std::shared_ptr<CachedFrame> CompositeService::compositeOnGpu(FrameState& r)
{
    const int64_t tick = r.tick;
    const uint32_t outW = r.outW;
    const uint32_t outH = r.outH;
    const bool scrubMode = r.scrubMode;
    const bool isNestedRecursion = r.isNestedRecursion;
    const auto& context = r.context;
    const auto& policy = r.policy;
    bool& postInvalidate = r.postInvalidate;
    const bool perfLog = r.perfLog;
    const auto perfT0 = r.perfT0;
    auto& perfTlayers = r.perfTlayers;
    const CompositeCacheKey& compositeCacheKey = r.cacheKey;
    auto& layers = r.layers;
    int& clipsAtTick = r.clipsAtTick;
    int& resolvedClipsAtTick = r.resolvedClipsAtTick;
    const auto& staticStateKey = r.staticStateKey;

    // ── PERF: layers collected ────────────────────────────────────────
    perfTlayers = std::chrono::high_resolution_clock::now();

    // GPU compositing path
    {
        int effectLayerCount = 0, effectPassCount = 0, transitionCount = 0;
        auto gpuResult = tryCompositeOnGpu(layers, outW, outH, tick, scrubMode,
                                            perfLog, perfT0, perfTlayers,
                                            effectLayerCount, effectPassCount,
                                            transitionCount, context,
                                            compositeCacheKey,
                                            isNestedRecursion);
        if (gpuResult) {
            // Skip the cache write entirely when this call is a recursive
            // descent into a nested SequenceClip's inner timeline. The
            // inner result must NOT replace the outer composite's last
            // good frame -- the presenter thread reads it concurrently,
            // and a swap mid-frame causes the visible "every other frame
            // is the nested sequence's first frame" glitch during
            // playback / scrub.
            // An NV12 export frame has no BGRA pixels, so it can't stand
            // in as the last good composite for any other consumer; the
            // static-composite reuse below is keyed by format.
            if (!isNestedRecursion && gpuResult->hasNv12()) {
                m_hold.rememberStaticOnly(gpuResult, staticStateKey);
            } else if (!isNestedRecursion) {
                // The clip ids let the shot-boundary checks skip the stale
                // fallback when the active clips have entirely changed.
                // A complete composite re-arms the settle window.
                const bool wasStale = m_hold.rememberFresh(
                    gpuResult, tick, visualLayerClipIds(layers), staticStateKey,
                    clipsAtTick == 0 || resolvedClipsAtTick >= clipsAtTick);
                // Measured-cost render bar: remember only clock-driven live
                // playback. Paused exact frames, scrub, export, and pre-render
                // have different quality/readback contracts and otherwise make
                // ordinary spans appear falsely over budget.
                if (policy.measureRealtimeCost) {
                    const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::high_resolution_clock::now() - perfT0).count();
                    recordFrameCost(tick, static_cast<float>(ms));
                }
                if (postInvalidate) {
                    // debug-level: fires for every composite after any
                    // invalidate (edits, scrub settle) — was the single
                    // biggest source of perf_log spam.
                    spdlog::debug("[LIVE-RELOAD] compositeFrame tick={}: stored "
                                  "FRESH composite (gpuView=0x{:X}, {}x{}, "
                                  "sameAsPrev={})",
                                  tick, gpuResult ? gpuResult->gpuImageView : 0,
                                  gpuResult ? gpuResult->width : 0,
                                  gpuResult ? gpuResult->height : 0, wasStale);
                }
            }
            return gpuResult;
        }
    }
    return nullptr;
}

} // namespace rt
