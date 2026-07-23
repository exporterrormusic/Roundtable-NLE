/*
 * MediaPoolPrefetchConvert.cpp — Frame conversion logic extracted from
 * MediaPoolPrefetch.cpp.
 *
 * Contains: convertDecodedToCache(), reopenPrefetchDecoder().
 */

#include "MediaPool.h"
#include "MediaPoolPrefetchInternal.h"
#include "PathUtils.h"
#include "decode/ConvertDecodedFrame.h"
#include "cache/FrameContentBounds.h"

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
        const AVPixelFormat srcFmt =
            static_cast<AVPixelFormat>(resolveDecodedAvFormat(decoded));

        const int w = static_cast<int>(decoded.width);
        const int h = static_cast<int>(decoded.height);

        const ResolutionTier decodeTier = task.exportFullRes
            ? ResolutionTier::Full : task.tier;
        const auto tierSize = resolutionTierDimensions(w, h, decodeTier);
        const int dstW = tierSize.width;
        const int dstH = tierSize.height;
        // NOTE: ProRes 4444 (yuva444p12le) GPU convert is intentionally NOT
        // wired here — the prefetch path runs on MULTIPLE worker threads, and
        // the shared Nv12Converter's command pool (GpuContext::m_cmdPool) is
        // NOT safe for concurrent use.  ProRes goes through the shared CPU
        // core below (correct, just slower).  The GPU path will be added via
        // the per-worker convertDecodedToCacheGpu path instead.
        if (!convertDecodedToBgra(
                decoded, static_cast<int>(srcFmt), dstW, dstH,
                task.info, task.filePath,
                SwsCacheRef{state.swsCtx, state.swsSrcW, state.swsSrcH,
                            state.swsSrcFmt, state.swsDstW, state.swsDstH},
                pool.get(), *cached))
        {
            return nullptr;   // sws_getContext failed
        }
#else
        return nullptr;
#endif
    }

    if (cached->pinned && !cached->pixels.empty())
        computeBgraContentBounds(*cached);
    m_perf.cpuConvertDecoded.fetch_add(1, std::memory_order_relaxed);
    return cached;
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
