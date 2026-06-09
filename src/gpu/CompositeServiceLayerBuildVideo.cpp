/*
 * CompositeServiceLayerBuildVideo.cpp - media frame resolution helper.
 * Extracted from CompositeServiceLayerBuild.cpp (modularization).
 *
 * Contains:
 *   - resolveMediaFrame()   — media frame resolution with cache-aware fetch
 *
 * Called from buildLayersForFrame() to reduce the size of the main
 * timeline traversal function.
 */

#include "CompositeService.h"

#include "media/FrameCache.h"
#include "media/MediaPool.h"
#include "media/UnifiedCache.h"
#include "Constants.h"
#include "timeline/VideoClip.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"

#include "CompositeEngine.h"
#include "GpuContext.h"
#include "GpuTextureCache.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <vector>

namespace rt {

// ─────────────────────────────────────────────────────────────────────────────
// resolveMediaFrame — cache-aware media frame fetch.
//
// Replaces the inline fetchMediaFrame lambda from buildLayersForFrame().
// During export (forceFullRes), tries tryGetFrame first then falls back to
// blocking getFrame.  During playback, tries non-blocking tryGetFrame only
// (the prefetch workers fill the cache asynchronously).  During scrub,
// returns nullptr on cache miss to avoid blocking the compositor mutex.
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr<CachedFrame> CompositeService::resolveMediaFrame(
    MediaHandle handle, int64_t frameNumber, ResolutionTier tier,
    bool scrubMode) const
{
    if (!m_mediaPool)
        return nullptr;

    // Phase B: declare a protected playback window for this media on each
    // resolve.  The window stays alive in FrameCache (via UnifiedCache
    // forwarding) so background eviction won't drop frames the user is
    // about to scrub onto.  Values chosen to match the existing prefetch
    // scheduler defaults: 8 behind (small backwards-scrub buffer) and
    // 60 ahead (~2s at 30fps — covers typical seek-then-play patterns).
    if (m_unifiedCache) {
        m_unifiedCache->setPlayheadWindow(handle, frameNumber,
                                          /*aheadCount=*/60,
                                          /*behindCount=*/8,
                                          tier);
        m_unifiedCache->markAccess({handle, frameNumber, tier});
    }
    // During export (forceFullRes), first try the cache via tryGetFrame
    // — this sets the playhead + extends the interactive playback window
    // (unlike getFrame with scrubMode=true which skips both).  If the
    // exact frame IS cached, return it instantly.  Otherwise fall through
    // to blocking getFrame with forceExact=true: it skips the ±N nearby-
    // frame fallback (which would otherwise hand back the PREVIOUS frame
    // as a stale neighbor → frame duplication / stutter) and does a
    // frame-accurate inline decode of the exact frame via the scrub
    // decoder.  The playhead is already set by tryGetFrame, so the cache
    // won't thrash-evict.  Also reject alternate-tier fallback frames
    // (e.g. Half when Full was requested) — export always wants full quality.
    if (m_forceFullResolution.load()) {
        auto frame = m_mediaPool->tryGetFrame(handle, frameNumber, tier);
        // Reject loop-pre-decoded cache entries during export.  The loop
        // pre-decoder (MediaPoolPrefetchLoop) seeks with SeekMode::Precise
        // and labels frames sequentially, which the rest of the engine
        // deliberately avoids (scrub/prefetch use Keyframe seek + forward
        // decode) because Precise seek mislabels the frames right after each
        // seek/wrap — i.e. source frame 0, the START of every clip.  Trusting
        // those entries here baked a 1-frame jump/jitter into the very start
        // of every video-character (Wells) clip in exports, even though
        // playback (which sees them only transiently) looked fine.  Force the
        // isolated forceExact decode for loop frames so export gets the exact
        // frame.  Regular prefetch frames (keyframe-accurate) are still trusted.
        if (frame && frame->frameNumber == frameNumber && frame->tier == tier &&
            !frame->isLoopFrame)
            return frame;
        // getFrame() has an alt-tier fallback (e.g. returns Half when Full
        // was requested).  During export/preview we MUST reject wrong-tier
        // frames — compositing a Half-tier character at the full viewport
        // resolution produces visibly blurry output.
        frame = m_mediaPool->getFrame(handle, frameNumber, tier,
                                      /*scrubMode=*/true, /*forceExact=*/true);
        if (frame && frame->tier != tier) {
            // Wrong tier from alt-tier fallback.  Schedule an urgent
            // correct-tier prefetch for the next frame, but for THIS
            // frame there's nothing better available — the compositor
            // will render it and the next frame will be correct.
            m_mediaPool->schedulePrefetch(handle, frameNumber, 1,
                                          /*urgent=*/true, tier);
            spdlog::warn("[EXPORT-TIER] handle={} frame={} requested tier={} "
                         "but getFrame returned tier={} — scheduling correct-tier prefetch",
                         handle, frameNumber, static_cast<int>(tier),
                         static_cast<int>(frame->tier));
        }
        return frame;
    }
    // Try non-blocking first (fast path for cached frames during playback).
    auto frame = m_mediaPool->tryGetFrame(handle, frameNumber, tier);
    if (frame) {
        // Tier mismatch (e.g. ResolutionTier::Half cached, Full requested).
        // The previous behavior was to REJECT the wrong-tier frame in
        // non-scrub mode (to avoid a briefly-blurry display) and return
        // nullptr, leaving the layer unrendered.  That had a fatal failure
        // mode during live file replacement: after invalidate, only a
        // single tier ends up in the cache before the paused composite
        // runs; if it's not Full, the layer is skipped, the output VkImage
        // retains the previous composite's pixels, and the user sees the
        // OLD media until they scrub (scrub mode bypassed this check).
        // Accept any-tier hit instead, and still schedule the correct-tier
        // prefetch so the next composite gets the sharper frame.  The
        // brief intermediate-tier moment is invisible for stills and
        // strictly better than displaying stale media.
        if (!scrubMode && frame->tier != tier) {
            m_mediaPool->schedulePrefetch(handle, frameNumber, 1, /*urgent=*/true, tier);
            // fall through and return the wrong-tier frame instead of nullptr
        }
        return frame;
    }
    // Cache miss path.
    //
    // SCRUB MODE: return nullptr instead of falling through to blocking
    // inline decode (2026-05-22 fix).  The previous code routed scrubMode
    // requests through MediaPool::getFrame's scrub-decoder path, which
    // the in-tree comment claimed cost "~5-15 ms" via NVDEC.  In practice
    // — see LAYER-SLOW perf_log entries at 2026-05-22 12:26:38–40 with
    // single-clip layer-build times of 60-872 ms during aggressive scrub
    // — NVDEC session contention (2 prefetch workers + N scrub decoders
    // approaching consumer NVDEC's 3-5 concurrent-session limit) blew
    // that budget by 50-100×.  Under that contention every layer's frame
    // fetch stalled the FrameProducer thread for hundreds of ms, which
    // is exactly the "scrubbing is basically unusable" symptom.
    //
    // The Premiere-style fix: tryGetFrame above has ALREADY scheduled an
    // urgent prefetch for the requested (handle, frame, tier) AND
    // returned the nearest cached frame if any exists within its ±5
    // window.  If it returned nullptr, no nearby frame exists yet and
    // we accept a brief stale-frame display while the prefetch worker
    // catches up — the compositor's last-good composite fallback covers
    // the gap.  This is the architectural model Premiere uses: never
    // block the compositor on decode, ever.  At normal scrub speeds the
    // user sees a single stale tick; at heavy scrub the display lags
    // ~100-300 ms behind the playhead but never freezes.
    if (scrubMode) {
        // Fast inline decode at Quarter tier via scrub decoder.
        // Quarter is 16x less data than Full, keeping NVDEC contention
        // low while providing responsive scrubbing. Full quality
        // arrives from background prefetch when scrubbing stops.
        auto scrubFrame = m_mediaPool->getFrame(handle, frameNumber,
            ResolutionTier::Quarter, /*scrubMode=*/true);
        if (scrubFrame) {
            m_mediaPool->schedulePrefetch(handle, frameNumber, 1,
                /*urgent=*/true, tier);
        }
        return scrubFrame;
    }

    // Non-scrub playback miss: same non-blocking treatment.  getFrame
    // with scrubMode=false returns a stale frame from the last-good
    // map without inline decoding, matching the "never stall the
    // render thread" design.
    return m_mediaPool->getFrame(handle, frameNumber, tier, /*scrubMode=*/false);
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveVideoClipHandle — lazily open / search-resolve a VideoClip's media.
//
// Extracted verbatim from the VideoClip branch of buildLayersForFrame().
// Return/skip contract (must match the original inline control flow):
//   • skipClip=true, returns 0  → caller should `continue` (no MediaPool,
//     empty path, or media unresolvable after the search-path fallback).
//   • skipClip=false, returns 0 → playbackNonBlocking open still pending;
//     caller proceeds with handle 0 so the sticky-frame fallback can run.
//   • skipClip=false, returns !=0 → resolved handle (cached in m_openMediaHandles).
// ─────────────────────────────────────────────────────────────────────────────
uint64_t CompositeService::resolveVideoClipHandle(
    VideoClip* videoClip, bool playbackNonBlocking, bool& skipClip)
{
    skipClip = false;
    if (!m_mediaPool) { skipClip = true; return 0; }  // VideoClip needs MediaPool
    const auto& mediaPath = videoClip->mediaPath();
    if (mediaPath.empty()) { skipClip = true; return 0; }

    // Lazy-open media handle (cached by path)
    uint64_t handle = 0;
    auto it = m_openMediaHandles.find(mediaPath);
    if (it != m_openMediaHandles.end() && it->second != 0) {
        handle = it->second;
    } else if (playbackNonBlocking && !m_forceFullResolution.load()) {
        // Check if MediaPool finished opening this path in the background.
        if (m_mediaPool->isPathOpen(mediaPath)) {
            handle = m_mediaPool->open(mediaPath);
            m_openMediaHandles[mediaPath] = handle;
        } else {
            // Not open yet — start or continue async open.
            m_mediaPool->openAsync(mediaPath);
            handle = 0;
        }
    } else {
        // Prefer .mp4 packed-alpha (NVDEC hw decode) over .mov/.webm
        // (software decode).  Try .mp4 FIRST before the original path
        // so we never open a slow ProRes when a fast HEVC exists.
        namespace fs = std::filesystem;
        fs::path p(mediaPath);
        if (p.extension() == ".mov" || p.extension() == ".webm") {
            fs::path mp4Path = fs::path(mediaPath).replace_extension(".mp4");
            if (fs::exists(mp4Path))
                handle = m_mediaPool->open(mp4Path);
            if (handle == 0) {
                fs::path mp4InVideos = fs::path("assets") / "videos" / mp4Path.filename();
                if (fs::exists(mp4InVideos))
                    handle = m_mediaPool->open(mp4InVideos);
            }
        }

        // Fall back to original path
        if (handle == 0)
            handle = m_mediaPool->open(mediaPath);

        // If that fails, try resolving common search paths
        if (handle == 0) {
            // Try alternate video extensions Ã¢â‚¬â€ prefer .mp4 packed-alpha
            // (NVDEC hardware decode) over .webm (software VP9).
            std::vector<fs::path> altExts;
            if (p.extension() == ".webm") {
                altExts.push_back(fs::path(mediaPath).replace_extension(".mp4"));
                altExts.push_back(fs::path(mediaPath).replace_extension(".mov"));
            } else if (p.extension() == ".mov") {
                altExts.push_back(fs::path(mediaPath).replace_extension(".mp4"));
                altExts.push_back(fs::path(mediaPath).replace_extension(".webm"));
            } else if (p.extension() == ".mp4") {
                altExts.push_back(fs::path(mediaPath).replace_extension(".webm"));
                altExts.push_back(fs::path(mediaPath).replace_extension(".mov"));
            }

            // Common image extensions — for background images referenced
            // as bare filenames without extension (e.g. "TABLE_LARGE_FINAL")
            const fs::path imgExts[] = {".png", ".jpg", ".jpeg"};
            for (const auto& imgExt : imgExts) {
                altExts.push_back(fs::path(mediaPath).replace_extension(imgExt));
            }

            // Search paths for unresolved media (bare filenames or
            // relative paths)
            std::vector<fs::path> searchPaths = {
                fs::path("assets") / "backgrounds" / p.filename(),
                fs::path("assets") / "videos" / p.filename(),
                fs::path("assets") / p,
                p.filename()
            };

            // Also try alternate extensions in each search dir
            for (const auto& altExt : altExts) {
                if (altExt.empty()) continue;
                fs::path altName = altExt.filename();
                searchPaths.push_back(altExt);
                searchPaths.push_back(fs::path("assets") / "videos" / altName);
                searchPaths.push_back(fs::path("assets") / altExt);
                searchPaths.push_back(altName);
            }

            for (const auto& candidate : searchPaths) {
                if (fs::exists(candidate)) {
                    handle = m_mediaPool->open(candidate);
                    if (handle != 0) {
                        break;
                    }
                }
            }
        }
        if (handle == 0) {
            spdlog::warn("compositeFrame: could not resolve media '{}'", mediaPath);
            skipClip = true;
            return 0;
        }
        m_openMediaHandles[mediaPath] = handle;
    }
    return handle;
}

} // namespace rt
