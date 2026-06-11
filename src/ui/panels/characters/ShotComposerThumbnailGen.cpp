/*
 * ShotComposerThumbnailGen.cpp - Character/shot thumbnail generation.
 * Split from ShotComposerThumbnails.cpp.
 */

#include "panels/characters/ShotComposer.h"
#include "PathUtils.h"
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

    // Per-character thumbnail crop adjustments — shared with the
    // persistent cache renderer (CharacterThumbnailCache).
    ThumbCropAdj cropAdj = thumbCropAdjustmentFor(charName);

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

        // Prefer the character's default outfit (first catalog entry) so the
        // thumbnail is stable — videoCharacterFiles() is an unordered_map, so
        // iterating it picks an arbitrary outfit once a character has several.
        auto pickExisting = [](const std::string& mute, const std::string& talk) -> std::string {
            if (!mute.empty() && QFileInfo::exists(QString::fromStdString(mute))) return mute;
            if (!talk.empty() && QFileInfo::exists(QString::fromStdString(talk))) return talk;
            return {};
        };
        for (const auto& o : videoCharacterOutfitsFor(charName)) {
            videoPath = pickExisting(o.mutePath, o.talkPath);
            if (!videoPath.empty()) break;
        }
        // Fallback: scan the file table for any existing media for this char.
        if (videoPath.empty()) {
            for (const auto& [filename, info] : videoCharacterFiles()) {
                (void)filename;
                if (info.charName != charName) continue;
                videoPath = pickExisting(info.mutePath, info.talkPath);
                if (!videoPath.empty()) break;
            }
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

                // Bounding box of visible content + head-and-shoulders
                // crop, via the shared helpers (CharacterThumbnailCache).
                QImage argbFrame = frame.convertToFormat(QImage::Format_ARGB32);
                const QRect vContent = thumbContentBoundingBox(argbFrame);
                const QRect vCrop = computeThumbCropRect(
                    vContent, argbFrame.width(), argbFrame.height(), cropAdj);

                QImage cropped = argbFrame.copy(vCrop);
                QColor bgColor = extractDominantThumbColor(argbFrame);

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
                        auto ext = pathToUtf8(entry.path().extension());
                        bool validExt = false;
                        for (const auto& ve : validExts) {
                            if (ext == ve) { validExt = true; break; }
                        }
                        if (!validExt) continue;
                        std::string animName = pathToUtf8(entry.path().stem());
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
    return QString::fromStdString(pathToUtf8(thumbDir / (sanitized + ".png")));
}

void ShotComposer::saveShotThumbnail(const ShotPreset& shot)
{
    QString path = shotThumbnailPath(
        ShotPresetManager::makeKey(shot.show(), shot.name()));
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

