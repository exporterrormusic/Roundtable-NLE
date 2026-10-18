/*
 * CompositeServiceLayerBuildVideo.cpp - media frame resolution helper.
 * Extracted from CompositeServiceLayerBuild.cpp (modularization).
 *
 * Contains:
 *   - resolveMediaFrame()   — picks the MediaPool frame-request policy
 *
 * Called from buildLayersForFrame() to reduce the size of the main
 * timeline traversal function.
 */

#include "CompositeService.h"

#include "cache/FrameCache.h"
#include "playback/MediaPool.h"
#include "cache/CachePolicy.h"
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
// resolveMediaFrame — the compositor's media frame fetch.
//
// Picks the MediaPool::requestFrame policy for this render (see FramePolicy):
//   export (forceFullResolution)  → ExactBlocking: every source must be the
//                                   exact frame before an output frame commits;
//   Program Monitor still          → ExactOrStandIn: never blocks FrameProducer;
//                                   a stand-in is flagged and the monitor retries;
//   temporal endpoint             → ExactCacheOnly;
//   scrub                         → Scrub: cached/nearby, else a quick
//                                   Quarter-tier decode;
//   playback                      → BestEffort: never blocks.
// result.exact says whether the frame is the one asked for.
// ─────────────────────────────────────────────────────────────────────────────
FrameResult CompositeService::resolveMediaFrame(
    MediaHandle handle, int64_t frameNumber, ResolutionTier tier,
    bool scrubMode, bool forceFullResolution,
    bool exactCacheOnly, bool stillMode, bool gpuSourceConvert) const
{
    if (!m_mediaPool)
        return {};

    // Phase B: declare a protected playback window for this media on each
    // resolve.  The window stays alive in FrameCache (via CachePolicy
    // forwarding) so background eviction won't drop frames the user is
    // about to scrub onto.  Values chosen to match the existing prefetch
    // scheduler defaults: 8 behind (small backwards-scrub buffer) and
    // 60 ahead (~2s at 30fps — covers typical seek-then-play patterns).
    if (m_cachePolicy) {
        m_cachePolicy->setPlayheadWindow(handle, frameNumber,
                                          /*aheadCount=*/60,
                                          /*behindCount=*/8,
                                          tier);
        m_cachePolicy->markAccess({handle, frameNumber, tier});
    }

    const FramePolicy policy =
        forceFullResolution ? FramePolicy::ExactBlocking
        : stillMode         ? FramePolicy::ExactOrStandIn
        : exactCacheOnly    ? FramePolicy::ExactCacheOnly
        : scrubMode         ? FramePolicy::Scrub
                            : FramePolicy::BestEffort;
    return m_mediaPool->requestFrame(
        {handle, frameNumber, tier, policy, gpuSourceConvert});
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
    VideoClip* videoClip, bool playbackNonBlocking,
    bool forceFullResolution, bool& skipClip,
    RenderExecutionOutcome* outcome)
{
    skipClip = false;
    if (!m_mediaPool) {
        if (outcome) {
            outcome->report(RenderResultStatus::Failed,
                            "video source cannot resolve without a media pool");
        }
        skipClip = true;
        return 0;
    }
    const auto& mediaPath = videoClip->mediaPath();
    if (mediaPath.empty()) {
        if (outcome) {
            outcome->report(RenderResultStatus::MissingMedia,
                            "video clip has no media path");
        }
        skipClip = true;
        return 0;
    }

    // Lazy-open media handle (cached by path)
    uint64_t handle = findMediaHandle(mediaPath);
    if (handle != 0) {
    } else if (playbackNonBlocking && !forceFullResolution) {
        // Check if MediaPool finished opening this path in the background.
        if (m_mediaPool->isPathOpen(mediaPath)) {
            handle = m_mediaPool->open(mediaPath);
            if (handle != 0)
                registerMediaHandle(mediaPath, handle);
        } else {
            // Not open yet — start or continue async open.
            if (m_mediaPool->pathState(mediaPath) == MediaPathState::Missing) {
                if (outcome) {
                    outcome->report(RenderResultStatus::MissingMedia,
                                    "media is offline: " + mediaPath);
                }
                skipClip = true;
                return 0;
            }
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
            // Try alternate video extensions — prefer .mp4 packed-alpha
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
            if (outcome &&
                m_mediaPool->pathState(mediaPath) == MediaPathState::Missing) {
                outcome->report(RenderResultStatus::MissingMedia,
                                "media is offline: " + mediaPath);
            }
            skipClip = true;
            return 0;
        }
        registerMediaHandle(mediaPath, handle);
    }
    return handle;
}

} // namespace rt
