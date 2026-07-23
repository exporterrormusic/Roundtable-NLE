/*
 * MediaPoolFrame.cpp — Frame access (getFrame, tryGetFrame) for MediaPool.
 * Extracted from MediaPool.cpp for maintainability.
 */

#include "MediaPool.h"
#include "playback/FrameFallbackPolicy.h"

#include <cstring>
#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

namespace {

constexpr int64_t kInteractivePlaybackDeferMs = 2500;

int64_t steadyClockMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void extendInteractivePlaybackWindow(std::atomic<int64_t>& untilMs)
{
    const int64_t candidate = steadyClockMillis() + kInteractivePlaybackDeferMs;
    int64_t current = untilMs.load(std::memory_order_relaxed);
    while (current < candidate &&
           !untilMs.compare_exchange_weak(current, candidate,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
    }
}

} // namespace

#include "GpuContext.h"
#include "Nv12Converter.h"
#include "cuda/CudaVulkanInterop.h"

#include <volk.h>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace rt {

// ─── Scrub decoder management ───────────────────────────────────────────────

PrefetchDecoderState& MediaPool::getScrubDecoder(
    MediaHandle handle, const std::filesystem::path& path, const VideoStreamInfo& /*info*/)
{
    auto& state = m_scrubDecoders[handle];
    if (!state.decoder) {
        state.decoder = std::make_unique<VideoDecoder>();
        // forceSoftware=false (use NVDEC if available): on H.264 NVDEC decodes
        // ~5–10× faster per frame than 2-thread software, which is what cold
        // scrubbing actually pays per position (keyframe seek + GOP forward).
        // The trade is one 500–650 ms NVDEC init the first time this clip is
        // scrubbed; every scrub after that is dramatically faster, instead of
        // every scrub paying SW GOP cost forever. VideoDecoder::open falls
        // back to software automatically if NVDEC/D3D11VA/QSV are all unavailable.
        // sliceOnlyThreading=true is retained for the SW fallback path —
        // FF_THREAD_FRAME causes H.264 reference frame corruption after
        // avcodec_flush_buffers, producing distorted multi-copy artifacts.
        if (!state.decoder->open(path, /*forceSoftware=*/false, /*maxThreads=*/2, /*sliceOnlyThreading=*/true)) {
            spdlog::warn("MediaPool: scrub decoder open failed for handle={}", handle);
            state.decoder.reset();
        }
        state.lastDecodedFrame = -1;
    }
    return state;
}

// ─── getFrame (blocking + scrub path) ───────────────────────────────────────

std::shared_ptr<CachedFrame> MediaPool::getFrame(
    MediaHandle handle, int64_t frameNumber, ResolutionTier tier, bool scrubMode,
    bool forceExact)
{
    auto perfGetT0 = std::chrono::high_resolution_clock::now();
    m_perf.totalRequests.fetch_add(1, std::memory_order_relaxed);

    // Periodic perf report (every ~2 seconds at 30fps = every 60 frames)
    {
        static std::atomic<int> s_reportCounter{0};
        if (++s_reportCounter % 60 == 0) {
            logPerfReport();
        }
    }

    if (!scrubMode)
        extendInteractivePlaybackWindow(m_interactivePlaybackUntilMs);

    // Defense-in-depth: clamp frame number to valid range.
    // Callers should do this, but a stale or miscalculated frame number
    // beyond the video's frame count causes a guaranteed decode failure
    // (grey/static output) and wastes 50-100ms on a futile seek.
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (entry && entry->info.frameCount > 1 && frameNumber >= entry->info.frameCount) {
            frameNumber = entry->info.frameCount - 1;
        }
    }
    if (frameNumber < 0) frameNumber = 0;

    // ── Seek-discontinuity detection ─────────────────────────────────────
    // Keep last-good only for a short sequential-playback miss.  A distant
    // forward seek is just as discontinuous as a backward seek: returning a
    // frame from the old position lets the compositor cache old pixels under
    // the new timeline tick.
    {
        std::lock_guard lg(m_lastGoodMtx);
        auto it = m_lastGoodFrame.find(handle);
        if (it != m_lastGoodFrame.end() && it->second) {
            int64_t delta = frameNumber - it->second->frameNumber;
            if (delta < -1 ||
                !isNearbyPlaybackFrame(frameNumber, it->second->frameNumber)) {
                m_lastGoodFrame.erase(it);
            }
        }
    }

    // Tell the cache where playback is so eviction protects future frames
    if (!scrubMode)
        m_cache->setPlayhead(handle, frameNumber);

    // First check cache (no decoder lock needed).
    // forceExact (export) MUST bypass the shared playback cache: it can hold
    // frames filled approximately by prefetch workers (their seek path matches
    // frames by timestamp) or produced under NVDEC contention, which surface
    // as the "random 1-frame old/frozen" glitch in exports.  Export decodes
    // the exact frame in isolation below instead.
    std::shared_ptr<CachedFrame> cached;
    if (!forceExact)
        cached = m_cache->get(handle, frameNumber, tier);
    if (cached) {
        m_perf.cacheHits.fetch_add(1, std::memory_order_relaxed);
        static int s_hitLog = 0;
        if (++s_hitLog % 120 == 0) {
            auto stats = m_cache->stats();
            spdlog::info("[PERF] FrameCache stats: {} entries, {:.0f}MB used / {:.0f}MB cap, hit={:.1f}% "
                         "(handle={} frame={} tier={})",
                         stats.frameCount,
                         stats.memoryUsed / 1048576.0,
                         stats.memoryCapacity / 1048576.0,
                         stats.hitRate() * 100.0,
                         handle, frameNumber, static_cast<int>(tier));
        }
        // Update last-good-frame for playback stale-frame fallback
        if (!scrubMode) {
            std::lock_guard lg(m_lastGoodMtx);
            m_lastGoodFrame[handle] = cached;
        }
        return cached;
    }

    // ── Disk cache check (second-level persistent cache) ─────────────
    // ~2-3 ms on NVMe SSD vs 10-100 ms re-decode.  Skipped for forceExact
    // (export) — same isolation rationale as the RAM cache above.
    if (!forceExact && m_diskCache) {
        cached = m_diskCache->get(handle, frameNumber, tier);
        if (cached) {
            m_cache->put(cached); // Promote back to RAM
            m_perf.cacheHits.fetch_add(1, std::memory_order_relaxed);
            if (!scrubMode) {
                std::lock_guard lg(m_lastGoodMtx);
                m_lastGoodFrame[handle] = cached;
            }
            return cached;
        }
    }


    // ── Frame-drop fallback (scrub AND playback) ────────────────────────
    // If the exact frame isn't cached, accept the nearest cached frame
    // within a small window.  During playback this is "frame dropping" —
    // the display shows a slightly stale frame while the prefetch thread
    // catches up.  This keeps the UI thread unblocked and is exactly what
    // Premiere Pro does for smooth playback.
    //
    // Search radius: ±4 for scrub, ±5 for playback (wider to absorb
    // prefetch lag and keep the compositor moving without blocking).
    // Also try the other resolution tier since the
    // prefetch thread may have filled a different tier.
    //
    // Export (forceExact) must NEVER accept a neighbor — returning the
    // previous frame for a missed exact frame is precisely the
    // duplication/stutter bug.  Skip straight to the blocking, frame-
    // accurate inline decode below.
    if (!forceExact) {
        const int64_t searchRadius = scrubMode ? 15 : 5;
        for (int64_t delta = 1; delta <= searchRadius; ++delta) {
            cached = m_cache->getNoPromote(handle, frameNumber - delta, tier);
            if (cached) {
                m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
                // Piggyback prefetch so the target frame is decoded soon
                schedulePrefetch(handle, frameNumber, perfProfile().prefetchAheadFrames, /*urgent=*/true, tier);
                if (!scrubMode) {
                    std::lock_guard lg(m_lastGoodMtx);
                    m_lastGoodFrame[handle] = cached;
                }
                return cached;
            }
            cached = m_cache->getNoPromote(handle, frameNumber + delta, tier);
            if (cached) {
                m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
                if (!scrubMode) {
                    std::lock_guard lg(m_lastGoodMtx);
                    m_lastGoodFrame[handle] = cached;
                }
                return cached;
            }
        }
        // Also try the opposite resolution tier as last resort.
        // Only accept a higher-or-equal tier — downgrading (Full→Half)
        // produces blurry output when composited at the full viewport
        // resolution (export path).  Upgrading (Half→Full) is always
        // safe: the frame is sharper than requested.
        ResolutionTier altTier;
        if (tier == ResolutionTier::Half) {
            altTier = ResolutionTier::Full;
        } else {
            // tier is Full or Quarter — don't downgrade to Half/Quarter
            // respectively.  Fall through to the blocking decode path
            // instead of returning a blurry fallback.
            altTier = tier;  // no downgrade
        }
        if (altTier != tier) {
            cached = m_cache->getNoPromote(handle, frameNumber, altTier);
            if (cached) {
                m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
                // Schedule prefetch for the REQUESTED tier so the next settle
                // cycle gets the correct resolution instead of the fallback.
                // Without this, the caller (resolveMediaFrame) may accept the
                // wrong-tier frame and composite it at full viewport → blurry.
                schedulePrefetch(handle, frameNumber, 1, /*urgent=*/true, tier);
                if (!scrubMode) {
                    std::lock_guard lg(m_lastGoodMtx);
                    m_lastGoodFrame[handle] = cached;
                }
                return cached;
            }
        }
        for (int64_t delta = 1; delta <= 2; ++delta) {
            cached = m_cache->getNoPromote(handle, frameNumber - delta, altTier);
            if (cached) {
                m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
                if (!scrubMode) {
                    std::lock_guard lg(m_lastGoodMtx);
                    m_lastGoodFrame[handle] = cached;
                }
                return cached;
            }
        }
    }

    // ── Playback cache-miss path ─────────────────────────────────────────
    // Schedule urgent prefetch so background workers decode this frame and
    // upcoming ones.  During playback, we NEVER block the render thread
    // with inline decode — matching Premiere Pro's architecture where the
    // composition thread never calls into the decoder.
    //
    // Changed: Previously fell through to inline decode for first-access
    // (no stale frame).  Now returns nullptr unconditionally during playback
    // and lets the compositor show the last good composite or a black layer
    // for 1-2 frames while prefetch workers fill the cache.  This eliminates
    // the 30-300ms stalls that caused visible judder on long-GOP codecs.
    // forceExact (export) skips this non-blocking stale-return path and
    // falls through to the blocking exact decode below — even when called
    // with scrubMode=false — so the encoder always gets the real frame.
    if (!scrubMode && !forceExact) {
        // Schedule aggressive prefetch: current frame + ahead
        schedulePrefetch(handle, frameNumber, perfProfile().prefetchAheadFrames, /*urgent=*/true, tier);

        // Try to return last-good-frame (stale but visible)
        std::shared_ptr<CachedFrame> stale;
        {
            std::lock_guard lg(m_lastGoodMtx);
            auto it = m_lastGoodFrame.find(handle);
            if (it != m_lastGoodFrame.end())
                stale = it->second;
        }
        if (stale) {
            m_perf.staleReturns.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> s_staleLog{0};
            if (++s_staleLog % 30 == 0) {
                spdlog::info("[PERF] getFrame STALE: handle={} wanted={} returning last-good "
                             "(prefetch scheduled)", handle, frameNumber);
            }
            return stale;
        }

        // No stale frame (first-ever access for this handle during playback).
        // Return nullptr — the compositor will show the last good composite
        // or a transparent layer.  Prefetch workers will fill this frame
        // within 1-3 callbacks (~30-100ms).  This is strictly better than
        // inline decode which blocks for 30-300ms and causes visible judder.
        m_perf.totalMisses.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> s_firstAccessLog{0};
        if (++s_firstAccessLog <= 20 || s_firstAccessLog % 60 == 0) {
            spdlog::info("[PERF] getFrame: first access for handle={} frame={} "
                         "-> non-blocking miss (prefetch will fill)", handle, frameNumber);
        }
        return nullptr;
    }

    // ── Scrub cache-miss: schedule urgent prefetch + lock-free decode ────
    // Uses a dedicated scrub decoder (separate from the main entry decoder)
    // so we NEVER hold m_mutex during the decode.  This eliminates the
    // 60-150ms UI-thread stall that caused scrub jitter.
    //
    // forceExact (export) does NOT schedule prefetch: prefetch workers
    // contend with this exact decode for NVDEC sessions (the source of stale
    // surfaces) and would pollute the shared cache with frames export must
    // not read.  Export decodes purely sequentially via the scrub decoder.
    if (!forceExact)
        schedulePrefetch(handle, frameNumber, perfProfile().prefetchAheadFrames, /*urgent=*/true, tier);

    m_perf.inlineDecodes.fetch_add(1, std::memory_order_relaxed);

    // Brief lock: copy metadata needed to open/reuse the scrub decoder.
    std::filesystem::path filePath;
    VideoStreamInfo info{};
    bool packedAlpha = false;
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (!entry || !entry->decoder) return nullptr;
        filePath = entry->path;
        info = entry->info;
        packedAlpha = entry->packedAlpha;
    }
    // m_mutex released — decode is now lock-free.

    auto& scrubState = getScrubDecoder(handle, filePath, info);

    // Build a PrefetchTask so we can reuse decodePrefetchFrame/convertDecodedToCache
    PrefetchTask scrubTask;
    scrubTask.handle = handle;
    scrubTask.filePath = filePath;
    scrubTask.frameNumber = frameNumber;
    scrubTask.tier = tier;
    scrubTask.fps = info.fps > 0 ? info.fps : 30.0;
    scrubTask.info = info;
    scrubTask.packedAlpha = packedAlpha;
    // Exact means "decode this requested frame now", not "ignore the
    // monitor's resolution selector". Full already maps to native source
    // dimensions; forcing a paused Half request to native mislabeled the
    // resulting texture as Half and collided with the real Half cache entry.
    scrubTask.exportFullRes = forceExact && tier == ResolutionTier::Full;

    // Scrub / on-demand / export(forceExact) decode.  Uses CPU convert
    // (convertDecodedToCache) — the scrub bottleneck is the decode + per-frame
    // overhead, NOT the YUV→RGB convert, so routing scrub through a GPU
    // converter gave no speedup and made export's first frame read a stale
    // GPU texture (readback raced the convert).  Steady playback still gets
    // GPU-resident frames via the prefetch workers' per-worker path.
    auto result = decodePrefetchFrame(scrubState, scrubTask);
    // Don't publish export decodes into the shared caches — keep export
    // side-effect-free so it can't evict playback frames or feed the
    // DiskFrameCache writer queue (which would pin VRAM via lazyReadback).
    if (result && !forceExact) {
        m_cache->put(result);
        if (m_diskCache) m_diskCache->putAsync(result);
    }

    // Always-on decode timing (Release-visible)
    {
        auto perfGetT1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(perfGetT1 - perfGetT0).count();
        static int s_missLog = 0;
        if (++s_missLog % 30 == 0 || ms > 20.0) {
            spdlog::info("[PERF] getFrame CACHE MISS: handle={} frame={} tier={} -> {:.1f}ms (gpuReady={}, {}x{})",
                         handle, frameNumber, static_cast<int>(tier), ms,
                         result ? result->gpuReady : false,
                         result ? result->width : 0, result ? result->height : 0);
        }
    }

    // Update last-good-frame so future playback misses can return stale
    if (result && !scrubMode) {
        std::lock_guard lg(m_lastGoodMtx);
        m_lastGoodFrame[handle] = result;
    }

    return result;
}

// ─── tryGetFrame (non-blocking playback path) ───────────────────────────────

std::shared_ptr<CachedFrame> MediaPool::tryGetExactFrame(
    MediaHandle handle, int64_t frameNumber, ResolutionTier tier)
{
    m_perf.totalRequests.fetch_add(1, std::memory_order_relaxed);
    extendInteractivePlaybackWindow(m_interactivePlaybackUntilMs);

    // Match the public frame-access clamp without consulting or mutating the
    // last-good map. Temporal interpolation deliberately requests a lower
    // endpoint again after the upper endpoint was used on the prior display
    // tick; the normal anti-rewind path must not replace that lower frame.
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (entry && entry->info.frameCount > 1 &&
            frameNumber >= entry->info.frameCount) {
            frameNumber = entry->info.frameCount - 1;
        }
    }
    if (frameNumber < 0) frameNumber = 0;

    m_cache->setPlayhead(handle, frameNumber);
    auto cached = m_cache->get(handle, frameNumber, tier);
    if (cached) {
        m_perf.cacheHits.fetch_add(1, std::memory_order_relaxed);
        return cached;
    }

    // Keep this path non-blocking. Exact endpoint queueing bypasses the normal
    // stride and rebuild cooldown, so a subsequent request for adjacent N+1
    // is merged independently rather than being suppressed.
    m_perf.totalMisses.fetch_add(1, std::memory_order_relaxed);
    scheduleExactPrefetch(handle, frameNumber, tier);
    return nullptr;
}

std::shared_ptr<CachedFrame> MediaPool::tryGetFrame(
    MediaHandle handle, int64_t frameNumber, ResolutionTier tier)
{
    m_perf.totalRequests.fetch_add(1, std::memory_order_relaxed);

    // Periodic perf report during playback (every ~2s at 30fps = every 60 frames)
    {
        static std::atomic<int> s_tryReportCounter{0};
        if (++s_tryReportCounter % 60 == 0) {
            logPerfReport();
        }
    }

    extendInteractivePlaybackWindow(m_interactivePlaybackUntilMs);

    bool isPackedAlpha = false;

    // Clamp frame number
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (entry) {
            isPackedAlpha = entry->packedAlpha;
            if (entry->info.frameCount > 1 && frameNumber >= entry->info.frameCount)
                frameNumber = entry->info.frameCount - 1;
        }
    }
    if (frameNumber < 0) frameNumber = 0;

    // ── Seek-discontinuity detection ─────────────────────────────────────
    // Clear stale anti-rewind state after backward OR distant forward seeks.
    // Without the forward bound, a cache miss at a new position can return
    // an arbitrarily old frame from this same MP4 handle.
    {
        std::lock_guard lg(m_lastGoodMtx);
        auto it = m_lastGoodFrame.find(handle);
        if (it != m_lastGoodFrame.end() && it->second) {
            int64_t delta = frameNumber - it->second->frameNumber;
            if (delta < -1 ||
                !isNearbyPlaybackFrame(frameNumber, it->second->frameNumber)) {
                m_lastGoodFrame.erase(it);
            }
        }
    }

    // Tell the cache where playback is so eviction protects future frames.
    // Previously missing from tryGetFrame — eviction was blind during
    // playback and could evict freshly prefetched ahead-of-playhead frames.
    m_cache->setPlayhead(handle, frameNumber);

    // Helper: update last-good-frame and return.  Ensures tryGetFrame
    // NEVER returns a frame older than the most recent one shown for
    // this handle — preventing the visual "rewind" flicker.
    auto returnFrame = [&](std::shared_ptr<CachedFrame> f) -> std::shared_ptr<CachedFrame> {
        std::lock_guard lg(m_lastGoodMtx);
        auto& lastGood = m_lastGoodFrame[handle];
        if (lastGood && f->frameNumber < lastGood->frameNumber) {
            // Would go backwards — return the last good frame instead.
            m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
            return lastGood;
        }
        lastGood = f;
        return f;
    };

    // Exact cache hit
    auto cached = m_cache->get(handle, frameNumber, tier);
    if (cached) {
        m_perf.cacheHits.fetch_add(1, std::memory_order_relaxed);
        return returnFrame(std::move(cached));
    }

    // Nearby-frame reuse in non-blocking playback.
    // Use a modest radius (±3) so the compositor can make forward progress
    // during the initial playback burst when only frame 0 is cached.
    // Packed-alpha content keeps a wider radius (±6) because character
    // media is more expensive to decode and warms up slower.
    // Search FORWARD first (frame+d) to prefer making visual progress
    // over going backward, which avoids the "stuck on first frame" freeze.
    const int nearbyRadius = isPackedAlpha ? 6 : 3;
    for (int d = 1; d <= nearbyRadius; ++d) {
        // Forward first — prefer advancing the display over repeating old frames
        cached = m_cache->get(handle, frameNumber + d, tier);
        if (cached) {
            m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
            schedulePrefetch(handle, frameNumber, perfProfile().prefetchAheadFrames, /*urgent=*/true, tier);
            return returnFrame(std::move(cached));
        }
        cached = m_cache->get(handle, frameNumber - d, tier);
        if (cached) {
            m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
            schedulePrefetch(handle, frameNumber, perfProfile().prefetchAheadFrames, /*urgent=*/true, tier);
            return returnFrame(std::move(cached));
        }
    }

    // Try alternate tier as last resort (exact match only — no forward
    // search to avoid the same temporal-jump issue at a different tier).
    // Only upgrade (Half→Full), never downgrade (Full→Half) —
    // compositing a lower-resolution frame at the full viewport produces
    // blurry output.  resolveMediaFrame already rejects wrong-tier frames
    // during export, but this prevents the wrong frame from entering the
    // playback path in the first place.
    ResolutionTier altTier;
    if (tier == ResolutionTier::Half) {
        altTier = ResolutionTier::Full;
    } else {
        altTier = tier;  // no downgrade
    }
    if (altTier != tier) {
        cached = m_cache->get(handle, frameNumber, altTier);
        if (cached) {
            m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
            schedulePrefetch(handle, frameNumber + 1, perfProfile().prefetchAheadFrames, /*urgent=*/false, tier);
            return returnFrame(std::move(cached));
        }
    }

    // Total miss — return last-good frame if available (holds the most
    // recent frame shown for this handle).  This prevents flicker:
    // FrameProducer re-publishes the same composed image, so the display
    // holds steady instead of oscillating between old and new frames.
    {
        std::lock_guard lg(m_lastGoodMtx);
        auto it = m_lastGoodFrame.find(handle);
        if (it != m_lastGoodFrame.end() && it->second) {
            m_perf.nearbyHits.fetch_add(1, std::memory_order_relaxed);
            schedulePrefetch(handle, frameNumber, perfProfile().prefetchAheadFrames, /*urgent=*/true, tier);
            return it->second;
        }
    }

    m_perf.totalMisses.fetch_add(1, std::memory_order_relaxed);
    schedulePrefetch(handle, frameNumber, perfProfile().prefetchAheadFrames, /*urgent=*/true, tier);

    static std::atomic<int> s_tryMissLog{0};
    if (++s_tryMissLog % 30 == 0) {
        spdlog::info("[PERF] tryGetFrame MISS: handle={} frame={} tier={} (prefetch scheduled)",
                     handle, frameNumber, static_cast<int>(tier));
    }
    return nullptr;
}

// ─── isFrameCached (non-blocking probe) ────────────────────────────────────

bool MediaPool::isFrameCached(MediaHandle handle, int64_t frameNumber,
                              ResolutionTier tier) const
{
    return m_cache && m_cache->contains(handle, frameNumber, tier);
}

} // namespace rt
