/*
 * CompositeServiceBlend.cpp - CPU pixel blending helpers.
 * Extracted from CompositeServiceFrame.cpp.
 *
 * Contains blitLayerWithTransform (affine transform + alpha blend)
 * and rasterizeMasks (CPU mask rasterizer for opacity masks).
 */

#include "CompositeServiceBlend.h"
#include "timeline/OpacityMask.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

/// Map a normalized frame-space point (0–1) into target pixel space.
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

/// Even-odd scanline fill of a closed polygon (xy pairs, target px).
void fillPolygon(const std::vector<float>& poly, uint32_t w, uint32_t h,
                 std::vector<uint8_t>& inside)
{
    const size_t n = poly.size() / 2;
    std::vector<float> xs;
    xs.reserve(16);
    for (uint32_t y = 0; y < h; ++y) {
        const float sy = static_cast<float>(y) + 0.5f;
        xs.clear();
        for (size_t i = 0; i < n; ++i) {
            size_t j = (i + 1) % n;
            float y0 = poly[i * 2 + 1], y1 = poly[j * 2 + 1];
            if ((y0 <= sy && y1 > sy) || (y1 <= sy && y0 > sy)) {
                float t = (sy - y0) / (y1 - y0);
                xs.push_back(poly[i * 2] + t * (poly[j * 2] - poly[i * 2]));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            int x0 = std::max(0, static_cast<int>(std::ceil(xs[k] - 0.5f)));
            int x1 = std::min(static_cast<int>(w) - 1,
                              static_cast<int>(std::floor(xs[k + 1] - 0.5f)));
            for (int x = x0; x <= x1; ++x)
                inside[static_cast<size_t>(y) * w + x] = 1;
        }
    }
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

/// 2D squared EDT: dist² to the nearest seed (seed = pixels where
/// seedMask[i] != seedValue... seeds are pixels with mask[i]==seed).
void edt2d(const std::vector<uint8_t>& grid, uint8_t seed,
           uint32_t w, uint32_t h, std::vector<float>& out)
{
    constexpr float INF = 1e18f;
    out.assign(static_cast<size_t>(w) * h, 0.0f);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = (grid[i] == seed) ? 0.0f : INF;

    const int maxDim = static_cast<int>(std::max(w, h));
    std::vector<float> f(maxDim), d(maxDim), z(maxDim + 1);
    std::vector<int>   v(maxDim);

    // Columns
    for (uint32_t x = 0; x < w; ++x) {
        for (uint32_t y = 0; y < h; ++y) f[y] = out[static_cast<size_t>(y) * w + x];
        edt1d(f.data(), d.data(), static_cast<int>(h), v.data(), z.data());
        for (uint32_t y = 0; y < h; ++y) out[static_cast<size_t>(y) * w + x] = d[y];
    }
    // Rows
    for (uint32_t y = 0; y < h; ++y) {
        float* row = out.data() + static_cast<size_t>(y) * w;
        std::copy(row, row + w, f.data());
        edt1d(f.data(), d.data(), static_cast<int>(w), v.data(), z.data());
        std::copy(d.data(), d.data() + w, row);
    }
}

} // namespace

std::vector<uint8_t> rasterizeMasks(const std::vector<MaskRenderState>& masks,
                                    uint32_t w, uint32_t h,
                                    const MaskRasterTransform* frameToTarget)
{
    const size_t npx = static_cast<size_t>(w) * h;
    std::vector<uint8_t> pixels(npx * 4, 0);
    if (w == 0 || h == 0) return pixels;

    // Feather/expansion are authored in output-frame pixels. When
    // rasterizing into a different grid (effect masks in clip space),
    // scale them by the mapping's average scale factor.
    float distScale = 1.0f;
    if (frameToTarget) {
        const float* m = frameToTarget->m;
        // Frame-norm unit steps (1/w, 1/h in identity mapping) map to:
        float sx = std::hypot(m[0], m[3]) / static_cast<float>(w);
        float sy = std::hypot(m[1], m[4]) / static_cast<float>(h);
        distScale = 0.5f * (sx + sy);
        if (!(distScale > 0.0f) || !std::isfinite(distScale)) distScale = 1.0f;
    }

    std::vector<float> accum(npx, 0.0f);
    std::vector<float> poly;
    std::vector<uint8_t> inside;
    std::vector<float> distIn, distOut;

    for (const auto& mask : masks) {
        float alphaConst = -1.0f; // >= 0 → uniform alpha, skip fill

        if (!buildMaskPolygon(mask, w, h, frameToTarget, poly)) {
            // Degenerate shape contributes nothing (or everything when
            // inverted).
            alphaConst = mask.inverted ? mask.maskOpacity : 0.0f;
        }

        if (alphaConst >= 0.0f) {
            if (alphaConst > 0.0f)
                for (size_t i = 0; i < npx; ++i)
                    accum[i] = std::min(1.0f, accum[i] + alphaConst);
            continue;
        }

        inside.assign(npx, 0);
        fillPolygon(poly, w, h, inside);

        edt2d(inside, 0, w, h, distIn);   // dist² to nearest OUTSIDE pixel... see below
        edt2d(inside, 1, w, h, distOut);  // dist² to nearest INSIDE pixel

        const float feather   = std::max(0.0f, mask.feather) * distScale;
        const float expansion = mask.expansion * distScale;
        const float opac      = std::clamp(mask.maskOpacity, 0.0f, 1.0f);

        for (size_t i = 0; i < npx; ++i) {
            // Signed distance: negative inside, positive outside. distIn
            // holds dist²-to-outside (0 at outside pixels), distOut holds
            // dist²-to-inside (0 at inside pixels).
            float d = inside[i]
                ? -(std::sqrt(distIn[i]) - 0.5f)
                :  (std::sqrt(distOut[i]) - 0.5f);
            d -= expansion;

            float a;
            if (feather > 0.5f) {
                float t = std::clamp(0.5f - d / feather, 0.0f, 1.0f);
                a = t * t * (3.0f - 2.0f * t);   // smoothstep — Premiere-soft
            } else {
                a = std::clamp(0.5f - d, 0.0f, 1.0f);  // ~1px anti-aliased edge
            }

            if (mask.inverted) a = 1.0f - a;
            a *= opac;
            accum[i] = std::min(1.0f, accum[i] + a);  // additive combine (union)
        }
    }

    for (size_t i = 0; i < npx; ++i) {
        uint8_t v8 = static_cast<uint8_t>(accum[i] * 255.0f + 0.5f);
        pixels[i * 4 + 0] = v8;
        pixels[i * 4 + 1] = v8;
        pixels[i * 4 + 2] = v8;
        pixels[i * 4 + 3] = v8;
    }
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
