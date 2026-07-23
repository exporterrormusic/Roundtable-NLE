#pragma once

#include "cache/FrameCache.h"

#include <algorithm>
#include <cstdint>

namespace rt {

/// Compute a tight non-zero-alpha rectangle for an 8-bit BGRA frame. Intended
/// for pinned/single-frame media; do not run this O(pixels) scan per video frame.
inline void computeBgraContentBounds(CachedFrame& frame) noexcept
{
    frame.contentBoundsValid = false;
    frame.contentFullyOpaque = false;
    if (frame.width == 0 || frame.height == 0 || frame.stride < frame.width * 4
        || frame.pixels.size() < static_cast<size_t>(frame.stride) * frame.height) {
        return;
    }

    uint32_t minX = frame.width;
    uint32_t minY = frame.height;
    uint32_t maxX = 0;
    uint32_t maxY = 0;
    bool any = false;
    bool opaque = true;
    for (uint32_t y = 0; y < frame.height; ++y) {
        const uint8_t* row = frame.pixels.data() + static_cast<size_t>(y) * frame.stride;
        for (uint32_t x = 0; x < frame.width; ++x) {
            const uint8_t alpha = row[static_cast<size_t>(x) * 4 + 3];
            opaque = opaque && alpha == 255;
            if (alpha == 0) continue;
            any = true;
            minX = std::min(minX, x); minY = std::min(minY, y);
            maxX = std::max(maxX, x); maxY = std::max(maxY, y);
        }
    }

    frame.contentBoundsValid = true;
    frame.contentFullyOpaque = opaque;
    if (!any) {
        frame.contentLeft = frame.contentTop = 0.0f;
        frame.contentRight = frame.contentBottom = 0.0f;
        return;
    }
    frame.contentLeft = static_cast<float>(minX) / frame.width;
    frame.contentTop = static_cast<float>(minY) / frame.height;
    frame.contentRight = static_cast<float>(maxX + 1) / frame.width;
    frame.contentBottom = static_cast<float>(maxY + 1) / frame.height;
}

} // namespace rt
