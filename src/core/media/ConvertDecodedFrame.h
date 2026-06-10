/*
 * ConvertDecodedFrame — the ONE CPU decoded-frame → BGRA conversion core.
 *
 * Shared by the three decode paths that previously each carried their own
 * copy of this logic (and diverged — see the packedTiles/contentHeight bug):
 *   - MediaPool::decodeFrame            (urgent/scrub decode, MediaPoolDecode.cpp)
 *   - MediaPool::convertDecodedToCache  (prefetch workers, MediaPoolPrefetchConvert.cpp)
 *   - MediaPool::loopPreDecodeWorker    (loop pre-decode, MediaPoolPrefetchLoop.cpp)
 *
 * What it does, identically for all callers:
 *   1. BGRA passthrough: when the source is already BGRA at target size,
 *      row-copy honoring the source linesize.
 *   2. Otherwise sws_scale → BGRA with a cached SwsContext (recreated only
 *      when src/dst dims or src format change) and BT.709 limited→full
 *      colorspace pinned for non-RGB sources — every producer (GPU
 *      Nv12Converter shaders, prefetch, loop pre-decode, urgent decode)
 *      must yield the SAME RGB for the same frame or the frame cache mixes
 *      colorspaces and playback flickers in brightness/saturation.
 *   3. Clears transparent-pixel RGB for native-alpha (non-packed) sources
 *      so GPU linear filtering can't bleed stale color into visible edges.
 *   4. Chroma-keys GREEN-suffixed media (#18FF00 green-screen renders).
 *
 * What stays at the call sites (deliberately):
 *   - Resolution-tier clamping (already unified via
 *     VideoStreamInfo::contentHeight()) and the GPU convert fast paths.
 *   - Failure policy: this returns false on sws_getContext failure; the
 *     urgent path logs-and-serves-empty, prefetch returns nullptr, loop
 *     skips the frame.
 */

#pragma once

#include <filesystem>

namespace rt {

struct DecodedFrame;
struct VideoStreamInfo;
struct CachedFrame;
class PixelBufferPool;

/// Resolve the effective AVPixelFormat (returned as int so FFmpeg headers
/// stay out of this header) of a CPU DecodedFrame: prefers the exact
/// rawFormat when the decoder recorded one, else maps the PixelFormat enum.
[[nodiscard]] int resolveDecodedAvFormat(const DecodedFrame& decoded);

/// Reference bundle over a caller-owned cached SwsContext + its key.
/// MediaEntry and PrefetchDecoderState already carry these six fields (and
/// free the context at their own teardown sites); the loop worker uses
/// locals.  Referencing them in place keeps ownership/teardown untouched.
struct SwsCacheRef {
    void*& ctx;      ///< SwsContext* (type-erased)
    int&   srcW;
    int&   srcH;
    int&   srcFmt;   ///< AVPixelFormat as int
    int&   dstW;
    int&   dstH;
};

/// Convert a CPU DecodedFrame to BGRA into `cached` (fills pixels, width,
/// height, stride — nothing else).  `srcFmt` from resolveDecodedAvFormat().
/// `pool` non-null → pixel buffer comes from the recycling pool; null →
/// plain vector resize.  Returns false when no conversion was possible
/// (sws_getContext failed); `cached` is left with empty pixels in that case.
bool convertDecodedToBgra(const DecodedFrame& decoded,
                          int srcFmt,
                          int dstW, int dstH,
                          const VideoStreamInfo& info,
                          const std::filesystem::path& sourceFile,
                          SwsCacheRef sws,
                          PixelBufferPool* pool,
                          CachedFrame& cached);

} // namespace rt
