/*
 * Rgba16fPack.cpp — see Rgba16fPack.h.  Pure math, no FFmpeg / no Vulkan.
 */

#include "Rgba16fPack.h"

#include <cmath>
#include <cstring>

namespace rt {

namespace {

// Clamp helper (no <algorithm> dependency churn; keeps this TU tiny).
inline float clampf(float v, float lo, float hi) noexcept
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int clampi(int v, int lo, int hi) noexcept
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/// One decoded RGBA pixel (non-linear BT.709 R'G'B', straight alpha), [0,1].
struct Rgba { float r, g, b, a; };

/// Read pixel (x,y) from the RGBA16F source, decoding the four halfs.
inline Rgba rgbaAt(const uint16_t* src, int strideBytes, int x, int y) noexcept
{
    const auto* row = reinterpret_cast<const uint16_t*>(
        reinterpret_cast<const uint8_t*>(src) + static_cast<size_t>(y) * strideBytes);
    const uint16_t* px = row + static_cast<size_t>(x) * 4;
    return { halfToFloat(px[0]), halfToFloat(px[1]),
             halfToFloat(px[2]), halfToFloat(px[3]) };
}

// BT.709 forward (non-linear R'G'B' → Y'CbCr, normalised).  The chroma
// divisors 1.8556 / 1.5748 are EXACTLY the multipliers in the decode shaders,
// so the round-trip is self-consistent.
inline float luma709(const Rgba& p) noexcept
{
    return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b;
}
inline float cb709(const Rgba& p, float y) noexcept { return (p.b - y) / 1.8556f; }
inline float cr709(const Rgba& p, float y) noexcept { return (p.r - y) / 1.5748f; }

// Average RGBA over a [x0,x1)×[y0,y1) block (edge-clamped by the caller).
inline Rgba avgBlock(const uint16_t* src, int strideBytes,
                     int x0, int y0, int x1, int y1) noexcept
{
    Rgba acc{0, 0, 0, 0};
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            Rgba p = rgbaAt(src, strideBytes, x, y);
            acc.r += p.r; acc.g += p.g; acc.b += p.b; acc.a += p.a;
            ++n;
        }
    const float inv = n > 0 ? 1.0f / static_cast<float>(n) : 0.0f;
    return { acc.r * inv, acc.g * inv, acc.b * inv, acc.a * inv };
}

inline uint16_t* rowPtr(uint16_t* base, int strideBytes, int y) noexcept
{
    return reinterpret_cast<uint16_t*>(
        reinterpret_cast<uint8_t*>(base) + static_cast<size_t>(y) * strideBytes);
}

} // namespace

float halfToFloat(uint16_t h) noexcept
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp  = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;

    if (exp == 0x1Fu)                 // Inf / NaN → clamp to 1.0 (never reach quantiser as non-finite)
        return 1.0f;
    if (exp == 0)                     // zero / subnormal → flush to ±0 (subnormals are ≈0 for colour)
        return sign ? -0.0f : 0.0f;

    // Normal: rebias exponent (15 → 127) and widen mantissa (10 → 23 bits).
    const uint32_t bits = sign | ((exp + 112u) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

uint16_t quantizeY10(float yLuma) noexcept
{
    const float y = clampf(yLuma, 0.0f, 1.0f);
    const int q = static_cast<int>(std::lround(876.0f * y + 64.0f));
    return static_cast<uint16_t>(clampi(q, 64, 940));
}

uint16_t quantizeC10(float chroma) noexcept
{
    const float c = clampf(chroma, -0.5f, 0.5f);
    const int q = static_cast<int>(std::lround(896.0f * c + 512.0f));
    return static_cast<uint16_t>(clampi(q, 64, 960));
}

uint16_t quantizeA10(float alpha) noexcept
{
    const float a = clampf(alpha, 0.0f, 1.0f);
    const int q = static_cast<int>(std::lround(1023.0f * a));
    return static_cast<uint16_t>(clampi(q, 0, 1023));
}

void packRgba16fToYuv(const uint16_t* srcRgba16f, int srcStrideBytes,
                      int width, int height,
                      PackTarget target, const PackPlanes& dst) noexcept
{
    if (!srcRgba16f || width <= 0 || height <= 0) return;

    // P010 stores the 10-bit value in the HIGH 10 bits (value << 6); every
    // planar *P10LE format stores it in the LOW 10 bits (value & 0x3FF).
    const bool highBits = (target == PackTarget::P010LE);
    auto storeWord = [highBits](uint16_t q10) noexcept -> uint16_t {
        return highBits ? static_cast<uint16_t>(q10 << 6)
                        : static_cast<uint16_t>(q10 & 0x3FFu);
    };

    // ── Luma plane (full resolution for every target) ───────────────────────
    if (dst.y) {
        for (int y = 0; y < height; ++y) {
            uint16_t* yrow = rowPtr(dst.y, dst.yStride, y);
            for (int x = 0; x < width; ++x) {
                const Rgba p = rgbaAt(srcRgba16f, srcStrideBytes, x, y);
                yrow[x] = storeWord(quantizeY10(luma709(p)));
            }
        }
    }

    // ── Alpha plane (YUVA444P10LE only — full resolution, full range) ────────
    if (target == PackTarget::YUVA444P10LE && dst.a) {
        for (int y = 0; y < height; ++y) {
            uint16_t* arow = rowPtr(dst.a, dst.aStride, y);
            for (int x = 0; x < width; ++x) {
                const Rgba p = rgbaAt(srcRgba16f, srcStrideBytes, x, y);
                arow[x] = storeWord(quantizeA10(p.a));
            }
        }
    }

    // ── Chroma planes ───────────────────────────────────────────────────────
    // Subsample factors per target: 4:2:0 (P010) = 2×2, 4:2:2 = 2×1, 4:4:4 = 1×1.
    int subX = 1, subY = 1;
    switch (target) {
        case PackTarget::P010LE:       subX = 2; subY = 2; break;
        case PackTarget::YUV422P10LE:  subX = 2; subY = 1; break;
        case PackTarget::YUV444P10LE:
        case PackTarget::YUVA444P10LE: subX = 1; subY = 1; break;
    }

    const int chromaW = (width  + subX - 1) / subX;
    const int chromaH = (height + subY - 1) / subY;

    for (int cy = 0; cy < chromaH; ++cy) {
        for (int cx = 0; cx < chromaW; ++cx) {
            // Box-average the source block (edge-clamped) co-sited top-left.
            const int x0 = cx * subX;
            const int y0 = cy * subY;
            const int x1 = (x0 + subX < width)  ? x0 + subX : width;
            const int y1 = (y0 + subY < height) ? y0 + subY : height;
            const Rgba p = (subX == 1 && subY == 1)
                ? rgbaAt(srcRgba16f, srcStrideBytes, x0, y0)
                : avgBlock(srcRgba16f, srcStrideBytes, x0, y0, x1, y1);
            const float y = luma709(p);
            const uint16_t cbWord = storeWord(quantizeC10(cb709(p, y)));
            const uint16_t crWord = storeWord(quantizeC10(cr709(p, y)));

            if (target == PackTarget::P010LE) {
                // Interleaved CbCr in the single chroma plane: [Cb,Cr] per sample.
                if (dst.u) {
                    uint16_t* urow = rowPtr(dst.u, dst.uStride, cy);
                    urow[cx * 2 + 0] = cbWord;
                    urow[cx * 2 + 1] = crWord;
                }
            } else {
                if (dst.u) rowPtr(dst.u, dst.uStride, cy)[cx] = cbWord;
                if (dst.v) rowPtr(dst.v, dst.vStride, cy)[cx] = crWord;
            }
        }
    }
}

} // namespace rt
