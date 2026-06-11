#include "ClipRenderers.h"  // src/core/ClipRenderers.h — shared with the gpu module

#include "media/FrameCache.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/CaptionClip.h"
#include "timeline/PngPuppetClip.h"
#include "Constants.h"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace rt {

// ─────────────────────────────────────────────────────────────────────────────
// TitleClip CPU rendering — draw text to a BGRA CachedFrame using QPainter
// ─────────────────────────────────────────────────────────────────────────────

std::shared_ptr<CachedFrame> renderTitleClip(
    TitleClip* clip, int64_t tick, uint32_t outW, uint32_t outH)
{
    if (!clip) return nullptr;

    const int64_t localTick = tick - clip->timelineIn();

    // Decode ARGB colors (0xAARRGGBB)
    auto toQColor = [](uint32_t c) -> QColor {
        return QColor(
            static_cast<int>((c >> 16) & 0xFF),  // R
            static_cast<int>((c >> 8)  & 0xFF),  // G
            static_cast<int>( c        & 0xFF),  // B
            static_cast<int>((c >> 24) & 0xFF)); // A
    };

    QColor textCol    = toQColor(clip->textColor());
    QColor bgCol      = toQColor(clip->bgColor());
    QColor outlineCol = toQColor(clip->outlineColor());

    // Create QImage (Format_ARGB32 = BGRA in memory on little-endian)
    QImage img(static_cast<int>(outW), static_cast<int>(outH),
               QImage::Format_ARGB32);
    img.fill(bgCol);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // Set up font
    QFont font(QString::fromStdString(clip->fontFamily()),
               static_cast<int>(clip->fontSize()));
    font.setBold(clip->isBold());
    font.setItalic(clip->isItalic());

    // Letter spacing (tracking) — animated
    float tracking = clip->tracking().evaluate(localTick);
    font.setLetterSpacing(QFont::AbsoluteSpacing, static_cast<qreal>(tracking));

    painter.setFont(font);

    // Text alignment flags
    int hAlign = Qt::AlignHCenter;
    switch (clip->alignment()) {
        case TextAlign::Left:   hAlign = Qt::AlignLeft;    break;
        case TextAlign::Center: hAlign = Qt::AlignHCenter; break;
        case TextAlign::Right:  hAlign = Qt::AlignRight;   break;
    }
    int vAlign = Qt::AlignVCenter;
    switch (clip->verticalAlignment()) {
        case TextVAlign::Top:    vAlign = Qt::AlignTop;     break;
        case TextVAlign::Middle: vAlign = Qt::AlignVCenter; break;
        case TextVAlign::Bottom: vAlign = Qt::AlignBottom;  break;
    }

    const QString text = QString::fromStdString(clip->text());
    const QRect textRect(20, 20, static_cast<int>(outW) - 40, static_cast<int>(outH) - 40);

    // Draw outline (stroke) by drawing offset text in outline color
    if (clip->outlineWidth() > 0.01f) {
        painter.setPen(outlineCol);
        const int ow = std::max(1, static_cast<int>(clip->outlineWidth()));
        for (int ox = -ow; ox <= ow; ++ox) {
            for (int oy = -ow; oy <= ow; ++oy) {
                if (ox == 0 && oy == 0) continue;
                QRect offsetRect = textRect.translated(ox, oy);
                painter.drawText(offsetRect, hAlign | vAlign | Qt::TextWordWrap, text);
            }
        }
    }

    // Draw main text
    painter.setPen(textCol);
    painter.drawText(textRect, hAlign | vAlign | Qt::TextWordWrap, text);
    painter.end();

    // Convert QImage → CachedFrame (ARGB32 memory = BGRA on little-endian)
    auto frame = std::make_shared<CachedFrame>();
    frame->width  = outW;
    frame->height = outH;
    frame->stride = static_cast<uint32_t>(img.bytesPerLine());
    frame->pixels.resize(static_cast<size_t>(frame->stride) * outH);
    std::memcpy(frame->pixels.data(), img.constBits(), frame->pixels.size());

    return frame;
}


// =========================================================================
// GraphicClip CPU rendering - multi-layer text/shape container
// =========================================================================

std::shared_ptr<CachedFrame> renderGraphicClip(
    GraphicClip* clip, int64_t tick, uint32_t outW, uint32_t outH,
    uint32_t refW, uint32_t refH)
{
    if (!clip || clip->layerCount() == 0) return nullptr;

    // Always render at the full project resolution so that text metrics,
    // font hinting, and pixel-based sizes are identical regardless of the
    // display resolution.  The result is downscaled to outW×outH at the
    // end.  GraphicClip rendering is cheap QPainter work, so the cost of
    // rendering at full-res and downscaling is negligible compared to the
    // visual consistency gain.
    const uint32_t renderW = (refW > 0 && refW > outW) ? refW : outW;
    const uint32_t renderH = (refH > 0 && refH > outH) ? refH : outH;
    const bool needsDownscale = (renderW != outW || renderH != outH);

    const int64_t localTick = tick - clip->timelineIn();

    auto toQColor = [](uint32_t c) -> QColor {
        return QColor(
            static_cast<int>((c >> 16) & 0xFF),
            static_cast<int>((c >> 8)  & 0xFF),
            static_cast<int>( c        & 0xFF),
            static_cast<int>((c >> 24) & 0xFF));
    };

    QImage canvas(static_cast<int>(renderW), static_cast<int>(renderH),
                  QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    for (size_t li = 0; li < clip->layerCount(); ++li) {
        const auto* layer = clip->layer(li);
        if (!layer || !layer->isVisible()) continue;

        const auto& ltf = layer->transform();
        float layerOpacity = ltf.opacity.evaluate(localTick);
        if (layerOpacity <= 0.001f) continue;

        float lpx = ltf.posX.evaluate(localTick);
        float lpy = ltf.posY.evaluate(localTick);
        float lsx = ltf.scaleX.evaluate(localTick);
        float lsy = ltf.scaleY.evaluate(localTick);
        float lrot = ltf.rotation.evaluate(localTick);
        // Anchor point — pivot for rotation/scale (Premiere-style). It's a
        // layer-LOCAL offset from the layer's geometric center; positioning
        // (posX/posY) still places the layer center, only the pivot moves.
        // Defaults to (0,0) → identical behavior to the pre-anchor renderer
        // for all existing clips.
        float lax = ltf.anchorX.evaluate(localTick);
        float lay = ltf.anchorY.evaluate(localTick);

        painter.save();
        painter.setOpacity(static_cast<double>(layerOpacity));

        float centerX = static_cast<float>(renderW) * 0.5f + lpx;
        float centerY = static_cast<float>(renderH) * 0.5f + lpy;
        painter.translate(static_cast<double>(centerX), static_cast<double>(centerY));
        // Shift origin to the anchor so rotate/scale below pivot around it,
        // then shift back so the layer's geometric center stays at posXY.
        painter.translate(static_cast<double>(lax), static_cast<double>(lay));
        if (std::abs(lrot) > 0.01f)
            painter.rotate(static_cast<double>(lrot));
        painter.scale(static_cast<double>(lsx), static_cast<double>(lsy));
        painter.translate(-static_cast<double>(lax), -static_cast<double>(lay));
        painter.translate(-static_cast<double>(renderW) * 0.5,
                          -static_cast<double>(renderH) * 0.5);

        if (layer->layerType() == GraphicLayerType::Text) {
            const auto* tl = static_cast<const TextLayer*>(layer);

            // Scale font size proportionally to canvas resolution
            int scaledFontSize = static_cast<int>(tl->fontSize());
            if (scaledFontSize < 1) scaledFontSize = 1;
            QFont font(QString::fromStdString(tl->fontFamily()), scaledFontSize);
            font.setWeight(static_cast<QFont::Weight>(tl->fontWeight()));
            font.setItalic(tl->isItalic());
            float tracking = tl->tracking().evaluate(localTick);
            font.setLetterSpacing(QFont::AbsoluteSpacing,
                                  static_cast<qreal>(tracking));
            painter.setFont(font);

            QString text = QString::fromStdString(tl->text());
            if (tl->allCaps()) text = text.toUpper();

            int hAlign = Qt::AlignHCenter;
            switch (tl->alignment()) {
                case GTextAlign::Left:    hAlign = Qt::AlignLeft;    break;
                case GTextAlign::Center:  hAlign = Qt::AlignHCenter; break;
                case GTextAlign::Right:   hAlign = Qt::AlignRight;   break;
                case GTextAlign::Justify: hAlign = Qt::AlignJustify; break;
            }
            int vAlign = Qt::AlignVCenter;
            switch (tl->vAlignment()) {
                case GTextVAlign::Top:    vAlign = Qt::AlignTop;     break;
                case GTextVAlign::Middle: vAlign = Qt::AlignVCenter; break;
                case GTextVAlign::Bottom: vAlign = Qt::AlignBottom;  break;
            }

            // Use a very large rect so text is never clipped/wrapped by canvas bounds
            int bigW = static_cast<int>(renderW) * 10;
            int bigH = static_cast<int>(renderH) * 10;
            const QRect textRect(-bigW / 2 + static_cast<int>(renderW) / 2,
                                 -bigH / 2 + static_cast<int>(renderH) / 2,
                                 bigW, bigH);

            const auto& app = layer->appearance();

            // Strokes (render behind fill)
            for (auto it = app.strokes.rbegin(); it != app.strokes.rend(); ++it) {
                if (!it->enabled || it->width < 0.1f) continue;
                painter.setPen(toQColor(it->color));
                int ow = std::max(1, static_cast<int>(it->width));
                for (int ox = -ow; ox <= ow; ++ox)
                    for (int oy = -ow; oy <= ow; ++oy) {
                        if (ox == 0 && oy == 0) continue;
                        painter.drawText(textRect.translated(ox, oy),
                                         hAlign | vAlign | Qt::TextWordWrap, text);
                    }
            }

            // Shadows
            for (auto it = app.shadows.rbegin(); it != app.shadows.rend(); ++it) {
                if (!it->enabled) continue;
                float rad = it->angle * 3.14159265f / 180.0f;
                int sdx = static_cast<int>(std::cos(rad) * it->distance);
                int sdy = static_cast<int>(std::sin(rad) * it->distance);
                QColor sc = toQColor(it->color);
                sc.setAlphaF(static_cast<double>(it->opacity));
                painter.setPen(sc);
                painter.drawText(textRect.translated(sdx, sdy),
                                 hAlign | vAlign | Qt::TextWordWrap, text);
            }

            // Fill text
            if (!app.fills.empty() && app.fills[0].enabled)
                painter.setPen(toQColor(app.fills[0].color));
            else
                painter.setPen(QColor(255, 255, 255));
            painter.drawText(textRect, hAlign | vAlign | Qt::TextWordWrap, text);

        } else if (layer->layerType() == GraphicLayerType::Shape) {
            const auto* sl = static_cast<const ShapeLayer*>(layer);

            float sw = sl->shapeWidth();
            float sh = sl->shapeHeight();
            QRectF shapeRect(
                static_cast<double>(renderW) * 0.5 - static_cast<double>(sw) * 0.5,
                static_cast<double>(renderH) * 0.5 - static_cast<double>(sh) * 0.5,
                static_cast<double>(sw), static_cast<double>(sh));

            const auto& app = layer->appearance();
            QColor fillCol = toQColor(sl->fillColor());
            if (!app.fills.empty() && app.fills[0].enabled)
                fillCol = toQColor(app.fills[0].color);

            painter.setPen(Qt::NoPen);
            painter.setBrush(fillCol);
            switch (sl->shapeType()) {
                case ShapeType::Rectangle:
                    painter.drawRect(shapeRect); break;
                case ShapeType::Ellipse:
                    painter.drawEllipse(shapeRect); break;
                case ShapeType::RoundedRect:
                    painter.drawRoundedRect(shapeRect,
                        static_cast<double>(sl->cornerRadius()),
                        static_cast<double>(sl->cornerRadius())); break;
            }

            for (const auto& stroke : app.strokes) {
                if (!stroke.enabled) continue;
                QPen pen(toQColor(stroke.color), static_cast<double>(stroke.width));
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                switch (sl->shapeType()) {
                    case ShapeType::Rectangle:  painter.drawRect(shapeRect); break;
                    case ShapeType::Ellipse:    painter.drawEllipse(shapeRect); break;
                    case ShapeType::RoundedRect:
                        painter.drawRoundedRect(shapeRect,
                            static_cast<double>(sl->cornerRadius()),
                            static_cast<double>(sl->cornerRadius())); break;
                }
            }
        }

        painter.restore();
    }

    painter.end();

    // Downscale to requested output resolution if we rendered at full-res
    if (needsDownscale)
        canvas = canvas.scaled(static_cast<int>(outW), static_cast<int>(outH),
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    auto frame = std::make_shared<CachedFrame>();
    frame->width  = outW;
    frame->height = outH;
    frame->stride = static_cast<uint32_t>(canvas.bytesPerLine());
    frame->pixels.resize(static_cast<size_t>(frame->stride) * outH);
    std::memcpy(frame->pixels.data(), canvas.constBits(), frame->pixels.size());

    return frame;
}


// =========================================================================
// CaptionClip CPU rendering — burned-in (open) subtitle overlay
// =========================================================================

std::shared_ptr<CachedFrame> renderCaptionClip(
    CaptionClip* clip, int64_t tick, uint32_t outW, uint32_t outH,
    uint32_t refW, uint32_t refH)
{
    (void)tick;
    if (!clip) return nullptr;
    const QString text = QString::fromStdString(clip->text());
    if (text.trimmed().isEmpty()) return nullptr;

    // Render at full project resolution (like renderGraphicClip) so font
    // metrics and pixel sizes match the authored values, then downscale.
    const uint32_t renderW = (refW > 0 && refW > outW) ? refW : outW;
    const uint32_t renderH = (refH > 0 && refH > outH) ? refH : outH;
    const bool needsDownscale = (renderW != outW || renderH != outH);

    auto toQColor = [](uint32_t c) -> QColor {
        return QColor(
            static_cast<int>((c >> 16) & 0xFF),
            static_cast<int>((c >> 8)  & 0xFF),
            static_cast<int>( c        & 0xFF),
            static_cast<int>((c >> 24) & 0xFF));
    };

    QImage canvas(static_cast<int>(renderW), static_cast<int>(renderH),
                  QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    int fontPx = std::max(1, static_cast<int>(std::lround(clip->fontSize())));
    QFont font(QString::fromStdString(clip->fontFamily()), fontPx);
    font.setBold(true);  // captions read best bold; Premiere defaults bold-ish
    painter.setFont(font);

    // Lay the text out within ~80% of frame width, word-wrapped & centered.
    const int margin = static_cast<int>(renderW) / 10;  // 10% side margins
    const int maxTextW = static_cast<int>(renderW) - 2 * margin;
    QFontMetrics fm(font);
    QRect boundUnbounded(0, 0, maxTextW, static_cast<int>(renderH));
    QRect textBound = fm.boundingRect(
        boundUnbounded, Qt::AlignHCenter | Qt::TextWordWrap, text);

    // Background box padding around the text.
    const int padX = std::max(6, fontPx / 3);
    const int padY = std::max(4, fontPx / 5);
    const int boxW = std::min(static_cast<int>(renderW) - 2 * (margin - padX),
                              textBound.width() + 2 * padX);
    const int boxH = textBound.height() + 2 * padY;
    const int boxX = (static_cast<int>(renderW) - boxW) / 2;

    // Vertical placement by position preset.
    int boxY;
    switch (clip->position()) {
        case CaptionPosition::Top:
            boxY = static_cast<int>(renderH) / 12;
            break;
        case CaptionPosition::Middle:
            boxY = (static_cast<int>(renderH) - boxH) / 2;
            break;
        case CaptionPosition::Bottom:
        default:
            // Lower third — sit the box bottom ~8% above the frame bottom.
            boxY = static_cast<int>(renderH) - boxH
                 - static_cast<int>(renderH) * 8 / 100;
            break;
    }
    if (boxY < 0) boxY = 0;

    const QRect boxRect(boxX, boxY, boxW, boxH);
    const QColor bgCol = toQColor(clip->bgColor());
    if (bgCol.alpha() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(bgCol);
        const double radius = std::max(2.0, fontPx * 0.18);
        painter.drawRoundedRect(boxRect, radius, radius);
    }

    painter.setPen(toQColor(clip->textColor()));
    painter.drawText(boxRect, Qt::AlignCenter | Qt::TextWordWrap, text);
    painter.end();

    if (needsDownscale)
        canvas = canvas.scaled(static_cast<int>(outW), static_cast<int>(outH),
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    auto frame = std::make_shared<CachedFrame>();
    frame->width  = outW;
    frame->height = outH;
    frame->stride = static_cast<uint32_t>(canvas.bytesPerLine());
    frame->pixels.resize(static_cast<size_t>(frame->stride) * outH);
    std::memcpy(frame->pixels.data(), canvas.constBits(), frame->pixels.size());

    return frame;
}

// ─────────────────────────────────────────────────────────────────────────────
// PngPuppetClip CPU rendering — pick 1 of 4 face PNGs, blit to a BGRA CachedFrame
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Decoded-image cache keyed by file path.  PNGs are small and there are only a
// handful per puppet, but decoding from disk every frame would be wasteful and
// would hammer the file system during playback/export.  QImage is implicitly
// shared, so handing out copies is cheap.
QImage loadPuppetImage(const std::string& path)
{
    static std::mutex s_mtx;
    static std::unordered_map<std::string, QImage> s_cache;

    std::lock_guard<std::mutex> lock(s_mtx);
    auto it = s_cache.find(path);
    if (it != s_cache.end())
        return it->second;

    QImage img;
    // Paths are stored UTF-8; QString::fromStdString uses fromUtf8 so Unicode
    // (e.g. yt-dlp's fullwidth characters) survives on Windows.
    img.load(QString::fromStdString(path));
    if (!img.isNull() && img.format() != QImage::Format_ARGB32)
        img = img.convertToFormat(QImage::Format_ARGB32);

    s_cache.emplace(path, img);   // cache even a null image to avoid retrying disk
    return img;
}

} // namespace

std::shared_ptr<CachedFrame> renderPngPuppetClip(
    PngPuppetClip* clip, int64_t tick, uint32_t outW, uint32_t outH)
{
    (void)outW;
    (void)outH;
    if (!clip) return nullptr;

    // `tick` is the GLOBAL timeline tick so talk/blink stay phase-continuous
    // across cuts between same-character clips (their seed is character-derived).
    const double t = ticksToSeconds(tick);
    const int faceIdx = clip->selectFace(t);

    std::string path = clip->facePath(faceIdx);
    if (path.empty())
        path = clip->facePath(PngPuppetClip::MouthClosedEyesOpen);  // resting fallback
    if (path.empty())
        return nullptr;

    QImage img = loadPuppetImage(path);
    if (img.isNull()) {
        // The chosen face failed to decode — fall back to the resting face so
        // a single bad/missing variant image doesn't drop the whole character.
        const std::string idle = clip->facePath(PngPuppetClip::MouthClosedEyesOpen);
        if (!idle.empty() && idle != path)
            img = loadPuppetImage(idle);
    }
    if (img.isNull())
        return nullptr;

    auto frame = std::make_shared<CachedFrame>();
    frame->width  = static_cast<uint32_t>(img.width());
    frame->height = static_cast<uint32_t>(img.height());
    frame->stride = static_cast<uint32_t>(img.bytesPerLine());
    frame->pixels.resize(static_cast<size_t>(frame->stride) * frame->height);
    std::memcpy(frame->pixels.data(), img.constBits(), frame->pixels.size());
    frame->unpackedAlpha = true;   // straight-alpha PNG; nothing to unpack
    return frame;
}

} // namespace rt
