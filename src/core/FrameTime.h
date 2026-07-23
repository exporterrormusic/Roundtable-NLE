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

} // namespace rt
