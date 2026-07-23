/*
 * FrameFallbackPolicy.h - bounds for temporary playback frame substitution.
 *
 * A nearby decoded frame can hide a short decoder miss during sequential
 * playback.  A frame from a distant source position must never be reused:
 * compositing it at the new timeline tick can make the old image persistent
 * in the composite cache (especially when adjustment effects are present).
 */

#pragma once

#include "Constants.h"

#include <cstdint>

namespace rt {

// Wide enough to cover a brief decode-ahead hiccup at common frame rates,
// but small enough that an explicit seek is treated as a discontinuity.
inline constexpr int64_t kMaxPlaybackFallbackDistanceFrames = 12;

// A composite hold may span a brief decoder stall during continuous
// playback, but not an explicit seek to another part of the same long clip.
inline constexpr int64_t kMaxPlaybackCompositeHoldTicks = kTicksPerSecond;

[[nodiscard]] inline constexpr bool isNearbyPlaybackFrame(
    int64_t requestedFrame, int64_t candidateFrame) noexcept
{
    return candidateFrame >= requestedFrame
        ? candidateFrame - requestedFrame <= kMaxPlaybackFallbackDistanceFrames
        : requestedFrame - candidateFrame <= kMaxPlaybackFallbackDistanceFrames;
}

[[nodiscard]] inline constexpr bool isNearbyPlaybackCompositeTick(
    int64_t requestedTick, int64_t candidateTick) noexcept
{
    return candidateTick >= requestedTick
        ? candidateTick - requestedTick <= kMaxPlaybackCompositeHoldTicks
        : requestedTick - candidateTick <= kMaxPlaybackCompositeHoldTicks;
}

} // namespace rt
