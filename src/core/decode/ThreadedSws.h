/*
 * ThreadedSws — swscale conversions that run on several threads.
 *
 * sws_getContext() builds a single-threaded context, and the legacy
 * sws_scale() entry point never threads even when a context has slice
 * contexts. A 4K frame converted on one core costs tens of milliseconds,
 * which capped export throughput on both ends of the pipeline (decoded
 * YUV → BGRA before compositing, composited BGRA → YUV before encoding).
 *
 * createSwsContext() builds the context with swscale's "threads" option and
 * swsScaleWhole() runs it through sws_scale_frame(), wrapping caller-owned
 * planes in non-owning AVFrames so nothing is copied. Output is identical to
 * the single-threaded conversion (test_threaded_sws).
 */

#pragma once

#include <cstdint>

struct SwsContext;

namespace rt {

/// Thread count for one full-frame conversion stream: the logical core
/// count, capped at 8 (more slices stop paying off at 4K).
[[nodiscard]] int defaultSwsThreads() noexcept;

/// Like sws_getContext(), plus a thread count: 1 = single-threaded (exactly
/// what sws_getContext builds), 0 = defaultSwsThreads(). Formats are
/// AVPixelFormat values passed as int. Returns nullptr on failure.
[[nodiscard]] SwsContext* createSwsContext(int srcW, int srcH, int srcFmt,
                                           int dstW, int dstH, int dstFmt,
                                           int flags, int threads);

/// Convert one whole frame, like sws_scale(ctx, src, srcStride, 0, srcH,
/// dst, dstStride). Threaded when the context was built with threads.
/// Returns the number of output rows (dstH) or a negative AVERROR.
int swsScaleWhole(SwsContext* ctx,
                  const uint8_t* const srcData[], const int srcLinesize[],
                  int srcW, int srcH, int srcFmt,
                  uint8_t* const dstData[], const int dstLinesize[],
                  int dstW, int dstH, int dstFmt);

} // namespace rt
