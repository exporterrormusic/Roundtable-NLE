/*
 * ShotComposerThumbnailGen.cpp - Character/shot thumbnail generation.
 * Split from ShotComposerThumbnails.cpp.
 */

#include "panels/characters/ShotComposer.h"
#include "panels/characters/ShotComposerInternal.h"
#include "panels/characters/CharacterThumbnailCache.h"

#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/SpineEngine.h"
#include "spine/AnimationVideoCache.h"
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>

#include <spdlog/spdlog.h>
#include <filesystem>


namespace rt {

// ── Filter icon generators ──────────────────────────────────────────────

QPixmap makeAllFilterIcon(int sz)
{
    const int w = sz;
    const int h = sz;
    QPixmap pix(w, h);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);

    // Green rounded background
    QColor green(48, 164, 74);
    p.setBrush(green);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(0, 0, w, h, 6, 6);

    // Big check mark in upper portion
    QPen checkPen(QColor(255, 255, 255), 5);
    checkPen.setCapStyle(Qt::RoundCap);
    checkPen.setJoinStyle(Qt::RoundJoin);
    p.setPen(checkPen);
    QPainterPath checkPath;
    checkPath.moveTo(w * 0.30f, h * 0.46f);
    checkPath.lineTo(w * 0.46f, h * 0.62f);
    checkPath.lineTo(w * 0.72f, h * 0.32f);
    p.drawPath(checkPath);

    // "ALL" label at bottom 1/4
    QFont f = p.font();
    f.setPixelSize(13);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255));
    QRectF labelRect(0, h * 0.68f, w, h * 0.32f);
    p.drawText(labelRect, Qt::AlignCenter, QStringLiteral("ALL"));
    p.end();
    return pix;
}

QPixmap makeUnassignedFilterIcon(int sz)
{
    const int w = sz;
    const int h = sz;
    QPixmap pix(w, h);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);

    // Orange rounded background
    QColor orange(210, 130, 30);
    p.setBrush(orange);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(0, 0, w, h, 6, 6);

    // Big empty-set symbol in upper portion
    QPen symbolPen(QColor(255, 255, 255), 3);
    p.setPen(symbolPen);
    QFont f = p.font();
    f.setPixelSize(34);
    p.setFont(f);
    QRectF symbolRect(0, 0, w, h * 0.70f);
    p.drawText(symbolRect, Qt::AlignCenter, QStringLiteral("\u2205"));

    // "UNASSIGNED" label at bottom 1/4
    f.setPixelSize(9);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255));
    QRectF labelRect(2, h * 0.68f, w - 4, h * 0.32f);
    p.drawText(labelRect, Qt::AlignCenter, QStringLiteral("UNASSIGNED"));
    p.end();
    return pix;
}

QPixmap makeFilterDividerIcon(int width)
{
    QPixmap pix(width, 8);
    pix.fill(Qt::transparent);
    QPainter dp(&pix);
    dp.setRenderHint(QPainter::Antialiasing);
    QPen linePen(Theme::colors().borderLight, 1);
    dp.setPen(linePen);
    dp.drawLine(6, 4, width - 6, 4);
    dp.end();
    return pix;
}


QPixmap ShotComposer::makeCharacterThumbnail(const std::string& charName, int sz)
{
    // Ã¢â€â‚¬Ã¢â€â‚¬ Check in-memory thumbnail cache first Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    std::string cacheKey = charName + ":" + std::to_string(sz);
    if (auto it = m_charThumbCache.find(cacheKey); it != m_charThumbCache.end())
        return it->second;

    // Ã¢â€â‚¬Ã¢â€â‚¬ Per-character outfit overrides for thumbnails Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    static const std::unordered_map<std::string, std::string> kThumbnailOutfitOverrides = {
        {"Modernia", "Outfit_02"},
    };
    std::string thumbOutfit = "default";
    {
        auto ovIt = kThumbnailOutfitOverrides.find(charName);
        if (ovIt != kThumbnailOutfitOverrides.end())
            thumbOutfit = ovIt->second;
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Per-character thumbnail crop adjustments Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // hShift: +moves crop right (char shifts left),  vShift: +moves crop down (char shifts up)
    // zoomW/zoomH: >1 = zoom out (show more), independently for width/height
    struct ThumbCropAdj { float hShift; float vShift; float zoomW; float zoomH; };
    static const std::unordered_map<std::string, ThumbCropAdj> kThumbCropAdj = {
        {"Kilo",    { 0.0f,   0.0f,  1.5f, 0.7f}},
        {"Chime",   { 0.15f,  0.0f,  1.0f, 1.0f}},
        {"Crown",   { 0.15f,  0.0f,  1.0f, 1.0f}},
        {"Dorothy", {-0.15f, -0.08f, 1.0f, 1.0f}},
        {"Wells",   { 0.0f,  -0.05f, 1.0f, 1.0f}},
        {"Bahamut", {-0.15f,  0.0f,  1.0f, 1.0f}},
        {"Ingrid",  { 0.0f,  -0.15f, 1.0f, 1.0f}},
    };
    ThumbCropAdj cropAdj{0.0f, 0.0f, 1.0f, 1.0f};
    {
        auto adjIt = kThumbCropAdj.find(charName);
        if (adjIt != kThumbCropAdj.end()) cropAdj = adjIt->second;
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Extract dominant colorful hue from an image for background Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    auto extractDominantColor = [](const QImage& img) -> QColor {
        int hueBins[18] = {};
        float satSum[18] = {};
        float valSum[18] = {};
        for (int y = 0; y < img.height(); y += 3) {
            for (int x = 0; x < img.width(); x += 3) {
                QColor c = img.pixelColor(x, y);
                if (c.alpha() < 30) continue;
                int h, s, v;
                c.getHsv(&h, &s, &v);
                if (h < 0 || s < 40 || v < 30) continue;
                if (v > 230 && s < 50) continue;
                int bin = h / 20;
                if (bin >= 18) bin = 17;
                hueBins[bin]++;
                satSum[bin] += s;
                valSum[bin] += v;
            }
        }
        int bestBin = -1, bestCount = 0;
        for (int i = 0; i < 18; ++i)
            if (hueBins[i] > bestCount) { bestCount = hueBins[i]; bestBin = i; }
        if (bestBin >= 0 && bestCount > 5) {
            int avgS = static_cast<int>(satSum[bestBin] / bestCount);
            int avgV = static_cast<int>(valSum[bestBin] / bestCount);
            return QColor::fromHsv(bestBin * 20 + 10,
                                   std::clamp(avgS - 20, 40, 200),
                                   std::clamp(avgV - 40, 30, 120));
        }
        return QColor(30, 30, 35);
    };

    // DISABLED: Opening a decoder per character (11 × 68-83ms + 6 prefetch
    // workers each) at startup just for thumbnails is too expensive.
    // Fall through to the letter-placeholder path instead.
#if 0  // was: #ifdef ROUNDTABLE_HAS_SPINE — thumbnail video cache probe
    if (m_animVideoCache) {
        // Try common idle animation names
        static const std::vector<std::string> idleNames = {
            "idle", "Idle", "IDLE", "idle_01", "wait", "stand"
        };

        std::shared_ptr<CachedFrame> frame;
        std::string matchedAnimName;
        for (const auto& animName : idleNames) {
            // Try several frame offsets â€” frame 0 may be fully transparent
            // if the animation starts with a fade-in.
            static const int64_t tryFrames[] = { 0, 5, 10, 15, 30 };
            for (int64_t tryF : tryFrames) {
                auto candidate = const_cast<AnimationVideoCache*>(m_animVideoCache)
                            ->getFrame(charName, thumbOutfit, animName, tryF);
                if (candidate && candidate->ensurePixels()) {
                    frame = std::move(candidate);
                    matchedAnimName = animName;
                    // Quick alpha scan: accept if any pixel has alpha > 10
                    bool hasContent = false;
                    const uint8_t* px = frame->pixels.data();
                    const size_t total = static_cast<size_t>(frame->width)
                                       * frame->height;
                    for (size_t i = 0; i < total; ++i) {
                        if (px[i * 4 + 3] > 10) { hasContent = true; break; }
                    }
                    if (hasContent) break;  // found a visible frame
                }
            }
            if (frame) break;
        }

        if (frame && frame->ensurePixels() && frame->width > 0 && frame->height > 0) {
            uint32_t fw = frame->width;
            uint32_t fh = frame->height;
            QImage fullFrame;

            // Detect packed-alpha layout (top-half RGB + bottom-half alpha)
            if (!frame->unpackedAlpha && fh > fw && (fh % 2 == 0) &&
                fh >= fw * 1.8) {
                fullFrame = unpackPackedAlpha(frame->pixels.data(), fw, fh);
                fw = static_cast<uint32_t>(fullFrame.width());
                fh = static_cast<uint32_t>(fullFrame.height());
            } else {
                fullFrame = QImage(frame->pixels.data(),
                             static_cast<int>(fw),
                             static_cast<int>(fh),
                             static_cast<int>(fw * 4),
                             QImage::Format_ARGB32_Premultiplied);
            }

            // Find the bounding box of non-transparent content
            int contentMinX = static_cast<int>(fw), contentMaxX = 0;
            int contentMinY = static_cast<int>(fh), contentMaxY = 0;
            for (int y = 0; y < static_cast<int>(fh); ++y) {
                const uchar* row = fullFrame.constScanLine(y);
                for (int x = 0; x < static_cast<int>(fw); ++x) {
                    // ARGB32_Premultiplied: byte order is BGRA on little-endian
                    uint8_t alpha = row[x * 4 + 3];
                    if (alpha > 10) {
                        if (x < contentMinX) contentMinX = x;
                        if (x > contentMaxX) contentMaxX = x;
                        if (y < contentMinY) contentMinY = y;
                        if (y > contentMaxY) contentMaxY = y;
                    }
                }
            }
            if (contentMaxX <= contentMinX || contentMaxY <= contentMinY) {
                contentMinX = 0; contentMaxX = static_cast<int>(fw) - 1;
                contentMinY = 0; contentMaxY = static_cast<int>(fh) - 1;
            }

            int contentW = contentMaxX - contentMinX + 1;
            int contentH = contentMaxY - contentMinY + 1;

            // For tall/thin characters not in the manual adjustment table
            // (e.g. Drake), apply Kilo-like defaults: tighter vertical zoom
            // to focus on the head/face, wider horizontal zoom to give
            // breathing room, keep crop anchored at top.
            if (contentW > 0 &&
                static_cast<float>(contentH) / static_cast<float>(contentW) > 2.2f) {
                bool hasManualAdj = kThumbCropAdj.find(charName) != kThumbCropAdj.end();
                if (!hasManualAdj) {
                    cropAdj.zoomH = 0.7f;   // tighter vertically → headshot
                    cropAdj.zoomW = 1.5f;   // wider horizontally → breathing room
                }
            }

            int contentCX = contentMinX + contentW / 2;

            int cropH = static_cast<int>(contentH * 0.55f * cropAdj.zoomH);
            if (cropH < 10) cropH = contentH;
            cropH = std::min(cropH, static_cast<int>(fh));
            int cropW = static_cast<int>(contentW * 0.80f * cropAdj.zoomW);
            if (cropW < 10) cropW = contentW;
            cropW = std::min(cropW, static_cast<int>(fw));
            int cropX = contentCX - cropW / 2 + static_cast<int>(contentW * cropAdj.hShift);
            int cropY = contentMinY + static_cast<int>(contentH * cropAdj.vShift);
            cropX = std::clamp(cropX, 0, static_cast<int>(fw) - cropW);
            cropY = std::clamp(cropY, 0, static_cast<int>(fh) - cropH);

            QImage cropped = fullFrame.copy(cropX, cropY, cropW, cropH);
            QColor bgColor = extractDominantColor(fullFrame);

            QImage scaled = cropped.scaled(sz, sz, Qt::KeepAspectRatioByExpanding,
                                           Qt::SmoothTransformation);
            int ox = (scaled.width() - sz) / 2;
            int oy = (scaled.height() - sz) / 2;
            if (ox < 0) ox = 0;
            if (oy < 0) oy = 0;
            QImage thumb = scaled.copy(ox, oy, sz, sz);

            QPixmap pix(sz, sz);
            pix.fill(Qt::transparent);
            QPainter p(&pix);
            p.setRenderHint(QPainter::Antialiasing);
            QPainterPath clipPath;
            clipPath.addRoundedRect(0, 0, sz, sz, 6, 6);
            p.setClipPath(clipPath);
            p.fillRect(0, 0, sz, sz, bgColor);
            p.drawImage(0, 0, thumb);
            p.end();
            m_charThumbCache[cacheKey] = pix;
            return pix;
        }
    }
#endif

    // Ã¢â€â‚¬Ã¢â€â‚¬ Spine CPU single-frame render fallback Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    //  When the animation cache isn't populated yet (app startup), render
    //  a single idle frame using the CPU software triangle rasterizer so
    //  that thumbnails show the actual assembled character pose instead of
    //  the raw sprite-sheet atlas or a letter placeholder.
    //  DISABLED: Loading ~115 Spine skeletons at startup is too expensive.
    //  Fall through to the video-character or letter-placeholder paths.
#if 0  // was: #ifdef ROUNDTABLE_HAS_SPINE
    {
        auto paths = SpineEngine::resolvePaths("assets", charName,
                                                thumbOutfit, CharacterStance::Default);
        if (paths.valid) {
            SpineEngine thumbEngine;
            if (thumbEngine.loadSkeleton(paths.skelPath, paths.atlasPath)) {
                // Pick an idle animation
                static const std::vector<std::string> idleNames = {
                    "idle", "Idle", "IDLE", "idle_01", "wait", "stand"
                };
                bool animSet = false;
                for (const auto& anim : idleNames) {
                    if (thumbEngine.animation().hasAnimation(anim)) {
                        thumbEngine.animation().setBodyAnimation(anim, false);
                        animSet = true;
                        break;
                    }
                }
                if (!animSet) {
                    // Use the first available animation
                    auto anims = thumbEngine.animation().listAnimations();
                    if (!anims.empty()) {
                        thumbEngine.animation().setBodyAnimation(anims[0].name, false);
                        animSet = true;
                    }
                }

                if (animSet) {
                    // Evaluate at t=0 (setup pose with idle applied)
                    thumbEngine.evaluateAtTime(0.0f);

                    // Get bounds
                    float bx, by, bw, bh;
                    thumbEngine.getBounds(bx, by, bw, bh);
                    if (bw > 1.0f && bh > 1.0f) {
                        // Render at a small size for thumbnail (256px tall max)
                        const float padding = 0.85f;
                        uint32_t renderH = std::min(256u, static_cast<uint32_t>(std::ceil(bh / padding)));
                        uint32_t renderW = std::min(256u, static_cast<uint32_t>(std::ceil(bw / padding)));
                        renderW = std::max(renderW, 16u);
                        renderH = std::max(renderH, 16u);

                        const float fW = static_cast<float>(renderW);
                        const float fH = static_cast<float>(renderH);
                        const float spineScale = std::min(fW / bw, fH / bh) * padding;
                        const float offsetX = fW * 0.5f;
                        const float offsetY = fH * 0.5f;
                        const float spineCX = bx + bw * 0.5f;
                        const float spineCY = by + bh * 0.5f;

                        // Load atlas page textures as QImages
                        const auto& atlas = thumbEngine.atlas();
                        const auto& atlasPages = atlas.pages();
                        const std::string& atlasDir = atlas.directory();
                        std::vector<QImage> pageImages;
                        for (const auto& page : atlasPages) {
                            QString texPath = QString::fromStdString(
                                atlasDir + "/" + page.texturePath);
                            QImage img(texPath);
                            if (!img.isNull())
                                img = img.convertToFormat(QImage::Format_RGBA8888);
                            pageImages.push_back(std::move(img));
                        }

                        // Extract meshes for the current pose
                        SpineRenderData renderData = thumbEngine.extractMeshes();

                        // Software rasterize into a frame buffer
                        std::vector<uint8_t> frameBuf(
                            static_cast<size_t>(renderW) * renderH * 4, 0);

                        for (const auto& batch : renderData.batches) {
                            if (batch.texturePageIndex < 0 ||
                                batch.texturePageIndex >= static_cast<int>(pageImages.size()))
                                continue;
                            const QImage& texImg = pageImages[batch.texturePageIndex];
                            if (texImg.isNull()) continue;
                            const int texW = texImg.width();
                            const int texH = texImg.height();
                            const uchar* texData = texImg.constBits();

                            for (size_t ti = 0; ti + 2 < batch.indices.size(); ti += 3) {
                                const auto& v0 = batch.vertices[batch.indices[ti]];
                                const auto& v1 = batch.vertices[batch.indices[ti + 1]];
                                const auto& v2 = batch.vertices[batch.indices[ti + 2]];

                                // Spine Ã¢â€ â€™ pixel coords
                                float px0 = (v0.x - spineCX) * spineScale + offsetX;
                                float py0 = -(v0.y - spineCY) * spineScale + offsetY;
                                float px1 = (v1.x - spineCX) * spineScale + offsetX;
                                float py1 = -(v1.y - spineCY) * spineScale + offsetY;
                                float px2 = (v2.x - spineCX) * spineScale + offsetX;
                                float py2 = -(v2.y - spineCY) * spineScale + offsetY;

                                int minXi = std::max(0, static_cast<int>(std::floor(std::min({px0, px1, px2}))));
                                int maxXi = std::min(static_cast<int>(renderW) - 1,
                                                     static_cast<int>(std::ceil(std::max({px0, px1, px2}))));
                                int minYi = std::max(0, static_cast<int>(std::floor(std::min({py0, py1, py2}))));
                                int maxYi = std::min(static_cast<int>(renderH) - 1,
                                                     static_cast<int>(std::ceil(std::max({py0, py1, py2}))));
                                if (minXi > maxXi || minYi > maxYi) continue;

                                const float denom = (py1 - py2) * (px0 - px2) + (px2 - px1) * (py0 - py2);
                                if (std::abs(denom) < 1e-8f) continue;
                                const float invDenom = 1.0f / denom;

                                for (int y = minYi; y <= maxYi; ++y) {
                                    for (int x = minXi; x <= maxXi; ++x) {
                                        const float fx = static_cast<float>(x) + 0.5f;
                                        const float fy = static_cast<float>(y) + 0.5f;
                                        const float w0b = ((py1 - py2) * (fx - px2) + (px2 - px1) * (fy - py2)) * invDenom;
                                        const float w1b = ((py2 - py0) * (fx - px2) + (px0 - px2) * (fy - py2)) * invDenom;
                                        const float w2b = 1.0f - w0b - w1b;
                                        if (w0b < 0.0f || w1b < 0.0f || w2b < 0.0f) continue;

                                        float u = w0b * v0.u + w1b * v1.u + w2b * v2.u;
                                        float vt = w0b * v0.v + w1b * v1.v + w2b * v2.v;
                                        float cr = w0b * v0.r + w1b * v1.r + w2b * v2.r;
                                        float cg = w0b * v0.g + w1b * v1.g + w2b * v2.g;
                                        float cb = w0b * v0.b + w1b * v1.b + w2b * v2.b;
                                        float ca = w0b * v0.a + w1b * v1.a + w2b * v2.a;

                                        int tx = std::clamp(static_cast<int>(u * texW), 0, texW - 1);
                                        int ty = std::clamp(static_cast<int>(vt * texH), 0, texH - 1);
                                        const uchar* texel = texData + (ty * texW + tx) * 4;

                                        float sr = texel[0] / 255.0f * cr;
                                        float sg = texel[1] / 255.0f * cg;
                                        float sb = texel[2] / 255.0f * cb;
                                        float sa = texel[3] / 255.0f * ca;
                                        if (sa < 0.001f) continue;

                                        uint8_t* dp = frameBuf.data() + (y * renderW + x) * 4;
                                        float da = dp[3] / 255.0f;
                                        float outA = sa + da * (1.0f - sa);
                                        if (outA > 0.001f) {
                                            dp[0] = static_cast<uint8_t>(std::clamp(
                                                (sr * sa + (dp[0] / 255.0f) * da * (1.0f - sa)) / outA * 255.0f, 0.0f, 255.0f));
                                            dp[1] = static_cast<uint8_t>(std::clamp(
                                                (sg * sa + (dp[1] / 255.0f) * da * (1.0f - sa)) / outA * 255.0f, 0.0f, 255.0f));
                                            dp[2] = static_cast<uint8_t>(std::clamp(
                                                (sb * sa + (dp[2] / 255.0f) * da * (1.0f - sa)) / outA * 255.0f, 0.0f, 255.0f));
                                            dp[3] = static_cast<uint8_t>(outA * 255.0f);
                                        }
                                    }
                                }
                            }
                        }

                        // Convert frame buffer to QImage
                        QImage fullFrame(frameBuf.data(),
                                         static_cast<int>(renderW),
                                         static_cast<int>(renderH),
                                         static_cast<int>(renderW * 4),
                                         QImage::Format_RGBA8888);

                        // Find bounding box of non-transparent content
                        int cMinX = static_cast<int>(renderW), cMaxX = 0;
                        int cMinY = static_cast<int>(renderH), cMaxY = 0;
                        for (int y = 0; y < static_cast<int>(renderH); ++y) {
                            const uint8_t* row = frameBuf.data() + y * renderW * 4;
                            for (int x = 0; x < static_cast<int>(renderW); ++x) {
                                if (row[x * 4 + 3] > 10) {
                                    if (x < cMinX) cMinX = x;
                                    if (x > cMaxX) cMaxX = x;
                                    if (y < cMinY) cMinY = y;
                                    if (y > cMaxY) cMaxY = y;
                                }
                            }
                        }
                        if (cMaxX <= cMinX || cMaxY <= cMinY) {
                            cMinX = 0; cMaxX = static_cast<int>(renderW) - 1;
                            cMinY = 0; cMaxY = static_cast<int>(renderH) - 1;
                        }
                        int cW = cMaxX - cMinX + 1;
                        int cH = cMaxY - cMinY + 1;
                        int cCX = cMinX + cW / 2;

                        int cropH = static_cast<int>(cH * 0.55f * cropAdj.zoomH);
                        if (cropH < 10) cropH = cH;
                        cropH = std::min(cropH, static_cast<int>(renderH));
                        int cropW = static_cast<int>(cW * 0.80f * cropAdj.zoomW);
                        if (cropW < 10) cropW = cW;
                        cropW = std::min(cropW, static_cast<int>(renderW));
                        int cropX = cCX - cropW / 2 + static_cast<int>(cW * cropAdj.hShift);
                        int cropY = cMinY + static_cast<int>(cH * cropAdj.vShift);
                        cropX = std::clamp(cropX, 0, static_cast<int>(renderW) - cropW);
                        cropY = std::clamp(cropY, 0, static_cast<int>(renderH) - cropH);

                        QImage cropped = fullFrame.copy(cropX, cropY, cropW, cropH);
                        QColor bgColor = extractDominantColor(fullFrame);
                        QImage scaled = cropped.scaled(sz, sz, Qt::KeepAspectRatioByExpanding,
                                                       Qt::SmoothTransformation);
                        int ox = (scaled.width() - sz) / 2;
                        int oy = (scaled.height() - sz) / 2;
                        if (ox < 0) ox = 0;
                        if (oy < 0) oy = 0;
                        QImage thumb = scaled.copy(ox, oy, sz, sz);

                        QPixmap pix(sz, sz);
                        pix.fill(Qt::transparent);
                        QPainter p(&pix);
                        p.setRenderHint(QPainter::Antialiasing);
                        QPainterPath clipPath;
                        clipPath.addRoundedRect(0, 0, sz, sz, 6, 6);
                        p.setClipPath(clipPath);
                        p.fillRect(0, 0, sz, sz, bgColor);
                        p.drawImage(0, 0, thumb);
                        p.end();
                        m_charThumbCache[cacheKey] = pix;
                        return pix;
                    }
                }
            }
        }
    }
#endif

    // -- Persistent disk cache (PRIORITY) --------------------------------------
    // Pre-rendered PNG thumbs in assets/cache/character_thumbs/{charName}.png
    // are produced by renderAndCacheCharacterThumbnail() with the correct
    // bbox crop and an extracted background color.  Check these BEFORE any
    // video path: the H264_Green converted videos below have a literal green
    // chroma-key background that's meant to be keyed at runtime — extracting
    // a raw frame from them produces a green-backed thumbnail, which is why
    // Kilo/Chime/Crown/Modernia/Trony/Yoyo all looked green.
    {
        QPixmap cached = loadCachedCharacterThumbnail(charName, sz);
        if (!cached.isNull()) {
            QPixmap rounded(sz, sz);
            rounded.fill(Qt::transparent);
            QPainter rp(&rounded);
            rp.setRenderHint(QPainter::Antialiasing);
            QPainterPath clipPath;
            clipPath.addRoundedRect(0, 0, sz, sz, 6, 6);
            rp.setClipPath(clipPath);
            rp.drawPixmap(0, 0, cached);
            rp.end();
            m_charThumbCache[cacheKey] = rounded;
            return rounded;
        }
    }

    // -- Video character fallback ---------------------------------------------
    // Only used for characters that don't have a pre-rendered PNG cache (e.g.
    // hardcoded video characters like Wells with packed-alpha videos).  We
    // intentionally do NOT touch assets/converted/H264_*/ here — those are
    // green/blue chroma-key sources, not finished thumbnails.
    {
        std::string videoPath;

        // Hardcoded video character table (Wells uses packed-alpha .mp4).
        const auto& vcFiles = videoCharacterFiles();
        for (const auto& [filename, info] : vcFiles) {
            const auto& [vcName, mutePath, talkPath] = info;
            if (vcName == charName) { videoPath = mutePath; break; }
        }

        if (!videoPath.empty()) {
            QImage frame = extractVideoThumbnail(videoPath);
            if (!frame.isNull()) {
                // If packed-alpha, unpack first
                if (frame.height() > frame.width() && (frame.height() % 2 == 0) &&
                    frame.height() >= frame.width() * 1.8) {
                    frame = unpackPackedAlpha(frame.bits(),
                        static_cast<uint32_t>(frame.width()),
                        static_cast<uint32_t>(frame.height()));
                }

                // Find bounding box of non-transparent content
                QImage argbFrame = frame.convertToFormat(QImage::Format_ARGB32);
                int vMinX = argbFrame.width(), vMaxX = 0;
                int vMinY = argbFrame.height(), vMaxY = 0;
                for (int y = 0; y < argbFrame.height(); ++y) {
                    const uchar* row = argbFrame.constScanLine(y);
                    for (int x = 0; x < argbFrame.width(); ++x) {
                        if (row[x * 4 + 3] > 10) {
                            if (x < vMinX) vMinX = x;
                            if (x > vMaxX) vMaxX = x;
                            if (y < vMinY) vMinY = y;
                            if (y > vMaxY) vMaxY = y;
                        }
                    }
                }
                if (vMaxX <= vMinX || vMaxY <= vMinY) {
                    vMinX = 0; vMaxX = argbFrame.width() - 1;
                    vMinY = 0; vMaxY = argbFrame.height() - 1;
                }

                int vcW = vMaxX - vMinX + 1;
                int vcH = vMaxY - vMinY + 1;
                int vcCX = vMinX + vcW / 2;

                int cropH = static_cast<int>(vcH * 0.55f * cropAdj.zoomH);
                if (cropH < 10) cropH = vcH;
                cropH = std::min(cropH, argbFrame.height());
                int cropW = static_cast<int>(vcW * 0.80f * cropAdj.zoomW);
                if (cropW < 10) cropW = vcW;
                cropW = std::min(cropW, argbFrame.width());
                int cropX = vcCX - cropW / 2 + static_cast<int>(vcW * cropAdj.hShift);
                int cropY = vMinY + static_cast<int>(vcH * cropAdj.vShift);
                cropX = std::clamp(cropX, 0, argbFrame.width() - cropW);
                cropY = std::clamp(cropY, 0, argbFrame.height() - cropH);

                QImage cropped = argbFrame.copy(cropX, cropY, cropW, cropH);
                QColor bgColor = extractDominantColor(argbFrame);

                QImage scaled = cropped.scaled(sz, sz, Qt::KeepAspectRatioByExpanding,
                                             Qt::SmoothTransformation);
                int ox = (scaled.width() - sz) / 2;
                int oy = (scaled.height() - sz) / 2;
                if (ox < 0) ox = 0;
                if (oy < 0) oy = 0;
                QImage thumb = scaled.copy(ox, oy, sz, sz);

                QPixmap pix(sz, sz);
                pix.fill(Qt::transparent);
                QPainter p(&pix);
                p.setRenderHint(QPainter::Antialiasing);
                QPainterPath clipPath;
                clipPath.addRoundedRect(0, 0, sz, sz, 6, 6);
                p.setClipPath(clipPath);
                p.fillRect(0, 0, sz, sz, bgColor);
                p.drawImage(0, 0, thumb);
                p.end();
                m_charThumbCache[cacheKey] = pix;
                return pix;
            }
        }
    }

    // (Persistent disk cache is now checked at the top of this function,
    //  before the video paths, so green-screen converted videos no longer
    //  hijack Spine characters that have a pre-rendered PNG thumb.)

    //Ã¢â€â‚¬Ã¢â€â‚¬ Fallback: colored placeholder with initial letter Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬

    QPixmap pix(sz, sz);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);

    // Determine background color from a simple hash of the name
    uint32_t hash = 0;
    for (char c : charName) hash = hash * 31 + static_cast<uint8_t>(c);
    int hue = static_cast<int>(hash % 360);
    QColor bgColor = QColor::fromHsv(hue, 80, 60);

    p.setBrush(bgColor);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(0, 0, sz, sz, 6, 6);

    // Draw the first letter
    QFont f("Arial", sz / 3, QFont::Bold);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, 200));
    QString initial = charName.empty() ? "?" : QString::fromStdString(charName).left(1).toUpper();
    p.drawText(QRect(0, 0, sz, sz), Qt::AlignCenter, initial);

    p.end();
    m_charThumbCache[cacheKey] = pix;
    return pix;
}

QPixmap ShotComposer::makeShotThumbnail(const ShotPreset& shot, int thumbW, int thumbH)
{
    QPixmap pix(thumbW, thumbH);
    pix.fill(QColor(30, 30, 35));

    QPainter p(&pix);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // Ã¢â€â‚¬Ã¢â€â‚¬ Composite layers in z-order (back-to-front) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // layerOrder[0] = front, layerOrder[last] = back
    for (int li = shot.layerCount() - 1; li >= 0; --li) {
        const auto& ref = shot.layerOrder()[static_cast<size_t>(li)];

        if (ref.type == LayerType::Background) {
            const auto* bg = shot.background(ref.index);
            if (!bg || bg->path.empty() || !bg->visible) continue;

            // Ã¢â€â‚¬Ã¢â€â‚¬ Video background layer Ã¢â‚¬â€ extract a frame for the thumbnail Ã¢â€â‚¬Ã¢â€â‚¬
            if (bg->isVideo()) {
                QImage frame = extractVideoThumbnail(bg->path);
                if (!frame.isNull()) {
                    // Unpack packed-alpha if needed
                    if (frame.height() > frame.width() && (frame.height() % 2 == 0) &&
                        frame.height() >= frame.width() * 1.8) {
                        frame = unpackPackedAlpha(frame.bits(),
                            static_cast<uint32_t>(frame.width()),
                            static_cast<uint32_t>(frame.height()));
                    }
                    float bgScale = bg->scale;
                    int scaledW = static_cast<int>(thumbW * bgScale);
                    int scaledH = static_cast<int>(thumbH * bgScale);
                    if (scaledW < 1) scaledW = thumbW;
                    if (scaledH < 1) scaledH = thumbH;
                    QImage scaled = frame.scaled(scaledW, scaledH,
                        Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                    int cx = static_cast<int>(bg->posX * thumbW);
                    int cy = static_cast<int>(bg->posY * thumbH);
                    int ox = cx - scaled.width() / 2;
                    int oy = cy - scaled.height() / 2;
                    if (bg->opacity < 0.996f)
                        p.setOpacity(static_cast<double>(bg->opacity));
                    p.drawImage(ox, oy, scaled);
                    p.setOpacity(1.0);
                }
                continue;
            }

            QString bgPath = QString::fromStdString(bg->path);
            if (!QFileInfo::exists(bgPath)) {
                bgPath = QStringLiteral("assets/backgrounds/") +
                         QFileInfo(bgPath).fileName();
            }
            if (!QFileInfo::exists(bgPath)) continue;

            QImage img;
            auto cacheIt = m_bgImageCache.find(bgPath.toStdString());
            if (cacheIt != m_bgImageCache.end()) {
                img = cacheIt->second;
            } else {
                img = QImage(bgPath);
                if (!img.isNull())
                    m_bgImageCache[bgPath.toStdString()] = img;
            }

            if (!img.isNull()) {
                // Scale bg to fill thumb (cover, honour bg scale/position)
                float bgScale = bg->scale;
                int scaledW = static_cast<int>(thumbW * bgScale);
                int scaledH = static_cast<int>(thumbH * bgScale);
                if (scaledW < 1) scaledW = thumbW;
                if (scaledH < 1) scaledH = thumbH;

                QImage scaled = img.scaled(scaledW, scaledH,
                    Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                // Position: posX/posY (0.5 = centred)
                int cx = static_cast<int>(bg->posX * thumbW);
                int cy = static_cast<int>(bg->posY * thumbH);
                int ox = cx - scaled.width() / 2;
                int oy = cy - scaled.height() / 2;

                if (bg->opacity < 0.996f)
                    p.setOpacity(static_cast<double>(bg->opacity));
                p.drawImage(ox, oy, scaled);
                p.setOpacity(1.0);
            }
            continue;
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ Character layer Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        const auto* ch = shot.character(ref.index);
        if (!ch || !ch->visible) continue;

        bool charRendered = false;
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_animVideoCache && !ch->isVideoCharacter()) {
            // Try to get frame 0 of the character's animation from cache
            static const std::vector<std::string> idleNames = {
                "idle", "Idle", "IDLE", "idle_01", "wait", "stand"
            };

            // Use the shot's configured outfit + animation
            std::string outfitKey = ch->outfit.empty() ? "default" : ch->outfit;
            std::shared_ptr<CachedFrame> frame;
            std::string animToUse = ch->animation;
            frame = const_cast<AnimationVideoCache*>(m_animVideoCache)
                        ->getFrame(ch->characterName, outfitKey, animToUse, 0);

            if (!frame || !frame->ensurePixels()) {
                for (const auto& animName : idleNames) {
                    frame = const_cast<AnimationVideoCache*>(m_animVideoCache)
                                ->getFrame(ch->characterName, outfitKey, animName, 0);
                    if (frame && frame->ensurePixels()) break;
                }
            }

            // If still not found, scan the cache directory for ANY animation
            // video available for this character+outfit and try each one.
            if (!frame || !frame->ensurePixels()) {
                namespace fs = std::filesystem;
                // Search across all format subdirectories
                static const char* fmtDirs[] = {"H264_Green", "H264_Blue", "H264_Custom", "ProRes"};
                fs::path outfitDir;
                for (const auto* fmt : fmtDirs) {
                    auto candidate = fs::path("assets/converted") / fmt / ch->characterName / outfitKey;
                    if (fs::exists(candidate)) { outfitDir = candidate; break; }
                }
                if (fs::exists(outfitDir) && fs::is_directory(outfitDir)) {
                    static const std::string validExts[] = {".mp4", ".mov", ".webm"};
                    for (const auto& entry : fs::directory_iterator(outfitDir)) {
                        if (!entry.is_regular_file()) continue;
                        auto ext = entry.path().extension().string();
                        bool validExt = false;
                        for (const auto& ve : validExts) {
                            if (ext == ve) { validExt = true; break; }
                        }
                        if (!validExt) continue;
                        std::string animName = entry.path().stem().string();
                        frame = const_cast<AnimationVideoCache*>(m_animVideoCache)
                                    ->getFrame(ch->characterName, outfitKey, animName, 0);
                        if (frame && frame->ensurePixels()) break;
                    }
                }
            }

            if (frame && frame->ensurePixels() && frame->width > 0 && frame->height > 0) {
                uint32_t fw = frame->width;
                uint32_t fh = frame->height;
                QImage charImg;

                // Detect packed-alpha layout (top-half RGB + bottom-half alpha)
                // used by HEVC packed-alpha cache videos
                if (!frame->unpackedAlpha && fh > fw && (fh % 2 == 0) &&
                    fh >= fw * 1.8) {
                    charImg = unpackPackedAlpha(frame->pixels.data(), fw, fh);
                    fw = static_cast<uint32_t>(charImg.width());
                    fh = static_cast<uint32_t>(charImg.height());
                } else {
                    charImg = QImage(frame->pixels.data(),
                                     static_cast<int>(fw),
                                     static_cast<int>(fh),
                                     static_cast<int>(fw * 4),
                                     QImage::Format_ARGB32_Premultiplied);
                }

                // Fit character to ~85% of thumb height, then apply shot scale
                float fitScale = static_cast<float>(thumbH) / static_cast<float>(fh) * 0.85f;
                float charScale = fitScale * ch->scale;

                int drawW = static_cast<int>(fw * charScale);
                int drawH = static_cast<int>(fh * charScale);
                if (drawW < 1 || drawH < 1) continue;

                QImage scaled = charImg.scaled(drawW, drawH,
                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

                if (ch->flipX || ch->flipY)
                    scaled = scaled.mirrored(ch->flipX, ch->flipY);

                // Position: posX/posY are normalized (0.5 = center)
                int cx = static_cast<int>(ch->posX * thumbW);
                int cy = static_cast<int>(ch->posY * thumbH);
                int drawX = cx - drawW / 2;
                int drawY = cy - drawH / 2;

                if (ch->opacity < 0.996f)
                    p.setOpacity(static_cast<double>(ch->opacity));
                if (std::abs(ch->rotation) > 0.01f) {
                    p.save();
                    p.translate(cx, cy);
                    p.rotate(static_cast<double>(ch->rotation));
                    p.drawImage(-drawW / 2, -drawH / 2, scaled);
                    p.restore();
                } else {
                    p.drawImage(drawX, drawY, scaled);
                }
                p.setOpacity(1.0);
                charRendered = true;
            }
        } else if (ch->isVideoCharacter()) {
            // For video characters, get a frame from the video player
            const std::string& videoPath = ch->activeVideoPath();
            spdlog::debug("makeShotThumbnail: video char '{}' videoPath='{}'",
                          ch->characterName, videoPath);
            QImage thumb;

            // Prefer the video player (already decodes & handles alpha)
#ifdef ROUNDTABLE_HAS_FFMPEG
            auto player = getOrCreateVideoPlayer(videoPath);
            if (player && !player->lastFrame.isNull()) {
                thumb = player->lastFrame;
                spdlog::debug("  Ã¢â€ â€™ got frame from video player {}x{}",
                              thumb.width(), thumb.height());
            } else {
                spdlog::debug("  Ã¢â€ â€™ video player {} lastFrame {}",
                              player ? "ok" : "null",
                              player ? (player->lastFrame.isNull() ? "null" : "ok") : "n/a");
            }
#endif
            // Fallback to ffmpeg extraction
            if (thumb.isNull()) {
                thumb = extractVideoThumbnail(videoPath);
                spdlog::debug("  Ã¢â€ â€™ extractVideoThumbnail result: {}",
                              thumb.isNull() ? "null" : "ok");
            }

            if (!thumb.isNull()) {
                // If this is a packed-alpha frame (2Ãƒâ€” height), unpack it
                if (thumb.height() > thumb.width() && (thumb.height() % 2 == 0) &&
                    thumb.height() >= thumb.width() * 1.8) {
                    thumb = unpackPackedAlpha(thumb.bits(),
                        static_cast<uint32_t>(thumb.width()),
                        static_cast<uint32_t>(thumb.height()));
                }

                float fitScale = static_cast<float>(thumbH) / static_cast<float>(thumb.height()) * 0.85f;
                float charScale = fitScale * ch->scale;
                int drawW = static_cast<int>(thumb.width() * charScale);
                int drawH = static_cast<int>(thumb.height() * charScale);
                if (drawW > 0 && drawH > 0) {
                    QImage scaled = thumb.scaled(drawW, drawH,
                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    if (ch->flipX || ch->flipY)
                        scaled = scaled.mirrored(ch->flipX, ch->flipY);
                    int cx = static_cast<int>(ch->posX * thumbW);
                    int cy = static_cast<int>(ch->posY * thumbH);
                    if (ch->opacity < 0.996f)
                        p.setOpacity(static_cast<double>(ch->opacity));
                    if (std::abs(ch->rotation) > 0.01f) {
                        p.save();
                        p.translate(cx, cy);
                        p.rotate(static_cast<double>(ch->rotation));
                        p.drawImage(-drawW / 2, -drawH / 2, scaled);
                        p.restore();
                    } else {
                        p.drawImage(cx - drawW / 2, cy - drawH / 2, scaled);
                    }
                    p.setOpacity(1.0);
                    charRendered = true;
                }
            }
        }
#endif
        // Fallback: use outfit-specific full-body cached render, then generic
        if (!charRendered) {
            QPixmap fullBody = loadCachedCharacterOutfitFullBody(
                ch->characterName, ch->outfit);
            if (!fullBody.isNull()) {
                // Fit character to ~85% of thumb height, then apply shot scale
                float fitScale = static_cast<float>(thumbH) /
                    static_cast<float>(fullBody.height()) * 0.85f;
                float charScale = fitScale * ch->scale;
                int drawW = static_cast<int>(fullBody.width() * charScale);
                int drawH = static_cast<int>(fullBody.height() * charScale);
                if (drawW > 0 && drawH > 0) {
                    QImage scaled = fullBody.toImage().scaled(drawW, drawH,
                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    if (ch->flipX || ch->flipY)
                        scaled = scaled.mirrored(ch->flipX, ch->flipY);
                    int cx = static_cast<int>(ch->posX * thumbW);
                    int cy = static_cast<int>(ch->posY * thumbH);
                    if (ch->opacity < 0.996f)
                        p.setOpacity(static_cast<double>(ch->opacity));
                    if (std::abs(ch->rotation) > 0.01f) {
                        p.save();
                        p.translate(cx, cy);
                        p.rotate(static_cast<double>(ch->rotation));
                        p.drawImage(-drawW / 2, -drawH / 2, scaled);
                        p.restore();
                    } else {
                        p.drawImage(cx - drawW / 2, cy - drawH / 2, scaled);
                    }
                    p.setOpacity(1.0);
                    charRendered = true;
                }
            }
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ Fallback: draw a colored silhouette placeholder Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        if (!charRendered) {
            // Hash character name for a consistent hue
            uint32_t hash = 0;
            for (char c2 : ch->characterName)
                hash = hash * 31 + static_cast<uint32_t>(c2);
            int hue = static_cast<int>(hash % 360);
            QColor silCol = QColor::fromHsv(hue, 120, 180, 160);

            int cx = static_cast<int>(ch->posX * thumbW);
            int cy = static_cast<int>(ch->posY * thumbH);
            int sH = static_cast<int>(thumbH * 0.65f * ch->scale);
            int sW = static_cast<int>(sH * 0.45f);
            if (sW < 4) sW = 4;
            if (sH < 6) sH = 6;

            // Draw rounded rect silhouette
            p.setBrush(silCol);
            p.setPen(Qt::NoPen);
            QRect sRect(cx - sW / 2, cy - sH / 2, sW, sH);
            p.drawRoundedRect(sRect, 4, 4);

            // Draw character initial
            if (!ch->characterName.empty()) {
                QFont sf("Arial", std::max(6, sH / 4), QFont::Bold);
                p.setFont(sf);
                p.setPen(QColor(255, 255, 255, 200));
                QString initial = QString(QChar(ch->characterName[0]).toUpper());
                p.drawText(sRect, Qt::AlignCenter, initial);
            }
        }
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Character count badge Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (shot.characterCount() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 120));
        int badgeW = 20;
        int badgeH = 14;
        int bx = thumbW - badgeW - 2;
        int by = 2;
        p.drawRoundedRect(bx, by, badgeW, badgeH, 3, 3);
        QFont sf("Arial", 8, QFont::Bold);
        p.setFont(sf);
        p.setPen(QColor(255, 255, 255, 200));
        p.drawText(QRect(bx, by, badgeW, badgeH), Qt::AlignCenter,
                   QString::number(shot.characterCount()));
    }

    p.end();
    return pix;
}

QString ShotComposer::shotThumbnailPath(const std::string& shotName) const
{
    // Store thumbnails in a "thumbnails" sub-directory next to the presets dir
    auto dir = m_presetManager.directory();
    if (dir.empty()) return {};
    auto thumbDir = dir / "thumbnails";
    // Sanitize name the same way as ShotPresetManager::pathForPreset
    std::string sanitized;
    sanitized.reserve(shotName.size());
    for (char c : shotName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            sanitized += '_';
        else
            sanitized += c;
    }
    return QString::fromStdString((thumbDir / (sanitized + ".png")).string());
}

void ShotComposer::saveShotThumbnail(const ShotPreset& shot)
{
    QString path = shotThumbnailPath(shot.name());
    if (path.isEmpty()) return;

    // Ensure thumbnails directory exists
    QDir().mkpath(QFileInfo(path).absolutePath());

    constexpr int kThumbW = 320;
    constexpr int kThumbH = 180;
    QPixmap thumb;

    // Prefer capturing the preview widget — it shows exactly what the
    // user sees, with the correct outfit, stance, and animation applied.
    // Temporarily reset to default zoom so the thumbnail always fits perfectly,
    // then restore the user's current zoom without any visual flicker.
    if (m_spinePreview && !m_spinePreview->isHidden()) {
        // viewPanX/viewPanY return PIXELS, but setCameraTransform takes
        // NORMALIZED pan (-1..+1) and re-multiplies by width()/height().
        // Capture in pixels, convert to normalized for the restore call so
        // the preview doesn't get panned off-screen (which left it
        // completely black after every save).
        float savedZoom = m_spinePreview->viewZoom();
        float savedPanXPx = m_spinePreview->viewPanX();
        float savedPanYPx = m_spinePreview->viewPanY();
        const float w = static_cast<float>(m_spinePreview->width());
        const float h = static_cast<float>(m_spinePreview->height());
        float savedPanXNorm = (w > 0.5f) ? savedPanXPx / w : 0.0f;
        float savedPanYNorm = (h > 0.5f) ? savedPanYPx / h : 0.0f;

        m_spinePreview->resetViewport();
        m_spinePreview->repaint();
        QPixmap preview = m_spinePreview->grab();

        m_spinePreview->setCameraTransform(savedZoom, savedPanXNorm, savedPanYNorm);
        m_spinePreview->repaint();

        if (!preview.isNull()) {
            // Scale to thumbnail size maintaining aspect ratio
            thumb = preview.scaled(kThumbW, kThumbH,
                Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }

    // Fall back to generating from cache frames if preview unavailable
    if (thumb.isNull())
        thumb = makeShotThumbnail(shot, kThumbW, kThumbH);

    if (!thumb.isNull())
        thumb.save(path, "PNG");
}


// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
// Video thumbnail extraction Ã¢â‚¬â€ uses ffmpeg to grab first frame
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â


} // namespace rt

