/*
 * FrameTime.h - one canonical frame-index <-> timeline-tick mapping.
 *
 * Export, pre-render, and cache lookup must produce the same integer tick for
 * the same frame. Keeping this in one header avoids the old mix of truncation
 * and rounding which caused cache misses at fractional (NTSC) frame rates.
 */

#pragma once

#include "Constants.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>

namespace rt {

struct RationalFrameRate
{
    uint32_t numerator{30};
    uint32_t denominator{1};
};

[[nodiscard]] inline RationalFrameRate canonicalFrameRate(double fps) noexcept
{
    if (!(fps > 0.0) || !std::isfinite(fps)) return {};

    const double integral = std::round(fps);
    if (std::abs(fps - integral) < 0.005)
        return {static_cast<uint32_t>(std::lround(integral)), 1};

    const auto ntscNominal = static_cast<uint32_t>(std::lround(fps * 1.001));
    const double ntsc = static_cast<double>(ntscNominal) * 1000.0 / 1001.0;
    if (ntscNominal > 0 && std::abs(fps - ntsc) < 0.005)
        return {ntscNominal * 1000u, 1001u};

    constexpr uint32_t kScale = 1'000'000u;
    uint64_t num64 = static_cast<uint64_t>(std::llround(fps * kScale));
    uint64_t den64 = kScale;
    const uint64_t divisor = std::gcd(num64, den64);
    num64 /= divisor;
    den64 /= divisor;
    if (num64 == 0 || num64 > std::numeric_limits<uint32_t>::max()
        || den64 > std::numeric_limits<uint32_t>::max()) return {};
    return {static_cast<uint32_t>(num64), static_cast<uint32_t>(den64)};
}

[[nodiscard]] inline int64_t frameIndexToTick(
    int64_t frameIndex, uint32_t fpsNumerator, uint32_t fpsDenominator) noexcept
{
    if (fpsNumerator == 0 || fpsDenominator == 0) return 0;
    const long double tick = static_cast<long double>(frameIndex)
                           * static_cast<long double>(kTicksPerSecond)
                           * static_cast<long double>(fpsDenominator)
                           / static_cast<long double>(fpsNumerator);
    return static_cast<int64_t>(std::llround(tick));
}

[[nodiscard]] inline int64_t frameIndexToTick(
    int64_t frameIndex, RationalFrameRate fps) noexcept
{
    return frameIndexToTick(frameIndex, fps.numerator, fps.denominator);
}

/// Exact inverse of frameIndexToTick: the frame ON SCREEN at `tick`, i.e.
/// the largest frame whose start tick is <= tick. Plain `seconds * fps`
/// truncation is wrong here — at NTSC rates frame starts are ROUNDED ticks
/// (29.97: frame 2 = tick 3203 → 1.9999 frames), so truncating an in point
/// sitting exactly on a frame returned the frame BEFORE it.
[[nodiscard]] inline int64_t tickToFrameIndex(
    int64_t tick, uint32_t fpsNumerator, uint32_t fpsDenominator) noexcept
{
    if (fpsNumerator == 0 || fpsDenominator == 0) return 0;
    auto frame = static_cast<int64_t>(std::floor(
        static_cast<long double>(tick) * static_cast<long double>(fpsNumerator)
        / (static_cast<long double>(kTicksPerSecond) * static_cast<long double>(fpsDenominator))));
    // Settle against the canonical (rounded) frame starts.
    while (frameIndexToTick(frame + 1, fpsNumerator, fpsDenominator) <= tick) ++frame;
    while (frameIndexToTick(frame, fpsNumerator, fpsDenominator) > tick) --frame;
    return frame;
}

/// Exclusive end frame for a range ending at `tick` (exclusive): a frame
/// boundary ends there; a mid-frame tick includes the partial frame.
[[nodiscard]] inline int64_t tickToEndFrameIndex(
    int64_t tick, uint32_t fpsNumerator, uint32_t fpsDenominator) noexcept
{
    return tickToFrameIndex(tick - 1, fpsNumerator, fpsDenominator) + 1;
}

[[nodiscard]] inline int64_t tickToFrameIndex(
    int64_t tick, RationalFrameRate fps) noexcept
{
    return tickToFrameIndex(tick, fps.numerator, fps.denominator);
}

// ── Frame-grid helpers ──────────────────────────────────────────────────────
// Use these instead of `48000 / fps`: at NTSC rates a frame is 1601.6 ticks,
// so frame starts alternate 1601/1602 apart. Truncating to 1601 (or rounding
// to 1602) builds a second grid that drifts off this one by a whole frame
// every ~90 s at 29.97 — repeated frames, missed pre-render cache keys, and
// cuts that land on the wrong frame.

/// Exact (fractional) frame length. For tolerances and pixel math only —
/// positions must come from frameIndexToTick().
[[nodiscard]] inline double exactTicksPerFrame(RationalFrameRate fps) noexcept
{
    if (fps.numerator == 0 || fps.denominator == 0) return 0.0;
    return static_cast<double>(kTicksPerSecond) * fps.denominator / fps.numerator;
}

/// Frame length rounded to whole ticks (1602 at 29.97), for tolerance windows.
[[nodiscard]] inline int64_t nominalTicksPerFrame(RationalFrameRate fps) noexcept
{
    const int64_t ticks = std::llround(exactTicksPerFrame(fps));
    return ticks > 0 ? ticks : 1;
}

/// Start of the frame on screen at `tick` (floor onto the frame grid).
[[nodiscard]] inline int64_t frameStartAtOrBefore(
    int64_t tick, RationalFrameRate fps) noexcept
{
    return frameIndexToTick(tickToFrameIndex(tick, fps), fps);
}

/// Frame whose start is nearest `tick`; a tie goes to the later frame.
[[nodiscard]] inline int64_t nearestFrameIndex(
    int64_t tick, RationalFrameRate fps) noexcept
{
    const int64_t frame = tickToFrameIndex(tick, fps);
    const int64_t start = frameIndexToTick(frame, fps);
    const int64_t next  = frameIndexToTick(frame + 1, fps);
    return (tick - start < next - tick) ? frame : frame + 1;
}

/// Nearest frame start to `tick`. Also snaps a signed offset to a whole
/// number of frames (n frames = frameIndexToTick(n)).
[[nodiscard]] inline int64_t snapTickToNearestFrame(
    int64_t tick, RationalFrameRate fps) noexcept
{
    return frameIndexToTick(nearestFrameIndex(tick, fps), fps);
}

/// Start of the frame `frames` away from the one on screen at `tick`,
/// never before frame 0. Stepping back from mid-frame lands on the previous
/// frame, so every step shows a different picture.
[[nodiscard]] inline int64_t stepFrames(
    int64_t tick, int64_t frames, RationalFrameRate fps) noexcept
{
    const int64_t frame = tickToFrameIndex(tick, fps) + frames;
    return frameIndexToTick(frame > 0 ? frame : 0, fps);
}

} // namespace rt
