/*
 * test_threaded_sws.cpp — threaded swscale (ThreadedSws.h) produces exactly
 * the single-threaded output for the conversions the render pipeline runs:
 * decoded YUV → BGRA with each source colour matrix/range, and composited
 * BGRA → the encoders' YUV formats.
 *
 * Also prints the speed-up for a 4K BGRA → YUV420P frame (no assertion —
 * timing on a shared CI machine is noise).
 */
#include <gtest/gtest.h>

#include "decode/ThreadedSws.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

using namespace rt;

#ifdef ROUNDTABLE_HAS_FFMPEG
namespace {

struct Image {
    uint8_t* data[4]{};
    int linesize[4]{};
    int w = 0, h = 0, fmt = AV_PIX_FMT_NONE;

    Image(int width, int height, int format) : w(width), h(height), fmt(format)
    {
        av_image_alloc(data, linesize, w, h, static_cast<AVPixelFormat>(fmt), 64);
    }
    ~Image() { av_freep(&data[0]); }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    /// Deterministic texture with gradients and hard edges in every plane.
    void fill()
    {
        int planes = av_pix_fmt_count_planes(static_cast<AVPixelFormat>(fmt));
        for (int p = 0; p < planes; ++p) {
            const int bytes = av_image_get_linesize(static_cast<AVPixelFormat>(fmt), w, p);
            int rows = h;
            if (p == 1 || p == 2) {
                const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(fmt));
                rows = AV_CEIL_RSHIFT(h, d->log2_chroma_h);
            }
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < bytes; ++x)
                    data[p][y * linesize[p] + x] =
                        static_cast<uint8_t>((x * 7 + y * 13 + p * 61 + ((x / 9 + y / 5) & 1) * 90) & 0xff);
        }
        // 10-bit little-endian samples must stay within 10 bits.
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(fmt));
        if (d->comp[0].depth > 8 && d->comp[0].depth <= 16) {
            for (int p = 0; p < planes; ++p) {
                const int bytes = av_image_get_linesize(static_cast<AVPixelFormat>(fmt), w, p);
                int rows = (p == 1 || p == 2) ? AV_CEIL_RSHIFT(h, d->log2_chroma_h) : h;
                for (int y = 0; y < rows; ++y)
                    for (int x = 1; x < bytes; x += 2)
                        data[p][y * linesize[p] + x] &= 0x03;
            }
        }
    }

    bool sameAs(const Image& o) const
    {
        const int planes = av_pix_fmt_count_planes(static_cast<AVPixelFormat>(fmt));
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(fmt));
        for (int p = 0; p < planes; ++p) {
            const int bytes = av_image_get_linesize(static_cast<AVPixelFormat>(fmt), w, p);
            const int rows = (p == 1 || p == 2) ? AV_CEIL_RSHIFT(h, d->log2_chroma_h) : h;
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < bytes; ++x)
                    if (data[p][y * linesize[p] + x] != o.data[p][y * o.linesize[p] + x])
                        return false;
        }
        return true;
    }
};

/// Convert `src` to `dstFmt` with the given thread count and colourspace.
bool convert(const Image& src, Image& dst, int threads, int flags,
             int colorspace, int srcRange)
{
    SwsContext* ctx = createSwsContext(src.w, src.h, src.fmt, dst.w, dst.h, dst.fmt,
                                       flags, threads);
    if (!ctx) return false;
    if (colorspace >= 0) {
        const int* t = sws_getCoefficients(colorspace);
        sws_setColorspaceDetails(ctx, t, srcRange, t, 1, 0, 1 << 16, 1 << 16);
    }
    const int rows = swsScaleWhole(ctx, src.data, src.linesize, src.w, src.h, src.fmt,
                                   dst.data, dst.linesize, dst.w, dst.h, dst.fmt);
    sws_freeContext(ctx);
    return rows == dst.h;
}

struct Case {
    const char* name;
    int srcFmt, dstFmt, flags, colorspace, srcRange;
};

} // namespace

TEST(ThreadedSws, MatchesSingleThreadedOutput)
{
    constexpr int kW = 1920, kH = 1080;
    const Case cases[] = {
        // Decode side (convertDecodedToBgra): every matrix/range it sets.
        {"nv12 709 limited",     AV_PIX_FMT_NV12,        AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, SWS_CS_ITU709, 0},
        {"yuv420p 601 full",     AV_PIX_FMT_YUV420P,     AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, SWS_CS_ITU601, 1},
        {"yuv422p10 709",        AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, SWS_CS_ITU709, 0},
        {"p010 2020",            AV_PIX_FMT_P010LE,      AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, SWS_CS_BT2020, 0},
        // Encode side (FfmpegEncoderBase): BGRA → each encoder format.
        {"bgra -> yuv420p",      AV_PIX_FMT_BGRA, AV_PIX_FMT_YUV420P,     SWS_BILINEAR, -1, 0},
        {"bgra -> yuv422p10",    AV_PIX_FMT_BGRA, AV_PIX_FMT_YUV422P10LE, SWS_BILINEAR, -1, 0},
        {"bgra -> yuva444p10",   AV_PIX_FMT_BGRA, AV_PIX_FMT_YUVA444P10LE, SWS_BILINEAR, -1, 0},
        {"bgra -> yuv422p",      AV_PIX_FMT_BGRA, AV_PIX_FMT_YUV422P,     SWS_BILINEAR, -1, 0},
    };
    for (const Case& c : cases) {
        Image src(kW, kH, c.srcFmt);
        ASSERT_TRUE(src.data[0]) << c.name;
        src.fill();
        Image single(kW, kH, c.dstFmt), threaded(kW, kH, c.dstFmt);
        ASSERT_TRUE(convert(src, single, 1, c.flags, c.colorspace, c.srcRange)) << c.name;
        ASSERT_TRUE(convert(src, threaded, 8, c.flags, c.colorspace, c.srcRange)) << c.name;
        EXPECT_TRUE(threaded.sameAs(single)) << c.name;
    }
}

TEST(ThreadedSws, Downscale)
{
    // The tier convert also resizes (Full → Half); slice threads must not
    // change filtered rows at the slice seams.
    Image src(1920, 1080, AV_PIX_FMT_NV12);
    src.fill();
    Image single(960, 540, AV_PIX_FMT_BGRA), threaded(960, 540, AV_PIX_FMT_BGRA);
    ASSERT_TRUE(convert(src, single, 1, SWS_FAST_BILINEAR, SWS_CS_ITU709, 0));
    ASSERT_TRUE(convert(src, threaded, 8, SWS_FAST_BILINEAR, SWS_CS_ITU709, 0));
    EXPECT_TRUE(threaded.sameAs(single));
}

TEST(ThreadedSws, FourKBgraToYuvSpeed)
{
    Image src(3840, 2160, AV_PIX_FMT_BGRA);
    src.fill();
    Image dst(3840, 2160, AV_PIX_FMT_YUV420P);
    const auto time = [&](int threads) {
        SwsContext* ctx = createSwsContext(src.w, src.h, src.fmt, dst.w, dst.h, dst.fmt,
                                           SWS_BILINEAR, threads);
        if (!ctx) return -1.0;
        const auto t0 = std::chrono::steady_clock::now();
        constexpr int kRuns = 10;
        for (int i = 0; i < kRuns; ++i)
            swsScaleWhole(ctx, src.data, src.linesize, src.w, src.h, src.fmt,
                          dst.data, dst.linesize, dst.w, dst.h, dst.fmt);
        sws_freeContext(ctx);
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0).count() / kRuns;
    };
    const double one = time(1);
    const double many = time(0);
    ASSERT_GT(one, 0.0);
    ASSERT_GT(many, 0.0);
    std::printf("[ThreadedSws] 4K BGRA->YUV420P: 1 thread %.2f ms, %d threads %.2f ms (%.1fx)\n",
                one, defaultSwsThreads(), many, one / many);
}
#endif
