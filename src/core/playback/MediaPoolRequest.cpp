/*
 * MediaPoolRequest.cpp - MediaPool::requestFrame, the one way a renderer asks
 * for a source frame.
 *
 * The policy says what the caller may wait for and what it may accept in
 * place of the frame it asked for; the result says whether it got that
 * frame. getFrame / tryGetFrame / tryGetExactFrame / peekExactFrame are the
 * building blocks underneath.
 */

#include "MediaPool.h"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace rt {

FrameResult MediaPool::requestFrame(const FrameRequest& request)
{
    const MediaHandle handle = request.handle;
    const ResolutionTier tier = request.tier;

    // The clamp every building block applies, done once so the result can
    // say which frame it answers for: the last frame of a clip whose mapping
    // runs past the end is still exact, and a still has only frame 0.
    int64_t frameNumber = std::max<int64_t>(0, request.frame);
    bool singleFrameMedia = false;
    {
        std::lock_guard lock(m_mutex);
        if (auto* entry = findEntry(handle)) {
            singleFrameMedia =
                entry->info.duration <= 0.0 || entry->info.frameCount <= 1;
            if (entry->info.frameCount > 1 && frameNumber >= entry->info.frameCount)
                frameNumber = entry->info.frameCount - 1;
        }
    }
    if (singleFrameMedia)
        frameNumber = 0;

    FrameResult result;
    result.resolvedFrame = frameNumber;
    result.frame = fetchForPolicy(handle, frameNumber, tier, request.policy,
                                  singleFrameMedia);
    result.exact = result.frame &&
                   result.frame->frameNumber == result.resolvedFrame;
    return result;
}

std::shared_ptr<CachedFrame> MediaPool::fetchForPolicy(
    MediaHandle handle, int64_t frameNumber, ResolutionTier tier,
    FramePolicy policy, bool singleFrameMedia)
{
    switch (policy) {
    case FramePolicy::ExactBlocking: {
        // Frame 0 is exact by definition for single-frame media. Export may
        // use the blocking cache path because every source must be present
        // before an output frame is committed.
        if (singleFrameMedia) {
            auto frame = getFrame(handle, frameNumber, tier,
                                  /*scrubMode=*/true, /*forceExact=*/false);
            if (frame && frame->frameNumber == 0 && frame->tier == tier)
                return frame;
            return nullptr;
        }
        // An exact cache hit is returned at once. peekExactFrame, not
        // tryGetExactFrame: a miss there queued a background decode of this
        // same frame, which then ran alongside the blocking decode below —
        // every export frame decoded twice.
        auto frame = peekExactFrame(handle, frameNumber, tier);
        // Loop pre-decoded entries are not trusted here. The loop pre-decoder
        // labels frames by counting from a precise seek; a seek that flushed
        // the target (root cause fixed 2026-10-12, test_video_decoder_seek)
        // mislabeled the frames right after each seek/wrap, which baked a
        // 1-frame jump into the start of every video-character (Wells) clip
        // in exports. Kept as defense in depth alongside the timestamp check
        // (FrameLabel.h). Regular prefetch frames are trusted.
        if (frame && frame->frameNumber == frameNumber && frame->tier == tier &&
            !frame->isLoopFrame)
            return frame;
        // forceExact skips the nearby-frame fallback (which would hand back
        // the PREVIOUS frame → duplication / stutter) and decodes the exact
        // frame inline on the export decoder.
        frame = getFrame(handle, frameNumber, tier,
                         /*scrubMode=*/true, /*forceExact=*/true);
        if (frame && frame->tier != tier) {
            // Compositing a lower tier at the full viewport is visibly
            // blurry. Nothing better exists for THIS frame; queue the right
            // tier for the next one.
            schedulePrefetch(handle, frameNumber, 1, /*urgent=*/true, tier);
            spdlog::warn("[EXPORT-TIER] handle={} frame={} requested tier={} "
                         "but getFrame returned tier={} — scheduling correct-tier prefetch",
                         handle, frameNumber, static_cast<int>(tier),
                         static_cast<int>(frame->tier));
        }
        return frame;
    }

    case FramePolicy::ExactOrStandIn: {
        // Program Monitor still requests are progressive: queue the exact
        // frame, return a cached nearby/last-good stand-in meanwhile. The
        // monitor retries while the result is a stand-in. No decoder call is
        // made inline, so a newer seek or Play runs promptly.
        auto frame = tryGetExactFrame(handle, frameNumber, tier);
        if (frame && frame->frameNumber == frameNumber && frame->tier == tier)
            return frame;
        return tryGetFrame(handle, frameNumber, tier);
    }

    case FramePolicy::ExactCacheOnly:
        // Optical-flow/frame-blend endpoints must be exact but never block
        // playback. They must not go through tryGetFrame's nearby/last-good
        // substitution: a slow retime asks for the same lower endpoint after
        // the prior call fetched the upper one, which the anti-rewind guard
        // would otherwise replace.
        return tryGetExactFrame(handle, frameNumber, tier);

    case FramePolicy::Scrub: {
        auto frame = tryGetFrame(handle, frameNumber, tier);
        if (frame)
            return frame;
        // Miss: a quick inline decode at Quarter tier on the scrub decoder
        // (16x less data than Full keeps NVDEC contention low); the
        // requested tier arrives from prefetch when scrubbing stops.
        auto scrubFrame = getFrame(handle, frameNumber, ResolutionTier::Quarter,
                                   /*scrubMode=*/true);
        if (scrubFrame)
            schedulePrefetch(handle, frameNumber, 1, /*urgent=*/true, tier);
        return scrubFrame;
    }

    case FramePolicy::BestEffort: {
        // Exact, nearby or last-good from the cache; a miss has queued the
        // decode and returns nothing (the compositor's sticky frame covers
        // it). The render thread never decodes.
        auto frame = tryGetFrame(handle, frameNumber, tier);
        // Any-tier hit is accepted (rejecting it left the layer unrendered
        // after a live file replace, showing the OLD media); queue the
        // requested tier so the next composite is sharp.
        if (frame && frame->tier != tier)
            schedulePrefetch(handle, frameNumber, 1, /*urgent=*/true, tier);
        return frame;
    }
    }
    return nullptr;
}

} // namespace rt
