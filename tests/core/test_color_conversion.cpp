/*
 * test_color_conversion.cpp — Phase 4.1 per-source colour decision.
 *
 * Exercises resolveColorConversion(), the FFmpeg-free function shared by the
 * CPU sws_scale path (ConvertDecodedFrame) and the GPU convert gate
 * (convertDecodedToCacheGpu / MediaPoolDecode).  The contract under test:
 *   - tagged matrix/range/transfer are honoured verbatim;
 *   - untagged sources resolve via the SD(≤576)→601 / HD→709 height
 *     heuristic at limited range;
 *   - isGpuShaderDefault is true ONLY for BT.709 + limited + SDR — the exact
 *     assumption the convert shaders hardcode — so anything else is forced to
 *     the CPU path and a clip never mixes colourspaces in the frame cache.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "decode/VideoDecoder.h"

namespace rt {
namespace {

VideoStreamInfo makeInfo(uint32_t height,
                         ColorMatrix m   = ColorMatrix::Unspecified,
                         ColorRange r    = ColorRange::Unspecified,
                         ColorTransfer t = ColorTransfer::Unspecified)
{
    VideoStreamInfo info;
    info.width         = (height * 16) / 9;   // plausible 16:9 width
    info.height        = height;
    info.colorMatrix   = m;
    info.colorRange    = r;
    info.colorTransfer = t;
    return info;
}

// ── Tagged sources are honoured verbatim ────────────────────────────────────

TEST(ColorConversion, Bt709LimitedSdrIsGpuDefault)
{
    const auto cc = resolveColorConversion(
        makeInfo(1080, ColorMatrix::BT709, ColorRange::Limited, ColorTransfer::Sdr));
    EXPECT_EQ(cc.matrix, ColorMatrix::BT709);
    EXPECT_FALSE(cc.fullRange);
    EXPECT_FALSE(cc.isHdr);
    EXPECT_TRUE(cc.isGpuShaderDefault);   // the one case the shaders handle
}

TEST(ColorConversion, TaggedBt601StaysOffGpu)
{
    const auto cc = resolveColorConversion(
        makeInfo(1080, ColorMatrix::BT601, ColorRange::Limited));
    EXPECT_EQ(cc.matrix, ColorMatrix::BT601);
    EXPECT_FALSE(cc.isGpuShaderDefault);  // wrong matrix for the 709 shaders
}

TEST(ColorConversion, FullRangeStaysOffGpu)
{
    // Full-range (JPEG/MJPEG) 709 content: matrix is fine but the shaders
    // assume studio swing, so it must take the CPU path or levels crush.
    const auto cc = resolveColorConversion(
        makeInfo(1080, ColorMatrix::BT709, ColorRange::Full));
    EXPECT_TRUE(cc.fullRange);
    EXPECT_FALSE(cc.isGpuShaderDefault);
}

TEST(ColorConversion, Bt2020PqIsHdrAndOffGpu)
{
    const auto cc = resolveColorConversion(
        makeInfo(2160, ColorMatrix::BT2020, ColorRange::Limited, ColorTransfer::PQ));
    EXPECT_EQ(cc.matrix, ColorMatrix::BT2020);
    EXPECT_TRUE(cc.isHdr);
    EXPECT_FALSE(cc.isGpuShaderDefault);
}

TEST(ColorConversion, Bt2020HlgIsHdr)
{
    const auto cc = resolveColorConversion(
        makeInfo(2160, ColorMatrix::BT2020, ColorRange::Limited, ColorTransfer::HLG));
    EXPECT_TRUE(cc.isHdr);
    EXPECT_FALSE(cc.isGpuShaderDefault);
}

// ── Untagged sources fall back to the SD/HD height heuristic ────────────────

TEST(ColorConversion, UntaggedHdResolvesTo709)
{
    const auto cc = resolveColorConversion(makeInfo(1080));
    EXPECT_EQ(cc.matrix, ColorMatrix::BT709);
    EXPECT_FALSE(cc.fullRange);            // untagged range = limited
    EXPECT_TRUE(cc.isGpuShaderDefault);    // untagged HD == the legacy behaviour
}

TEST(ColorConversion, UntaggedSdResolvesTo601)
{
    const auto cc = resolveColorConversion(makeInfo(480));
    EXPECT_EQ(cc.matrix, ColorMatrix::BT601);
    EXPECT_FALSE(cc.isGpuShaderDefault);   // SD → CPU path
}

TEST(ColorConversion, HeuristicBoundaryAt576)
{
    EXPECT_EQ(resolveColorConversion(makeInfo(576)).matrix, ColorMatrix::BT601);
    EXPECT_EQ(resolveColorConversion(makeInfo(577)).matrix, ColorMatrix::BT709);
}

TEST(ColorConversion, OtherMatrixUsesHeuristic)
{
    // FCC / SMPTE240M / RGB map to ColorMatrix::Other → height heuristic.
    EXPECT_EQ(resolveColorConversion(makeInfo(720, ColorMatrix::Other)).matrix,
              ColorMatrix::BT709);
    EXPECT_EQ(resolveColorConversion(makeInfo(480, ColorMatrix::Other)).matrix,
              ColorMatrix::BT601);
}

// ── Packed-alpha sources use the nominal (unpacked) picture height ──────────

TEST(ColorConversion, PackedAlphaUsesNominalHeight)
{
    // Wells-style stacked HEVC: 1080×3776 packed (2 tiles) → 1888 nominal.
    // The heuristic must see 1888 (HD → 709), not 3776.
    VideoStreamInfo info;
    info.width       = 1080;
    info.height      = 3776;
    info.packedAlpha = true;
    info.packedTiles = 2;
    const auto cc = resolveColorConversion(info);
    EXPECT_EQ(cc.matrix, ColorMatrix::BT709);
}

// ── snapDisplayRotation: angle → clockwise quarter-turn in {0,90,180,270} ───
//
// The caller passes the CLOCKWISE display angle (it negates FFmpeg's CCW
// av_display_rotation_get result first).  The snap must round to the nearest
// quarter turn and fold any sign / wrap into [0,360).

TEST(DisplayRotation, ExactQuarterTurns)
{
    EXPECT_EQ(snapDisplayRotation(0.0),   0);
    EXPECT_EQ(snapDisplayRotation(90.0),  90);
    EXPECT_EQ(snapDisplayRotation(180.0), 180);
    EXPECT_EQ(snapDisplayRotation(270.0), 270);
}

TEST(DisplayRotation, NegativeAnglesFoldIntoRange)
{
    // -90° clockwise == 270°; FFmpeg commonly reports portrait as ±90.
    EXPECT_EQ(snapDisplayRotation(-90.0),  270);
    EXPECT_EQ(snapDisplayRotation(-180.0), 180);
    EXPECT_EQ(snapDisplayRotation(-270.0), 90);
}

TEST(DisplayRotation, WrapsAndRounds)
{
    EXPECT_EQ(snapDisplayRotation(360.0),  0);
    EXPECT_EQ(snapDisplayRotation(450.0),  90);    // 360 + 90
    EXPECT_EQ(snapDisplayRotation(89.4),   90);    // rounds to nearest 90
    EXPECT_EQ(snapDisplayRotation(269.6),  270);
}

TEST(DisplayRotation, NonFiniteIsZero)
{
    EXPECT_EQ(snapDisplayRotation(std::nan("")), 0);
    EXPECT_EQ(snapDisplayRotation(std::numeric_limits<double>::infinity()), 0);
}

} // namespace
} // namespace rt
