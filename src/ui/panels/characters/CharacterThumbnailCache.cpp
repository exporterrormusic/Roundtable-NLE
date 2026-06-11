/*
 * CharacterThumbnailCache.cpp — Persistent on-disk character thumbnail cache.
 *
 * Renders a single idle frame via Spine CPU software rasterization and
 * saves it as a PNG to the user data directory.  This runs once per
 * character (at download time), so per-character Spine loading cost
 * is acceptable.
 */

#include "CharacterThumbnailCache.h"
#include "QtHelpers.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/SpineEngine.h"
#include "spine/ShotPreset.h" // for CharacterStance
#endif

#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace rt {

std::string cachedCharacterThumbnailPath(const std::string& charName)
{
    return std::string("assets/cache/")
        + kCharacterThumbCacheDir + "/"
        + charName + ".png";
}

std::string cachedCharacterFullBodyPath(const std::string& charName)
{
    return std::string("assets/cache/")
        + kCharacterThumbCacheDir + "/"
        + charName + "_full.png";
}

bool hasCachedCharacterThumbnail(const std::string& charName)
{
    return std::filesystem::exists(cachedCharacterThumbnailPath(charName));
}

QPixmap loadCachedCharacterThumbnail(const std::string& charName, int sz)
{
    std::string path = cachedCharacterThumbnailPath(charName);
    QImage img(path.c_str());
    if (img.isNull()) return {};

    // Scale to the requested size (square, keep aspect ratio)
    QImage scaled = img.scaled(sz, sz, Qt::KeepAspectRatioByExpanding,
                               Qt::SmoothTransformation);
    int ox = (scaled.width() - sz) / 2;
    int oy = (scaled.height() - sz) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    QImage cropped = scaled.copy(ox, oy, sz, sz);

    QPixmap pix(sz, sz);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clipPath;
    clipPath.addRoundedRect(0, 0, sz, sz, 6, 6);
    p.setClipPath(clipPath);
    p.drawImage(0, 0, cropped);
    p.end();
    return pix;
}

QPixmap loadCachedCharacterFullBody(const std::string& charName)
{
    std::string path = cachedCharacterFullBodyPath(charName);
    QImage img(path.c_str());
    if (img.isNull()) return {};
    return QPixmap::fromImage(img);
}

QPixmap loadCachedCharacterOutfitFullBody(const std::string& charName,
                                           const std::string& outfit)
{
    // Try outfit-specific path first
    std::string outfitPath = cachedCharacterOutfitFullBodyPath(charName, outfit);
    QImage img(outfitPath.c_str());
    if (!img.isNull())
        return QPixmap::fromImage(img);
    // Fall back to generic full-body
    return loadCachedCharacterFullBody(charName);
}

std::string cachedCharacterOutfitFullBodyPath(const std::string& charName,
                                                const std::string& outfit)
{
    if (outfit.empty() || outfit == "default")
        return cachedCharacterFullBodyPath(charName);
    return std::string("assets/cache/")
        + kCharacterThumbCacheDir + "/"
        + charName + "_" + outfit + "_full.png";
}

// ── Shared thumbnail framing helpers (see header) ───────────────────────
// NOT inside the ROUNDTABLE_HAS_SPINE guard: ShotComposer's video-character
// thumbnail fallback uses these regardless of Spine availability.

ThumbCropAdj thumbCropAdjustmentFor(const std::string& charName,
                                    bool* hasManualAdjustment)
{
    static const std::unordered_map<std::string, ThumbCropAdj> kThumbCropAdj = {
        {"Kilo",    { 0.0f,   0.0f,  1.5f, 0.7f}},
        {"Chime",   { 0.15f,  0.0f,  1.0f, 1.0f}},
        {"Crown",   { 0.15f,  0.0f,  1.0f, 1.0f}},
        {"Dorothy", {-0.15f, -0.08f, 1.0f, 1.0f}},
        {"Wells",   { 0.0f,  -0.05f, 1.0f, 1.0f}},
        {"Bahamut", {-0.15f,  0.0f,  1.0f, 1.0f}},
        {"Ingrid",  { 0.0f,  -0.15f, 1.0f, 1.0f}},
    };
    auto it = kThumbCropAdj.find(charName);
    if (hasManualAdjustment) *hasManualAdjustment = (it != kThumbCropAdj.end());
    return it != kThumbCropAdj.end() ? it->second : ThumbCropAdj{};
}

QRect computeThumbCropRect(const QRect& content, int frameW, int frameH,
                           const ThumbCropAdj& adj)
{
    const int cW = content.width();
    const int cH = content.height();
    const int cCX = content.left() + cW / 2;

    int cropH = static_cast<int>(cH * 0.55f * adj.zoomH);
    if (cropH < 10) cropH = cH;
    cropH = std::min(cropH, frameH);
    int cropW = static_cast<int>(cW * 0.80f * adj.zoomW);
    if (cropW < 10) cropW = cW;
    cropW = std::min(cropW, frameW);
    int cropX = cCX - cropW / 2 + static_cast<int>(cW * adj.hShift);
    int cropY = content.top() + static_cast<int>(cH * adj.vShift);
    cropX = std::clamp(cropX, 0, frameW - cropW);
    cropY = std::clamp(cropY, 0, frameH - cropH);
    return QRect(cropX, cropY, cropW, cropH);
}

QRect thumbContentBoundingBox(const QImage& img)
{
    const int w = img.width();
    const int h = img.height();
    int minX = w, maxX = 0, minY = h, maxY = 0;
    for (int y = 0; y < h; ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (row[x * 4 + 3] > 10) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX <= minX || maxY <= minY)
        return QRect(0, 0, w, h);
    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

QColor extractDominantThumbColor(const QImage& img)
{
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
}

#ifdef ROUNDTABLE_HAS_SPINE

bool renderAndCacheCharacterThumbnail(const std::string& charName,
                                      const std::string& outfit)
{
    // Resolve file paths for this character
    auto paths = SpineEngine::resolvePaths("assets", charName,
                                            outfit, CharacterStance::Default);
    if (!paths.valid) {
        // Try user data dir
        // All characters are now in assets/characters/ (not AppData)
        paths = SpineEngine::resolvePaths(
            "assets", charName, outfit, CharacterStance::Default);
    }
    if (!paths.valid) {
        spdlog::warn("ThumbnailCache: cannot resolve paths for '{}'", charName);
        return false;
    }

    SpineEngine engine;
    if (!engine.loadSkeleton(paths.skelPath, paths.atlasPath)) {
        spdlog::warn("ThumbnailCache: failed to load skeleton for '{}'", charName);
        return false;
    }

    // Find the first available idle-like animation
    static const std::vector<std::string> idleNames = {
        "idle", "Idle", "IDLE", "idle_01", "wait", "stand"
    };
    bool animSet = false;
    for (const auto& anim : idleNames) {
        if (engine.animation().hasAnimation(anim)) {
            engine.animation().setBodyAnimation(anim, false);
            animSet = true;
            break;
        }
    }
    if (!animSet) {
        auto anims = engine.animation().listAnimations();
        if (!anims.empty()) {
            engine.animation().setBodyAnimation(anims[0].name, false);
            animSet = true;
        }
    }

    if (!animSet) {
        spdlog::warn("ThumbnailCache: no animation found for '{}'", charName);
        return false;
    }

    // Evaluate at setup pose with idle applied
    engine.evaluateAtTime(0.0f);

    // Get bounding box
    float bx, by, bw, bh;
    engine.getBounds(bx, by, bw, bh);
    if (bw < 1.0f || bh < 1.0f) {
        spdlog::warn("ThumbnailCache: zero bounds for '{}'", charName);
        return false;
    }

    // Per-character thumbnail crop adjustments (shared with ShotComposer's
    // video-character fallback so tuning a character is done once).
    bool hasManualAdj = false;
    ThumbCropAdj cropAdj = thumbCropAdjustmentFor(charName, &hasManualAdj);

    // Render size: 512px on the longest side for crisp thumbnails
    const float padding = 0.85f;
    const uint32_t maxDim = 512;
    uint32_t renderH = static_cast<uint32_t>(std::ceil(std::max(bw, bh) / padding));
    uint32_t renderW = renderH;
    renderW = std::max(renderW, 16u);
    renderH = std::max(renderH, 16u);
    if (renderW > maxDim || renderH > maxDim) {
        float scale = static_cast<float>(maxDim) / std::max(renderW, renderH);
        renderW = static_cast<uint32_t>(renderW * scale);
        renderH = static_cast<uint32_t>(renderH * scale);
        renderW = std::max(renderW, 16u);
        renderH = std::max(renderH, 16u);
    }

    const float fW = static_cast<float>(renderW);
    const float fH = static_cast<float>(renderH);
    const float spineScale = std::min(fW / bw, fH / bh) * padding;
    const float offsetX = fW * 0.5f;
    const float offsetY = fH * 0.5f;
    const float spineCX = bx + bw * 0.5f;
    const float spineCY = by + bh * 0.5f;

    // Load atlas page textures as QImages
    const auto& atlas = engine.atlas();
    const auto& atlasPages = atlas.pages();
    const std::string& atlasDir = atlas.directory();
    std::vector<QImage> pageImages;
    for (const auto& page : atlasPages) {
        QString texPath = QString::fromStdString(atlasDir + "/" + page.texturePath);
        QImage img(texPath);
        if (!img.isNull())
            img = img.convertToFormat(QImage::Format_RGBA8888);
        pageImages.push_back(std::move(img));
    }

    // Extract meshes for the current pose
    SpineRenderData renderData = engine.extractMeshes();

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

            // Spine → pixel coords
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

                    // Spine textures use premultiplied alpha (PMA).
                    // texel[0..2] already have alpha baked in by the atlas.
                    // Vertex colors from extractMeshes() are also in PMA space.
                    // So sr,sg,sb below are PMA values — blend using PMA-over
                    // and convert to straight alpha for the frame buffer.
                    float sr = texel[0] / 255.0f * cr;
                    float sg = texel[1] / 255.0f * cg;
                    float sb = texel[2] / 255.0f * cb;
                    float sa = texel[3] / 255.0f * ca;
                    if (sa < 0.001f) continue;

                    uint8_t* dp = frameBuf.data() + (y * renderW + x) * 4;
                    float da = dp[3] / 255.0f;
                    float outA = sa + da * (1.0f - sa);
                    if (outA > 0.001f) {
                        // PMA source over straight-alpha dest:
                        //   result_pma = src_pma + dst_straight * dst_alpha * (1 - src_alpha)
                        // Then convert to straight alpha: result_rgb / result_alpha
                        float invOutA = 1.0f / outA;
                        dp[0] = static_cast<uint8_t>(std::clamp(
                            (sr + (dp[0] / 255.0f) * da * (1.0f - sa)) * invOutA * 255.0f, 0.0f, 255.0f));
                        dp[1] = static_cast<uint8_t>(std::clamp(
                            (sg + (dp[1] / 255.0f) * da * (1.0f - sa)) * invOutA * 255.0f, 0.0f, 255.0f));
                        dp[2] = static_cast<uint8_t>(std::clamp(
                            (sb + (dp[2] / 255.0f) * da * (1.0f - sa)) * invOutA * 255.0f, 0.0f, 255.0f));
                        dp[3] = static_cast<uint8_t>(std::clamp(outA * 255.0f, 0.0f, 255.0f));
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
    const QRect content = thumbContentBoundingBox(fullFrame);

    // For tall/thin characters not in the manual adjustment table (e.g. Drake),
    // apply Kilo-like defaults: tighter vertical zoom to focus on the head/face,
    // wider horizontal zoom to give breathing room, keep crop anchored at top.
    if (content.width() > 0 && !hasManualAdj &&
        static_cast<float>(content.height()) /
            static_cast<float>(content.width()) > 2.2f) {
        cropAdj.zoomH = 0.7f;   // tighter vertically → headshot
        cropAdj.zoomW = 1.5f;   // wider horizontally → breathing room
    }

    const QRect crop = computeThumbCropRect(content,
                                            static_cast<int>(renderW),
                                            static_cast<int>(renderH), cropAdj);

    // Save full-body render (uncropped, transparent background) for shot thumbnails
    {
        std::string fullPath = cachedCharacterFullBodyPath(charName);
        std::filesystem::path fullDir = std::filesystem::path(fullPath).parent_path();
        std::error_code ec;
        std::filesystem::create_directories(fullDir, ec);
        // Copy frame buffer to a standalone ARGB32 QImage for reliable PNG save.
        // QImage::Format_RGBA8888 wrapping external data may not save correctly.
        QImage fullCopy(static_cast<int>(renderW), static_cast<int>(renderH),
                        QImage::Format_ARGB32);
        for (int y = 0; y < static_cast<int>(renderH); ++y) {
            const uint8_t* src = frameBuf.data() + y * renderW * 4;
            uint8_t* dst = fullCopy.scanLine(y);
            for (int x = 0; x < static_cast<int>(renderW); ++x) {
                // Convert RGBA8888 -> ARGB32 (BGRA on little-endian)
                uint8_t r = src[x * 4 + 0], g = src[x * 4 + 1];
                uint8_t b = src[x * 4 + 2], a = src[x * 4 + 3];
                dst[x * 4 + 0] = b; // Blue
                dst[x * 4 + 1] = g; // Green
                dst[x * 4 + 2] = r; // Red
                dst[x * 4 + 3] = a; // Alpha
            }
        }
        if (!fullCopy.save(QString::fromStdString(fullPath), "PNG")) {
            spdlog::warn("ThumbnailCache: failed to save full-body for '{}'",
                         charName);
        } else {
            spdlog::info("ThumbnailCache: saved full-body for '{}' ({}x{})",
                         charName, fullCopy.width(), fullCopy.height());
        }

        // Also save an outfit-specific full-body render if this is a non-default outfit
        if (!outfit.empty() && outfit != "default") {
            std::string outfitPath = cachedCharacterOutfitFullBodyPath(charName, outfit);
            if (!fullCopy.save(QString::fromStdString(outfitPath), "PNG")) {
                spdlog::warn("ThumbnailCache: failed to save full-body for '{}/{}'",
                             charName, outfit);
            } else {
                spdlog::info("ThumbnailCache: saved full-body for '{}/{}' ({}x{})",
                             charName, outfit, fullCopy.width(), fullCopy.height());
            }
        }
    }

    QImage cropped = fullFrame.copy(crop);
    QColor bgColor = extractDominantThumbColor(fullFrame);

    // Composite onto background color
    QImage final(cropped.width(), cropped.height(), QImage::Format_ARGB32);
    final.fill(bgColor.rgba());
    QPainter fp(&final);
    fp.drawImage(0, 0, cropped);
    fp.end();

    // Save cropped library thumbnail to cache directory
    std::string cachePath = cachedCharacterThumbnailPath(charName);
    std::filesystem::path cacheDir = std::filesystem::path(cachePath).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);

    if (!final.save(QString::fromStdString(cachePath), "PNG")) {
        spdlog::warn("ThumbnailCache: failed to save thumbnail for '{}' at '{}'",
                     charName, cachePath);
        return false;
    }

    spdlog::info("ThumbnailCache: saved thumbnail for '{}' ({}x{}) at '{}'",
                 charName, final.width(), final.height(), cachePath);
    return true;
}

#else // !ROUNDTABLE_HAS_SPINE

bool renderAndCacheCharacterThumbnail(const std::string& charName,
                                      const std::string& outfit)
{
    (void)charName;
    (void)outfit;
    return false;
}

#endif // ROUNDTABLE_HAS_SPINE

} // namespace rt
