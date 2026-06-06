// SpinePreviewRendering.cpp - Rendering (extracted from SpinePreviewWidget.cpp).

#ifdef ROUNDTABLE_HAS_SPINE

#include "widgets/SpinePreviewWidget.h"
#include "Theme.h"
#include "spine/SpineEngine.h"
#include <QPainter>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <future>
#include <thread>
#include <spdlog/spdlog.h>

namespace rt {

static void boxBlurH(const uint32_t* src, uint32_t* dst, int w, int h, int radius)
{
    float invSize = 1.0f / static_cast<float>(radius + radius + 1);
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = src + y * w;
        uint32_t* out = dst + y * w;
        float rSum = 0, gSum = 0, bSum = 0, aSum = 0;
        // Initialize accumulator with left edge repeated
        for (int x = -radius; x <= radius; ++x) {
            int ix = std::clamp(x, 0, w - 1);
            uint32_t px = row[ix];
            aSum += static_cast<float>((px >> 24) & 0xFF);
            rSum += static_cast<float>((px >> 16) & 0xFF);
            gSum += static_cast<float>((px >>  8) & 0xFF);
            bSum += static_cast<float>((px      ) & 0xFF);
        }
        for (int x = 0; x < w; ++x) {
            out[x] = (static_cast<uint32_t>(aSum * invSize + 0.5f) << 24) |
                     (static_cast<uint32_t>(rSum * invSize + 0.5f) << 16) |
                     (static_cast<uint32_t>(gSum * invSize + 0.5f) <<  8) |
                      static_cast<uint32_t>(bSum * invSize + 0.5f);
            // Slide window
            int addIdx = std::min(x + radius + 1, w - 1);
            int subIdx = std::max(x - radius,     0);
            uint32_t addPx = row[addIdx];
            uint32_t subPx = row[subIdx];
            aSum += static_cast<float>((addPx >> 24) & 0xFF) - static_cast<float>((subPx >> 24) & 0xFF);
            rSum += static_cast<float>((addPx >> 16) & 0xFF) - static_cast<float>((subPx >> 16) & 0xFF);
            gSum += static_cast<float>((addPx >>  8) & 0xFF) - static_cast<float>((subPx >>  8) & 0xFF);
            bSum += static_cast<float>((addPx      ) & 0xFF) - static_cast<float>((subPx      ) & 0xFF);
        }
    }
}

static void boxBlurV(const uint32_t* src, uint32_t* dst, int w, int h, int radius)
{
    float invSize = 1.0f / static_cast<float>(radius + radius + 1);
    for (int x = 0; x < w; ++x) {
        float rSum = 0, gSum = 0, bSum = 0, aSum = 0;
        for (int y = -radius; y <= radius; ++y) {
            int iy = std::clamp(y, 0, h - 1);
            uint32_t px = src[iy * w + x];
            aSum += static_cast<float>((px >> 24) & 0xFF);
            rSum += static_cast<float>((px >> 16) & 0xFF);
            gSum += static_cast<float>((px >>  8) & 0xFF);
            bSum += static_cast<float>((px      ) & 0xFF);
        }
        for (int y = 0; y < h; ++y) {
            dst[y * w + x] = (static_cast<uint32_t>(aSum * invSize + 0.5f) << 24) |
                             (static_cast<uint32_t>(rSum * invSize + 0.5f) << 16) |
                             (static_cast<uint32_t>(gSum * invSize + 0.5f) <<  8) |
                              static_cast<uint32_t>(bSum * invSize + 0.5f);
            int addIdx = std::min(y + radius + 1, h - 1);
            int subIdx = std::max(y - radius,     0);
            uint32_t addPx = src[addIdx * w + x];
            uint32_t subPx = src[subIdx * w + x];
            aSum += static_cast<float>((addPx >> 24) & 0xFF) - static_cast<float>((subPx >> 24) & 0xFF);
            rSum += static_cast<float>((addPx >> 16) & 0xFF) - static_cast<float>((subPx >> 16) & 0xFF);
            gSum += static_cast<float>((addPx >>  8) & 0xFF) - static_cast<float>((subPx >>  8) & 0xFF);
            bSum += static_cast<float>((addPx      ) & 0xFF) - static_cast<float>((subPx      ) & 0xFF);
        }
    }
}

/// Apply a box blur to a QImage in-place (3 passes for Gaussian approximation).
/// @param img  Must be ARGB32 or ARGB32_Premultiplied format.
/// @param radius  Blur radius in pixels (0 = no-op).
static void applyBoxBlur(QImage& img, int radius)
{
    if (radius <= 0) return;
    int w = img.width(), h = img.height();
    if (w < 1 || h < 1) return;

    QImage tmp(w, h, img.format());
    auto* srcPx = reinterpret_cast<uint32_t*>(img.bits());
    auto* tmpPx = reinterpret_cast<uint32_t*>(tmp.bits());

    // Three passes: srcâ†’tmp (H), tmpâ†’src (V), repeat
    for (int pass = 0; pass < 3; ++pass) {
        boxBlurH(srcPx, tmpPx, w, h, radius);
        boxBlurV(tmpPx, srcPx, w, h, radius);
    }
}

/// Apply a box blur only within a sub-rectangle of an image (3-pass).
/// Pixels outside the rect are untouched. Much faster when the character
/// occupies a small fraction of the full-screen buffer.
static void applyBoxBlurRect(QImage& img, int radius, QRect rect)
{
    if (radius <= 0) return;
    // Clamp rect to image bounds
    rect = rect.intersected(img.rect());
    if (rect.isEmpty()) return;

    // Extract the sub-region, blur it, then paste back
    QImage sub = img.copy(rect);
    applyBoxBlur(sub, radius);

    // Blit blurred sub-region back
    const int subW = sub.width();
    const int subH = sub.height();
    const int imgStride = img.bytesPerLine() / 4;
    const int subStride = sub.bytesPerLine() / 4;
    auto* dstPx = reinterpret_cast<uint32_t*>(img.bits());
    const auto* srcPx = reinterpret_cast<const uint32_t*>(sub.constBits());
    for (int y = 0; y < subH; ++y) {
        std::memcpy(dstPx + (rect.y() + y) * imgStride + rect.x(),
                    srcPx + y * subStride,
                    static_cast<size_t>(subW) * 4);
    }
}

namespace {

inline void blendNormal(uint32_t* dst, uint32_t sR, uint32_t sG, uint32_t sB, uint32_t sA)
{
    if (sA == 0) return;
    if (sA >= 255) {
        *dst = (255u << 24) | (sR << 16) | (sG << 8) | sB;
        return;
    }
    uint32_t dVal = *dst;
    uint32_t invA = 255 - sA;
    uint32_t dR = (dVal >> 16) & 0xFF;
    uint32_t dG = (dVal >>  8) & 0xFF;
    uint32_t dB = (dVal      ) & 0xFF;
    uint32_t dA = (dVal >> 24) & 0xFF;
    uint32_t oR = sR + ((dR * invA + 127) / 255);
    uint32_t oG = sG + ((dG * invA + 127) / 255);
    uint32_t oB = sB + ((dB * invA + 127) / 255);
    uint32_t oA = sA + ((dA * invA + 127) / 255);
    *dst = (std::min(oA, 255u) << 24) |
           (std::min(oR, 255u) << 16) |
           (std::min(oG, 255u) <<  8) |
            std::min(oB, 255u);
}

inline void blendAdditive(uint32_t* dst, uint32_t sR, uint32_t sG, uint32_t sB, uint32_t /*sA*/)
{
    uint32_t dVal = *dst;
    uint32_t oR = std::min(255u, ((dVal >> 16) & 0xFF) + sR);
    uint32_t oG = std::min(255u, ((dVal >>  8) & 0xFF) + sG);
    uint32_t oB = std::min(255u, ((dVal      ) & 0xFF) + sB);
    uint32_t oA = (dVal >> 24) & 0xFF;
    *dst = (oA << 24) | (oR << 16) | (oG << 8) | oB;
}

inline void blendMultiply(uint32_t* dst, uint32_t sR, uint32_t sG, uint32_t sB, uint32_t sA)
{
    if (sA == 0) return;
    uint32_t dVal = *dst;
    uint32_t dR = (dVal >> 16) & 0xFF;
    uint32_t dG = (dVal >>  8) & 0xFF;
    uint32_t dB = (dVal      ) & 0xFF;
    uint32_t invA = 255 - sA;
    uint32_t oR = (dR * sR + dR * invA + 127) / 255;
    uint32_t oG = (dG * sG + dG * invA + 127) / 255;
    uint32_t oB = (dB * sB + dB * invA + 127) / 255;
    uint32_t oA = (dVal >> 24) & 0xFF;
    *dst = (oA << 24) | (std::min(oR,255u) << 16) | (std::min(oG,255u) << 8) | std::min(oB,255u);
}

inline void blendScreen(uint32_t* dst, uint32_t sR, uint32_t sG, uint32_t sB, uint32_t sA)
{
    if (sA == 0) return;
    uint32_t dVal = *dst;
    uint32_t dR = (dVal >> 16) & 0xFF;
    uint32_t dG = (dVal >>  8) & 0xFF;
    uint32_t dB = (dVal      ) & 0xFF;
    uint32_t oR = std::min(255u, sR + ((dR * (255 - sR) + 127) / 255));
    uint32_t oG = std::min(255u, sG + ((dG * (255 - sG) + 127) / 255));
    uint32_t oB = std::min(255u, sB + ((dB * (255 - sB) + 127) / 255));
    uint32_t oA = (dVal >> 24) & 0xFF;
    *dst = (oA << 24) | (oR << 16) | (oG << 8) | oB;
}

void rasteriseBatches(const SpineRenderData& meshData,
                      const std::vector<QImage>& textures,
                      uint32_t* bufPixels, int bufStride, int ww, int wh,
                      float offsetX, float offsetY, float scale,
                      bool flipX, bool flipY, float layerOpacity)
{
    for (const auto& batch : meshData.batches) {
        const QImage* tex = nullptr;
        if (batch.texturePageIndex >= 0 &&
            batch.texturePageIndex < static_cast<int>(textures.size())) {
            tex = &textures[static_cast<size_t>(batch.texturePageIndex)];
        }
        if (!tex || tex->isNull()) continue;

        const uint32_t* texPixels = reinterpret_cast<const uint32_t*>(tex->constBits());
        int texW = tex->width();
        int texH = tex->height();
        int texStride = tex->bytesPerLine() / 4;

        // Pick blend function once per batch (avoids per-pixel switch)
        using BlendFn = void(*)(uint32_t*, uint32_t, uint32_t, uint32_t, uint32_t);
        BlendFn blendFn;
        switch (batch.blendMode) {
        case SpineBlendMode::Additive: blendFn = blendAdditive; break;
        case SpineBlendMode::Multiply: blendFn = blendMultiply; break;
        case SpineBlendMode::Screen:   blendFn = blendScreen;   break;
        default:                       blendFn = blendNormal;   break;
        }

        for (size_t t = 0; t + 2 < batch.indices.size(); t += 3) {
            const auto& v0 = batch.vertices[batch.indices[t]];
            const auto& v1 = batch.vertices[batch.indices[t + 1]];
            const auto& v2 = batch.vertices[batch.indices[t + 2]];

            if (v0.a < 0.004f) continue;

            float xMul = flipX ? -scale : scale;
            float yMul = flipY ? -scale : scale;
            float sx0 = v0.x * xMul + offsetX;
            float sy0 = static_cast<float>(wh) - (v0.y * yMul + offsetY);
            float sx1 = v1.x * xMul + offsetX;
            float sy1 = static_cast<float>(wh) - (v1.y * yMul + offsetY);
            float sx2 = v2.x * xMul + offsetX;
            float sy2 = static_cast<float>(wh) - (v2.y * yMul + offsetY);

            int minX = std::max(0, static_cast<int>(std::floor(std::min({sx0, sx1, sx2}))));
            int maxX = std::min(ww - 1, static_cast<int>(std::ceil(std::max({sx0, sx1, sx2}))));
            int minY = std::max(0, static_cast<int>(std::floor(std::min({sy0, sy1, sy2}))));
            int maxY = std::min(wh - 1, static_cast<int>(std::ceil(std::max({sy0, sy1, sy2}))));

            if (minX > maxX || minY > maxY) continue;

            float denom = (sy1 - sy2) * (sx0 - sx2) + (sx2 - sx1) * (sy0 - sy2);
            if (std::abs(denom) < 1e-4f) continue;
            float invDenom = 1.0f / denom;

            float vA = v0.a * layerOpacity;
            float vR = v0.r * vA;
            float vG = v0.g * vA;
            float vB = v0.b * vA;

            // â”€â”€ Incremental barycentric + UV interpolation â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            // Precompute per-pixel x/y increments for w0, w1
            float w0_dx = (sy1 - sy2) * invDenom;
            float w1_dx = (sy2 - sy0) * invDenom;
            float w0_dy = (sx2 - sx1) * invDenom;
            float w1_dy = (sx0 - sx2) * invDenom;

            // Precompute UV increments (u = w0*(v0.u-v2.u) + w1*(v1.u-v2.u) + v2.u)
            float du_dw0 = v0.u - v2.u;
            float du_dw1 = v1.u - v2.u;
            float dv_dw0 = v0.v - v2.v;
            float dv_dw1 = v1.v - v2.v;

            float u_dx = w0_dx * du_dw0 + w1_dx * du_dw1;
            float tv_dx = w0_dx * dv_dw0 + w1_dx * dv_dw1;
            float u_dy = w0_dy * du_dw0 + w1_dy * du_dw1;
            float tv_dy = w0_dy * dv_dw0 + w1_dy * dv_dw1;

            // Initial values at (minX+0.5, minY+0.5)
            float startFx = static_cast<float>(minX) + 0.5f;
            float startFy = static_cast<float>(minY) + 0.5f;
            float w0_row = ((sy1 - sy2) * (startFx - sx2) + (sx2 - sx1) * (startFy - sy2)) * invDenom;
            float w1_row = ((sy2 - sy0) * (startFx - sx2) + (sx0 - sx2) * (startFy - sy2)) * invDenom;
            float u_row  = w0_row * du_dw0 + w1_row * du_dw1 + v2.u;
            float tv_row = w0_row * dv_dw0 + w1_row * dv_dw1 + v2.v;

            for (int py = minY; py <= maxY; ++py) {
                float cw0 = w0_row;
                float cw1 = w1_row;
                float cu  = u_row;
                float cv  = tv_row;
                uint32_t* row = bufPixels + py * bufStride;

                for (int px = minX; px <= maxX; ++px) {
                    float cw2 = 1.0f - cw0 - cw1;

                    if (cw0 >= 0.0f && cw1 >= 0.0f && cw2 >= 0.0f) {
                        int txI = static_cast<int>(cu * texW);
                        int tyI = static_cast<int>(cv * texH);
                        if (txI < 0) txI = 0; else if (txI >= texW) txI = texW - 1;
                        if (tyI < 0) tyI = 0; else if (tyI >= texH) tyI = texH - 1;

                        uint32_t texel = texPixels[tyI * texStride + txI];
                        uint32_t tA = (texel >> 24) & 0xFF;
                        if (tA != 0) {
                            uint32_t tR = (texel >> 16) & 0xFF;
                            uint32_t tG = (texel >>  8) & 0xFF;
                            uint32_t tB = (texel      ) & 0xFF;

                            // Premultiply by texture alpha so the blend
                            // function (premultiplied over) works correctly.
                            // Without this, semi-transparent edge texels
                            // produce colours brighter than their alpha,
                            // causing a visible 1px bright fringe.
                            float tAf = tA * (1.0f / 255.0f);
                            uint32_t oR = std::min(255u, static_cast<uint32_t>(tR * vR * tAf));
                            uint32_t oG = std::min(255u, static_cast<uint32_t>(tG * vG * tAf));
                            uint32_t oB = std::min(255u, static_cast<uint32_t>(tB * vB * tAf));
                            uint32_t oA = std::min(255u, static_cast<uint32_t>(tA * vA));

                            blendFn(&row[px], oR, oG, oB, oA);
                        }
                    }
                    cw0 += w0_dx;
                    cw1 += w1_dx;
                    cu  += u_dx;
                    cv  += tv_dx;
                }
                w0_row += w0_dy;
                w1_row += w1_dy;
                u_row  += u_dy;
                tv_row += tv_dy;
            }
        }
    }
}

} // anonymous namespace

void SpinePreviewWidget::renderSingleEngine(QPainter& painter)
{
    auto meshData = m_engine->extractMeshes();
    if (meshData.batches.empty()) {
        painter.fillRect(rect(), m_bgColor);
        painter.setPen(Theme::colors().textDisabled);
        painter.drawText(rect(), Qt::AlignCenter, "No mesh data");
        return;
    }

    if (!m_boundsCached) {
        m_engine->getBounds(m_cachedBoundsX, m_cachedBoundsY,
                            m_cachedBoundsW, m_cachedBoundsH);
        m_boundsCached = true;
    }

    float bx = m_cachedBoundsX, by = m_cachedBoundsY;
    float bw = m_cachedBoundsW, bh = m_cachedBoundsH;
    if (bw < 1.0f || bh < 1.0f) {
        bx = -200; by = -600; bw = 400; bh = 700;
    }

    int ww = width(), wh = height();

    float margin = 0.05f;
    float viewW = bw * (1.0f + 2.0f * margin);
    float viewH = bh * (1.0f + 2.0f * margin);
    float viewX = bx - bw * margin;
    float viewY = by - bh * margin;

    float scaleX = static_cast<float>(ww) / viewW;
    float scaleY = static_cast<float>(wh) / viewH;
    float scale = std::min(scaleX, scaleY);

    float offsetX = (ww - viewW * scale) * 0.5f - viewX * scale;
    float offsetY = (wh - viewH * scale) * 0.5f - viewY * scale;

    if (m_backBuffer.width() != ww || m_backBuffer.height() != wh)
        m_backBuffer = QImage(ww, wh, QImage::Format_ARGB32_Premultiplied);
    m_backBuffer.fill(m_bgColor);

    if (!m_bgImage.isNull()) {
        QPainter bgPainter(&m_backBuffer);
        QImage scaled = m_bgImage.scaled(ww, wh, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        bgPainter.drawImage((ww - scaled.width()) / 2, (wh - scaled.height()) / 2, scaled);
        bgPainter.end();
    }

    uint32_t* bufPixels = reinterpret_cast<uint32_t*>(m_backBuffer.bits());
    int bufStride = m_backBuffer.bytesPerLine() / 4;

    rasteriseBatches(meshData, m_textures, bufPixels, bufStride,
                     ww, wh, offsetX, offsetY, scale, false, false, 1.0f);

    painter.drawImage(0, 0, m_backBuffer);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Multi-layer rendering (ShotComposer â€” composites all characters)
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void SpinePreviewWidget::renderMultiLayer(QPainter& painter)
{
    int ww = width(), wh = height();

    if (m_backBuffer.width() != ww || m_backBuffer.height() != wh)
        m_backBuffer = QImage(ww, wh, QImage::Format_ARGB32_Premultiplied);
    m_backBuffer.fill(m_bgColor);

    // ── Use fixed 16:9 logical canvas (1920×1080) for parity with the
    // Program Monitor output.  computeCanvasTransform() is the single source
    // of truth shared with the overlay frame / hit-testing / dragging, so
    // content and the shot frame stay locked together at every zoom level.
    float canvasW, canvasH, canvasOriginX, canvasOriginY;
    computeCanvasTransform(canvasOriginX, canvasOriginY, canvasW, canvasH);

    // If no layers exist but we have a global BG image (legacy path), draw it
    if (m_layers.empty() && !m_bgImage.isNull()) {
        QPainter bgPainter(&m_backBuffer);
        int cw = std::max(1, static_cast<int>(canvasW));
        int ch = std::max(1, static_cast<int>(canvasH));
        QImage scaledBg = m_bgImage.scaled(cw, ch,
            Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        int bgX = static_cast<int>(canvasOriginX) + (cw - scaledBg.width()) / 2;
        int bgY = static_cast<int>(canvasOriginY) + (ch - scaledBg.height()) / 2;
        bgPainter.drawImage(bgX, bgY, scaledBg);
        bgPainter.end();
    }

    uint32_t* bufPixels = reinterpret_cast<uint32_t*>(m_backBuffer.bits());
    int bufStride = m_backBuffer.bytesPerLine() / 4;

    // â”€â”€ Collect layers and extract mesh data on the main thread â”€â”€â”€â”€â”€â”€â”€â”€
    // SpineEngine::extractMeshes() is NOT thread-safe, so we call it here.
    // Then we dispatch the actual rasterization to worker threads.

    struct CharRasterJob {
        SpineRenderData meshData;
        const std::vector<QImage>* textures;
        float offsetX, offsetY, charScale;
        bool  flipX;
        bool  flipY;
        float opacity;
        int   zOrder;  // position in the layer stack
        bool  hasCrop{false};
        QRect cropRect;  // screen-space clip rect when hasCrop is true
        float blur{0.0f};
        QRect screenRect; // character bounding rect for targeted blur
    };

    std::vector<CharRasterJob> charJobs;
    int layerZOrder = 0;

    // Render each layer (bottom to top) â€” backgrounds + characters interleaved
    for (auto& layer : m_layers) {
        if (!layer.visible) { ++layerZOrder; continue; }
        if (layer.opacity < 0.004f) { ++layerZOrder; continue; }

        // â”€â”€ Background / video image layer â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (layer.isBackground) {
            // Flush any pending character jobs BEFORE this background
            // (characters below this BG must be composited first)
            if (!charJobs.empty()) {
                // Check if any job needs crop clipping or blur
                bool anyCrop = false;
                bool anyBlur = false;
                for (const auto& j : charJobs) {
                    if (j.hasCrop) anyCrop = true;
                    if (j.blur > 0.5f) anyBlur = true;
                }

                if (charJobs.size() == 1 && !anyCrop && !anyBlur) {
                    // Single job, no crop, no blur â€” rasterise directly onto the back buffer
                    auto& job = charJobs[0];
                    rasteriseBatches(job.meshData, *job.textures, bufPixels, bufStride,
                                     ww, wh, job.offsetX, job.offsetY, job.charScale,
                                     job.flipX, job.flipY, job.opacity);
                } else {
                    // Rasterise each job into its own buffer, then composite with clip
                    std::vector<QImage> layerBuffers(charJobs.size());
                    std::vector<std::future<void>> futures;
                    futures.reserve(charJobs.size());

                    for (size_t ji = 0; ji < charJobs.size(); ++ji) {
                        layerBuffers[ji] = QImage(ww, wh, QImage::Format_ARGB32_Premultiplied);
                        layerBuffers[ji].fill(Qt::transparent);
                        futures.push_back(std::async(std::launch::async,
                            [&charJobs, &layerBuffers, ji, ww, wh]() {
                                auto& job = charJobs[ji];
                                auto* px = reinterpret_cast<uint32_t*>(layerBuffers[ji].bits());
                                int stride = layerBuffers[ji].bytesPerLine() / 4;
                                rasteriseBatches(job.meshData, *job.textures, px, stride,
                                                 ww, wh, job.offsetX, job.offsetY, job.charScale,
                                                 job.flipX, job.flipY, job.opacity);
                            }));
                    }
                    for (auto& f : futures) f.get();

                    // Apply blur to individual layer buffers before compositing
                    for (size_t ji = 0; ji < charJobs.size(); ++ji) {
                        if (charJobs[ji].blur > 0.5f) {
                            int blurRadius = std::max(1, static_cast<int>(charJobs[ji].blur * 0.5f));
                            QRect blurRect = charJobs[ji].screenRect.adjusted(
                                -blurRadius * 3, -blurRadius * 3,
                                 blurRadius * 3,  blurRadius * 3);
                            applyBoxBlurRect(layerBuffers[ji], blurRadius, blurRect);
                        }
                    }

                    // Composite layer buffers onto back buffer â€” with crop clip per job
                    QPainter comp(&m_backBuffer);
                    comp.setCompositionMode(QPainter::CompositionMode_SourceOver);
                    for (size_t ji = 0; ji < charJobs.size(); ++ji) {
                        if (charJobs[ji].hasCrop) {
                            comp.save();
                            comp.setClipRect(charJobs[ji].cropRect);
                            comp.drawImage(0, 0, layerBuffers[ji]);
                            comp.restore();
                        } else {
                            comp.drawImage(0, 0, layerBuffers[ji]);
                        }
                    }
                    comp.end();
                    bufPixels = reinterpret_cast<uint32_t*>(m_backBuffer.bits());
                }
                charJobs.clear();
            }

            if (layer.backgroundImage.isNull()) { ++layerZOrder; continue; }

            // Compute display size
            float imgW = static_cast<float>(layer.backgroundImage.width());
            float imgH = static_cast<float>(layer.backgroundImage.height());
            int displayW, displayH;
            if (layer.isVideoCharacter) {
                // Character-style sizing: fit to ~85% of canvas height
                float fitScale = canvasH / imgH * 0.85f;
                float charScale = fitScale * layer.scale;
                displayW = std::max(1, static_cast<int>(imgW * charScale));
                displayH = std::max(1, static_cast<int>(imgH * charScale));
                static int vcComposeLog = 0;
                if (vcComposeLog < 10) {
                    spdlog::warn("[VC-COMPOSE] img={}x{} canvas={:.0f}x{:.0f} "
                        "fitScale={:.4f} layer.scale={:.3f} charScale={:.4f} "
                        "display={}x{} display/canvas={:.1f}%",
                        static_cast<int>(imgW), static_cast<int>(imgH),
                        canvasW, canvasH, fitScale, layer.scale, charScale,
                        displayW, displayH, displayH/canvasH*100.0f);
                    ++vcComposeLog;
                }
            } else {
                // Background: scale to fill canvas
                float scaleToFill = std::max(canvasW / imgW, canvasH / imgH);
                displayW = std::max(1, static_cast<int>(imgW * scaleToFill * layer.scale));
                displayH = std::max(1, static_cast<int>(imgH * scaleToFill * layer.scale));
            }

            // Cache the scaled image (invalidate when display size changes)
            if (layer.scaledBgCache.isNull() ||
                layer.scaledBgCacheW != displayW ||
                layer.scaledBgCacheH != displayH) {
                // Use FastTransformation for all actively-playing video layers
                // (both backgrounds and characters) since they update every frame
                // and the source is already high-res. SmoothTransformation for
                // static images only.
                auto transformMode = layer.videoFrameProvider
                    ? Qt::FastTransformation : Qt::SmoothTransformation;
                layer.scaledBgCache = layer.backgroundImage.scaled(
                    displayW, displayH,
                    Qt::IgnoreAspectRatio, transformMode);
                layer.scaledBgCacheW = displayW;
                layer.scaledBgCacheH = displayH;
            }

            // Position: posX/posY map to canvas coordinates (0.5 = center)
            float centerX = canvasOriginX + layer.posX * canvasW;
            float centerY = canvasOriginY + layer.posY * canvasH;
            int bgX = static_cast<int>(centerX - displayW * 0.5f);
            int bgY = static_cast<int>(centerY - displayH * 0.5f);

            // Apply blur to background if requested (cached)
            const QImage* drawImgPtr = &layer.scaledBgCache;
            if (layer.blur > 0.5f) {
                // Re-blur only when blur value changes or source cache was rebuilt
                bool needReblur = layer.blurredBgCache.isNull()
                    || std::abs(layer.blurredBgBlurVal - layer.blur) > 0.01f
                    || layer.blurredBgCache.size() != layer.scaledBgCache.size();
                if (needReblur) {
                    layer.blurredBgCache = layer.scaledBgCache.copy();
                    int blurRadius = std::max(1, static_cast<int>(layer.blur * 0.5f));
                    applyBoxBlur(layer.blurredBgCache, blurRadius);
                    layer.blurredBgBlurVal = layer.blur;
                }
                drawImgPtr = &layer.blurredBgCache;
            }
            const QImage& drawImg = *drawImgPtr;

            QPainter bgPainter(&m_backBuffer);
            if (layer.opacity < 0.996f) {
                bgPainter.setOpacity(static_cast<double>(layer.opacity));
            }
            // Apply crop clipping if any crop values are set
            bool bgHasCrop = (layer.cropLeft > 0.01f || layer.cropRight > 0.01f ||
                              layer.cropTop > 0.01f || layer.cropBottom > 0.01f);
            if (bgHasCrop) {
                float cL = displayW * (layer.cropLeft / 100.0f);
                float cR = displayW * (layer.cropRight / 100.0f);
                float cT = displayH * (layer.cropTop / 100.0f);
                float cB = displayH * (layer.cropBottom / 100.0f);
                QRectF clipRect(bgX + cL, bgY + cT,
                                displayW - cL - cR, displayH - cT - cB);
                bgPainter.setClipRect(clipRect);
            }
            // Apply rotation around the center of the image
            bool hasRotation = (std::abs(layer.rotation) > 0.01f);
            if (hasRotation) {
                bgPainter.save();
                bgPainter.translate(centerX, centerY);
                bgPainter.rotate(static_cast<double>(layer.rotation));
                if (layer.flipX || layer.flipY) {
                    bgPainter.scale(layer.flipX ? -1.0 : 1.0,
                                    layer.flipY ? -1.0 : 1.0);
                }
                bgPainter.drawImage(-displayW / 2, -displayH / 2, drawImg);
                bgPainter.restore();
            } else if (layer.flipX || layer.flipY) {
                bgPainter.save();
                // Translate to the far edge of each flipped axis so the
                // mirrored image still lands in the same screen rect.
                bgPainter.translate(bgX + (layer.flipX ? displayW : 0),
                                    bgY + (layer.flipY ? displayH : 0));
                bgPainter.scale(layer.flipX ? -1.0 : 1.0,
                                layer.flipY ? -1.0 : 1.0);
                bgPainter.drawImage(0, 0, drawImg);
                bgPainter.restore();
            } else {
                bgPainter.drawImage(bgX, bgY, drawImg);
            }
            bgPainter.end();

            // Refresh pixel pointer after QPainter usage (safety)
            bufPixels = reinterpret_cast<uint32_t*>(m_backBuffer.bits());
            ++layerZOrder;
            continue;
        }

        // â”€â”€ Character (Spine) layer â€” queue for parallel rasterization â”€â”€
        if (!layer.engine || !layer.engine->isLoaded()) {
            ++layerZOrder;
            continue;
        }

        auto meshData = layer.engine->extractMeshes();
        if (meshData.batches.empty()) { ++layerZOrder; continue; }

        // Cache bounds
        if (!layer.boundsCached) {
            layer.engine->getBounds(layer.boundsX, layer.boundsY,
                                     layer.boundsW, layer.boundsH);
            layer.boundsCached = true;
        }

        float bw = layer.boundsW;
        float bh = layer.boundsH;
        if (bw < 1.0f) bw = 400.0f;
        if (bh < 1.0f) bh = 700.0f;

        // Fit character to ~85% of widget height, then multiply by preset scale
        float fitScale = static_cast<float>(wh) / bh * 0.85f;
        float charScale = fitScale * layer.scale * m_viewZoom;

        // The character's visual center in Spine coordinates
        float spCenterX = layer.boundsX + layer.boundsW * 0.5f;
        float spCenterY = layer.boundsY + layer.boundsH * 0.5f;

        // posX/posY are normalized 0â€“1 (0.5 = center of canvas)
        float xMul = layer.flipX ? -charScale : charScale;
        float yMul = layer.flipY ? -charScale : charScale;
        float screenCenterX = canvasOriginX + layer.posX * canvasW;
        float screenCenterY = canvasOriginY + layer.posY * canvasH;

        float offsetX = screenCenterX - spCenterX * xMul;
        float offsetY = static_cast<float>(wh) - screenCenterY - spCenterY * yMul;

        // Compute crop clip rect if needed
        bool hasCrop = (layer.cropLeft > 0.01f || layer.cropRight > 0.01f ||
                        layer.cropTop > 0.01f || layer.cropBottom > 0.01f);
        QRect cropRect;
        if (hasCrop) {
            QRect lr = layerScreenRect(layer);
            float cL = lr.width()  * (layer.cropLeft / 100.0f);
            float cR = lr.width()  * (layer.cropRight / 100.0f);
            float cT = lr.height() * (layer.cropTop / 100.0f);
            float cB = lr.height() * (layer.cropBottom / 100.0f);
            cropRect = QRect(static_cast<int>(lr.left() + cL),
                             static_cast<int>(lr.top() + cT),
                             static_cast<int>(lr.width() - cL - cR),
                             static_cast<int>(lr.height() - cT - cB));
        }

        charJobs.push_back({
            std::move(meshData), &layer.textures,
            offsetX, offsetY, charScale,
            layer.flipX, layer.flipY, layer.opacity, layerZOrder,
            hasCrop, cropRect, layer.blur,
            layerScreenRect(layer)
        });

        ++layerZOrder;
    }

    // â”€â”€ Flush remaining character jobs â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (!charJobs.empty()) {
        bool anyCrop = false;
        bool anyBlur = false;
        for (const auto& j : charJobs) {
            if (j.hasCrop) anyCrop = true;
            if (j.blur > 0.5f) anyBlur = true;
        }

        if (charJobs.size() == 1 && !anyCrop && !anyBlur) {
            auto& job = charJobs[0];
            rasteriseBatches(job.meshData, *job.textures, bufPixels, bufStride,
                             ww, wh, job.offsetX, job.offsetY, job.charScale,
                             job.flipX, job.flipY, job.opacity);
        } else {
            std::vector<QImage> layerBuffers(charJobs.size());
            std::vector<std::future<void>> futures;
            futures.reserve(charJobs.size());

            for (size_t ji = 0; ji < charJobs.size(); ++ji) {
                layerBuffers[ji] = QImage(ww, wh, QImage::Format_ARGB32_Premultiplied);
                layerBuffers[ji].fill(Qt::transparent);
                futures.push_back(std::async(std::launch::async,
                    [&charJobs, &layerBuffers, ji, ww, wh]() {
                        auto& job = charJobs[ji];
                        auto* px = reinterpret_cast<uint32_t*>(layerBuffers[ji].bits());
                        int stride = layerBuffers[ji].bytesPerLine() / 4;
                        rasteriseBatches(job.meshData, *job.textures, px, stride,
                                         ww, wh, job.offsetX, job.offsetY, job.charScale,
                                         job.flipX, job.flipY, job.opacity);
                    }));
            }
            for (auto& f : futures) f.get();

            // Apply blur to individual layer buffers before compositing
            for (size_t ji = 0; ji < charJobs.size(); ++ji) {
                if (charJobs[ji].blur > 0.5f) {
                    int blurRadius = std::max(1, static_cast<int>(charJobs[ji].blur * 0.5f));
                    QRect blurRect = charJobs[ji].screenRect.adjusted(
                        -blurRadius * 3, -blurRadius * 3,
                         blurRadius * 3,  blurRadius * 3);
                    applyBoxBlurRect(layerBuffers[ji], blurRadius, blurRect);
                }
            }

            QPainter comp(&m_backBuffer);
            comp.setCompositionMode(QPainter::CompositionMode_SourceOver);
            for (size_t ji = 0; ji < charJobs.size(); ++ji) {
                if (charJobs[ji].hasCrop) {
                    comp.save();
                    comp.setClipRect(charJobs[ji].cropRect);
                    comp.drawImage(0, 0, layerBuffers[ji]);
                    comp.restore();
                } else {
                    comp.drawImage(0, 0, layerBuffers[ji]);
                }
            }
            comp.end();
        }
    }

    painter.drawImage(0, 0, m_backBuffer);

    // Draw transform overlay on top
    drawTransformOverlay(painter);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Transform overlay â€” Photoshop-style bounding box + corner handles
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
