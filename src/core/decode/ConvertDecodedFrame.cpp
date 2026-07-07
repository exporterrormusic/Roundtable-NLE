/*
 * ConvertDecodedFrame.cpp — shared CPU decoded-frame → BGRA conversion core.
 * See ConvertDecodedFrame.h for the contract and the list of callers.
 */

#include "decode/ConvertDecodedFrame.h"
#include "PathUtils.h"

#include "cache/FrameCache.h"     // CachedFrame
#include "decode/PixelOps.h"      // clearTransparentPixelRGB + chromaKeyInPlace
#include "playback/MediaPool.h"      // PixelBufferPool
#include "decode/VideoDecoder.h"   // DecodedFrame, VideoStreamInfo

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

int resolveDecodedAvFormat(const DecodedFrame& decoded)
{
    if (decoded.rawFormat >= 0)
        return decoded.rawFormat;
    switch (decoded.format) {
        case PixelFormat::YUV420P: return AV_PIX_FMT_YUV420P;
        case PixelFormat::NV12:    return AV_PIX_FMT_NV12;
        case PixelFormat::BGRA:    return AV_PIX_FMT_BGRA;
        case PixelFormat::RGBA:    return AV_PIX_FMT_RGBA;
        default:                   return AV_PIX_FMT_YUV420P;
    }
}

namespace {

/// Acquire the pixel buffer: recycling pool when available, plain resize
/// otherwise (the pool path is used by the high-churn prefetch workers).
void acquirePixels(CachedFrame& cached, PixelBufferPool* pool, size_t bytes)
{
    if (pool)
        cached.pixels = pool->acquire(bytes);
    else
        cached.pixels.resize(bytes);
}

} // namespace

bool convertDecodedToBgra(const DecodedFrame& decoded,
                          int srcFmt,
                          int dstW, int dstH,
                          const VideoStreamInfo& info,
                          const std::filesystem::path& sourceFile,
                          SwsCacheRef sws,
                          PixelBufferPool* pool,
                          CachedFrame& cached)
{
    const int w = static_cast<int>(decoded.width);
    const int h = static_cast<int>(decoded.height);
    const bool needsResize = (dstW != w || dstH != h);

    if (srcFmt == AV_PIX_FMT_BGRA && !needsResize) {
        // ── Already BGRA at target size — direct row copy ───────────────
        const size_t srcStride = static_cast<size_t>(decoded.linesize[0]);
        cached.width  = static_cast<uint32_t>(w);
        cached.height = static_cast<uint32_t>(h);
        cached.stride = static_cast<uint32_t>(w) * 4;
        acquirePixels(cached, pool, static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            std::memcpy(cached.pixels.data() + static_cast<size_t>(y) * cached.stride,
                        decoded.data[0] + static_cast<size_t>(y) * srcStride,
                        static_cast<size_t>(w) * 4);
        }
    } else {
        // ── sws_scale → BGRA with cached context ────────────────────────
        SwsContext* ctx = nullptr;
        if (sws.ctx && sws.srcW == w && sws.srcH == h &&
            sws.srcFmt == srcFmt && sws.dstW == dstW && sws.dstH == dstH) {
            ctx = static_cast<SwsContext*>(sws.ctx);
        } else {
            if (sws.ctx) {
                sws_freeContext(static_cast<SwsContext*>(sws.ctx));
                sws.ctx = nullptr;
            }
            ctx = sws_getContext(
                w, h, static_cast<AVPixelFormat>(srcFmt),
                dstW, dstH, AV_PIX_FMT_BGRA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (ctx) {
                // Per-source colourspace (Phase 4.1).  swscale defaults to
                // BT.601 for everything; instead we derive the matrix + range
                // from the source's tags (resolveColorConversion) so SD/BT.601,
                // full-range JPEG, and BT.2020 content convert correctly rather
                // than all being forced through 709-limited.  The GPU convert
                // path runs only for the 709-limited-SDR case (see the gate in
                // convertDecodedToCacheGpu) and defers everything else here, so
                // a clip's frames never mix matrices across the CPU/GPU cache
                // boundary.  dstRange=1 = full-range RGB out (unchanged).
                if (srcFmt != AV_PIX_FMT_BGRA && srcFmt != AV_PIX_FMT_RGBA) {
                    const ColorConversion cc = resolveColorConversion(info);
                    int swsCs = SWS_CS_ITU709;
                    switch (cc.matrix) {
                        case ColorMatrix::BT601:  swsCs = SWS_CS_ITU601;  break;
                        case ColorMatrix::BT2020: swsCs = SWS_CS_BT2020;  break;
                        default:                  swsCs = SWS_CS_ITU709;  break;
                    }
                    const int* t = sws_getCoefficients(swsCs);
                    sws_setColorspaceDetails(ctx, t, /*srcRange=*/cc.fullRange ? 1 : 0,
                                             t, /*dstRange=*/1,
                                             0, 1 << 16, 1 << 16);
                }
                sws.ctx    = ctx;
                sws.srcW   = w;
                sws.srcH   = h;
                sws.srcFmt = srcFmt;
                sws.dstW   = dstW;
                sws.dstH   = dstH;
            }
        }
        if (!ctx)
            return false;   // caller decides: log / skip frame / drop

        cached.width  = static_cast<uint32_t>(dstW);
        cached.height = static_cast<uint32_t>(dstH);
        cached.stride = static_cast<uint32_t>(dstW) * 4;
        // swscale writes the FINAL destination row in chunks of up to 64
        // bytes (SIMD store width), so a row narrower than that overruns an
        // exact-fit allocation (measured: dstW < 16 → up to 44 B past the
        // end). Unreachable at current resolution tiers, but one tier-clamp
        // change away — pad so the tail chunk always lands in-bounds.
        const size_t rowBytes = static_cast<size_t>(dstW) * 4;
        acquirePixels(cached, pool,
                      static_cast<size_t>(dstW) * dstH * 4
                          + (rowBytes < 64 ? 64 - rowBytes : 0));

        uint8_t* dstData[1] = { cached.pixels.data() };
        int dstLinesize[1]  = { static_cast<int>(cached.stride) };
        sws_scale(ctx, decoded.data, decoded.linesize, 0, h,
                  dstData, dstLinesize);
    }

    // ── Clear transparent-pixel RGB for native-alpha video ──────────────
    // sws_scale produces straight-alpha BGRA; transparent pixels may carry
    // non-zero RGB that GPU linear filtering would bleed into visible
    // edges.  Skipped for packed-alpha sources: their alpha lives in a
    // separate tile region, so clearing on A=0 would nuke real color.
    if (info.hasAlpha && !info.packedAlpha && !cached.pixels.empty()) {
        clearTransparentPixelRGB(cached.pixels.data(),
                                 cached.pixels.size() / 4);
    }

    // ── Chroma-key green-screen media (#18FF00) ──────────────────────────
    // GREEN-suffixed files are H.264 renders of originally-alpha content on
    // a chroma green background; key them here so the rest of the pipeline
    // (compositor, thumbnails, library) never sees the green.
    if (!cached.pixels.empty()) {
        std::string fn = pathToUtf8(sourceFile.filename());
        std::transform(fn.begin(), fn.end(), fn.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        if (fn.find("GREEN") != std::string::npos) {
            chromaKeyInPlace(cached.pixels.data(), cached.pixels.size() / 4);
        }
    }

    return true;
}

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
