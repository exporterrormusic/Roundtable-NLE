/*
 * VideoFrameMapping — THE single authority for timeline-tick → source-frame
 * mapping of video clips.
 *
 * Three copies of this math used to live in the composite service (live
 * layer build, pending-frame collector, lookahead prewarm) and had already
 * drifted: the collector resolved fps in a different priority order than
 * the renderer, so it could prefetch a different frame than the one the
 * renderer would actually request.  Every consumer — render, prefetch
 * scheduling, prewarm — must agree on the EXACT frame number or caches
 * miss and frames repeat/skip.
 *
 * Rules encoded here (each was a bug at some point):
 *   - Speed scaling: localTick × effectiveSpeed(localTick), so 2x clips
 *     advance source twice as fast (matches EditOperationsTrim).
 *   - Negative srcTick clamps to 0 (NOT skipped): during transition
 *     overlap the incoming clip has tick < timelineIn; frame 0 is the
 *     correct visual for the transition.
 *   - ROUND, don't truncate: ticksToSeconds()*fps is a double and a
 *     frame-aligned tick lands on x.9999999 as often as x.0000001 (e.g.
 *     Wells 30fps on a 30fps timeline: srcTick=1600 → 0.99999… truncates
 *     to 0 instead of 1).  Truncation biases those frames one source
 *     frame early — an irregular repeat-then-skip "slight flicker" in
 *     both preview and export.
 *   - fps priority: the clip's stored sourceFps wins; only when the clip
 *     has none does the MediaPool's authoritative fps apply (re-mapping
 *     the frame), with 24 as the last-resort default.
 *   - Still images (frameCount <= 1) are always frame 0; video characters
 *     wrap (their idle loops repeat for the clip's whole duration);
 *     everything else clamps to [0, frameCount-1].
 */

#pragma once

#include "Constants.h"
#include "decode/VideoDecoder.h"     // VideoStreamInfo
#include "timeline/VideoClip.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rt {

struct MappedSourceFrame {
    int64_t frame{0};   ///< Source frame number (wrapped/clamped per policy)
    double  fps{24.0};  ///< The fps that produced it (clip > media > 24)
};

inline MappedSourceFrame mapTickToSourceFrame(const VideoClip& clip,
                                              int64_t tick,
                                              const VideoStreamInfo* info)
{
    const int64_t localTick = tick - clip.timelineIn();
    int64_t srcTick = clip.sourceIn() +
        static_cast<int64_t>(localTick * clip.effectiveSpeed(localTick));
    if (srcTick < 0) srcTick = 0;

    double fps = clip.sourceFps();
    if (fps <= 0.0) fps = 24.0;
    int64_t frame = std::llround(ticksToSeconds(srcTick) * fps);

    if (clip.sourceFps() <= 0.0 && info && info->fps > 0.0) {
        fps   = info->fps;
        frame = std::llround(ticksToSeconds(srcTick) * fps);
    }

    if (info) {
        if (info->frameCount <= 1) {
            frame = 0;
        } else if (clip.isVideoCharacter()) {
            frame = ((frame % info->frameCount) + info->frameCount)
                    % info->frameCount;
        } else if (info->frameCount > 0) {
            frame = std::clamp(frame, int64_t(0), info->frameCount - 1);
        }
    }

    return {frame, fps};
}

} // namespace rt
