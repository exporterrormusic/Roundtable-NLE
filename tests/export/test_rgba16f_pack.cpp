/*
 * test_rgba16f_pack.cpp — Phase 4.2 RGBA16F → 10-bit YUV pack (export passthrough).
 *
 * Exercises the pure, FFmpeg-free / Vulkan-free packer used by the targeted
 * export passthrough (a single full-frame opaque >8-bit clip → ProRes / DNxHR
 * at real 10-bit, bypassing the lossy 8-bit BGRA compositor stage).  Contract:
 *   - halfToFloat decodes IEEE-754 binary16, flushing subnormals and clamping
 *     Inf/NaN to a finite value (so they never reach the integer quantisers);
 *   - the BT.709 limited-range quantisers hit their documented anchor codes;
 *   - P010 stores the 10-bit value in the HIGH bits, every planar *P10LE in the
 *     LOW bits;
 *   - pack→unpack (against the SAME 876/896 inverses) round-trips within ±2
 *     codes;
 *   - row strides are honoured (no cross-row bleed) and chroma is box-averaged.
 */

#include <gtest/gtest.h>

#include "Rgba16fPack.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace rt {
namespace {

// ── Test helper: float32 → IEEE-754 binary16 (round-to-nearest) ─────────────
// Used only to BUILD inputs.  Round-trip tests decode back through the
// production halfToFloat so half-encode error never masquerades as pack error.
uint16_t f2h(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0)    return static_cast<uint16_t>(sign);              // flush subnormal/zero
    if (exp >= 0x1F) return static_cast<uint16_t>(sign | 0x7C00u);   // Inf
    uint32_t mant10 = mant >> 13;
    if (mant & 0x1000u) ++mant10;                                    // round-to-nearest
    uint32_t h = sign | (static_cast<uint32_t>(exp) << 10) | mant10;
    return static_cast<uint16_t>(h);                                 // mantissa carry into exp is fine
}

// Pack one RGBA pixel (replicated to width×height) into a tightly-packed buffer.
std::vector<uint16_t> makeFlat(int w, int h, float r, float g, float b, float a)
{
    std::vector<uint16_t> buf(static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        buf[i * 4 + 0] = f2h(r);
        buf[i * 4 + 1] = f2h(g);
        buf[i * 4 + 2] = f2h(b);
        buf[i * 4 + 3] = f2h(a);
    }
    return buf;
}

// Dequantise with the SAME inverses used to encode (NOT the shader's rounded
// constants) so the round-trip is self-consistent.
float deY(uint16_t q) { return (static_cast<float>(q) - 64.0f) / 876.0f; }
float deC(uint16_t q) { return (static_cast<float>(q) - 512.0f) / 896.0f; }

// ── halfToFloat ─────────────────────────────────────────────────────────────
TEST(HalfToFloat, KnownValues)
{
    EXPECT_FLOAT_EQ(halfToFloat(0x0000), 0.0f);
    EXPECT_FLOAT_EQ(halfToFloat(0x3C00), 1.0f);
    EXPECT_FLOAT_EQ(halfToFloat(0x3800), 0.5f);
    EXPECT_FLOAT_EQ(halfToFloat(0x3400), 0.25f);
    EXPECT_NEAR    (halfToFloat(0x3555), 0.33325f, 1e-4f);   // ≈1/3
}

TEST(HalfToFloat, SubnormalFlushesToZero)
{
    EXPECT_FLOAT_EQ(halfToFloat(0x0001), 0.0f);   // smallest positive subnormal
    EXPECT_FLOAT_EQ(halfToFloat(0x03FF), 0.0f);   // largest subnormal
}

TEST(HalfToFloat, InfNaNClampFinite)
{
    EXPECT_TRUE(std::isfinite(halfToFloat(0x7C00)));   // +Inf
    EXPECT_TRUE(std::isfinite(halfToFloat(0xFC00)));   // -Inf
    EXPECT_TRUE(std::isfinite(halfToFloat(0x7E00)));   // NaN
    EXPECT_FLOAT_EQ(halfToFloat(0x7C00), 1.0f);
}

// ── Quantisers ──────────────────────────────────────────────────────────────
TEST(Quantize, LumaAnchors)
{
    EXPECT_EQ(quantizeY10(0.0f), 64u);
    EXPECT_EQ(quantizeY10(1.0f), 940u);
    EXPECT_EQ(quantizeY10(0.5f), 502u);   // round(876*0.5 + 64)
    EXPECT_EQ(quantizeY10(-1.0f), 64u);   // clamps
    EXPECT_EQ(quantizeY10(2.0f), 940u);   // clamps
}

TEST(Quantize, ChromaAnchors)
{
    EXPECT_EQ(quantizeC10(0.0f), 512u);
    EXPECT_EQ(quantizeC10(0.5f), 960u);
    EXPECT_EQ(quantizeC10(-0.5f), 64u);
    EXPECT_EQ(quantizeC10(1.0f), 960u);   // clamps
}

TEST(Quantize, AlphaAnchors)
{
    EXPECT_EQ(quantizeA10(0.0f), 0u);
    EXPECT_EQ(quantizeA10(1.0f), 1023u);
    EXPECT_EQ(quantizeA10(0.5f), 512u);   // round(511.5)
}

// ── Bit positioning ─────────────────────────────────────────────────────────
TEST(PackLayout, P010UsesHighBits)
{
    const int w = 2, h = 2;
    auto src = makeFlat(w, h, 0.5f, 0.5f, 0.5f, 1.0f);
    std::vector<uint16_t> y(w * h, 0xFFFF), uv(w * h, 0xFFFF);   // uv oversized; ok
    PackPlanes pl;
    pl.y = y.data(); pl.yStride = w * 2;
    pl.u = uv.data(); pl.uStride = w * 2;
    packRgba16fToYuv(src.data(), w * 2 * 4, w, h, PackTarget::P010LE, pl);

    // Y' = 0.5 → q = 502. P010 stores it shifted up by 6 (low 6 bits zero).
    EXPECT_EQ(y[0], static_cast<uint16_t>(502u << 6));
    EXPECT_EQ(y[0] & 0x3Fu, 0u);
}

TEST(PackLayout, PlanarUsesLowBits)
{
    const int w = 2, h = 2;
    auto src = makeFlat(w, h, 0.5f, 0.5f, 0.5f, 1.0f);
    std::vector<uint16_t> y(w * h), u(w * h), v(w * h);
    PackPlanes pl;
    pl.y = y.data(); pl.yStride = w * 2;
    pl.u = u.data(); pl.uStride = w * 2;
    pl.v = v.data(); pl.vStride = w * 2;
    packRgba16fToYuv(src.data(), w * 2 * 4, w, h, PackTarget::YUV444P10LE, pl);

    EXPECT_EQ(y[0], 502u);            // value in low 10 bits, high 6 zero
    EXPECT_EQ(y[0] & 0xFC00u, 0u);
    EXPECT_EQ(u[0], 512u);            // neutral chroma for grey
    EXPECT_EQ(v[0], 512u);
}

// ── Round-trip ──────────────────────────────────────────────────────────────
TEST(PackRoundTrip, Yuv444RecoversColor)
{
    const int w = 4, h = 4;
    const float R = 0.80f, G = 0.30f, B = 0.10f;
    auto src = makeFlat(w, h, R, G, B, 1.0f);

    // The actual values the pipeline sees after half-encode/decode.
    const float r = halfToFloat(f2h(R));
    const float g = halfToFloat(f2h(G));
    const float b = halfToFloat(f2h(B));

    std::vector<uint16_t> y(w * h), u(w * h), v(w * h);
    PackPlanes pl;
    pl.y = y.data(); pl.yStride = w * 2;
    pl.u = u.data(); pl.uStride = w * 2;
    pl.v = v.data(); pl.vStride = w * 2;
    packRgba16fToYuv(src.data(), w * 2 * 4, w, h, PackTarget::YUV444P10LE, pl);

    // Unpack centre pixel with the matching inverses.
    const float Y  = deY(y[5]);
    const float Cb = deC(u[5]);
    const float Cr = deC(v[5]);
    const float rr = Y + 1.5748f * Cr;
    const float gg = Y - 0.1873f * Cb - 0.4681f * Cr;
    const float bb = Y + 1.8556f * Cb;

    EXPECT_NEAR(rr, r, 0.003f);   // ≤2 codes in 10-bit ≈ 0.002
    EXPECT_NEAR(gg, g, 0.003f);
    EXPECT_NEAR(bb, b, 0.003f);
}

TEST(PackRoundTrip, AlphaStraightThroughForYuva)
{
    const int w = 2, h = 2;
    auto src = makeFlat(w, h, 0.2f, 0.2f, 0.2f, 0.6f);
    std::vector<uint16_t> y(w * h), u(w * h), v(w * h), a(w * h);
    PackPlanes pl;
    pl.y = y.data(); pl.yStride = w * 2;
    pl.u = u.data(); pl.uStride = w * 2;
    pl.v = v.data(); pl.vStride = w * 2;
    pl.a = a.data(); pl.aStride = w * 2;
    packRgba16fToYuv(src.data(), w * 2 * 4, w, h, PackTarget::YUVA444P10LE, pl);

    EXPECT_EQ(a[0], quantizeA10(halfToFloat(f2h(0.6f))));   // full-range, straight
}

// ── Stride independence ─────────────────────────────────────────────────────
TEST(PackStride, PaddedLinesizeNoBleed)
{
    const int w = 3, h = 2;
    auto src = makeFlat(w, h, 0.5f, 0.5f, 0.5f, 1.0f);
    const int yStrideBytes = (w * 2) + 16;          // padded (FFmpeg-style alignment)
    std::vector<uint16_t> y(static_cast<size_t>(yStrideBytes / 2) * h, 0xDEAD);
    std::vector<uint16_t> u(static_cast<size_t>(yStrideBytes / 2) * h, 0);
    std::vector<uint16_t> v(static_cast<size_t>(yStrideBytes / 2) * h, 0);
    PackPlanes pl;
    pl.y = y.data(); pl.yStride = yStrideBytes;
    pl.u = u.data(); pl.uStride = yStrideBytes;
    pl.v = v.data(); pl.vStride = yStrideBytes;
    packRgba16fToYuv(src.data(), w * 2 * 4, w, h, PackTarget::YUV444P10LE, pl);

    // The pad word right after row 0's 3 written words must be untouched.
    EXPECT_EQ(y[3], 0xDEADu);
    // Row 1 starts at stride/2 words in — must hold the packed value, not bleed.
    EXPECT_EQ(y[yStrideBytes / 2], 502u);
}

// ── Chroma subsample ────────────────────────────────────────────────────────
TEST(PackChroma, Yuv422HalfWidthBoxAverage)
{
    // Two columns: left red-ish, right blue-ish → 4:2:2 chroma averages the pair.
    const int w = 2, h = 1;
    std::vector<uint16_t> src(static_cast<size_t>(w) * h * 4);
    auto setpx = [&](int x, float r, float g, float b) {
        src[x * 4 + 0] = f2h(r); src[x * 4 + 1] = f2h(g);
        src[x * 4 + 2] = f2h(b); src[x * 4 + 3] = f2h(1.0f);
    };
    setpx(0, 0.9f, 0.1f, 0.1f);
    setpx(1, 0.1f, 0.1f, 0.9f);

    std::vector<uint16_t> y(w * h), u(1 * h), vv(1 * h);
    PackPlanes pl;
    pl.y = y.data(); pl.yStride = w * 2;
    pl.u = u.data(); pl.uStride = 1 * 2;
    pl.v = vv.data(); pl.vStride = 1 * 2;
    packRgba16fToYuv(src.data(), w * 2 * 4, w, h, PackTarget::YUV422P10LE, pl);

    // One chroma sample for the 2-px row; equals chroma of the averaged RGB.
    const float r = (halfToFloat(f2h(0.9f)) + halfToFloat(f2h(0.1f))) * 0.5f;
    const float g = halfToFloat(f2h(0.1f));
    const float b = (halfToFloat(f2h(0.1f)) + halfToFloat(f2h(0.9f))) * 0.5f;
    const float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    EXPECT_EQ(u[0], quantizeC10((b - Y) / 1.8556f));
    EXPECT_EQ(vv[0], quantizeC10((r - Y) / 1.5748f));
    // Luma stays full-resolution (two distinct samples).
    EXPECT_NE(y[0], y[1]);
}


TEST(PackThreads, LargeFrameMatchesStripByStripPacking)
{
    // A 4K-class frame is packed on several threads in row bands. Packing
    // the same image as 32-row strips (each too small to split) must give
    // identical planes for every target, including the odd last row.
    constexpr int W = 1920, H = 1081, kStrip = 32;
    std::vector<uint16_t> src(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint16_t* p = &src[(static_cast<size_t>(y) * W + x) * 4];
            p[0] = f2h(((x * 7 + y * 3) % 256) / 255.0f);
            p[1] = f2h(((x + y * 11) % 256) / 255.0f);
            p[2] = f2h((((x / 5) ^ (y / 3)) % 256) / 255.0f);
            p[3] = f2h(((x * y) % 256) / 255.0f);
        }
    const int strideBytes = W * 4 * 2;

    for (PackTarget target : {PackTarget::P010LE, PackTarget::YUV422P10LE,
                              PackTarget::YUV444P10LE, PackTarget::YUVA444P10LE}) {
        const bool p010 = target == PackTarget::P010LE;
        const int subX = (target == PackTarget::YUV422P10LE || p010) ? 2 : 1;
        const int subY = p010 ? 2 : 1;
        const int chromaW = (W + subX - 1) / subX, chromaH = (H + subY - 1) / subY;
        const int uWords = p010 ? chromaW * 2 : chromaW;

        struct Planes { std::vector<uint16_t> y, u, v, a; };
        auto alloc = [&] {
            Planes p;
            p.y.assign(static_cast<size_t>(W) * H, 0);
            p.u.assign(static_cast<size_t>(uWords) * chromaH, 0);
            p.v.assign(static_cast<size_t>(chromaW) * chromaH, 0);
            p.a.assign(static_cast<size_t>(W) * H, 0);
            return p;
        };
        auto planesAt = [&](Planes& p, int row) {
            PackPlanes d;
            d.y = p.y.data() + static_cast<size_t>(row) * W;              d.yStride = W * 2;
            d.u = p.u.data() + static_cast<size_t>(row / subY) * uWords;  d.uStride = uWords * 2;
            d.v = p010 ? nullptr : p.v.data() + static_cast<size_t>(row / subY) * chromaW;
            d.vStride = chromaW * 2;
            if (target == PackTarget::YUVA444P10LE) {
                d.a = p.a.data() + static_cast<size_t>(row) * W;          d.aStride = W * 2;
            }
            return d;
        };

        Planes whole = alloc(), strips = alloc();
        packRgba16fToYuv(src.data(), strideBytes, W, H, target, planesAt(whole, 0));
        for (int row = 0; row < H; row += kStrip) {
            const int rows = std::min(kStrip, H - row);
            packRgba16fToYuv(src.data() + static_cast<size_t>(row) * W * 4, strideBytes,
                             W, rows, target, planesAt(strips, row));
        }
        EXPECT_EQ(whole.y, strips.y) << static_cast<int>(target);
        EXPECT_EQ(whole.u, strips.u) << static_cast<int>(target);
        EXPECT_EQ(whole.v, strips.v) << static_cast<int>(target);
        EXPECT_EQ(whole.a, strips.a) << static_cast<int>(target);
    }
}

} // namespace
} // namespace rt
