/*
 * FrameLabel.h - does a decoded picture's own timestamp agree with the frame
 * number the decode was asked for?
 *
 * MediaPool names source frame N "the picture a precise seek to N / fps
 * returns" (VideoDecoder::seek and VideoFrameMapping use the same absolute
 * time). The decode paths label each picture with the frame they REQUESTED;
 * a decoder that ends up one picture early or late (a flush that dropped the
 * target, a skipped picture) then caches the wrong picture under that number.
 * These helpers let a decode path check the label against the picture.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace rt {

/// FFmpeg's AV_NOPTS_VALUE, without pulling libavutil into the header.
inline constexpr int64_t kNoPts = std::numeric_limits<int64_t>::min();

/// The frame a picture shown at `seconds` belongs to.
[[nodiscard]] inline int64_t frameFromTimestamp(double seconds, double fps) noexcept
{
    return static_cast<int64_t>(std::llround(seconds * fps));
}

/// True when a picture shown at `seconds` is source frame `frame`. A real
/// mislabel is a whole picture off; the slack absorbs a stream whose average
/// rate differs slightly from its picture spacing.
[[nodiscard]] inline bool timestampMatchesFrame(double seconds, int64_t frame,
                                                double fps) noexcept
{
    if (!(fps > 0.0)) return true;
    return std::abs(seconds * fps - static_cast<double>(frame)) < 0.75;
}

} // namespace rt
