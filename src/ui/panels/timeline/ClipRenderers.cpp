#include "ClipRenderers.h"  // src/core/ClipRenderers.h — shared with the gpu module

#include "cache/FrameCache.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/CaptionClip.h"
#include "timeline/PngPuppetClip.h"
#include "Constants.h"
#include "PathUtils.h"   // utf8ToPath — Unicode-safe std::string → std::filesystem::path

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
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
// Small-caps text layout helpers (Photoshop/Premiere "small caps": every
// letter is drawn uppercase, but originally-lowercase letters are reduced to
// a fraction of the cap height). A single QPainter::drawText cannot vary glyph
// size, so the small-caps path lays out runs manually and shares a baseline.
// =========================================================================

namespace {

/// Fraction of full cap height used for originally-lowercase letters.
constexpr qreal kSmallCapScale = 0.75;

/// A maximal run of characters sharing a size class within one line. `text`
/// is already uppercased; `small` marks runs that were originally lowercase.
struct SmallCapsRun { QString text; bool small; };

/// Split text into lines (on '\n') of size-classed, uppercased runs.
std::vector<std::vector<SmallCapsRun>> buildSmallCapsLines(const QString& text)
{
    std::vector<std::vector<SmallCapsRun>> lines;
    const QStringList rawLines = text.split(QChar('\n'));
    for (const QString& line : rawLines) {
        std::vector<SmallCapsRun> runs;
        for (const QChar& ch : line) {
            const bool small = ch.isLower();          // digits/space/punct/upper => full size
            const QString up = QString(ch).toUpper();
            if (!runs.empty() && runs.back().small == small)
                runs.back().text += up;
            else
                runs.push_back({up, small});
        }
        lines.push_back(std::move(runs));
    }
    return lines;
}

/// Build a glyph-outline QPainterPath for multi-line (and optionally small-caps)
/// text, laid out around the layer anchor (cx, cy) with the given alignment.
/// Both the stroke (QPen on the path) and the fill (fillPath) use this single
/// path, so they always register exactly. Replaces the old O(width^2) offset-
/// grid stroke: a QPen outline is independent of stroke width, so scrubbing a
/// stroke / moving stroked text no longer stalls the compositor.
///
/// Positioning mirrors the previous point-text alignment (centered text matches
/// the old single-drawText path). For non-small-caps callers, pass one full-size
/// run per line and fmSmall == fmFull.
QPainterPath buildTextPath(const std::vector<std::vector<SmallCapsRun>>& lines,
                           const QFont& fullFont, const QFont& smallFont,
                           const QFontMetricsF& fmFull, const QFontMetricsF& fmSmall,
                           qreal cx, qreal cy, int hAlign, int vAlign)
{
    QPainterPath path;
    const qreal lineSpacing = fmFull.lineSpacing();
    const qreal ascent      = fmFull.ascent();
    const int   n           = static_cast<int>(lines.size());
    const qreal totalH      = lineSpacing * n;

    qreal blockTop;
    if (vAlign == Qt::AlignTop)         blockTop = cy;
    else if (vAlign == Qt::AlignBottom) blockTop = cy - totalH;
    else                                blockTop = cy - totalH * 0.5;   // VCenter

    for (int i = 0; i < n; ++i) {
        const auto& runs = lines[static_cast<size_t>(i)];
        qreal w = 0;
        for (const auto& r : runs)
            w += (r.small ? fmSmall : fmFull).horizontalAdvance(r.text);

        qreal startX;
        if (hAlign == Qt::AlignLeft)        startX = cx;
        else if (hAlign == Qt::AlignRight)  startX = cx - w;
        else                                startX = cx - w * 0.5;       // HCenter/Justify

        const qreal baseY = blockTop + ascent + lineSpacing * i;
        qreal penX = startX;
        for (const auto& r : runs) {
            const QFont& f = r.small ? smallFont : fullFont;
            if (!r.text.isEmpty())
                path.addText(QPointF(penX, baseY), f, r.text);
            penX += (r.small ? fmSmall : fmFull).horizontalAdvance(r.text);
        }
    }
    return path;
}

} // namespace

// Render-state hash for renderGraphicClip's memo cache. Folds in EVERY field
// that affects the output bitmap — including each layer transform evaluated at
// localTick (those are baked into the bitmap), so an ANIMATED clip yields a new
// hash per tick (re-renders) while a STATIC clip yields a constant hash (cache
// hits across frames). Because every editable field is in the hash, an edit
// changes the key and forces a fresh render — the cache is self-invalidating
// and cannot serve a stale frame.
static uint64_t hashGraphicClipRenderState(GraphicClip* clip, int64_t localTick,
                                           uint32_t outW, uint32_t outH,
                                           uint32_t renderW, uint32_t renderH)
{
    uint64_t h = 1469598103934665603ULL;            // FNV-1a 64
    auto mixBytes = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    };
    auto mixU = [&](uint64_t v) { mixBytes(&v, sizeof(v)); };
    auto mixF = [&](float f)    { mixBytes(&f, sizeof(f)); };

    mixU(clip->id()); mixU(outW); mixU(outH); mixU(renderW); mixU(renderH);
    mixU(clip->layerCount());
    for (size_t li = 0; li < clip->layerCount(); ++li) {
        const auto* layer = clip->layer(li);
        if (!layer) { mixU(0); continue; }
        mixU(static_cast<uint64_t>(layer->layerType()));
        mixU(layer->isVisible() ? 1u : 0u);
        const auto& t = layer->transform();
        mixF(t.posX.evaluate(localTick));    mixF(t.posY.evaluate(localTick));
        mixF(t.scaleX.evaluate(localTick));  mixF(t.scaleY.evaluate(localTick));
        mixF(t.rotation.evaluate(localTick));
        mixF(t.anchorX.evaluate(localTick)); mixF(t.anchorY.evaluate(localTick));
        mixF(t.opacity.evaluate(localTick));
        const auto& app = layer->appearance();
        mixU(app.fills.size());
        for (const auto& f : app.fills)   { mixU(f.color); mixF(f.opacity); mixU(f.enabled ? 1u : 0u); }
        mixU(app.strokes.size());
        for (const auto& s : app.strokes) { mixU(s.color); mixF(s.width); mixU(static_cast<uint64_t>(s.position)); mixF(s.opacity); mixU(s.enabled ? 1u : 0u); }
        mixU(app.shadows.size());
        for (const auto& sh : app.shadows){ mixU(sh.color); mixF(sh.distance); mixF(sh.angle); mixF(sh.softness); mixF(sh.opacity); mixU(sh.enabled ? 1u : 0u); }
        if (layer->layerType() == GraphicLayerType::Text) {
            const auto* tl = static_cast<const TextLayer*>(layer);
            mixBytes(tl->text().data(), tl->text().size());            mixU(0x5354u);
            mixBytes(tl->fontFamily().data(), tl->fontFamily().size()); mixU(0x4646u);
            mixF(tl->fontSize()); mixU(static_cast<uint64_t>(tl->fontWeight()));
            mixU(tl->isItalic() ? 1u : 0u); mixU(tl->allCaps() ? 1u : 0u); mixU(tl->smallCaps() ? 1u : 0u);
            mixU(static_cast<uint64_t>(tl->alignment())); mixU(static_cast<uint64_t>(tl->vAlignment()));
            mixF(tl->tracking().evaluate(localTick));
            mixF(tl->leading().evaluate(localTick));
            mixF(tl->baselineShift().evaluate(localTick));
        } else {
            const auto* sl = static_cast<const ShapeLayer*>(layer);
            mixU(static_cast<uint64_t>(sl->shapeType()));
            mixF(sl->shapeWidth()); mixF(sl->shapeHeight()); mixF(sl->cornerRadius());
            mixU(sl->fillColor());
        }
    }
    return h;
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

    // ── Memoized render ─────────────────────────────────────────────────
    // A static text/graphic clip composited over MOVING video would otherwise
    // re-run the full text layout + (O(width^2)) stroke passes every frame.
    // Return the cached bitmap when nothing that affects the output changed.
    // The key folds in all output-affecting state (incl. localTick-evaluated
    // transforms), so it self-invalidates on edit — no stale frame. Function-
    // local statics are unity-build-safe (scoped to this function).
    static std::mutex                                                         s_gcMtx;
    static std::unordered_map<uint64_t, std::pair<uint64_t, std::shared_ptr<CachedFrame>>> s_gcCache;
    const uint64_t renderKey =
        hashGraphicClipRenderState(clip, localTick, outW, outH, renderW, renderH);
    {
        std::lock_guard<std::mutex> lock(s_gcMtx);
        auto it = s_gcCache.find(clip->id());
        if (it != s_gcCache.end() && it->second.first == renderKey && it->second.second)
            return it->second.second;
    }

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
            // ALL CAPS takes precedence over small caps when both are set
            // (matches Premiere). Small caps only applies when allCaps is off.
            const bool smallCaps = tl->smallCaps() && !tl->allCaps();
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

            const auto& app = layer->appearance();

            // ── Vector text rendering (fast at any stroke width) ──────────
            // Build the glyph outline ONCE around the layer anchor, then
            // stroke/shadow/fill that single path. The previous renderer
            // stamped the whole text once per pixel of stroke width via an
            // offset grid — O(width^2) drawText calls — so scrubbing a stroke
            // or dragging stroked text re-ran that storm every composited
            // frame and stalled live updates. A QPen outline is independent of
            // stroke width; fill and stroke share the same path so they always
            // register exactly (no offset outline).
            const qreal cx = static_cast<qreal>(renderW) * 0.5;
            const qreal cy = static_cast<qreal>(renderH) * 0.5;

            QFont smallFont = font;
            std::vector<std::vector<SmallCapsRun>> lines;
            if (smallCaps) {
                // Lowercase-origin glyphs drawn smaller, sharing one baseline.
                lines = buildSmallCapsLines(text);
                const int sps = std::max(1,
                    static_cast<int>(std::lround(scaledFontSize * kSmallCapScale)));
                smallFont.setPointSize(sps);
            } else {
                // One full-size run per line (split only on '\n' — matches the
                // old huge-rect behaviour where word-wrap never triggered).
                for (const QString& ln : text.split(QChar('\n')))
                    lines.push_back({ SmallCapsRun{ ln, false } });
            }
            const QFontMetricsF fmFull(font), fmSmall(smallFont);

            const QPainterPath glyphPath = buildTextPath(
                lines, font, smallFont, fmFull, fmSmall, cx, cy, hAlign, vAlign);

            painter.setRenderHint(QPainter::Antialiasing, true);

            // Strokes (render behind fill). QPen width = 2*w so the band extends
            // w px outside the glyph edge (the inner half is hidden by the
            // fill), reproducing the old dilation-by-w silhouette.
            for (auto it = app.strokes.rbegin(); it != app.strokes.rend(); ++it) {
                if (!it->enabled || it->width < 0.1f) continue;
                QPen pen(toQColor(it->color));
                pen.setWidthF(static_cast<qreal>(it->width) * 2.0);
                pen.setJoinStyle(Qt::RoundJoin);
                pen.setCapStyle(Qt::RoundCap);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(glyphPath);
            }

            // Shadows: the filled glyph offset in the shadow direction.
            for (auto it = app.shadows.rbegin(); it != app.shadows.rend(); ++it) {
                if (!it->enabled) continue;
                float rad = it->angle * 3.14159265f / 180.0f;
                qreal sdx = std::cos(rad) * static_cast<qreal>(it->distance);
                qreal sdy = std::sin(rad) * static_cast<qreal>(it->distance);
                QColor sc = toQColor(it->color);
                sc.setAlphaF(static_cast<double>(it->opacity));
                painter.fillPath(glyphPath.translated(sdx, sdy), sc);
            }

            // Fill
            const QColor fillC = (!app.fills.empty() && app.fills[0].enabled)
                               ? toQColor(app.fills[0].color)
                               : QColor(255, 255, 255);
            painter.fillPath(glyphPath, fillC);

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

    // Store in the memo (one entry per clip). Bound total memory by clearing
    // when the map grows large — stale entries for deleted clips can't recur.
    {
        std::lock_guard<std::mutex> lock(s_gcMtx);
        if (s_gcCache.size() > 64) s_gcCache.clear();
        s_gcCache[clip->id()] = { renderKey, frame };
    }

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
//
// Self-heal on disk change: the cache stores each file's last-write-time and
// re-decodes when it differs, so swapping a puppet PNG in Explorer shows up on
// the timeline (mirrors MediaPool's mtime re-probe). Without this the timeline
// served the stale decode forever while the Puppets tab — which loads fresh —
// already reflected the swap.
QImage loadPuppetImage(const std::string& path)
{
    struct Entry { QImage img; std::filesystem::file_time_type mtime{}; bool haveMtime{false}; };
    static std::mutex s_mtx;
    static std::unordered_map<std::string, Entry> s_cache;

    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(utf8ToPath(path), ec);
    const bool haveMtime = !ec;

    std::lock_guard<std::mutex> lock(s_mtx);
    auto it = s_cache.find(path);
    if (it != s_cache.end()) {
        // Reuse the cached decode unless the file changed on disk. If the mtime
        // is unreadable (e.g. offline), keep serving the cached copy.
        if (!haveMtime || (it->second.haveMtime && it->second.mtime == mtime))
            return it->second.img;
    }

    QImage img;
    // Paths are stored UTF-8; QString::fromStdString uses fromUtf8 so Unicode
    // (e.g. yt-dlp's fullwidth characters) survives on Windows.
    img.load(QString::fromStdString(path));
    if (!img.isNull() && img.format() != QImage::Format_ARGB32)
        img = img.convertToFormat(QImage::Format_ARGB32);

    s_cache[path] = Entry{img, mtime, haveMtime};   // cache even a null image to avoid retrying disk
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
