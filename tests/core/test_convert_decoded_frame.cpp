/*
 * test_convert_decoded_frame.cpp — the ONE CPU decoded-frame → BGRA core.
 *
 * Exercises convertDecodedToBgra() / resolveDecodedAvFormat()
 * (decode/ConvertDecodedFrame.h), the conversion shared by urgent decode,
 * prefetch workers and loop pre-decode.  All inputs are tiny synthetic
 * DecodedFrames (no files, no decoder, no GPU).  The contract under test:
 *   - BGRA-at-target-size passthrough row-copies honoring the source
 *     linesize (padding never leaks into the tightly-packed output);
 *   - everything else goes through sws_scale → BGRA with the colourspace
 *     pinned per-source via resolveColorConversion — BT.601 and BT.709
 *     sources MUST produce different RGB for the same YUV or the frame
 *     cache mixes matrices (brightness/saturation flicker);
 *   - limited-range sources expand to full-swing RGB, full-range tags are
 *     honoured verbatim;
 *   - native-alpha (non-packed) sources get transparent-pixel RGB cleared;
 *     packed-alpha sources are left alone (their alpha lives in a tile);
 *   - GREEN-suffixed filenames are chroma-keyed (#18FF00), others never;
 *   - the caller-owned SwsContext cache is reused for identical geometry
 *     and rebuilt when it changes; sws failure returns false with empty
 *     pixels.
 *
 * Synthetic source planes are 64-byte aligned with 64-aligned linesizes and
 * tail padding, mirroring FFmpeg's own allocation (av_frame_get_buffer):
 * swscale's SIMD paths read whole aligned chunks and go out of bounds on
 * tightly-packed tiny planes (observed as nondeterministic crashes with
 * this repo's FFmpeg 7.x).  Real DecodedFrames are always FFmpeg-allocated,
 * so padding is a legitimate precondition of the production input.  The
 * DESTINATION stays exactly w*h*4 with stride w*4 — that is precisely what
 * production allocates, so dst-overrun coverage is retained.
 *
 * Solid-colour frames make the expectations exact up to swscale's
 * fixed-point rounding (tolerance ±4): bilinear over identical samples is
 * the identity, so chroma upsampling cannot smear anything.
 */

#include <gtest/gtest.h>

#include "cache/FrameCache.h"
#include "decode/ConvertDecodedFrame.h"
#include "cache/FrameContentBounds.h"
#include "decode/VideoDecoder.h"
#include "playback/MediaPool.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#ifdef ROUNDTABLE_HAS_FFMPEG

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace rt {
namespace {

// ── Synthetic frames (FFmpeg-style plane allocation) ────────────────────────

/// Owns the plane storage a DecodedFrame points into.
struct SyntheticFrame {
    std::array<std::vector<uint8_t>, 4> buffers;
    DecodedFrame df;

    /// Allocate plane `idx`: 64-aligned base, 64-aligned linesize, 64-byte
    /// tail pad (mirrors av_frame_get_buffer(frame, 64)); filled with `fill`.
    uint8_t* setPlane(int idx, int rowBytes, int rows, uint8_t fill)
    {
        const int linesize = (rowBytes + 63) & ~63;
        auto& buf = buffers[static_cast<size_t>(idx)];
        buf.assign(static_cast<size_t>(linesize) * rows + 128, fill);
        const auto addr = reinterpret_cast<uintptr_t>(buf.data());
        auto* base = reinterpret_cast<uint8_t*>((addr + 63) & ~uintptr_t{63});
        df.data[idx]     = base;
        df.linesize[idx] = linesize;
        return base;
    }
};

SyntheticFrame makeYuv420(int w, int h, uint8_t y, uint8_t u, uint8_t v)
{
    SyntheticFrame f;
    const int cW = (w + 1) / 2, cH = (h + 1) / 2;
    f.setPlane(0, w, h, y);
    f.setPlane(1, cW, cH, u);
    f.setPlane(2, cW, cH, v);
    f.df.width  = static_cast<uint32_t>(w);
    f.df.height = static_cast<uint32_t>(h);
    f.df.format = PixelFormat::YUV420P;
    f.df.rawFormat = AV_PIX_FMT_YUV420P;
    return f;
}

SyntheticFrame makeYuva420(int w, int h, uint8_t y, uint8_t u, uint8_t v,
                           uint8_t a)
{
    SyntheticFrame f = makeYuv420(w, h, y, u, v);
    f.setPlane(3, w, h, a);
    f.df.format = PixelFormat::Unknown;       // rawFormat must win
    f.df.rawFormat = AV_PIX_FMT_YUVA420P;
    return f;
}

SyntheticFrame makeNv12(int w, int h, uint8_t y, uint8_t u, uint8_t v)
{
    SyntheticFrame f;
    const int cH = (h + 1) / 2;
    f.setPlane(0, w, h, y);
    uint8_t* uv = f.setPlane(1, w, cH, u);    // interleaved U,V pairs
    for (int row = 0; row < cH; ++row) {
        uint8_t* p = uv + static_cast<size_t>(row) * f.df.linesize[1];
        for (int i = 0; i + 1 < f.df.linesize[1]; i += 2) {
            p[i]     = u;
            p[i + 1] = v;
        }
    }
    f.df.width  = static_cast<uint32_t>(w);
    f.df.height = static_cast<uint32_t>(h);
    f.df.format = PixelFormat::NV12;
    f.df.rawFormat = AV_PIX_FMT_NV12;
    return f;
}

/// BGRA frame; pixel values from `at(x,y)`.  The aligned linesize exceeds
/// w*4, so the row-pad region (sentinel 0xEE) proves stride handling.
template <typename Fn>
SyntheticFrame makeBgra(int w, int h, Fn at)
{
    SyntheticFrame f;
    uint8_t* base = f.setPlane(0, w * 4, h, 0xEE);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t px = at(x, y);
            uint8_t* p = base + static_cast<size_t>(y) * f.df.linesize[0]
                              + static_cast<size_t>(x) * 4;
            p[0] = static_cast<uint8_t>(px);          // B
            p[1] = static_cast<uint8_t>(px >> 8);     // G
            p[2] = static_cast<uint8_t>(px >> 16);    // R
            p[3] = static_cast<uint8_t>(px >> 24);    // A
        }
    f.df.width  = static_cast<uint32_t>(w);
    f.df.height = static_cast<uint32_t>(h);
    f.df.format = PixelFormat::BGRA;
    f.df.rawFormat = AV_PIX_FMT_BGRA;
    return f;
}

// ── Caller-owned sws cache (mirrors MediaEntry / PrefetchDecoderState) ──────

struct SwsCache {
    void* ctx{nullptr};
    int   srcW{0}, srcH{0}, srcFmt{-1}, dstW{0}, dstH{0};
    SwsCacheRef ref() { return {ctx, srcW, srcH, srcFmt, dstW, dstH}; }
    ~SwsCache()
    {
        if (ctx) sws_freeContext(static_cast<SwsContext*>(ctx));
    }
};

VideoStreamInfo makeInfo(int w, int h,
                         ColorMatrix m = ColorMatrix::Unspecified,
                         ColorRange r  = ColorRange::Unspecified)
{
    VideoStreamInfo info;
    info.width  = static_cast<uint32_t>(w);
    info.height = static_cast<uint32_t>(h);
    info.colorMatrix = m;
    info.colorRange  = r;
    return info;
}

bool convert(const SyntheticFrame& f, const VideoStreamInfo& info,
             const std::filesystem::path& file, int dstW, int dstH,
             SwsCache& sws, CachedFrame& out, PixelBufferPool* pool = nullptr)
{
    return convertDecodedToBgra(f.df, resolveDecodedAvFormat(f.df),
                                dstW, dstH, info, file, sws.ref(), pool, out);
}

uint32_t pixelAt(const CachedFrame& c, int x, int y)
{
    const uint8_t* p = c.pixels.data()
                     + static_cast<size_t>(y) * c.stride + static_cast<size_t>(x) * 4;
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

/// Every pixel within `tol` of (r,g,b,a)?  Reports the first offender.
::testing::AssertionResult uniformNear(const CachedFrame& c,
                                       int r, int g, int b, int a, int tol)
{
    for (uint32_t y = 0; y < c.height; ++y)
        for (uint32_t x = 0; x < c.width; ++x) {
            const uint32_t px = pixelAt(c, static_cast<int>(x), static_cast<int>(y));
            const int pb =  px        & 0xFF;
            const int pg = (px >>  8) & 0xFF;
            const int pr = (px >> 16) & 0xFF;
            const int pa = (px >> 24) & 0xFF;
            if (std::abs(pr - r) > tol || std::abs(pg - g) > tol ||
                std::abs(pb - b) > tol || std::abs(pa - a) > tol)
                return ::testing::AssertionFailure()
                    << "pixel (" << x << "," << y << ") = RGBA(" << pr << ","
                    << pg << "," << pb << "," << pa << "), expected near ("
                    << r << "," << g << "," << b << "," << a << ") tol " << tol;
        }
    return ::testing::AssertionSuccess();
}

// ── resolveDecodedAvFormat ───────────────────────────────────────────────────

TEST(ResolveDecodedAvFormat, RawFormatWinsOverEnum)
{
    DecodedFrame df;
    df.format = PixelFormat::YUV420P;
    df.rawFormat = AV_PIX_FMT_YUVA420P;
    EXPECT_EQ(resolveDecodedAvFormat(df), AV_PIX_FMT_YUVA420P);
}

TEST(ResolveDecodedAvFormat, EnumMappingsAndDefault)
{
    DecodedFrame df;
    df.rawFormat = -1;
    df.format = PixelFormat::YUV420P;
    EXPECT_EQ(resolveDecodedAvFormat(df), AV_PIX_FMT_YUV420P);
    df.format = PixelFormat::NV12;
    EXPECT_EQ(resolveDecodedAvFormat(df), AV_PIX_FMT_NV12);
    df.format = PixelFormat::BGRA;
    EXPECT_EQ(resolveDecodedAvFormat(df), AV_PIX_FMT_BGRA);
    df.format = PixelFormat::RGBA;
    EXPECT_EQ(resolveDecodedAvFormat(df), AV_PIX_FMT_RGBA);
    df.format = PixelFormat::Unknown;   // last-resort default
    EXPECT_EQ(resolveDecodedAvFormat(df), AV_PIX_FMT_YUV420P);
}

// ── BGRA passthrough ─────────────────────────────────────────────────────────

TEST(ConvertDecodedFrame, BgraPassthroughHonorsSourceStride)
{
    // 4×3 BGRA: source linesize is 64 (aligned) vs 16 payload bytes per row;
    // the copy must take exactly w*4 bytes per row and the output must be
    // tightly packed — the 0xEE pad sentinel must never leak through.
    const auto at = [](int x, int y) {
        return 0xFF400000u | (static_cast<uint32_t>(y) << 8)
                           | static_cast<uint32_t>(x);
    };
    const auto f = makeBgra(4, 3, at);
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, makeInfo(4, 3), "clip.mp4", 4, 3, sws, out));

    EXPECT_EQ(out.width, 4u);
    EXPECT_EQ(out.height, 3u);
    EXPECT_EQ(out.stride, 16u);                       // w*4, no pad
    ASSERT_EQ(out.pixels.size(), 4u * 3u * 4u);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 4; ++x)
            EXPECT_EQ(pixelAt(out, x, y), at(x, y)) << "(" << x << "," << y << ")";
    EXPECT_EQ(sws.ctx, nullptr);                      // no sws for passthrough
}

// ── sws path: range and matrix handling ─────────────────────────────────────

TEST(ConvertDecodedFrame, LimitedRangeGreyExpandsToFullSwing)
{
    // BT.709 limited Y=126 (mid grey): 1.1644×(126−16) = 128.1.
    const auto f = makeYuv420(16, 16, 126, 128, 128);
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, makeInfo(16, 16, ColorMatrix::BT709, ColorRange::Limited),
                        "clip.mp4", 16, 16, sws, out));
    EXPECT_EQ(out.width, 16u);
    EXPECT_EQ(out.height, 16u);
    EXPECT_EQ(out.stride, 64u);
    EXPECT_TRUE(uniformNear(out, 128, 128, 128, 255, 3));
}

TEST(ConvertDecodedFrame, LimitedRangeBlackAndWhiteEndpoints)
{
    SwsCache sws;
    CachedFrame out;
    const auto info = makeInfo(16, 16, ColorMatrix::BT709, ColorRange::Limited);

    const auto black = makeYuv420(16, 16, 16, 128, 128);   // studio black → 0
    ASSERT_TRUE(convert(black, info, "clip.mp4", 16, 16, sws, out));
    EXPECT_TRUE(uniformNear(out, 0, 0, 0, 255, 2));

    const auto white = makeYuv420(16, 16, 235, 128, 128);  // studio white → 255
    ASSERT_TRUE(convert(white, info, "clip.mp4", 16, 16, sws, out));
    EXPECT_TRUE(uniformNear(out, 255, 255, 255, 255, 2));
}

TEST(ConvertDecodedFrame, FullRangeTagIsHonoured)
{
    // Full-range grey Y=200 must stay 200; a wrongly-applied limited
    // expansion would give 1.1644×(200−16) = 214.
    const auto f = makeYuv420(16, 16, 200, 128, 128);
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, makeInfo(16, 16, ColorMatrix::BT709, ColorRange::Full),
                        "clip.mp4", 16, 16, sws, out));
    EXPECT_TRUE(uniformNear(out, 200, 200, 200, 255, 3));
}

TEST(ConvertDecodedFrame, Bt601AndBt709ProduceDifferentRgb)
{
    // The 601-vs-709 must-not-mix contract.  Y=100 U=100 V=190, limited:
    //   Y' = 1.1644×84 = 97.8; U−128 = −28; V−128 = 62.
    //   601: R = 97.8+1.596×62 = 197, G = 97.8+0.392×28−0.813×62 = 58,
    //        B = 97.8−2.017×28 = 41
    //   709: R = 97.8+1.793×62 = 209, G = 97.8+0.213×28−0.533×62 = 71,
    //        B = 97.8−2.112×28 = 39
    // With ±4 tolerance the R/G expectations cannot overlap.
    const auto f = makeYuv420(16, 16, 100, 100, 190);

    {
        SwsCache sws;
        CachedFrame out;
        ASSERT_TRUE(convert(f, makeInfo(16, 16, ColorMatrix::BT601, ColorRange::Limited),
                            "sd.mp4", 16, 16, sws, out));
        EXPECT_TRUE(uniformNear(out, 197, 58, 41, 255, 4));
    }
    {
        SwsCache sws;
        CachedFrame out;
        ASSERT_TRUE(convert(f, makeInfo(16, 16, ColorMatrix::BT709, ColorRange::Limited),
                            "hd.mp4", 16, 16, sws, out));
        EXPECT_TRUE(uniformNear(out, 209, 71, 39, 255, 4));
    }
    {
        // Untagged SD-height source resolves 601 via the height heuristic —
        // the convert path must consult resolveColorConversion, not assume 709.
        SwsCache sws;
        CachedFrame out;
        ASSERT_TRUE(convert(f, makeInfo(16, 16), "untagged_sd.mp4", 16, 16, sws, out));
        EXPECT_TRUE(uniformNear(out, 197, 58, 41, 255, 4));
    }
}

TEST(ConvertDecodedFrame, Nv12ConvertsLikeYuv420)
{
    // NVDEC's native layout: Y plane + interleaved UV.  Same grey contract.
    const auto f = makeNv12(16, 16, 126, 128, 128);
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, makeInfo(16, 16, ColorMatrix::BT709, ColorRange::Limited),
                        "clip.mp4", 16, 16, sws, out));
    EXPECT_TRUE(uniformNear(out, 128, 128, 128, 255, 3));
}

// ── sws path: geometry ───────────────────────────────────────────────────────

TEST(ConvertDecodedFrame, DownscaleSolidStaysSolid)
{
    const auto f = makeYuv420(32, 32, 126, 128, 128);
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, makeInfo(32, 32, ColorMatrix::BT709, ColorRange::Limited),
                        "clip.mp4", 16, 16, sws, out));
    EXPECT_EQ(out.width, 16u);
    EXPECT_EQ(out.height, 16u);
    EXPECT_EQ(out.stride, 64u);
    ASSERT_EQ(out.pixels.size(), 16u * 16u * 4u);
    EXPECT_TRUE(uniformNear(out, 128, 128, 128, 255, 3));
}

TEST(ConvertDecodedFrame, OddDestinationWidthConverts)
{
    // Even 4:2:0 source (decoded 4:2:0 is always even-coded) downscaled to
    // an ODD destination width, as a resolution tier can produce.  Every
    // output pixel — including the last odd column — must be written, and
    // the output tightly packed at stride 17*4 = 68.
    // (An odd SOURCE width at same-size is not tested: swscale's unscaled
    // yuv420p converter works in chroma pairs and leaves the final odd
    // column unwritten, but such frames cannot come out of a real decoder.)
    const auto f = makeYuv420(34, 6, 126, 128, 128);
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, makeInfo(34, 6, ColorMatrix::BT709, ColorRange::Limited),
                        "clip.mp4", 17, 6, sws, out));
    EXPECT_EQ(out.width, 17u);
    EXPECT_EQ(out.height, 6u);
    EXPECT_EQ(out.stride, 68u);
    ASSERT_EQ(out.pixels.size(), 17u * 6u * 4u);
    EXPECT_TRUE(uniformNear(out, 128, 128, 128, 255, 3));
}

TEST(ConvertDecodedFrame, TinyDstWidthDoesNotOverrunAllocation)
{
    // swscale (FFmpeg 7.x) writes the LAST destination row in chunks of up
    // to 64 bytes, so a dst stride < 64 (dstW < 16 px) overruns an exact
    // w*h*4 allocation — measured on the unpadded buffer: 9×6 → 28 B,
    // 5×4 → 44 B, 8×8 → 32 B past the end.  convertDecodedToBgra now pads
    // the allocation so the tail chunk lands in-bounds; this exercises
    // exactly the sizes that used to corrupt the heap.
    const std::vector<std::pair<int, int>> sizes{{9, 6}, {5, 4}, {8, 8}};
    for (const auto& wh : sizes) {
        const int w = wh.first, h = wh.second;
        const auto f = makeYuv420(32, 32, 126, 128, 128);
        SwsCache sws;
        CachedFrame out;
        ASSERT_TRUE(convert(f, makeInfo(32, 32, ColorMatrix::BT709, ColorRange::Limited),
                            "clip.mp4", w, h, sws, out))
            << w << "x" << h;
        EXPECT_EQ(out.width,  static_cast<uint32_t>(w));
        EXPECT_EQ(out.height, static_cast<uint32_t>(h));
        EXPECT_EQ(out.stride, static_cast<uint32_t>(w) * 4u);
        // Allocation covers the payload plus the swscale tail pad.
        EXPECT_GE(out.pixels.size(), static_cast<size_t>(w) * h * 4u);
        EXPECT_TRUE(uniformNear(out, 128, 128, 128, 255, 3)) << w << "x" << h;
    }
}

// ── sws context cache ────────────────────────────────────────────────────────

TEST(ConvertDecodedFrame, SwsContextIsCachedAndRebuiltOnGeometryChange)
{
    const auto f = makeYuv420(32, 32, 126, 128, 128);
    const auto info = makeInfo(32, 32, ColorMatrix::BT709, ColorRange::Limited);
    SwsCache sws;
    CachedFrame out;

    ASSERT_TRUE(convert(f, info, "clip.mp4", 32, 32, sws, out));
    void* first = sws.ctx;
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(sws.srcW, 32);
    EXPECT_EQ(sws.srcH, 32);
    EXPECT_EQ(sws.srcFmt, AV_PIX_FMT_YUV420P);
    EXPECT_EQ(sws.dstW, 32);
    EXPECT_EQ(sws.dstH, 32);

    // Same geometry → the exact same context is reused.
    ASSERT_TRUE(convert(f, info, "clip.mp4", 32, 32, sws, out));
    EXPECT_EQ(sws.ctx, first);

    // New destination size → rebuilt, key fields updated, output correct.
    ASSERT_TRUE(convert(f, info, "clip.mp4", 16, 16, sws, out));
    ASSERT_NE(sws.ctx, nullptr);
    EXPECT_EQ(sws.dstW, 16);
    EXPECT_EQ(sws.dstH, 16);
    EXPECT_EQ(out.width, 16u);
    EXPECT_TRUE(uniformNear(out, 128, 128, 128, 255, 3));
}

TEST(ConvertDecodedFrame, UnsupportedFormatReturnsFalseWithEmptyPixels)
{
    // A hardware format is valid but not sws-supported as input, so
    // sws_getContext fails cleanly and the convert must return false.
    // (AV_PIX_FMT_NONE is unusable here: this FFmpeg build has asserts on
    // and aborts inside libswscale on a NULL pix-fmt descriptor.)
    auto f = makeYuv420(16, 16, 126, 128, 128);
    f.df.rawFormat = AV_PIX_FMT_CUDA;
    SwsCache sws;
    CachedFrame out;
    EXPECT_FALSE(convertDecodedToBgra(f.df, AV_PIX_FMT_CUDA, 16, 16,
                                      makeInfo(16, 16), "clip.mp4",
                                      sws.ref(), nullptr, out));
    EXPECT_TRUE(out.pixels.empty());
    EXPECT_EQ(sws.ctx, nullptr);
}

TEST(ConvertDecodedFrame, PixelBufferPoolPathProducesSameResult)
{
    const auto f = makeYuv420(16, 16, 126, 128, 128);
    PixelBufferPool pool;
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, makeInfo(16, 16, ColorMatrix::BT709, ColorRange::Limited),
                        "clip.mp4", 16, 16, sws, out, &pool));
    ASSERT_EQ(out.pixels.size(), 16u * 16u * 4u);
    EXPECT_TRUE(uniformNear(out, 128, 128, 128, 255, 3));
}

// ── Alpha handling ───────────────────────────────────────────────────────────

TEST(ConvertDecodedFrame, NativeAlphaTransparentPixelsFullyCleared)
{
    // yuva420p, alpha plane all 0, grey RGB: sws leaves non-zero RGB in
    // transparent pixels; the convert core must zero the whole pixel so GPU
    // linear filtering can't bleed stale colour into visible edges.
    const auto f = makeYuva420(16, 16, 126, 128, 128, /*a=*/0);
    auto info = makeInfo(16, 16, ColorMatrix::BT709, ColorRange::Limited);
    info.hasAlpha = true;
    info.packedAlpha = false;
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, info, "clip.mov", 16, 16, sws, out));
    EXPECT_TRUE(uniformNear(out, 0, 0, 0, 0, 0));   // exactly 0x00000000
}

TEST(ConvertDecodedFrame, NativeAlphaSemiTransparentKeepsColourAndAlpha)
{
    const auto f = makeYuva420(16, 16, 126, 128, 128, /*a=*/128);
    auto info = makeInfo(16, 16, ColorMatrix::BT709, ColorRange::Limited);
    info.hasAlpha = true;
    info.packedAlpha = false;
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, info, "clip.mov", 16, 16, sws, out));
    EXPECT_TRUE(uniformNear(out, 128, 128, 128, 128, 4));
}

TEST(ConvertDecodedFrame, PackedAlphaSkipsTransparentClear)
{
    // Packed-alpha sources carry alpha in a separate tile region; clearing
    // on A=0 would nuke real colour.  Same input, packedAlpha flips the rule.
    const uint32_t rgbNoA = 0x001E140Au;   // A=0, R=30, G=20, B=10
    const auto at = [&](int, int) { return rgbNoA; };
    const auto f = makeBgra(4, 4, at);

    auto packed = makeInfo(4, 4);
    packed.hasAlpha = true;
    packed.packedAlpha = true;
    {
        SwsCache sws;
        CachedFrame out;
        ASSERT_TRUE(convert(f, packed, "wells.mp4", 4, 4, sws, out));
        EXPECT_EQ(pixelAt(out, 0, 0), rgbNoA);      // RGB preserved
        EXPECT_EQ(pixelAt(out, 3, 3), rgbNoA);
    }

    auto native = makeInfo(4, 4);
    native.hasAlpha = true;
    native.packedAlpha = false;
    {
        SwsCache sws;
        CachedFrame out;
        ASSERT_TRUE(convert(f, native, "clip.mov", 4, 4, sws, out));
        EXPECT_EQ(pixelAt(out, 0, 0), 0u);          // whole pixel zeroed
        EXPECT_EQ(pixelAt(out, 3, 3), 0u);
    }
}

// ── GREEN chroma key ─────────────────────────────────────────────────────────

TEST(ConvertDecodedFrame, GreenSuffixedFileIsChromaKeyed)
{
    // #18FF00 chroma green (BGRA passthrough for exact input control).
    const uint32_t green = 0xFF18FF00u;
    const auto f = makeBgra(4, 4, [&](int, int) { return green; });
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, makeInfo(4, 4), "shot_GREEN.mp4", 4, 4, sws, out));
    EXPECT_TRUE(uniformNear(out, 0, 0, 0, 0, 0));   // hard-keyed transparent
}

TEST(ConvertDecodedFrame, GreenMatchIsCaseInsensitive)
{
    const uint32_t green = 0xFF18FF00u;
    const auto f = makeBgra(4, 4, [&](int, int) { return green; });
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, makeInfo(4, 4), "shot_green.mp4", 4, 4, sws, out));
    EXPECT_TRUE(uniformNear(out, 0, 0, 0, 0, 0));
}

TEST(ConvertDecodedFrame, GreenKeySparesNonGreenPixels)
{
    // Skin tone (R180 G120 B100) in a GREEN file: neither hard key
    // (G !> R+40) nor spill suppression (G !> R) may touch it.
    const uint32_t skin = 0xFFB47864u;
    const auto f = makeBgra(4, 4, [&](int, int) { return skin; });
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, makeInfo(4, 4), "actor_GREEN.mp4", 4, 4, sws, out));
    EXPECT_EQ(pixelAt(out, 0, 0), skin);
    EXPECT_EQ(pixelAt(out, 3, 3), skin);
}

TEST(ConvertDecodedFrame, NonGreenFileNeverKeyed)
{
    const uint32_t green = 0xFF18FF00u;
    const auto f = makeBgra(4, 4, [&](int, int) { return green; });
    SwsCache sws;
    CachedFrame out;
    ASSERT_TRUE(convert(f, makeInfo(4, 4), "shot.mp4", 4, 4, sws, out));
    EXPECT_EQ(pixelAt(out, 0, 0), green);           // untouched
    EXPECT_EQ(pixelAt(out, 3, 3), green);
}

TEST(FrameContentBounds, FindsTightAlphaRectangle)
{
    CachedFrame frame;
    frame.width = 4;
    frame.height = 3;
    frame.stride = 16;
    frame.pixels.assign(48, 0);
    frame.pixels[1 * frame.stride + 1 * 4 + 3] = 255;
    frame.pixels[2 * frame.stride + 2 * 4 + 3] = 128;

    computeBgraContentBounds(frame);
    ASSERT_TRUE(frame.contentBoundsValid);
    EXPECT_FALSE(frame.contentFullyOpaque);
    EXPECT_FLOAT_EQ(frame.contentLeft, 0.25f);
    EXPECT_FLOAT_EQ(frame.contentTop, 1.0f / 3.0f);
    EXPECT_FLOAT_EQ(frame.contentRight, 0.75f);
    EXPECT_FLOAT_EQ(frame.contentBottom, 1.0f);
}

} // namespace
} // namespace rt

#else // !ROUNDTABLE_HAS_FFMPEG

TEST(ConvertDecodedFrame, RequiresFFmpeg)
{
    GTEST_SKIP() << "convertDecodedToBgra is only compiled with FFmpeg";
}

#endif // ROUNDTABLE_HAS_FFMPEG
