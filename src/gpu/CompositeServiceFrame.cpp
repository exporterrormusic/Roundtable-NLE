/*
 * CompositeServiceFrame.cpp - Frame compositing orchestrator.
 * Extracted from CompositeService.cpp; prewarm methods moved to
 * CompositeServiceFramePrewarm.cpp (Step 3.1 of modularization plan).
 *
 * Contains:
 *   - compositeFrame()   — main compositing entry point
 *   - renderTitleClip()  — TitleClip rendering wrapper
 *   - renderGraphicClip() — GraphicClip rendering wrapper
 */

#include "CompositeService.h"
#include "ClipRenderers.h"
#include "CompositeServiceBlend.h"
#include "CompositeServiceLayerBuild.h"

// Media / timeline
#include "cache/FrameCache.h"
#include "cache/SegmentRenderCache.h"
#include "playback/MediaPool.h"
#include "decode/VideoFrameMapping.h"
#include "cache/CachePolicy.h"
#include "project/RenderStateHash.h"
#include "Constants.h"
#include "timeline/AudioClip.h"
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

std::shared_ptr<CachedFrame> CompositeService::compositeFrame(int64_t tick, uint32_t outW, uint32_t outH,
                                                                bool scrubMode,
                                                                bool isNestedRecursion)
try
{
    if (m_shutdown.load(std::memory_order_acquire))
        return nullptr;
    if (!m_timeline) return nullptr;

    // Suppress GPU compositing when a modal dialog is active (QDialog::exec
    // nested event loop).  Paint events still fire for widgets behind the
    // dialog, and invoking the Vulkan compositor from a paint event cascade
    // triggers deep driver call stacks that overflow the NVIDIA driver stack
    // (STACK_OVERFLOW in nvoglv64.dll).  Return the last good cached frame
    // instead, keeping the display valid without touching the GPU.
    if (s_modalDialogActive.load(std::memory_order_acquire)) {
        // Nested recursion: don't fall back to OUTER's last-good frame
        // for an INNER layer (would render outer content inside itself).
        if (isNestedRecursion) return nullptr;
        std::lock_guard lg(m_lastCompositeMtx);
        if (m_lastGoodComposite) {
            m_lastGoodComposite->gpuReady     = false;
            m_lastGoodComposite->gpuImageView = 0;
            m_lastGoodComposite->gpuSampler   = 0;
            m_lastGoodComposite->gpuSemaphore = 0;
        }
        return m_lastGoodComposite;
    }

    // Re-entrancy guard — safety net against unexpected recursive calls.
    // FrameProducer is the sole compositor thread, so this should never
    // trigger in normal operation.  Kept as defense-in-depth against
    // signal re-entrancy or callback chains.
    //
    // A2: Allow exactly ONE level of legitimate recursion (nested
    // SequenceClip composition).  The buildLayersForFrame path swaps
    // m_timeline to the inner sequence and recursively calls
    // compositeFrame to render the inner timeline.  Without this
    // allowance, the previous depth>0 → return-lastGood logic caused
    // nested sequences to silently show stale frames.  Two levels of
    // nesting is enough for almost any real project; deeper recursion
    // is treated as runaway re-entry and short-circuits.
    auto& depth = compositeDepth();
    constexpr int kMaxCompositeDepth = 2;
    if (depth >= kMaxCompositeDepth) {
        if (isNestedRecursion) return nullptr;
        std::lock_guard lg(m_lastCompositeMtx);
        return m_lastGoodComposite;
    }
    ++depth;
    struct DepthGuard {
        ~DepthGuard() { --compositeDepth(); }
    } depthGuard;

    // P2: CPU safe-mode fallback was deleted here.  When GPU compositing
    // fails persistently, GpuContext::tryRecover() transitions to Failed
    // and fires the fatal-failure callback (modal restart dialog).  No
    // per-frame "safe-mode" compositor — every major NLE treats device-
    // lost as fatal; we do the same.

    // Blocking lock — FrameProducer is now the sole compositor thread.
    // The old try_to_lock pattern (which returned stale m_lastGoodComposite
    // on contention) is removed.  compositeFrame() now ALWAYS produces the
    // correct frame for the requested tick.  Never returns stale frames.
    // Uses unique_lock (not lock_guard) because buildLayersForFrame()
    // temporarily unlocks to allow async media opens.
    std::unique_lock lock(m_compositeMutex);

    // Phase B: advance the CachePolicy generation counter and run its
    // throttled eviction + adaptive-budget passes.  These are very cheap
    // — eviction is gated by a 3s throttle, rebalance is gated by a
    // 30-frame throttle — but having the call site here ensures every
    // composite tick contributes to its LRU view.
    if (m_cachePolicy) {
        m_cachePolicy->onFrameStart();
        m_cachePolicy->runEvictionPass();
        m_cachePolicy->rebalanceBudgets();
    }

    // Check deferred cache invalidation request (set by requestCacheInvalidation
    // from the UI thread via atomic flag, avoiding deadlock).
    //
    // Skip during nested-sequence recursion: the OUTER frame already
    // consumed the flag at its own entry and reset its own state. If the
    // inner re-consumes the flag (it could read it while still set from a
    // late UI invalidation), inner would reset the outer's m_lastGoodComposite
    // and wipe the outer's LRU mid-frame -- producing the "every other frame
    // glitches to a stale frame" symptom on nested sequences.
    bool postInvalidate = false;
    if (!isNestedRecursion
            && m_cacheInvalidateRequested.exchange(false, std::memory_order_acquire)) {
        postInvalidate = true;
        spdlog::debug("[LIVE-RELOAD] compositeFrame tick={}: invalidate flag "
                      "consumed — clearLru + reset lastGoodComposite", tick);
        if (m_engine)
            m_engine->clearLru();
        {
            std::lock_guard lg(m_lastCompositeMtx);
            m_lastGoodComposite.reset();
            m_lastGoodCompositeTick = -1;
            // Match invalidateCacheDirect(): re-arm the A1 settle window
            // so post-invalidation views get the same first-view grace.
            m_lastFullCompositeAt = std::chrono::steady_clock::time_point{};
            m_lastFullLayerCount  = 0;
        }
    }

    // ── §4.6 2c-read: serve a pre-rendered ("green") segment from cache ──
    // After invalidation handling (so a just-consumed edit can't serve stale)
    // and only for a top-level composite whose effective tier + output size
    // match the cached render.  configHash gates correctness: any edit changes
    // the hash, so a stale entry silently misses.  The cached frame is a CPU
    // snapshot (gpuReady=false) — a presentation path the modal fallback above
    // already exercises.  Default ON now that it's verified; the count()==0
    // fast-out makes it free (no per-frame hashing) until something is cached.
    // The consult also fires during export (forceFullResolution): export reuses
    // pre-rendered FULL-tier segments, and effectiveCacheTier() keeps export
    // (Full) and reduced playback (Half/…) keyed apart.
    if ((m_segmentCacheReadEnabled.load(std::memory_order_relaxed)
             || m_forceFullResolution.load(std::memory_order_relaxed))
            && !isNestedRecursion && m_segmentRenderCache
            && m_segmentRenderCache->count() > 0) {
        const ResolutionTier tier = effectiveCacheTier();
        const uint64_t cfgHash = hashCompositeConfigAt(*m_timeline, tick, m_project);
        auto cached = m_segmentRenderCache->get(tick, tier, cfgHash);
        if (!cached) {
            // The (tick,tier,hash) key isn't present — the common, expected
            // case for any not-yet-rendered frame.  Debug-only tally.
            const uint64_t n = ++m_segCacheKeyMiss;
            if (n == 1 || (n % 600) == 0)
                spdlog::debug("[SEGCACHE] miss #{}: tick={} tier={} cfgHash={:#x}",
                              n, tick, static_cast<int>(tier), cfgHash);
        }
        if (cached) {
            if (cached->width == outW && cached->height == outH
                    && !cached->pixels.empty()) {
                // Keep the last-good fallback coherent with what we present.
                {
                    std::lock_guard lg(m_lastCompositeMtx);
                    m_lastGoodComposite     = cached;
                    m_lastGoodCompositeTick = tick;
                }
                const uint64_t n = ++m_segCacheHits;
                if (n == 1 || (n % 300) == 0)
                    spdlog::debug("[SEGCACHE] served {} frame(s) from cache "
                                  "(tick={} {}x{} tier={})",
                                  n, tick, outW, outH, static_cast<int>(tier));
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

    // All composites use non-blocking tryGetFrame — the old blocking inline
    // NVDEC decode (which held m_compositeMutex for 50-500ms, freezing the
    // UI) was migrated to async prefetch workers.  The ProgramMonitor settle
    // mechanism retries until the frame arrives (~30-100ms).
    // The blocking lock on m_compositeMutex (instead of old try_to_lock)
    // ensures compositeFrame() ALWAYS produces the correct frame.
    bool playbackNonBlocking = true;
    // Only one GPU Spine render per compositeFrame — the shared FBO gets
    // cleared by beginFrame(), destroying previous renders.
    bool gpuSpineUsedThisFrame = false;

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
    if (!scrubMode || m_forceFullResolution.load()) {
        prewarmUpcomingShots(tick);
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
    if (!scrubMode || m_forceFullResolution.load()) {
        std::unordered_set<uint64_t> currentClipIds;
        for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
            auto* track = m_timeline->track(ti - 1);
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
            for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
                auto* track = m_timeline->track(ti - 1);
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
                            ? (m_forceFullResolution.load()
                               ? ResolutionTier::Full
                               : playbackTier())
                            : playbackTier();
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

    if (m_mediaPool) {
        m_mediaPool->scheduler().setPlayhead(tick);
    }

    auto shotBoundaryT1 = std::chrono::high_resolution_clock::now();

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

    auto fetchMediaFrame = [&](MediaHandle handle, int64_t frameNumber,
                               ResolutionTier tier) -> std::shared_ptr<CachedFrame> {
        if (!m_mediaPool)
            return nullptr;
        return m_mediaPool->tryGetFrame(handle, frameNumber, tier);
    };

    // ── ONE-TIME STARTUP DIAGNOSTIC ──────────────────────────────────
    {
        static bool s_startupLogged = false;
        if (!s_startupLogged) {
            s_startupLogged = true;
            auto& gpu = GpuContext::get();
            spdlog::info("========== COMPOSITE PIPELINE CONFIG ==========");
            spdlog::info("  gpuDisplayMode = {}", m_gpuDisplayMode);
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

    // ── PERF TIMING ─────────────────────────────────────────────────
    static int s_perfCounter = 0;
    const bool perfLog = (++s_perfCounter % 10 == 0);
    auto perfT0 = std::chrono::high_resolution_clock::now();
    auto perfTlayers = perfT0;
    auto perfTgpuUp  = perfT0;
    auto perfTcomp   = perfT0;

    // Composite result LRU cache.
    //
    // Skip entirely during nested-sequence recursion.  The LRU is keyed
    // only by (tick, w, h) and is SHARED between the outer program
    // composite and the inner nested-sequence composite (same engine,
    // m_timeline swapped).  The inner composite runs in forced CPU mode
    // and writes its result (the inner timeline WITHOUT the SequenceClip
    // transform) into this same LRU.  When innerTick collides with the
    // outer tick, the outer's checkLru would return that inner-only frame
    // as the whole program output -- the "nested sequence flickers its
    // untransformed position every other frame" bug.  The inner must
    // always freshly composite its own content (it is snapshotted to a
    // CPU layer by the caller anyway).
    //
    // Scrub: ALLOW.  The LRU only stores CPU-readback frames (insertLru
    // is gated on !result->gpuReady), and they are keyed by (tick, w, h)
    // so there is no staleness risk.  Letting scrub hit the LRU means an
    // oscillating drag around the same tick at the same divisor reuses
    // the prior composite instead of redoing the full layer build.
    if (m_engine && !isNestedRecursion) {
        auto cached = m_engine->checkLru(tick, outW, outH);
        if (cached)
            return cached;
    }

    if (m_engine) {
        m_engine->flushLruOnResize(outW, outH);
    }

    if (outW < 64) outW = 64;
    if (outH < 36) outH = 36;

    std::vector<LayerInfo> layers;

    int clipsAtTick = 0;
    layers = buildLayersForFrame(tick, outW, outH, scrubMode, playbackNonBlocking,
                                 clipsAtTick, perfLog, lock, gpuSpineUsedThisFrame);

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

        const auto wantTier = m_forceFullResolution.load()
            ? ResolutionTier::Full
            : playbackTier();

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

                    MediaHandle handle = 0;
                    {
                        auto it = m_openMediaHandles.find(mp);
                        if (it != m_openMediaHandles.end())
                            handle = it->second;
                    }
                    if (handle == 0) continue;

                    const auto* info = m_mediaPool->getInfo(handle);
                    if (!info) continue;

                    // Shared mapping authority (VideoFrameMapping.h).  This
                    // collector previously resolved fps in a DIFFERENT
                    // priority order than the renderer (media fps first
                    // instead of clip fps first), so it could prefetch a
                    // different frame than the renderer would request —
                    // wasted prefetch + a cache miss on the real frame.
                    const int64_t frameNum =
                        mapTickToSourceFrame(*vc, atTick, info).frame;

                    if (!m_mediaPool->isFrameCached(handle, frameNum, wantTier))
                        pending.push_back({handle, frameNum, wantTier});
                }
            }
        };

        // Outer timeline.
        collectPending(m_timeline, tick);

        // Inner timelines reachable through one level of SequenceClip
        // (matches the depth=2 cap on compositeFrame recursion).  Without
        // this the nested sequence's first frame on a fresh shot shows
        // black instead of waiting up to 16 ms for inner decoders.
        if (m_project) {
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                auto* trk = m_timeline->track(ti);
                if (!trk || trk->type() != TrackType::Video || trk->isMuted())
                    continue;
                for (auto* c : trk->clipsAtTime(tick)) {
                    if (!c || !c->isEnabled()) continue;
                    auto* seqClip = dynamic_cast<SequenceClip*>(c);
                    if (!seqClip) continue;
                    if (seqClip->sequenceIndex() >= m_project->sequenceCount())
                        continue;
                    auto* innerTl = m_project->sequence(seqClip->sequenceIndex());
                    if (!innerTl || innerTl == m_timeline) continue;
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
                    clipsAtTick, perfLog, lock, gpuSpineUsedThisFrame);
                spdlog::info("[BLOCK-ON-ARRIVAL] tick={}: retry yielded "
                             "{}/{} layers", tick, layers.size(), clipsAtTick);
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

    // ── Nested-sequence recursion isolation ─────────────────────────────
    // When this call is an inner SequenceClip composite and it cannot
    // fully resolve its own layers this tick, return nullptr instead of
    // falling through to the settle-hold / empty-sentinel paths below.
    // Those paths return the OUTER's m_lastGoodComposite (outer program
    // content) or an empty width=0 sentinel; either one used as the
    // inner-sequence layer paints the program inside itself / blanks the
    // clip, flickering against correctly-composited frames.  Skipping the
    // inner layer for this tick (it retries next tick) is the documented
    // correct behavior — see the isNestedRecursion nullptr fallback after
    // tryCompositeOnGpu below.
    if (isNestedRecursion &&
        ((clipsAtTick > 0 && static_cast<int>(layers.size()) < clipsAtTick) ||
         layers.empty())) {
        return nullptr;
    }

    // ── A1: Settle-window fallback (Premiere-style hold) ─────────────────
    // If we resolved FEWER layers than the active clip count, the composite
    // would render with missing layers — the visible "layers build up over
    // several frames" symptom.  Hold the prior complete composite (or, on
    // first view when there's no prior, return an empty sentinel so the
    // viewport stays clear) for up to kSettleWindowMs ms.  After that
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
        clipsAtTick > 0 && static_cast<int>(layers.size()) < clipsAtTick)
    {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        std::shared_ptr<CachedFrame> hold;
        bool withinSettle = false;
        bool firstView = false;
        {
            std::lock_guard lg(m_lastCompositeMtx);
            // First-view init: arm the settle clock on the first observation
            // of an incomplete composite.  Without this, the very first
            // composite at startup (or after cache invalidation) finds
            // m_lastFullCompositeAt at its default (epoch 0), skips the
            // settle entirely, and the user sees layers pop in one by one
            // while the cold NVDEC decoders open.
            if (m_lastFullCompositeAt.time_since_epoch().count() == 0) {
                m_lastFullCompositeAt = now;
                firstView = true;
            }
            const auto elapsed = duration_cast<milliseconds>(
                now - m_lastFullCompositeAt).count();
            if (elapsed < kSettleWindowMs) {
                // Shot-boundary guard: don't hold the prior shot when the
                // active clips have entirely changed.  Without this the
                // previous shot lingers for 1 frame after its clips end.
                bool sameShot = true;
                if (m_lastGoodComposite && !m_lastGoodCompositeClipIds.empty()) {
                    sameShot = false;
                    for (size_t ti = 0; ti < m_timeline->trackCount() && !sameShot; ++ti) {
                        auto* trk = m_timeline->track(ti);
                        if (!trk || trk->type() != TrackType::Video || trk->isMuted())
                            continue;
                        for (auto* c : trk->clipsAtTime(tick)) {
                            if (!c) continue;
                            uint64_t cid = c->id();
                            for (uint64_t prev : m_lastGoodCompositeClipIds) {
                                if (cid == prev) { sameShot = true; break; }
                            }
                            if (sameShot) break;
                        }
                    }
                }
                if (sameShot) {
                    hold = m_lastGoodComposite;   // may be null on first view
                    withinSettle = true;
                }
            }
        }
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
            // By kSettleWindowMs the prefetch workers should have decoded
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

        if (clipsAtTick > 0) {
            // Shot-boundary guard: collect the clip IDs that would be
            // active at this tick.  The stale fallback below is meant to
            // mask mid-shot decoder warm-up, NOT to leak the previous
            // shot's frame past its clip end.  If NONE of the currently
            // active clips appear in m_lastGoodCompositeClipIds, we just
            // crossed a shot boundary — return an empty sentinel instead
            // of the prior shot's composite (which would cause "Wells
            // visible for 1 frame after her clip ends" et al.).
            std::vector<uint64_t> currentClipIds;
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                auto* trk = m_timeline->track(ti);
                if (!trk || trk->type() != TrackType::Video || trk->isMuted())
                    continue;
                for (auto* c : trk->clipsAtTime(tick)) {
                    if (c) currentClipIds.push_back(c->id());
                }
            }

            std::lock_guard lg(m_lastCompositeMtx);
            if (m_lastGoodComposite && m_lastGoodCompositeTick >= 0) {
                bool anyOverlap = false;
                for (uint64_t cur : currentClipIds) {
                    for (uint64_t prev : m_lastGoodCompositeClipIds) {
                        if (cur == prev) { anyOverlap = true; break; }
                    }
                    if (anyOverlap) break;
                }
                if (!anyOverlap) {
                    // Different shot — don't reuse prior composite. Drop
                    // it so subsequent ticks at this position don't keep
                    // returning the old shot either.
                    m_lastGoodComposite.reset();
                    m_lastGoodCompositeTick = -1;
                    m_lastGoodCompositeClipIds.clear();
                    return std::make_shared<CachedFrame>();
                }

                // Allow at most 2 stale frames so the viewport doesn't
                // freeze on the previous shot for multiple frame durations
                // while the new shot's first frame decodes.  At 24fps each
                // frame is 2000 ticks (48000/24); at 60fps it's 800 ticks.
                // Use 2000 ticks which is 1 frame at 24fps or ~2.5 frames
                // at 60fps — brief enough to avoid visible freeze.
                constexpr int64_t kMaxStaleTicks = 48000 / 24;
                int64_t tickDelta = std::abs(tick - m_lastGoodCompositeTick);
                if (tickDelta <= kMaxStaleTicks) {
                    if (postInvalidate) {
                        // debug-level: routine stale-fallback during settle,
                        // not an anomaly (see LIVE-RELOAD FRESH note below).
                        spdlog::debug("[LIVE-RELOAD] compositeFrame tick={}: "
                                      "returning STALE m_lastGoodComposite "
                                      "(lastTick={}, clipsAtTick={}, delta={})",
                                      tick, m_lastGoodCompositeTick,
                                      clipsAtTick, tickDelta);
                    }
                    return m_lastGoodComposite;
                }
                spdlog::info("[COMPOSITE] Suppressing stale lastGoodComposite "
                             "(tick={} vs last={}, delta={})",
                             tick, m_lastGoodCompositeTick, tickDelta);
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
        {
            auto emptyFrame = std::make_shared<CachedFrame>();
            std::lock_guard lg(m_lastCompositeMtx);
            m_lastGoodComposite.reset();
            m_lastGoodCompositeTick = -1;
            // Re-arm the A1 settle window: the next composite at this
            // tick (after prefetches land) gets the first-view grace
            // instead of accepting another partial right away.
            m_lastFullCompositeAt = std::chrono::steady_clock::time_point{};
            m_lastFullLayerCount  = 0;
            return emptyFrame;
        }
    }

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


    // Single layer fast path
    if (!m_gpuDisplayMode && layers.size() == 1) {
        const auto& L = layers[0];
        bool isIdentity = L.opacity >= 0.999f &&
                          std::abs(L.posX) < 0.5f && std::abs(L.posY) < 0.5f &&
                          std::abs(L.scX - 1.0f) < 0.001f &&
                          std::abs(L.scY - 1.0f) < 0.001f &&
                          std::abs(L.rot) < 0.01f &&
                          L.cropL < 0.01f && L.cropR < 0.01f &&
                          L.cropT < 0.01f && L.cropB < 0.01f &&
                          L.effects.empty() &&
                          !L.gpuTextureReady;
        if (isIdentity && L.frame && L.frame->width == outW && L.frame->height == outH)
            return L.frame;
    }

    // ── PERF: layers collected ────────────────────────────────────────
    perfTlayers = std::chrono::high_resolution_clock::now();

    // GPU compositing path
    {
        int effectLayerCount = 0, effectPassCount = 0, transitionCount = 0;
        auto gpuResult = tryCompositeOnGpu(layers, outW, outH, tick, scrubMode,
                                            perfLog, perfT0, perfTlayers,
                                            effectLayerCount, effectPassCount,
                                            transitionCount, isNestedRecursion);
        if (gpuResult) {
            // Skip the cache write entirely when this call is a recursive
            // descent into a nested SequenceClip's inner timeline. The
            // inner result must NOT replace the outer composite's
            // m_lastGoodComposite -- the presenter thread reads that
            // field concurrently, and a swap mid-frame causes the
            // visible "every other frame is the nested sequence's first
            // frame" glitch during playback / scrub.
            if (!isNestedRecursion) {
                std::lock_guard lg(m_lastCompositeMtx);
                const bool wasStale = (m_lastGoodComposite == gpuResult);
                m_lastGoodComposite     = gpuResult;
                m_lastGoodCompositeTick = tick;
                // Measured-cost render bar: remember how long this FRESH
                // real-time composite took (perfT0 = start of composite work).
                // Skip scrub/render passes — those aren't representative of the
                // playback budget and use a different tick grid.
                if (!scrubMode) {
                    const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::high_resolution_clock::now() - perfT0).count();
                    recordFrameCost(tick, static_cast<float>(ms));
                }
                // Record which clips produced this composite so the
                // shot-boundary check below can skip the stale fallback
                // when active clips have entirely changed.
                m_lastGoodCompositeClipIds.clear();
                m_lastGoodCompositeClipIds.reserve(layers.size());
                for (const auto& L : layers) {
                    if (L.clipId != 0)
                        m_lastGoodCompositeClipIds.push_back(L.clipId);
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
                if (clipsAtTick == 0 ||
                    static_cast<int>(layers.size()) >= clipsAtTick)
                {
                    m_lastFullCompositeAt = std::chrono::steady_clock::now();
                    m_lastFullLayerCount  = static_cast<int>(layers.size());
                }
            }
            return gpuResult;
        }
    }
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
    // Nested recursion: never fall back to the outer composite's
    // m_lastGoodComposite -- that frame represents OUTER content. Using
    // it as the inner-sequence layer paints the outer's last frame
    // INSIDE itself, which is the visual feedback loop the user sees.
    // Returning nullptr cleanly skips the inner layer for this frame;
    // the next composite tick will retry.
    if (isNestedRecursion)
        return nullptr;
    {
        std::lock_guard lg(m_lastCompositeMtx);
        if (m_lastGoodComposite) {
            m_lastGoodComposite->gpuReady     = false;
            m_lastGoodComposite->gpuImageView = 0;
            m_lastGoodComposite->gpuSampler   = 0;
            m_lastGoodComposite->gpuSemaphore = 0;
        }
        return m_lastGoodComposite;
    }
}
catch (const std::exception& ex)
{
    spdlog::error("compositeFrame: exception: {}", ex.what());
    if (isNestedRecursion) return nullptr;
    std::lock_guard lg(m_lastCompositeMtx);
    return m_lastGoodComposite;
}
catch (...)
{
    spdlog::error("compositeFrame: unknown exception");
    if (isNestedRecursion) return nullptr;
    std::lock_guard lg(m_lastCompositeMtx);
    return m_lastGoodComposite;
}


std::shared_ptr<CachedFrame> CompositeService::renderTitleClip(
    TitleClip* clip, int64_t tick, uint32_t outW, uint32_t outH)
{
    return rt::renderTitleClip(clip, tick, outW, outH);
}

std::shared_ptr<CachedFrame> CompositeService::renderGraphicClip(
    GraphicClip* clip, int64_t tick, uint32_t outW, uint32_t outH)
{
    uint32_t refW = 0, refH = 0;
    if (m_project) {
        refW = m_project->settings().resolution().width;
        refH = m_project->settings().resolution().height;
    }
    return rt::renderGraphicClip(clip, tick, outW, outH, refW, refH);
}

namespace {
// Clean CPU-only snapshot of a composite output frame.  compositeFrame returns
// the SHARED m_lastGoodComposite which the next tick reuses, so the cache must
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
    const uint64_t cfgHash = hashCompositeConfigAt(*m_timeline, tick, m_project);
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
    const uint64_t cfgHash = hashCompositeConfigAt(*m_timeline, tick, m_project);
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
    const std::function<void(int, int)>& onProgress)
{
    if (!m_timeline || !m_segmentRenderCache || outW == 0 || outH == 0)
        return 0;
    if (toTick < fromTick) std::swap(fromTick, toTick);

    // Ticks are 48kHz sample-accurate; the frame grid comes from the
    // sequence fps.  Render exactly the frame-aligned ticks the playback
    // clock would land on (see RenderQueue's frame->tick mapping), so the
    // 2c-read consult later keys identically.
    double fps = m_timeline->settings().frameRate();
    if (!(fps > 0.0)) fps = 30.0;
    const double ticksPerFrame = static_cast<double>(kTicksPerSecond) / fps;

    // Frame indices whose tick falls within [fromTick, toTick] (inclusive).
    const int64_t firstFrame = static_cast<int64_t>(
        std::ceil(static_cast<double>(fromTick) / ticksPerFrame));
    const int64_t lastFrame = static_cast<int64_t>(
        std::floor(static_cast<double>(toTick) / ticksPerFrame));
    if (lastFrame < firstFrame) return 0;

    const ResolutionTier tier  = effectiveCacheTier();
    const int            total = static_cast<int>(lastFrame - firstFrame + 1);

    // Force CPU readback so the composited pixels are materialised before we
    // snapshot them — GPU display mode leaves CachedFrame::pixels empty (the
    // same convention as the thumbnail grab in ProjectControllerMisc).
    const bool wasGpuMode = m_gpuDisplayMode;
    m_gpuDisplayMode = false;

    int rendered  = 0;
    int processed = 0;
    for (int64_t f = firstFrame; f <= lastFrame; ++f) {
        if (shouldCancel && shouldCancel()) break;

        const int64_t tick = static_cast<int64_t>(
            std::llround(static_cast<double>(f) * ticksPerFrame));
        const uint64_t cfgHash = hashCompositeConfigAt(*m_timeline, tick, m_project);

        // Skip frames already cached under the current config (idempotent /
        // resumable — re-running over an unchanged range is nearly free).
        if (!m_segmentRenderCache->hasFresh(tick, tier, cfgHash)) {
            auto frame = compositeFrame(tick, outW, outH, /*scrubMode=*/true);
            if (frame && frame->ensurePixels() && !frame->pixels.empty()) {
                m_segmentRenderCache->put(tick, tier, cfgHash,
                                          snapshotForCache(*frame, tier));
                ++rendered;
            }
        }

        ++processed;
        if (onProgress) onProgress(processed, total);
    }

    m_gpuDisplayMode = wasGpuMode;

    spdlog::debug("renderRangeToCache: [{}, {}] tier={} -> {} of {} frames "
                  "rendered ({} MB cached)",
                  fromTick, toTick, static_cast<int>(tier), rendered, total,
                  m_segmentRenderCache->sizeBytes() / (1024 * 1024));
    return rendered;
}


} // namespace rt
