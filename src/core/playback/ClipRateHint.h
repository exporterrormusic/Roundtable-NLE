#pragma once
/*
 * ClipRateHint.h — tell MediaPool how a video clip walks its file during
 * playback (MediaPool::noteClipFrameRate), from the same mapping the
 * renderer uses.  Shared by the render path (CompositeServiceLayerBuild),
 * the play-start preroll and the lookahead prewarm
 * (CompositeServiceFramePrewarm) so all three agree.
 */

#include "Constants.h"
#include "decode/VideoFrameMapping.h"
#include "playback/MediaPool.h"
#include "timeline/VideoClip.h"

#include <cmath>
#include <cstdint>
#include <optional>

namespace rt {

/// `mapped` = mapTickToSourceFrame(clip, tick, info); `requestedFrame` the
/// frame actually being asked for (mapped.frame, or lowerFrame for a
/// temporal blend — `temporal`).  Returns the keyframe live playback should
/// show instead when the clip is fast enough for keyframe-only playback
/// (KeyframeIndex.h), else nullopt.  Use the result ONLY for live playback.
inline std::optional<int64_t> noteVideoClipRate(MediaPool& pool, MediaHandle handle,
                                 const VideoClip& clip, int64_t tick,
                                 const MappedSourceFrame& mapped,
                                 int64_t requestedFrame, bool temporal,
                                 const VideoStreamInfo* info)
{
    if (handle == 0 || !info || info->frameCount <= 1) return std::nullopt;
    // Source rate from the mapping itself (handles speed ramps, whose rate
    // is not the instantaneous speed).
    constexpr int64_t kRateProbeTicks = kTicksPerSecond / 10;
    const double srcTicksPerTick =
        static_cast<double>(unclampedSourceTick(clip, tick + kRateProbeTicks) -
                            unclampedSourceTick(clip, tick)) /
        static_cast<double>(kRateProbeTicks);
    // Shift the position onto the frame actually requested (end clamp,
    // character loop wrap) keeping its phase.
    const double basis = temporal
        ? std::floor(mapped.position)
        : static_cast<double>(std::llround(mapped.position));
    pool.noteClipFrameRate(
        handle, requestedFrame,
        mapped.position + (static_cast<double>(requestedFrame) - basis),
        srcTicksPerTick * mapped.fps,
        clip.timeInterpolation() != TimeInterpolation::FrameSampling, info);
    return pool.playbackKeyframeFor(handle, requestedFrame);
}

} // namespace rt
