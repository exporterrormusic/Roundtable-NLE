/*
 * ThreadedSws.cpp — see ThreadedSws.h.
 */

#include "decode/ThreadedSws.h"

#include <algorithm>
#include <thread>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace rt {

int defaultSwsThreads() noexcept
{
    const unsigned cores = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp(cores, 1u, 8u));
}

#ifdef ROUNDTABLE_HAS_FFMPEG

namespace {

void keepBuffer(void*, uint8_t*) {}

/// Point `frame` at caller-owned planes. sws_scale_frame() references its
/// frames, and av_frame_ref() deep-copies a frame with no buf[0] — and a
/// destination with no buf[0] gets a freshly allocated buffer instead of
/// ours. A non-owning buffer over the first plane avoids both.
bool wrapPlanes(AVFrame* frame, const uint8_t* const data[],
                const int linesize[], int w, int h, int fmt)
{
    frame->buf[0] = av_buffer_create(const_cast<uint8_t*>(data[0]), 1,
                                     &keepBuffer, nullptr, 0);
    if (!frame->buf[0]) return false;
    for (int i = 0; i < 4; ++i) {
        frame->data[i] = const_cast<uint8_t*>(data[i]);
        frame->linesize[i] = linesize[i];
    }
    frame->width = w;
    frame->height = h;
    frame->format = fmt;
    return true;
}

int contextThreads(SwsContext* ctx)
{
    int64_t threads = 1;
    if (av_opt_get_int(ctx, "threads", 0, &threads) < 0) return 1;
    return static_cast<int>(threads);
}

} // namespace

SwsContext* createSwsContext(int srcW, int srcH, int srcFmt,
                             int dstW, int dstH, int dstFmt,
                             int flags, int threads)
{
    if (threads == 1) {
        return sws_getContext(srcW, srcH, static_cast<AVPixelFormat>(srcFmt),
                              dstW, dstH, static_cast<AVPixelFormat>(dstFmt),
                              flags, nullptr, nullptr, nullptr);
    }
    if (threads <= 0) threads = defaultSwsThreads();

    SwsContext* ctx = sws_alloc_context();
    if (!ctx) return nullptr;
    const bool configured =
        av_opt_set_int(ctx, "srcw", srcW, 0) >= 0 &&
        av_opt_set_int(ctx, "srch", srcH, 0) >= 0 &&
        av_opt_set_int(ctx, "src_format", srcFmt, 0) >= 0 &&
        av_opt_set_int(ctx, "dstw", dstW, 0) >= 0 &&
        av_opt_set_int(ctx, "dsth", dstH, 0) >= 0 &&
        av_opt_set_int(ctx, "dst_format", dstFmt, 0) >= 0 &&
        av_opt_set_int(ctx, "sws_flags", flags, 0) >= 0 &&
        av_opt_set_int(ctx, "threads", threads, 0) >= 0;
    if (!configured || sws_init_context(ctx, nullptr, nullptr) < 0) {
        sws_freeContext(ctx);
        return nullptr;
    }
    return ctx;
}

int swsScaleWhole(SwsContext* ctx,
                  const uint8_t* const srcData[], const int srcLinesize[],
                  int srcW, int srcH, int srcFmt,
                  uint8_t* const dstData[], const int dstLinesize[],
                  int dstW, int dstH, int dstFmt)
{
    if (!ctx) return AVERROR(EINVAL);
    if (contextThreads(ctx) == 1)
        return sws_scale(ctx, srcData, srcLinesize, 0, srcH, dstData, dstLinesize);

    AVFrame* src = av_frame_alloc();
    AVFrame* dst = av_frame_alloc();
    int ret = AVERROR(ENOMEM);
    if (src && dst &&
        wrapPlanes(src, srcData, srcLinesize, srcW, srcH, srcFmt) &&
        wrapPlanes(dst, dstData, dstLinesize, dstW, dstH, dstFmt)) {
        ret = sws_scale_frame(ctx, dst, src);
        if (ret >= 0) ret = dstH;
    }
    av_frame_free(&src);
    av_frame_free(&dst);
    return ret;
}

#else // !ROUNDTABLE_HAS_FFMPEG

SwsContext* createSwsContext(int, int, int, int, int, int, int, int) { return nullptr; }
int swsScaleWhole(SwsContext*, const uint8_t* const[], const int[], int, int, int,
                  uint8_t* const[], const int[], int, int, int) { return -1; }

#endif

} // namespace rt
