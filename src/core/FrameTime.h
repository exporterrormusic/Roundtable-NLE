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

} // namespace rt
