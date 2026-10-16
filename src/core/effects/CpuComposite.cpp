/*
 * CpuComposite.cpp - CPU pixel blending helpers.
 *
 * Contains blitLayerWithTransform (affine transform + alpha blend)
 * and rasterizeMasks (CPU mask rasterizer for opacity masks).
 */

#include "effects/CpuComposite.h"
#include "timeline/OpacityMask.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace rt {

/// Helper: alpha-blend a source BGRA layer onto a destination BGRA buffer
/// with affine transform (position, scale, rotation).
/// Source is fill-to-output (covers entire output, may crop edges) first,
/// then clip transforms applied.
void blitLayerWithTransform(
    uint8_t* dst, uint32_t dstW, uint32_t dstH,
    const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t srcStride,
    float opacity,
    float posXPx, float posYPx,
    float scX, float scY,
    float rotDeg,
    float cropL, float cropR,
    float cropT, float cropB,
    bool containFit)
{
    if (opacity < 0.001f) return;

    // Guard against zero / degenerate scale that would cause division-by-zero
    // or astronomically large bounding boxes.
    if (std::abs(scX) < 0.001f || std::abs(scY) < 0.001f) return;

    // Cover/contain fit: scale source to fill (cover) or fit within (contain)
    // the output.  Resolution-independent — same visual at any output size.
    const float scaleToFitW = static_cast<float>(dstW) / static_cast<float>(srcW);
    const float scaleToFitH = static_cast<float>(dstH) / static_cast<float>(srcH);
    const float fitScale = containFit
        ? std::min(scaleToFitW, scaleToFitH)
        : std::max(scaleToFitW, scaleToFitH);

    const float fittedW = srcW * fitScale;
    const float fittedH = srcH * fitScale;
    const float baseOffX = (dstW - fittedW) * 0.5f;
    const float baseOffY = (dstH - fittedH) * 0.5f;

    // Combined transform center is output center
    const float cx = dstW * 0.5f;
    const float cy = dstH * 0.5f;

    // 3) Rotation matrix
    const float radians = rotDeg * 3.14159265358979f / 180.0f;
    const float cosR = std::cos(radians);
    const float sinR = std::sin(radians);

    // Fast path: identity transform, same dimensions — direct blit
    //    Avoid per-pixel inverse-transform computation entirely.
    const bool hasCrop = cropL > 0.01f || cropR > 0.01f || cropT > 0.01f || cropB > 0.01f;
    const bool noTransform = !hasCrop &&
                             std::abs(posXPx) < 0.5f && std::abs(posYPx) < 0.5f &&
                             std::abs(scX - 1.0f) < 0.001f && std::abs(scY - 1.0f) < 0.001f &&
                             std::abs(rotDeg) < 0.01f;
    if (noTransform && srcW == dstW && srcH == dstH && opacity >= 0.999f) {
        // Source matches output: opaque copy (skip alpha blend for bg layer)
        for (uint32_t y = 0; y < dstH; ++y) {
            const uint8_t* sp = src + y * srcStride;
            uint8_t* dp = dst + y * dstW * 4;
            std::memcpy(dp, sp, static_cast<size_t>(dstW) * 4);
        }
        return;
    }

    // Precompute fixed-point opacity: 0-256 range for fast integer blend.
    // 256 means "fully opaque" and lets us use (x >> 8) instead of (x / 255).
    const uint32_t opac256 = static_cast<uint32_t>(opacity * 256.0f + 0.5f);

    // Fast integer "source-over" alpha blend helper (inline lambda).
    // Uses fixed-point 0-255 math with no floating-point or division.
    //   sa256 = source alpha * opacity, in 0-256 range
    //   Blends BGRA in-place: dst = src*sa + dst*(255-sa'), approximated via
    //   the exact integer divide-by-255: (v + 1 + (v >> 8)) >> 8.
    auto blendPixel = [](uint8_t* dp, const uint8_t* sp, uint32_t sa256) {
        // sa = effective source alpha in 0-255
        uint32_t sa = (static_cast<uint32_t>(sp[3]) * sa256) >> 8;
        if (sa == 0) return;
        if (sa >= 255) {
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
            return;
        }
        uint32_t invA = 255u - sa;
        // Exact divide-by-255 for each channel: ((a*sa + b*invA) + 1 + (tmp>>8)) >> 8
        auto div255 = [](uint32_t v) -> uint8_t {
            return static_cast<uint8_t>((v + 1 + (v >> 8)) >> 8);
        };
        dp[0] = div255(sp[0] * sa + dp[0] * invA);
        dp[1] = div255(sp[1] * sa + dp[1] * invA);
        dp[2] = div255(sp[2] * sa + dp[2] * invA);
        // Output alpha: sa + da*(1-sa/255) — sa + da*invA/255
        uint32_t outA = sa + ((dp[3] * invA + 127) / 255);
        dp[3] = static_cast<uint8_t>(std::min(outA, 255u));
    };

    if (noTransform && srcW == dstW && srcH == dstH) {
        // Same dimensions, no transform, but opacity < 1
        for (uint32_t i = 0; i < dstW * dstH; ++i) {
            const uint8_t* sp = src + i * 4;
            uint8_t* dp = dst + i * 4;
            if (sp[3] == 0) continue;
            blendPixel(dp, sp, opac256);
        }
        return;
    }

    // Bounding-box clipping
    //    Instead of iterating ALL output pixels, compute the output-space
    //    AABB of the source rectangle and iterate only those pixels.
    // Forward transform: fitSpace -> scale -> rotate -> position
    auto forwardXY = [&](float fitX, float fitY, float& outX, float& outY) {
        float rx = (fitX - cx + baseOffX) * scX;
        float ry = (fitY - cy + baseOffY) * scY;
        outX = rx * cosR - ry * sinR + cx + posXPx;
        outY = rx * sinR + ry * cosR + cy + posYPx;
    };

    // Transform the 4 corners of the fitted source rectangle
    float ox0, oy0, ox1, oy1, ox2, oy2, ox3, oy3;
    forwardXY(0.0f,    0.0f,    ox0, oy0);  // top-left
    forwardXY(fittedW, 0.0f,    ox1, oy1);  // top-right
    forwardXY(0.0f,    fittedH, ox2, oy2);  // bottom-left
    forwardXY(fittedW, fittedH, ox3, oy3);  // bottom-right

    float aabbMinX = std::min({ox0, ox1, ox2, ox3});
    float aabbMaxX = std::max({ox0, ox1, ox2, ox3});
    float aabbMinY = std::min({oy0, oy1, oy2, oy3});
    float aabbMaxY = std::max({oy0, oy1, oy2, oy3});

    // Apply crop: narrow the AABB by the crop percentages.
    // Crop values are 0-100 representing percentage of the layer to cut off.
    if (cropL > 0.01f || cropR > 0.01f || cropT > 0.01f || cropB > 0.01f) {
        float aabbW = aabbMaxX - aabbMinX;
        float aabbH = aabbMaxY - aabbMinY;
        aabbMinX += aabbW * (cropL / 100.0f);
        aabbMaxX -= aabbW * (cropR / 100.0f);
        aabbMinY += aabbH * (cropT / 100.0f);
        aabbMaxY -= aabbH * (cropB / 100.0f);
        if (aabbMinX >= aabbMaxX || aabbMinY >= aabbMaxY) return;
    }

    // Clamp to output bounds
    uint32_t startX = static_cast<uint32_t>(std::max(0, static_cast<int>(std::floor(aabbMinX))));
    uint32_t startY = static_cast<uint32_t>(std::max(0, static_cast<int>(std::floor(aabbMinY))));
    uint32_t endX   = static_cast<uint32_t>(std::min(static_cast<int>(dstW),
                                                      static_cast<int>(std::ceil(aabbMaxX)) + 1));
    uint32_t endY   = static_cast<uint32_t>(std::min(static_cast<int>(dstH),
                                                      static_cast<int>(std::ceil(aabbMaxY)) + 1));

    if (startX >= endX || startY >= endY) return;

    // 4) For each output pixel IN THE BOUNDING BOX, compute inverse
    //    transform to find source pixel.
    for (uint32_t dy = startY; dy < endY; ++dy) {
        for (uint32_t dx = startX; dx < endX; ++dx) {
            // Output pixel relative to center + position offset
            float px = static_cast<float>(dx) - cx - posXPx;
            float py = static_cast<float>(dy) - cy - posYPx;

            // Inverse rotation
            float rx = px * cosR + py * sinR;
            float ry = -px * sinR + py * cosR;

            // Inverse scale
            rx /= scX;
            ry /= scY;

            // Back to output pixel space (relative to fit origin)
            float fitX = rx + cx - baseOffX;
            float fitY = ry + cy - baseOffY;

            // Map from fitted space to source pixel
            float sx = fitX / fitScale;
            float sy = fitY / fitScale;

            // Nearest-neighbor sampling
            int isx = static_cast<int>(sx);
            int isy = static_cast<int>(sy);

            if (isx < 0 || isx >= static_cast<int>(srcW) ||
                isy < 0 || isy >= static_cast<int>(srcH))
                continue;

            const uint8_t* sp = src + isy * srcStride + isx * 4;
            uint8_t* dp = dst + (dy * dstW + dx) * 4;

            // Skip fully transparent source pixels
            if (sp[3] == 0) continue;

            // Integer alpha blending (source-over)
            blendPixel(dp, sp, opac256);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  CPU mask rasterizer — Premiere Pro semantics.
//
//  Every shape is flattened to a closed polygon in target pixel space,
//  scanline-filled, and converted to a signed distance field via an exact
//  Euclidean distance transform (Felzenszwalb & Huttenlocher). Feather and
//  expansion are then uniform distance-based operations for all shapes:
//    alpha = smoothstep over [-feather/2, +feather/2] around (edge + expansion)
//  Masks combine additively (union), matching Premiere: each mask reveals
//  its region; a single inverted mask cuts a hole.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/// Map a normalized mask-space point (0–1) into target pixel space.
inline void mapPoint(float u, float v, uint32_t w, uint32_t h,
                     const MaskRasterTransform* xf, float& px, float& py)
{
    if (xf) {
        px = xf->m[0] * u + xf->m[1] * v + xf->m[2];
        py = xf->m[3] * u + xf->m[4] * v + xf->m[5];
    } else {
        px = u * static_cast<float>(w);
        py = v * static_cast<float>(h);
    }
}

/// Flatten one mask's geometry to a closed polygon in target pixel space.
/// Returns false when the shape is degenerate (nothing to fill).
bool buildMaskPolygon(const MaskRenderState& mask, uint32_t w, uint32_t h,
                      const MaskRasterTransform* xf, std::vector<float>& poly)
{
    poly.clear();
    const auto& g = mask.geometry;

    auto push = [&](float u, float v) {
        float px, py;
        mapPoint(u, v, w, h, xf, px, py);
        poly.push_back(px);
        poly.push_back(py);
    };

    if (mask.shape == MaskShape::Ellipse || mask.shape == MaskShape::Rectangle) {
        const float rotRad = g.rotation * 3.14159265f / 180.0f;
        const float cosR = std::cos(rotRad), sinR = std::sin(rotRad);
        // Rotation is defined in frame-pixel space (matches the overlay's
        // math, which rotates widget-space offsets) — build the local
        // offsets in normalized space but rotate the aspect-corrected
        // vector. Use a nominal frame aspect from the identity mapping.
        auto pushLocal = [&](float dx, float dy) {
            // dx/dy are normalized half-extents; rotate in aspect space
            float ax = dx * static_cast<float>(w);
            float ay = dy * static_cast<float>(h);
            float rx = ax * cosR - ay * sinR;
            float ry = ax * sinR + ay * cosR;
            push(g.centerX + rx / static_cast<float>(w),
                 g.centerY + ry / static_cast<float>(h));
        };

        const float hwN = g.width * 0.5f;
        const float hhN = g.height * 0.5f;
        if (hwN <= 0.0f || hhN <= 0.0f) return false;

        if (mask.shape == MaskShape::Ellipse) {
            constexpr int kSegments = 96;
            for (int i = 0; i < kSegments; ++i) {
                float a = 2.0f * 3.14159265f * static_cast<float>(i) / kSegments;
                pushLocal(hwN * std::cos(a), hhN * std::sin(a));
            }
        } else {
            pushLocal(-hwN, -hhN);
            pushLocal( hwN, -hhN);
            pushLocal( hwN,  hhN);
            pushLocal(-hwN,  hhN);
        }
        return true;
    }

    // FreeDrawBezier — flatten each cubic segment. Tangents are stored as
    // normalized-frame offsets from the vertex (same convention as the
    // monitor overlay).
    const auto& verts = g.vertices;
    if (verts.size() < 3) return false;

    constexpr int kSubdiv = 24;
    for (size_t vi = 0; vi < verts.size(); ++vi) {
        size_t ni = (vi + 1) % verts.size();
        const float p0x = verts[vi].x,                    p0y = verts[vi].y;
        const float c1x = p0x + verts[vi].outTanX,        c1y = p0y + verts[vi].outTanY;
        const float p1x = verts[ni].x,                    p1y = verts[ni].y;
        const float c2x = p1x + verts[ni].inTanX,         c2y = p1y + verts[ni].inTanY;

        const bool straight =
            verts[vi].outTanX == 0.0f && verts[vi].outTanY == 0.0f &&
            verts[ni].inTanX  == 0.0f && verts[ni].inTanY  == 0.0f;
        const int steps = straight ? 1 : kSubdiv;

        for (int s = 0; s < steps; ++s) {
            float t  = static_cast<float>(s) / steps;
            float mt = 1.0f - t;
            float bx = mt*mt*mt*p0x + 3*mt*mt*t*c1x + 3*mt*t*t*c2x + t*t*t*p1x;
            float by = mt*mt*mt*p0y + 3*mt*mt*t*c1y + 3*mt*t*t*c2y + t*t*t*p1y;
            push(bx, by);
        }
    }
    return poly.size() >= 6;
}

/// Run fn(begin, end) over [0, count) split across hardware threads. Rows
/// and columns of every raster pass are independent, so a full-frame mask
/// (two distance transforms over the whole source) finishes in a fraction of
/// the single-threaded time — this runs on every edit of a dragged mask.
template <typename Fn>
void parallelSpans(uint32_t count, uint64_t workPerItem, Fn&& fn)
{
    // Below ~256k units of work the thread start-up costs more than it saves.
    const uint64_t totalWork = static_cast<uint64_t>(count) * workPerItem;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t threads = totalWork < (1u << 18)
        ? 1u
        : std::min<uint32_t>({hw, 16u, count});
    if (threads <= 1) {
        fn(0u, count);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    const uint32_t step = (count + threads - 1) / threads;
    for (uint32_t t = 1; t < threads; ++t) {
        const uint32_t begin = t * step;
        const uint32_t end = std::min(count, begin + step);
        if (begin >= end) break;
        pool.emplace_back([&fn, begin, end]() { fn(begin, end); });
    }
    fn(0u, std::min(count, step));
    for (auto& th : pool) th.join();
}

/// Even-odd scanline fill of a closed polygon (xy pairs, target px) into a
/// bw×bh window whose top-left pixel is (x0, y0) in target pixels.
void fillPolygon(const std::vector<float>& poly, int x0, int y0,
                 uint32_t bw, uint32_t bh, std::vector<uint8_t>& inside)
{
    const size_t n = poly.size() / 2;
    parallelSpans(bh, n + bw / 8, [&](uint32_t rowBegin, uint32_t rowEnd) {
        std::vector<float> xs;
        xs.reserve(16);
        for (uint32_t y = rowBegin; y < rowEnd; ++y) {
            const float sy = static_cast<float>(y0 + static_cast<int>(y)) + 0.5f;
            xs.clear();
            for (size_t i = 0; i < n; ++i) {
                size_t j = (i + 1) % n;
                float ya = poly[i * 2 + 1], yb = poly[j * 2 + 1];
                if ((ya <= sy && yb > sy) || (yb <= sy && ya > sy)) {
                    float t = (sy - ya) / (yb - ya);
                    xs.push_back(poly[i * 2] + t * (poly[j * 2] - poly[i * 2]));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                int xa = std::max(0, static_cast<int>(std::ceil(xs[k] - 0.5f)) - x0);
                int xb = std::min(static_cast<int>(bw) - 1,
                                  static_cast<int>(std::floor(xs[k + 1] - 0.5f)) - x0);
                for (int x = xa; x <= xb; ++x)
                    inside[static_cast<size_t>(y) * bw + x] = 1;
            }
        }
    });
}

/// 1D squared Euclidean distance transform (Felzenszwalb & Huttenlocher).
/// f = input (INF where empty), d = output, n = length. v/z are scratch.
void edt1d(const float* f, float* d, int n, int* v, float* z)
{
    int k = 0;
    v[0] = 0;
    z[0] = -1e20f;
    z[1] = 1e20f;
    for (int q = 1; q < n; ++q) {
        float s;
        while (true) {
            s = ((f[q] + q * static_cast<float>(q)) -
                 (f[v[k]] + v[k] * static_cast<float>(v[k]))) /
                (2.0f * (q - v[k]));
            if (s <= z[k]) { --k; } else break;
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e20f;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<float>(q)) ++k;
        float dq = static_cast<float>(q - v[k]);
        d[q] = dq * dq + f[v[k]];
    }
}

/// 2D squared EDT over a w×h grid: dist² to the nearest pixel whose
/// grid value equals `seed`.
void edt2d(const std::vector<uint8_t>& grid, uint8_t seed,
           uint32_t w, uint32_t h, std::vector<float>& out)
{
    constexpr float INF = 1e18f;
    out.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = (grid[i] == seed) ? 0.0f : INF;

    struct Scratch {
        std::vector<float> f, d, z;
        std::vector<int> v;
        explicit Scratch(int n) : f(n), d(n), z(n + 1), v(n) {}
    };

    // Columns, a block of adjacent columns at a time: gathering one column
    // alone strides a whole row per element, so each block is copied out
    // row by row (contiguous reads), transformed, and written back.
    constexpr uint32_t kBlock = 16;
    const uint32_t blocks = (w + kBlock - 1) / kBlock;
    parallelSpans(blocks, static_cast<uint64_t>(h) * kBlock,
                  [&](uint32_t begin, uint32_t end) {
        Scratch s(static_cast<int>(h));
        std::vector<float> tile(static_cast<size_t>(kBlock) * h);
        for (uint32_t blk = begin; blk < end; ++blk) {
            const uint32_t x0 = blk * kBlock;
            const uint32_t n = std::min(kBlock, w - x0);
            for (uint32_t y = 0; y < h; ++y) {
                const float* src = out.data() + static_cast<size_t>(y) * w + x0;
                for (uint32_t c = 0; c < n; ++c)
                    tile[static_cast<size_t>(c) * h + y] = src[c];
            }
            for (uint32_t c = 0; c < n; ++c) {
                float* col = tile.data() + static_cast<size_t>(c) * h;
                edt1d(col, s.d.data(), static_cast<int>(h),
                      s.v.data(), s.z.data());
                std::copy(s.d.data(), s.d.data() + h, col);
            }
            for (uint32_t y = 0; y < h; ++y) {
                float* dst = out.data() + static_cast<size_t>(y) * w + x0;
                for (uint32_t c = 0; c < n; ++c)
                    dst[c] = tile[static_cast<size_t>(c) * h + y];
            }
        }
    });
    // Rows
    parallelSpans(h, w, [&](uint32_t begin, uint32_t end) {
        Scratch s(static_cast<int>(w));
        for (uint32_t y = begin; y < end; ++y) {
            float* row = out.data() + static_cast<size_t>(y) * w;
            std::copy(row, row + w, s.f.data());
            edt1d(s.f.data(), s.d.data(), static_cast<int>(w),
                  s.v.data(), s.z.data());
            std::copy(s.d.data(), s.d.data() + w, row);
        }
    });
}

} // namespace

std::vector<uint8_t> rasterizeMasks(const std::vector<MaskRenderState>& masks,
                                    uint32_t w, uint32_t h,
                                    const MaskRasterTransform* frameToTarget)
{
    const size_t npx = static_cast<size_t>(w) * h;
    std::vector<uint8_t> pixels(npx * 4, 0);
    if (w == 0 || h == 0) return pixels;

    // Feather/expansion are measured in the mask raster's pixels. For the
    // optional non-identity mapping, scale them by its average scale factor.
    float distScale = 1.0f;
    if (frameToTarget) {
        const float* m = frameToTarget->m;
        // Frame-norm unit steps (1/w, 1/h in identity mapping) map to:
        float sx = std::hypot(m[0], m[3]) / static_cast<float>(w);
        float sy = std::hypot(m[1], m[4]) / static_cast<float>(h);
        distScale = 0.5f * (sx + sy);
        if (!(distScale > 0.0f) || !std::isfinite(distScale)) distScale = 1.0f;
    }

    // Only a shape's bounding box, grown by the reach of feather and
    // expansion (+2 px for the anti-aliased edge), can differ from "fully
    // outside". Distances inside that window are exact: every pixel beyond
    // it is outside the shape, and a ring of outside pixels separates it
    // from the shape wherever the window is not clipped by the frame edge.
    // A small mask on a 4K clip no longer pays for two full-frame distance
    // transforms on every drag step.
    struct Prepared {
        std::vector<float> poly;   // empty = degenerate shape
        int x0{0}, y0{0}, x1{0}, y1{0};
        float feather{0.0f}, expansion{0.0f}, opacity{0.0f};
        bool inverted{false};
    };
    auto clampCoord = [](float v, uint32_t limit) {
        if (!(v > 0.0f)) return 0;  // also catches NaN
        if (v >= static_cast<float>(limit)) return static_cast<int>(limit);
        return static_cast<int>(v);
    };
    std::vector<Prepared> prepared;
    prepared.reserve(masks.size());
    // Union of everything any mask can change; the full frame as soon as
    // one mask is inverted (it lifts every pixel outside its shape).
    int ux0 = static_cast<int>(w), uy0 = static_cast<int>(h), ux1 = 0, uy1 = 0;
    for (const auto& mask : masks) {
        Prepared pm;
        pm.feather   = std::max(0.0f, mask.feather) * distScale;
        pm.expansion = mask.expansion * distScale;
        pm.opacity   = std::clamp(mask.maskOpacity, 0.0f, 1.0f);
        pm.inverted  = mask.inverted;
        if (buildMaskPolygon(mask, w, h, frameToTarget, pm.poly)) {
            float minX = pm.poly[0], maxX = pm.poly[0];
            float minY = pm.poly[1], maxY = pm.poly[1];
            for (size_t i = 2; i + 1 < pm.poly.size(); i += 2) {
                minX = std::min(minX, pm.poly[i]);
                maxX = std::max(maxX, pm.poly[i]);
                minY = std::min(minY, pm.poly[i + 1]);
                maxY = std::max(maxY, pm.poly[i + 1]);
            }
            const float margin =
                0.5f * pm.feather + std::max(0.0f, pm.expansion) + 2.0f;
            pm.x0 = clampCoord(std::floor(minX - margin), w);
            pm.y0 = clampCoord(std::floor(minY - margin), h);
            pm.x1 = clampCoord(std::ceil(maxX + margin) + 1.0f, w);
            pm.y1 = clampCoord(std::ceil(maxY + margin) + 1.0f, h);
        } else {
            pm.poly.clear();
        }
        if (pm.inverted && pm.opacity > 0.0f) {
            ux0 = 0; uy0 = 0;
            ux1 = static_cast<int>(w); uy1 = static_cast<int>(h);
        } else if (!pm.poly.empty() && pm.x1 > pm.x0 && pm.y1 > pm.y0) {
            ux0 = std::min(ux0, pm.x0); uy0 = std::min(uy0, pm.y0);
            ux1 = std::max(ux1, pm.x1); uy1 = std::max(uy1, pm.y1);
        }
        prepared.push_back(std::move(pm));
    }
    if (ux1 <= ux0 || uy1 <= uy0) return pixels;  // nothing visible

    const uint32_t uw = static_cast<uint32_t>(ux1 - ux0);
    const uint32_t uh = static_cast<uint32_t>(uy1 - uy0);
    std::vector<float> accum(static_cast<size_t>(uw) * uh, 0.0f);
    std::vector<uint8_t> inside;
    std::vector<float> distIn, distOut;

    for (const Prepared& pm : prepared) {
        const bool hasWindow =
            !pm.poly.empty() && pm.x1 > pm.x0 && pm.y1 > pm.y0;

        // Pixels outside the window are "far outside": alpha 0, or the full
        // mask opacity when inverted (everything, for a degenerate shape).
        if (pm.inverted && pm.opacity > 0.0f) {
            parallelSpans(uh, uw, [&](uint32_t begin, uint32_t end) {
                for (uint32_t y = begin; y < end; ++y) {
                    const int gy = uy0 + static_cast<int>(y);
                    const bool rowInWindow =
                        hasWindow && gy >= pm.y0 && gy < pm.y1;
                    float* row = accum.data() + static_cast<size_t>(y) * uw;
                    for (uint32_t x = 0; x < uw; ++x) {
                        const int gx = ux0 + static_cast<int>(x);
                        if (rowInWindow && gx >= pm.x0 && gx < pm.x1) continue;
                        row[x] = std::min(1.0f, row[x] + pm.opacity);
                    }
                }
            });
        }
        if (!hasWindow) continue;

        const uint32_t bw = static_cast<uint32_t>(pm.x1 - pm.x0);
        const uint32_t bh = static_cast<uint32_t>(pm.y1 - pm.y0);
        inside.assign(static_cast<size_t>(bw) * bh, 0);
        fillPolygon(pm.poly, pm.x0, pm.y0, bw, bh, inside);

        edt2d(inside, 0, bw, bh, distIn);   // dist² to nearest OUTSIDE pixel
        edt2d(inside, 1, bw, bh, distOut);  // dist² to nearest INSIDE pixel

        const float feather = pm.feather;
        parallelSpans(bh, bw, [&](uint32_t begin, uint32_t end) {
            for (uint32_t y = begin; y < end; ++y) {
                float* row = accum.data()
                    + static_cast<size_t>(pm.y0 - uy0 + static_cast<int>(y)) * uw
                    + (pm.x0 - ux0);
                const size_t base = static_cast<size_t>(y) * bw;
                for (uint32_t x = 0; x < bw; ++x) {
                    const size_t i = base + x;
                    // Signed distance: negative inside, positive outside.
                    float d = inside[i]
                        ? -(std::sqrt(distIn[i]) - 0.5f)
                        :  (std::sqrt(distOut[i]) - 0.5f);
                    d -= pm.expansion;

                    float a;
                    if (feather > 0.5f) {
                        float t = std::clamp(0.5f - d / feather, 0.0f, 1.0f);
                        a = t * t * (3.0f - 2.0f * t);   // smoothstep — Premiere-soft
                    } else {
                        a = std::clamp(0.5f - d, 0.0f, 1.0f);  // ~1px anti-aliased edge
                    }

                    if (pm.inverted) a = 1.0f - a;
                    a *= pm.opacity;
                    row[x] = std::min(1.0f, row[x] + a);  // additive combine (union)
                }
            }
        });
    }

    parallelSpans(uh, uw, [&](uint32_t begin, uint32_t end) {
        for (uint32_t y = begin; y < end; ++y) {
            const float* src = accum.data() + static_cast<size_t>(y) * uw;
            uint8_t* dst = pixels.data()
                + (static_cast<size_t>(uy0 + static_cast<int>(y)) * w + ux0) * 4;
            for (uint32_t x = 0; x < uw; ++x) {
                const uint8_t v8 = static_cast<uint8_t>(src[x] * 255.0f + 0.5f);
                std::memset(dst + static_cast<size_t>(x) * 4, v8, 4);
            }
        }
    });
    return pixels;
}

uint64_t hashMaskStates(const std::vector<MaskRenderState>& masks,
                        uint32_t w, uint32_t h,
                        const MaskRasterTransform* frameToTarget)
{
    uint64_t hash = 1469598103934665603ULL; // FNV offset basis
    auto mix = [&hash](const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            hash ^= p[i];
            hash *= 1099511628211ULL;
        }
    };
    mix(&w, sizeof(w));
    mix(&h, sizeof(h));
    if (frameToTarget) mix(frameToTarget->m, sizeof(frameToTarget->m));
    for (const auto& m : masks) {
        mix(&m.shape, sizeof(m.shape));
        mix(&m.coordinateSpace, sizeof(m.coordinateSpace));
        mix(&m.inverted, sizeof(m.inverted));
        mix(&m.feather, sizeof(m.feather));
        mix(&m.expansion, sizeof(m.expansion));
        mix(&m.maskOpacity, sizeof(m.maskOpacity));
        const auto& g = m.geometry;
        mix(&g.centerX, sizeof(g.centerX));
        mix(&g.centerY, sizeof(g.centerY));
        mix(&g.width, sizeof(g.width));
        mix(&g.height, sizeof(g.height));
        mix(&g.rotation, sizeof(g.rotation));
        if (!g.vertices.empty())
            mix(g.vertices.data(), g.vertices.size() * sizeof(MaskVertex));
    }
    return hash;
}

} // namespace rt
