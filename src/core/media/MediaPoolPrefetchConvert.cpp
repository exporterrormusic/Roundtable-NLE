/*
 * MediaPoolPrefetchConvert.cpp — Frame conversion logic extracted from
 * MediaPoolPrefetch.cpp.
 *
 * Contains: convertDecodedToCache(), reopenPrefetchDecoder().
 */

#include "MediaPool.h"
#include "MediaPoolPrefetchInternal.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>
#include <cstring>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace rt {

// Consolidated: reopen decoder with specified mode (software or hardware).
bool reopenPrefetchDecoder(PrefetchDecoderState& state,
                            const PrefetchTask& task,
                            bool forceSoftware)
{
    auto newDecoder = std::make_unique<VideoDecoder>();
    const int maxThreads = forceSoftware ? 2 : 0;
    if (!newDecoder->open(task.filePath, /*forceSoftware=*/forceSoftware, /*maxThreads=*/maxThreads)) {
        const char* mode = forceSoftware ? "software" : "hardware";
        spdlog::warn("MediaPool prefetch: failed to reopen '{}' in {} mode after slow path",
                     pathToUtf8(task.filePath.filename()), mode);
        return false;
    }

    state.decoder = std::move(newDecoder);
    state.lastDecodedFrame = -1;
#ifdef ROUNDTABLE_HAS_FFMPEG
    resetPrefetchConversionState(state);
#endif
    return true;
}

std::shared_ptr<CachedFrame> MediaPool::convertDecodedToCache(
    PrefetchDecoderState& state, const PrefetchTask& task,
    DecodedFrame& decoded, int64_t frameNumber)
{
    if (decoded.isHardware) {
        DecodedFrame cpuFrame;
        if (!state.decoder->transferHardwareFrame(decoded, cpuFrame))
            return nullptr;
        decoded = std::move(cpuFrame);
    }

    auto pool = m_pixelPool;
    auto cached = std::shared_ptr<CachedFrame>(new CachedFrame, [pool](CachedFrame* f) {
        pool->recycle(std::move(f->pixels));
        delete f;
    });
    cached->mediaId     = task.handle;
    cached->frameNumber = frameNumber;
    cached->width       = decoded.width;
    cached->height      = decoded.height;
    cached->tier        = task.tier;
    cached->isKeyframe  = decoded.isKeyframe;
    cached->timestamp   = decoded.timestamp;
    cached->pinned      = (task.info.frameCount <= 1);
    cached->isLoopFrame = task.isLoop;
    cached->origin      = ConverterOrigin::CpuSws;

    if (decoded.data[0] && decoded.width > 0 && decoded.height > 0) {
#ifdef ROUNDTABLE_HAS_FFMPEG
        AVPixelFormat srcFmt = AV_PIX_FMT_YUV420P;
        if (decoded.rawFormat >= 0) {
            srcFmt = static_cast<AVPixelFormat>(decoded.rawFormat);
        } else {
            switch (decoded.format) {
                case PixelFormat::YUV420P: srcFmt = AV_PIX_FMT_YUV420P; break;
                case PixelFormat::NV12:    srcFmt = AV_PIX_FMT_NV12;    break;
                case PixelFormat::BGRA:    srcFmt = AV_PIX_FMT_BGRA;    break;
                case PixelFormat::RGBA:    srcFmt = AV_PIX_FMT_RGBA;    break;
                default:                   srcFmt = AV_PIX_FMT_YUV420P; break;
            }
        }

        const int w = static_cast<int>(decoded.width);
        const int h = static_cast<int>(decoded.height);

        int maxDim = 1920;
        switch (task.tier) {
            case ResolutionTier::Half:    maxDim =  960; break;
            case ResolutionTier::Quarter: maxDim =  480; break;
            default:                      maxDim = 1920; break;
        }
        const int contentH = (task.packedAlpha && h > 1) ? h / 2 : h;
        int dstW = w, dstH = h;
        // exportFullRes (export decode) skips the tier cap → native resolution.
        if (!task.exportFullRes && (w > maxDim || contentH > maxDim)) {
            const float scale = std::min(
                static_cast<float>(maxDim) / w,
                static_cast<float>(maxDim) / contentH);
            dstW = std::max(2, static_cast<int>(w * scale) & ~1);
            dstH = std::max(2, static_cast<int>(h * scale) & ~1);
        }

        const bool needsResize = (dstW != w || dstH != h);

        // NOTE: ProRes 4444 (yuva444p12le) GPU convert is intentionally NOT
        // wired here — the prefetch path runs on MULTIPLE worker threads, and
        // the shared Nv12Converter's command pool (GpuContext::m_cmdPool) is
        // NOT safe for concurrent use.  ProRes goes through CPU sws_scale below
        // (correct, just slower).  The GPU path will be added via the
        // per-worker convertDecodedToCacheGpu path instead.  See git history.
        if (srcFmt == AV_PIX_FMT_BGRA && !needsResize) {
            const uint32_t stride = static_cast<uint32_t>(decoded.linesize[0]);
            cached->stride = w * 4;
            cached->pixels = pool->acquire(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                std::memcpy(cached->pixels.data() + y * cached->stride,
                            decoded.data[0] + y * stride,
                            static_cast<size_t>(w) * 4);
            }
        } else {
            SwsContext* sws = nullptr;
            if (state.swsCtx && state.swsSrcW == w && state.swsSrcH == h &&
                state.swsSrcFmt == static_cast<int>(srcFmt) &&
                state.swsDstW == dstW && state.swsDstH == dstH) {
                sws = static_cast<SwsContext*>(state.swsCtx);
            } else {
                if (state.swsCtx) {
                    sws_freeContext(static_cast<SwsContext*>(state.swsCtx));
                    state.swsCtx = nullptr;
                }
                sws = sws_getContext(
                    w, h, srcFmt,
                    dstW, dstH, AV_PIX_FMT_BGRA,
                    SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                if (sws) {
                    // Pin BT.709 limited→full so this CPU path produces the
                    // SAME RGB as the GPU Nv12Converter shaders.  Without it
                    // swscale defaults to BT.601, and frames for the same
                    // (handle,frame,tier) filled by the GPU vs CPU path
                    // differ → brightness/saturation flicker once the cache
                    // churns. srcRange=0 (studio swing in), dstRange=1 (full RGB).
                    if (srcFmt != AV_PIX_FMT_BGRA && srcFmt != AV_PIX_FMT_RGBA) {
                        const int* t = sws_getCoefficients(SWS_CS_ITU709);
                        sws_setColorspaceDetails(sws, t, /*srcRange=*/0,
                                                 t, /*dstRange=*/1,
                                                 0, 1 << 16, 1 << 16);
                    }
                    state.swsCtx    = sws;
                    state.swsSrcW   = w;
                    state.swsSrcH   = h;
                    state.swsSrcFmt = static_cast<int>(srcFmt);
                    state.swsDstW   = dstW;
                    state.swsDstH   = dstH;
                }
            }

            if (sws) {
                cached->width  = static_cast<uint32_t>(dstW);
                cached->height = static_cast<uint32_t>(dstH);
                cached->stride = static_cast<uint32_t>(dstW) * 4;
                cached->pixels = pool->acquire(static_cast<size_t>(dstW) * dstH * 4);

                uint8_t* dstData[1] = { cached->pixels.data() };
                int dstLinesize[1] = { static_cast<int>(cached->stride) };

                sws_scale(sws,
                          decoded.data, decoded.linesize,
                          0, h,
                          dstData, dstLinesize);
            } else {
                return nullptr;
            }
        }

        if (task.info.hasAlpha && !task.packedAlpha && !cached->pixels.empty()) {
            clearTransparentPixelRGB(cached->pixels.data(),
                                     cached->pixels.size() / 4);
        }

        // ── Chroma-key green-screen media (#18FF00) ───────────────────
        if (!cached->pixels.empty()) {
            std::string fn = pathToUtf8(task.filePath.filename());
            std::transform(fn.begin(), fn.end(), fn.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (fn.find("GREEN") != std::string::npos) {
                chromaKeyInPlace(cached->pixels.data(),
                                 cached->pixels.size() / 4);
            }
        }
#else
        return nullptr;
#endif
    }

    m_perf.cpuConvertDecoded.fetch_add(1, std::memory_order_relaxed);
    return cached;
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
