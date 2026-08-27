#include "ClipRenderers.h"  // src/core/ClipRenderers.h — shared with the gpu module

#include "cache/FrameCache.h"
#include "cache/FrameContentBounds.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/CaptionClip.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/TierListClip.h"
#include "Constants.h"
#include "PathUtils.h"   // utf8ToPath — Unicode-safe std::string → std::filesystem::path

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QRawFont>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QAbstractTextDocumentLayout>
#include <QRectF>
#include <QString>
#include <QTextBlockFormat>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
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


// ─────────────────────────────────────────────────────────────────────────────
// TierListClip CPU rendering — the ranking board (grid + entry pool + spotlight).
// Unique-prefixed statics so the roundtable_ui UNITY build can't collide them.
// ─────────────────────────────────────────────────────────────────────────────

static QColor tierlistDecodeArgb(uint32_t c)
{
    return QColor(static_cast<int>((c >> 16) & 0xFF),
                  static_cast<int>((c >> 8)  & 0xFF),
                  static_cast<int>( c        & 0xFF),
                  static_cast<int>((c >> 24) & 0xFF));
}

// Small path→QImage decode cache so entry PNGs aren't re-read every frame.
// Include mtime in the validity check so a same-name replacement appears
// immediately rather than remaining pinned for the rest of the session.
namespace {

struct TierScaledImageKey {
    std::string path;
    int width{0};
    int height{0};
    bool operator==(const TierScaledImageKey& o) const noexcept
    { return path == o.path && width == o.width && height == o.height; }
};

struct TierScaledImageKeyHash {
    size_t operator()(const TierScaledImageKey& k) const noexcept
    {
        size_t h = std::hash<std::string>{}(k.path);
        h ^= std::hash<int>{}(k.width) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.height) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

struct TierFrameKey {
    uint64_t clipId{0};
    uint64_t revision{0};
    uint64_t visualState{0};
    uint64_t assetGeneration{0};
    uint32_t width{0};
    uint32_t height{0};
    bool operator==(const TierFrameKey& o) const noexcept
    {
        return clipId == o.clipId && revision == o.revision
            && visualState == o.visualState
            && assetGeneration == o.assetGeneration
            && width == o.width && height == o.height;
    }
};

struct TierFrameKeyHash {
    size_t operator()(const TierFrameKey& k) const noexcept
    {
        size_t h = std::hash<uint64_t>{}(k.clipId);
        auto mix = [&h](auto value) {
            h ^= std::hash<decltype(value)>{}(value)
               + static_cast<size_t>(0x9e3779b97f4a7c15ull) + (h << 6) + (h >> 2);
        };
        mix(k.revision); mix(k.visualState); mix(k.assetGeneration);
        mix(k.width); mix(k.height);
        return h;
    }
};

std::mutex g_tierCacheMutex;
std::unordered_map<std::string, QImage> g_tierSourceImages;
std::unordered_map<TierScaledImageKey, QImage, TierScaledImageKeyHash> g_tierScaledImages;
std::unordered_map<TierFrameKey, std::shared_ptr<CachedFrame>, TierFrameKeyHash>
    g_tierRenderedFrames;
std::deque<TierFrameKey> g_tierFrameOrder;
size_t g_tierFrameBytes = 0;
uint64_t g_tierAssetGeneration = 1;
constexpr size_t kTierFrameCacheBudget = 256ull * 1024ull * 1024ull;

uint64_t tierlistVisualStateHash(const TierListClip& clip, int64_t eventTick)
{
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](uint64_t v) {
        for (unsigned i = 0; i < 8; ++i) {
            h ^= static_cast<uint8_t>(v >> (i * 8));
            h *= 1099511628211ull;
        }
    };
    size_t index = 0;
    for (const TierEvent& event : clip.events()) {
        bool affectsFrame = eventTick >= event.start;
        if (event.type == TierEventType::Popup)
            affectsFrame = affectsFrame && eventTick < event.end;
        mix(static_cast<uint64_t>(index++));
        mix(affectsFrame ? 1u : 0u);
    }
    return h;
}

} // namespace

static QImage tierlistLoadImage(const std::string& path)
{
    if (path.empty()) return QImage();
    {
        std::lock_guard<std::mutex> lk(g_tierCacheMutex);
        auto it = g_tierSourceImages.find(path);
        if (it != g_tierSourceImages.end()) return it->second;
    }
    const auto filePath = utf8ToPath(path);
    QImage im;
    im.load(QString::fromStdWString(filePath.wstring()));
    {
        std::lock_guard<std::mutex> lk(g_tierCacheMutex);
        g_tierSourceImages[path] = im;
    }
    return im;
}

static QImage tierlistScaledImage(const std::string& path, int width, int height)
{
    if (path.empty() || width <= 0 || height <= 0) return {};
    const TierScaledImageKey key{path, width, height};
    {
        std::lock_guard<std::mutex> lk(g_tierCacheMutex);
        auto it = g_tierScaledImages.find(key);
        if (it != g_tierScaledImages.end()) return it->second;
    }
    const QImage source = tierlistLoadImage(path);
    const QImage scaled = source.isNull() ? QImage() : source.scaled(
        width, height, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    {
        std::lock_guard<std::mutex> lk(g_tierCacheMutex);
        g_tierScaledImages[key] = scaled;
    }
    return scaled;
}

void invalidateTierListRenderCache(const std::string& imagePath)
{
    std::lock_guard<std::mutex> lk(g_tierCacheMutex);
    if (imagePath.empty()) {
        g_tierSourceImages.clear();
        g_tierScaledImages.clear();
    } else {
        // Changed-file notifications may differ in slash/case/canonical form
        // from the stored project string on Windows. Asset changes are rare;
        // clearing all decoded thumbnails is cheap and guarantees freshness.
        g_tierSourceImages.clear();
        g_tierScaledImages.clear();
    }
    g_tierRenderedFrames.clear();
    g_tierFrameOrder.clear();
    g_tierFrameBytes = 0;
    ++g_tierAssetGeneration;
}

static void tierlistDrawEntry(QPainter& p, const TierEntry* e, const QRectF& cell)
{
    const int targetW = std::max(1, static_cast<int>(std::lround(cell.width())));
    const int targetH = std::max(1, static_cast<int>(std::lround(cell.height())));
    QImage im = e ? tierlistScaledImage(e->imagePath, targetW, targetH) : QImage();
    if (!im.isNull()) {
        const double sx = (im.width()  - cell.width())  * 0.5;
        const double sy = (im.height() - cell.height()) * 0.5;
        p.drawImage(cell, im, QRectF(sx, sy, cell.width(), cell.height()));
    } else {
        p.fillRect(cell, QColor(0x33, 0x33, 0x3E));
        p.setPen(QColor(0xB0, 0xB0, 0xBA));
        QFont f("Arial", std::max(6, static_cast<int>(std::lround(cell.height() * 0.14))));
        p.setFont(f);
        p.drawText(cell.adjusted(3, 3, -3, -3), Qt::AlignCenter | Qt::TextWordWrap,
                   e ? QString::fromStdString(e->title) : QStringLiteral("?"));
    }
    // White sharp-cornered frame around every entry.
    p.setBrush(Qt::NoBrush);
    QPen frame(QColor(255, 255, 255));
    frame.setWidthF(std::clamp(cell.height() * 0.02, 1.5, 4.0));
    frame.setJoinStyle(Qt::MiterJoin);   // sharp corners, not bevelled/rounded
    p.setPen(frame);
    p.drawRect(cell);
}

static double tierlistEntryAspect(const TierEntry* entry, double fallback)
{
    const QImage image = entry ? tierlistLoadImage(entry->imagePath) : QImage();
    if (!image.isNull() && image.width() > 0 && image.height() > 0)
        return static_cast<double>(image.width()) / image.height();
    return fallback > 0.0 ? fallback : 1.0;
}

std::shared_ptr<CachedFrame> renderTierListClip(
    TierListClip* clip, int64_t tick, uint32_t outW, uint32_t outH,
    uint32_t /*refW*/, uint32_t /*refH*/)
{
    if (!clip || outW == 0 || outH == 0) return nullptr;

    const TierListClip& model = *clip;
    const int64_t eventTick = model.eventTickAt(tick);
    const uint64_t visualState = tierlistVisualStateHash(model, eventTick);
    TierFrameKey frameKey;
    {
        std::lock_guard<std::mutex> lk(g_tierCacheMutex);
        frameKey = {model.id(), model.renderRevision(), visualState,
                    g_tierAssetGeneration, outW, outH};
        auto it = g_tierRenderedFrames.find(frameKey);
        if (it != g_tierRenderedFrames.end()) return it->second;
    }
    const int W = static_cast<int>(outW);
    const int H = static_cast<int>(outH);

    QImage img(W, H, QImage::Format_ARGB32);
    img.fill(Qt::transparent);   // grid is opaque; top/right gutters stay transparent

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // Safe-margin insets — reserve top band (channel banner) + right strip (commentators).
    const int topInset    = static_cast<int>(std::lround(model.topSafeMargin()   * H));
    const int rightInset  = static_cast<int>(std::lround(model.rightSafeMargin() * W));
    const int bottomInset = static_cast<int>(std::lround(topInset * 0.44)); // ~half the top band (measured)
    const QRectF gridArea(0.0, static_cast<double>(topInset),
                          static_cast<double>(W - rightInset),
                          static_cast<double>(H - topInset - bottomInset));

    const auto& tiers = model.tiers();
    const int nTiers  = static_cast<int>(tiers.size());

    // Left title sidebar (rotated).
    const double sidebarW = std::max(24.0, W * 0.0568);   // measured
    const QRectF sidebar(gridArea.left(), gridArea.top(), sidebarW, gridArea.height());
    p.fillRect(sidebar, QColor(0, 0, 0));
    if (!model.title().empty()) {
        p.save();
        p.translate(sidebar.center());
        p.rotate(-90.0);
        QFont tf("Impact");   // the vertical list name uses Impact (already heavy/condensed — no synthetic bold)
        tf.setPixelSize(std::max(10, static_cast<int>(std::lround(sidebarW * 0.65))));   // measured glyph ~0.47 of sidebar
        p.setFont(tf);
        p.setPen(QColor(255, 255, 255));
        const QRectF tr(-sidebar.height() * 0.5, -sidebar.width() * 0.5,
                        sidebar.height(), sidebar.width());
        p.drawText(tr, Qt::AlignCenter, QString::fromStdString(model.title()));
        p.restore();
    }

    const QRectF rowsArea(sidebar.right(), gridArea.top(),
                          gridArea.width() - sidebarW, gridArea.height());

    if (nTiers > 0 && rowsArea.width() > 4.0 && rowsArea.height() > 4.0) {
        // Replay events → per-tier ordered entry ids + the active POPUP. An
        // entry cuts into its row the moment its DROP starts (no animation);
        // the POPUP shows it enlarged and centred over its own window.
        std::vector<std::vector<uint64_t>> rows(static_cast<size_t>(nTiers));
        uint64_t popupEntry = 0;   bool   hasPopup = false;

        std::vector<TierEvent> evs = model.events();
        std::stable_sort(evs.begin(), evs.end(),
                         [](const TierEvent& a, const TierEvent& b) { return a.start < b.start; });

        std::unordered_map<uint64_t, const TierEntry*> entriesById;
        entriesById.reserve(model.entries().size());
        for (const TierEntry& entry : model.entries())
            entriesById.emplace(entry.id, &entry);
        const auto entryById = [&entriesById](uint64_t id) -> const TierEntry* {
            auto it = entriesById.find(id);
            return it != entriesById.end() ? it->second : nullptr;
        };

        auto removeId = [&](uint64_t id) {
            for (auto& row : rows)
                row.erase(std::remove(row.begin(), row.end(), id), row.end());
        };

        for (const auto& e : evs) {
            if (e.type == TierEventType::Popup) {
                if (eventTick >= e.start && eventTick < e.end) { popupEntry = e.entryId; hasPopup = true; }
            } else if (e.type == TierEventType::Drop) {
                if (eventTick >= e.start && e.tier >= 0 && e.tier < nTiers) {
                    removeId(e.entryId);
                    auto& row = rows[static_cast<size_t>(e.tier)];
                    int idx = e.index;
                    if (idx < 0 || idx > static_cast<int>(row.size())) idx = static_cast<int>(row.size());
                    row.insert(row.begin() + idx, e.entryId);
                    // No animation — the drop is an instant cut into the row.
                }
            } else { // Reorder
                if (eventTick >= e.start && e.tier >= 0 && e.tier < nTiers) {
                    auto& row = rows[static_cast<size_t>(e.tier)];
                    const int from = e.fromIndex;
                    if (from >= 0 && from < static_cast<int>(row.size())) {
                        const uint64_t v = row[static_cast<size_t>(from)];
                        row.erase(row.begin() + from);
                        int to = e.toIndex;
                        if (to < 0) to = 0;
                        if (to > static_cast<int>(row.size())) to = static_cast<int>(row.size());
                        row.insert(row.begin() + to, v);
                    }
                }
            }
        }

        // ── Layout in "natural" units, then a single uniform scale to fit the
        //    grid height. Entries share a height but derive their width from the
        //    source image. A tier that overflows its content width WRAPS to more
        //    lines and GROWS taller. When the total exceeds the grid, everything
        //    scales down uniformly so image proportions are never squashed.
        const double fallbackAspect = static_cast<double>(model.entryAspectRatio());
        const double unit           = rowsArea.height() / nTiers;   // one-line row height
        const double pad            = unit * 0.10;
        const double eh0            = unit * 0.80;
        const double labelW0        = std::max(20.0, unit * 0.575); // measured: ~0.575 x row height
        const double contentW0      = rowsArea.width() - labelW0;
        const double maxEntryW0     = std::max(1.0, contentW0 - 2.0 * pad);

        struct NaturalEntryLayout {
            uint64_t id;
            QRectF rect;
        };
        std::vector<std::vector<NaturalEntryLayout>> entryLayouts(
            static_cast<size_t>(nTiers));
        std::vector<int> lines(static_cast<size_t>(nTiers), 1);
        double totalNaturalH = 0.0;
        for (int i = 0; i < nTiers; ++i) {
            double x = pad;
            int line = 0;
            auto& layouts = entryLayouts[static_cast<size_t>(i)];
            for (uint64_t id : rows[static_cast<size_t>(i)]) {
                const double imageAspect = tierlistEntryAspect(
                    entryById(id), fallbackAspect);
                double entryH = eh0;
                double entryW = entryH * imageAspect;
                if (entryW > maxEntryW0) {
                    entryW = maxEntryW0;
                    entryH = entryW / imageAspect;
                }

                if (x > pad && x + entryW > contentW0 - pad) {
                    x = pad;
                    ++line;
                }
                const double y = pad + line * (eh0 + pad) + (eh0 - entryH) * 0.5;
                layouts.push_back({id, QRectF(x, y, entryW, entryH)});
                x += entryW + pad;
            }

            const int lineCount = line + 1;
            lines[static_cast<size_t>(i)] = lineCount;
            totalNaturalH += (lineCount + 1) * pad + lineCount * eh0;
        }
        const double scale = (totalNaturalH > 1.0)
                           ? std::min(1.0, rowsArea.height() / totalNaturalH) : 1.0;

        const double labelW = labelW0 * scale;

        double y = rowsArea.top();
        std::vector<double> boundaries;
        for (int i = 0; i < nTiers; ++i) {
            const int    L     = lines[static_cast<size_t>(i)];
            const double tierH = ((L + 1) * pad + L * eh0) * scale;
            const QRectF rrow(rowsArea.left(), y, rowsArea.width(), tierH);

            const QRectF lcell(rrow.left(), rrow.top(), labelW, tierH);
            p.fillRect(lcell, tierlistDecodeArgb(tiers[static_cast<size_t>(i)].color));
            {
                QFont lf("Gotham");   // tier letters use Gotham
                lf.setBold(true);
                const int letterPx = std::max(8, static_cast<int>(std::lround(unit * scale * 0.5)));
                lf.setPixelSize(letterPx);
                const QString letter = QString::fromStdString(tiers[static_cast<size_t>(i)].label);
                // White glyph with a thin black outline (reads on the light C/D cells).
                QFontMetricsF fm(lf);
                const QRectF tb = fm.tightBoundingRect(letter);
                QPainterPath gp;
                gp.addText(lcell.center().x() - tb.center().x(),
                           lcell.center().y() - tb.center().y(), lf, letter);
                // 2px stroke at 1080p, scaled with resolution so it stays proportional.
                QPen pen(QColor(0, 0, 0), std::max(1.0, 2.0 * H / 1080.0));
                pen.setJoinStyle(Qt::RoundJoin);
                p.setPen(pen);
                p.setBrush(QColor(255, 255, 255));
                p.drawPath(gp);
                p.setBrush(Qt::NoBrush);
            }

            const QRectF ccell(lcell.right(), rrow.top(), rrow.width() - labelW, tierH);
            p.fillRect(ccell, QColor(0x1A, 0x1A, 0x17));   // measured content grey

            for (const auto& layout : entryLayouts[static_cast<size_t>(i)]) {
                const QRectF cell(ccell.left() + layout.rect.left() * scale,
                                  ccell.top()  + layout.rect.top() * scale,
                                  layout.rect.width() * scale,
                                  layout.rect.height() * scale);
                if (cell.width() > 1.0 && cell.height() > 1.0)
                    tierlistDrawEntry(p, entryById(layout.id), cell);
            }

            y += tierH;
            if (i < nTiers - 1) boundaries.push_back(y);
        }

        // Row dividers — drawn AFTER all rows so the next row's content fill can't
        // paint over the previous divider (that caused missing lines between ranks).
        for (double dy : boundaries)
            p.fillRect(QRectF(rowsArea.left(), std::round(dy) - 2.0, rowsArea.width(), 4.0),
                       QColor(0, 0, 0));   // measured ~4px

        // Spotlight — the discussed entry, centred and enlarged. It cuts in and
        // out with its POPUP window (no animation), on a white sharp-cornered
        // frame. The board behind stays fully visible (no dimming).
        if (hasPopup) {
            // The full spotlight view also preserves the source image's shape.
            const TierEntry* popup = entryById(popupEntry);
            const double popupAspect = tierlistEntryAspect(popup, fallbackAspect);

            double panelH = H * 0.72;
            double panelW = panelH * popupAspect;
            const double maxW = (W - rightInset) * 0.86;
            if (panelW > maxW) {
                panelW = maxW;
                if (popupAspect > 0.0) panelH = panelW / popupAspect;
            }
            const QRectF panel((W - panelW) * 0.5, (H - panelH) * 0.5, panelW, panelH);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255));
            p.drawRect(panel.adjusted(-10, -10, 10, 10));   // white frame, sharp corners
            tierlistDrawEntry(p, popup, panel);
        }
    }

    // Bottom black border — a design element, ~half the top band's height.
    if (bottomInset > 0)
        p.fillRect(QRectF(0.0, static_cast<double>(H - bottomInset),
                          static_cast<double>(W), static_cast<double>(bottomInset)),
                   QColor(0, 0, 0));

    p.end();

    auto frame = std::make_shared<CachedFrame>();
    frame->width  = outW;
    frame->height = outH;
    frame->stride = static_cast<uint32_t>(img.bytesPerLine());
    frame->pixels.resize(static_cast<size_t>(frame->stride) * outH);
    std::memcpy(frame->pixels.data(), img.constBits(), frame->pixels.size());
    frame->pinned = true;
    computeBgraContentBounds(*frame);
    frame->mediaId = 0xF17E000000000000ull
                   ^ static_cast<uint64_t>(TierFrameKeyHash{}(frameKey));
    frame->frameNumber = static_cast<int64_t>(visualState & 0x7FFFFFFFFFFFFFFFull);

    {
        std::lock_guard<std::mutex> lk(g_tierCacheMutex);
        auto [it, inserted] = g_tierRenderedFrames.emplace(frameKey, frame);
        if (!inserted) return it->second;
        g_tierFrameOrder.push_back(frameKey);
        g_tierFrameBytes += frame->pixels.size();
        while (g_tierFrameBytes > kTierFrameCacheBudget && !g_tierFrameOrder.empty()) {
            const TierFrameKey oldest = g_tierFrameOrder.front();
            g_tierFrameOrder.pop_front();
            auto oldIt = g_tierRenderedFrames.find(oldest);
            if (oldIt == g_tierRenderedFrames.end()) continue;
            g_tierFrameBytes -= oldIt->second->pixels.size();
            g_tierRenderedFrames.erase(oldIt);
        }
    }
    return frame;
}


// =========================================================================
// Small-caps text layout helpers (Photoshop/Premiere "small caps": every
// letter is drawn uppercase, but originally-lowercase letters are reduced to
// a fraction of the cap height). A single QPainter::drawText cannot vary glyph
// size, so the small-caps path lays out runs manually and shares a baseline.
// =========================================================================

namespace {

void boxBlurHorizontal(const uint32_t* source, uint32_t* destination,
                       int width, int height, int radius)
{
    const float divisor = 1.0f / static_cast<float>(radius * 2 + 1);
    for (int y = 0; y < height; ++y) {
        const uint32_t* row = source + y * width;
        uint32_t* output = destination + y * width;
        float a = 0, r = 0, g = 0, b = 0;
        for (int x = -radius; x <= radius; ++x) {
            const uint32_t pixel = row[std::clamp(x, 0, width - 1)];
            a += static_cast<float>((pixel >> 24) & 0xFF);
            r += static_cast<float>((pixel >> 16) & 0xFF);
            g += static_cast<float>((pixel >> 8) & 0xFF);
            b += static_cast<float>(pixel & 0xFF);
        }
        for (int x = 0; x < width; ++x) {
            output[x] = (static_cast<uint32_t>(a * divisor + 0.5f) << 24)
                | (static_cast<uint32_t>(r * divisor + 0.5f) << 16)
                | (static_cast<uint32_t>(g * divisor + 0.5f) << 8)
                | static_cast<uint32_t>(b * divisor + 0.5f);
            const uint32_t add = row[std::min(x + radius + 1, width - 1)];
            const uint32_t sub = row[std::max(x - radius, 0)];
            a += static_cast<float>((add >> 24) & 0xFF)
                - static_cast<float>((sub >> 24) & 0xFF);
            r += static_cast<float>((add >> 16) & 0xFF)
                - static_cast<float>((sub >> 16) & 0xFF);
            g += static_cast<float>((add >> 8) & 0xFF)
                - static_cast<float>((sub >> 8) & 0xFF);
            b += static_cast<float>(add & 0xFF)
                - static_cast<float>(sub & 0xFF);
        }
    }
}

void boxBlurVertical(const uint32_t* source, uint32_t* destination,
                     int width, int height, int radius)
{
    const float divisor = 1.0f / static_cast<float>(radius * 2 + 1);
    for (int x = 0; x < width; ++x) {
        float a = 0, r = 0, g = 0, b = 0;
        for (int y = -radius; y <= radius; ++y) {
            const uint32_t pixel = source[
                std::clamp(y, 0, height - 1) * width + x];
            a += static_cast<float>((pixel >> 24) & 0xFF);
            r += static_cast<float>((pixel >> 16) & 0xFF);
            g += static_cast<float>((pixel >> 8) & 0xFF);
            b += static_cast<float>(pixel & 0xFF);
        }
        for (int y = 0; y < height; ++y) {
            destination[y * width + x] =
                (static_cast<uint32_t>(a * divisor + 0.5f) << 24)
                | (static_cast<uint32_t>(r * divisor + 0.5f) << 16)
                | (static_cast<uint32_t>(g * divisor + 0.5f) << 8)
                | static_cast<uint32_t>(b * divisor + 0.5f);
            const uint32_t add = source[
                std::min(y + radius + 1, height - 1) * width + x];
            const uint32_t sub = source[
                std::max(y - radius, 0) * width + x];
            a += static_cast<float>((add >> 24) & 0xFF)
                - static_cast<float>((sub >> 24) & 0xFF);
            r += static_cast<float>((add >> 16) & 0xFF)
                - static_cast<float>((sub >> 16) & 0xFF);
            g += static_cast<float>((add >> 8) & 0xFF)
                - static_cast<float>((sub >> 8) & 0xFF);
            b += static_cast<float>(add & 0xFF)
                - static_cast<float>(sub & 0xFF);
        }
    }
}

void blurImage(QImage& image, int radius)
{
    if (radius <= 0 || image.isNull()) return;
    QImage temporary(image.size(), image.format());
    auto* pixels = reinterpret_cast<uint32_t*>(image.bits());
    auto* scratch = reinterpret_cast<uint32_t*>(temporary.bits());
    for (int pass = 0; pass < 3; ++pass) {
        boxBlurHorizontal(pixels, scratch, image.width(), image.height(), radius);
        boxBlurVertical(scratch, pixels, image.width(), image.height(), radius);
    }
}

void paintTextShadow(QPainter& painter, const QImage& canvas,
                     const QPainterPath& glyphs, const QColor& color,
                     qreal offsetX, qreal offsetY, qreal softness)
{
    const QPainterPath offsetPath = glyphs.translated(offsetX, offsetY);
    if (softness < 0.5) {
        painter.fillPath(offsetPath, color);
        return;
    }

    const QTransform transform = painter.transform();
    const qreal scaleX = std::hypot(transform.m11(), transform.m12());
    const qreal scaleY = std::hypot(transform.m21(), transform.m22());
    const int radius = std::clamp(static_cast<int>(std::lround(
        softness * std::max(scaleX, scaleY) / 3.0)), 1, 128);
    const QPainterPath devicePath = transform.map(offsetPath);
    QRect bounds = devicePath.boundingRect().toAlignedRect().adjusted(
        -radius * 3, -radius * 3, radius * 3, radius * 3);
    bounds = bounds.intersected(canvas.rect());
    if (bounds.isEmpty()) return;

    QImage shadow(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    shadow.fill(Qt::transparent);
    QPainter shadowPainter(&shadow);
    shadowPainter.setRenderHint(QPainter::Antialiasing, true);
    shadowPainter.fillPath(
        devicePath.translated(-bounds.left(), -bounds.top()), color);
    shadowPainter.end();
    blurImage(shadow, radius);

    const qreal layerOpacity = painter.opacity();
    painter.save();
    painter.resetTransform();
    painter.setOpacity(layerOpacity);
    painter.drawImage(bounds.topLeft(), shadow);
    painter.restore();
}

/// Fraction of full cap height used for originally-lowercase letters.
constexpr qreal kSmallCapScale = 0.75;

/// A maximal run of characters sharing a size class within one line. `text`
/// is already uppercased; `small` marks runs that were originally lowercase.
struct SmallCapsRun { QString text; bool small; };

/// Greedy word-wrap: re-line `text` (splitting only on existing '\n') so no
/// line's advance exceeds maxW. When `measureUpper` is set, candidates are
/// measured uppercased — small-caps rendering draws every glyph uppercase at
/// <= full size, so full-size uppercase advance is a safe upper bound (wraps
/// a hair early, never overflows). Words wider than maxW get their own line.
QString wrapToWidth(const QString& text, const QFontMetricsF& fm, qreal maxW,
                    bool measureUpper)
{
    QStringList out;
    for (const QString& para : text.split(QChar('\n'))) {
        const QStringList words = para.split(QChar(' '), Qt::SkipEmptyParts);
        if (words.isEmpty()) { out << QString(); continue; }
        QString line;
        for (const QString& word : words) {
            const QString cand = line.isEmpty() ? word
                                                : line + QChar(' ') + word;
            const qreal w = fm.horizontalAdvance(measureUpper ? cand.toUpper()
                                                              : cand);
            if (!line.isEmpty() && w > maxW) {
                out << line;
                line = word;
            } else {
                line = cand;
            }
        }
        out << line;
    }
    return out.join(QChar('\n'));
}

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
                           qreal cx, qreal cy, int hAlign, int vAlign,
                           qreal extraLeading = 0.0, qreal baselineShift = 0.0,
                           bool pointTextGrowsDown = false)
{
    QPainterPath path;
    // `extraLeading` is ADDITIVE px on top of the font's natural line spacing
    // (0 = exactly the pre-leading layout). `baselineShift` follows the
    // typographic convention: positive shifts glyphs UP off the baseline.
    const qreal lineSpacing = fmFull.lineSpacing() + extraLeading;
    const qreal ascent      = fmFull.ascent();
    const int   n           = static_cast<int>(lines.size());
    const qreal totalH      = lineSpacing * n;

    qreal blockTop;
    if (pointTextGrowsDown)             blockTop = cy - lineSpacing * 0.5;
    else if (vAlign == Qt::AlignTop)    blockTop = cy;
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

        const qreal baseY = blockTop + ascent + lineSpacing * i - baselineShift;
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

struct StyledGlyphRun {
    QString text;
    QFont font;
    qreal baselineShift{0.0};
    qreal leading{0.0};
    qreal advanceOverride{-1.0};
    TextStyleRun style;
    int sourceStart{0};
    int sourceLength{0};
};

struct StyledGlyphUnit {
    QString text;
    QFont font;
    bool whitespace{false};
    qreal baselineShift{0.0};
    qreal leading{0.0};
    qreal advanceOverride{-1.0};
    TextStyleRun style;
    int sourceStart{0};
    int sourceLength{0};
};

struct StyledPathPiece {
    QPainterPath path;
    QRectF layoutRect;
    TextStyleRun style;
    QPointF origin;
    QString text;
    QFont font;
    bool colorFont{false};
};

struct StyledTextPath {
    QPainterPath combined;
    std::vector<StyledPathPiece> pieces;
    QRectF layoutRect;
    bool hasLayoutRect{false};
    std::vector<GraphicTextCaretGeometry> carets;
};

bool fontHasColorGlyphs(const QFont& font)
{
    const QRawFont raw = QRawFont::fromFont(font);
    if (!raw.isValid()) return false;
    // QPainter can preserve the embedded palette/bitmap/SVG data from these
    // OpenType formats. Converting the glyph to QPainterPath cannot.
    return !raw.fontTable("COLR").isEmpty()
        || !raw.fontTable("CBDT").isEmpty()
        || !raw.fontTable("sbix").isEmpty()
        || !raw.fontTable("SVG ").isEmpty();
}

qreal textUnderlinePadding(const QFontMetricsF& metrics)
{
    // Keep the decoration clear of descenders (g, p, q, y). A small
    // scale-aware gap reads like Premiere's underline instead of touching
    // the glyph outline, while the clamp prevents extreme sizes drifting.
    return std::clamp(metrics.height() * 0.04, 2.0, 5.0);
}

/// Character-style-aware counterpart to buildTextPath(). TextStyleRun offsets
/// are UTF-16 positions, exactly matching QString/QTextCursor positions.
StyledTextPath buildStyledTextPath(const QString& source,
                                   const TextLayer& layer,
                                   int64_t localTick, qreal cx, qreal cy,
                                   int hAlign, int vAlign)
{
    std::vector<std::vector<StyledGlyphUnit>> paragraphs(1);
    std::vector<int> paragraphStarts(1, 0);
    std::vector<int> paragraphAlignments(1, hAlign);
    std::vector<bool> paragraphDirections(1, layer.rightToLeft());
    const auto& styles = layer.styleRuns();
    auto styleAt = [&](int position) -> TextStyleRun {
        for (const auto& run : styles) {
            const uint64_t end = static_cast<uint64_t>(run.start) + run.length;
            if (position >= static_cast<int>(run.start)
                && static_cast<uint64_t>(position) < end) {
                TextStyleRun resolved = run;
                if (!(run.overrideMask & TextOverrideCapitalization)) {
                    resolved.allCaps = layer.allCaps();
                    resolved.smallCaps = layer.smallCaps();
                }
                if (!(run.overrideMask & TextOverrideTracking))
                    resolved.tracking = layer.tracking().evaluate(localTick);
                if (!(run.overrideMask & TextOverrideBaseline))
                    resolved.baselineShift =
                        layer.baselineShift().evaluate(localTick);
                if (!(run.overrideMask & TextOverrideLeading))
                    resolved.leading = layer.leading().evaluate(localTick);
                if (!(run.overrideMask & TextOverrideFontStyle))
                    resolved.fontStyle = layer.fontStyle();
                if (!(run.overrideMask & TextOverrideKerning))
                    resolved.kerning = layer.kerning();
                if (!(run.overrideMask & TextOverrideTabWidth))
                    resolved.tabWidth = layer.tabWidth();
                if (!(run.overrideMask & TextOverrideTsume))
                    resolved.tsume = layer.tsume();
                if (!(run.overrideMask & TextOverrideFauxStyle)) {
                    resolved.fauxBold = layer.fauxBold();
                    resolved.fauxItalic = layer.fauxItalic();
                }
                if (!(run.overrideMask & TextOverrideDecoration))
                    resolved.underline = layer.underline();
                if (!(run.overrideMask & TextOverrideScript)) {
                    resolved.superscript = layer.superscript();
                    resolved.subscript = layer.subscript();
                }
                const auto& app = layer.appearance();
                if (!(run.overrideMask & TextOverrideFill)) {
                    resolved.appearance.fillEnabled = !app.fills.empty()
                        && app.fills.front().enabled;
                    if (!app.fills.empty())
                        resolved.appearance.fillColor = app.fills.front().color;
                }
                if (!(run.overrideMask & TextOverrideStroke)) {
                    resolved.appearance.strokeEnabled = !app.strokes.empty()
                        && app.strokes.front().enabled;
                    if (!app.strokes.empty()) {
                        resolved.appearance.strokeColor = app.strokes.front().color;
                        resolved.appearance.strokeWidth = app.strokes.front().width;
                        resolved.appearance.strokePosition = app.strokes.front().position;
                    }
                }
                if (!(run.overrideMask & TextOverrideShadow)) {
                    resolved.appearance.shadowEnabled = !app.shadows.empty()
                        && app.shadows.front().enabled;
                    if (!app.shadows.empty()) {
                        resolved.appearance.shadowColor = app.shadows.front().color;
                        resolved.appearance.shadowDistance = app.shadows.front().distance;
                        resolved.appearance.shadowAngle = app.shadows.front().angle;
                        resolved.appearance.shadowSoftness = app.shadows.front().softness;
                        resolved.appearance.shadowOpacity = app.shadows.front().opacity;
                    }
                }
                if (!(run.overrideMask & TextOverrideBackground)) {
                    resolved.appearance.backgroundEnabled = layer.backgroundEnabled();
                    resolved.appearance.backgroundColor = layer.backgroundColor();
                    resolved.appearance.backgroundPadding = layer.backgroundPadding();
                }
                return resolved;
            }
        }
        TextStyleRun fallback;
        fallback.fontFamily = layer.fontFamily();
        fallback.fontStyle = layer.fontStyle();
        fallback.fontSize = layer.fontSize();
        fallback.fontWeight = layer.fontWeight();
        fallback.italic = layer.isItalic();
        fallback.allCaps = layer.allCaps();
        fallback.smallCaps = layer.smallCaps();
        fallback.tracking = layer.tracking().evaluate(localTick);
        fallback.baselineShift = layer.baselineShift().evaluate(localTick);
        fallback.leading = layer.leading().evaluate(localTick);
        fallback.kerning = layer.kerning();
        fallback.tabWidth = layer.tabWidth();
        fallback.tsume = layer.tsume();
        fallback.fauxBold = layer.fauxBold();
        fallback.fauxItalic = layer.fauxItalic();
        fallback.underline = layer.underline();
        fallback.superscript = layer.superscript();
        fallback.subscript = layer.subscript();
        const auto& app = layer.appearance();
        fallback.appearance.fillEnabled = !app.fills.empty()
            && app.fills.front().enabled;
        if (!app.fills.empty()) fallback.appearance.fillColor = app.fills.front().color;
        fallback.appearance.strokeEnabled = !app.strokes.empty()
            && app.strokes.front().enabled;
        if (!app.strokes.empty()) {
            fallback.appearance.strokeColor = app.strokes.front().color;
            fallback.appearance.strokeWidth = app.strokes.front().width;
            fallback.appearance.strokePosition = app.strokes.front().position;
        }
        fallback.appearance.shadowEnabled = !app.shadows.empty()
            && app.shadows.front().enabled;
        if (!app.shadows.empty()) {
            fallback.appearance.shadowColor = app.shadows.front().color;
            fallback.appearance.shadowDistance = app.shadows.front().distance;
            fallback.appearance.shadowAngle = app.shadows.front().angle;
            fallback.appearance.shadowSoftness = app.shadows.front().softness;
            fallback.appearance.shadowOpacity = app.shadows.front().opacity;
        }
        fallback.appearance.backgroundEnabled = layer.backgroundEnabled();
        fallback.appearance.backgroundColor = layer.backgroundColor();
        fallback.appearance.backgroundPadding = layer.backgroundPadding();
        return fallback;
    };

    auto append = [&](const QString& text, TextStyleRun style, bool small,
                       bool whitespace, int sourceStart, int sourceLength) {
        if (text.isEmpty()) return;
        const double authoredPointSize = std::max(1.0,
            static_cast<double>(style.fontSize)
                * (small ? kSmallCapScale : 1.0));
        QFont font(QString::fromStdString(style.fontFamily),
                   std::max(1, static_cast<int>(std::lround(authoredPointSize))));
        font.setWeight(static_cast<QFont::Weight>(
            std::clamp(style.fontWeight, 1, 1000)));
        font.setItalic(style.italic);
        if (!style.fontStyle.empty())
            font.setStyleName(QString::fromStdString(style.fontStyle));
        if (style.fauxBold)
            font.setWeight(static_cast<QFont::Weight>(
                std::max(700, static_cast<int>(font.weight()))));
        if (style.fauxItalic) font.setItalic(true);
        // Underlines are added to the vector path below so we can control
        // their offset. QFont's native underline commonly intersects glyph
        // descenders and cannot be given extra padding.
        font.setUnderline(false);
        if (style.superscript || style.subscript) {
            font.setPointSizeF(std::max(1.0, font.pointSizeF() * 0.6));
            style.baselineShift += style.superscript
                ? style.fontSize * 0.35f : -style.fontSize * 0.2f;
        }
        font.setLetterSpacing(QFont::AbsoluteSpacing,
            static_cast<qreal>(style.tracking + style.kerning));
        // QFont's default stretch of 0 preserves the face's native width
        // class. Impact and other condensed faces become roughly 4/3 wider if
        // we explicitly set 100, even though 100 sounds like a no-op. Only
        // override the native width when the user actually authored Tsume.
        if (std::abs(style.tsume) > 0.001f) {
            font.setStretch(std::clamp(static_cast<int>(std::round(
                100.0 * std::clamp(1.0 - style.tsume / 100.0, 0.1, 1.0))),
                1, 4000));
        }

        paragraphs.back().push_back(
            {text, font, whitespace, static_cast<qreal>(style.baselineShift),
             static_cast<qreal>(style.leading),
              text == QStringLiteral("\t")
                 ? static_cast<qreal>(style.tabWidth) : -1.0,
              std::move(style), sourceStart, sourceLength});
    };

    auto paragraphAlignmentAt = [&](int position) {
        for (const auto& paragraph : layer.paragraphStyles()) {
            const uint64_t end = static_cast<uint64_t>(paragraph.start)
                + paragraph.length;
            if (position >= static_cast<int>(paragraph.start)
                && static_cast<uint64_t>(position) < end) {
                switch (paragraph.alignment) {
                case GTextAlign::Left: return int(Qt::AlignLeft);
                case GTextAlign::Right: return int(Qt::AlignRight);
                case GTextAlign::Justify: return int(Qt::AlignJustify);
                case GTextAlign::Center:
                default: return int(Qt::AlignHCenter);
                }
            }
        }
        return hAlign;
    };

    auto paragraphDirectionAt = [&](int position) {
        for (const auto& paragraph : layer.paragraphStyles()) {
            const uint64_t end = static_cast<uint64_t>(paragraph.start)
                + paragraph.length;
            if (position >= static_cast<int>(paragraph.start)
                && static_cast<uint64_t>(position) < end)
                return paragraph.rightToLeft;
        }
        return layer.rightToLeft();
    };

    for (int pos = 0; pos < source.size(); ++pos) {
        const QChar ch = source.at(pos);
        if (ch == QChar('\n')) {
            paragraphs.emplace_back();
            paragraphStarts.push_back(pos + 1);
            paragraphAlignments.push_back(paragraphAlignmentAt(pos + 1));
            paragraphDirections.push_back(paragraphDirectionAt(pos + 1));
            continue;
        }
        int units = 1;
        if (ch.isHighSurrogate() && pos + 1 < source.size()
            && source.at(pos + 1).isLowSurrogate())
            units = 2;
        TextStyleRun style = styleAt(pos);
        const bool small = style.smallCaps && !style.allCaps && ch.isLower();
        QString glyph = source.mid(pos, units);
        if (style.allCaps || style.smallCaps) glyph = glyph.toUpper();
        append(glyph, std::move(style), small, ch.isSpace(), pos, units);
        pos += units - 1;
    }

    std::vector<std::vector<StyledGlyphUnit>> unitLines;
    std::vector<int> lineAlignments;
    std::vector<bool> lineDirections;
    std::vector<int> lineStarts;
    const bool wrap = layer.useParagraphBox() && layer.boxWidth() > 1.0f;
    const qreal maxWidth = static_cast<qreal>(layer.boxWidth());
    auto widthOf = [](const std::vector<StyledGlyphUnit>& units) {
        qreal width = 0.0;
        for (const auto& unit : units)
            width += unit.advanceOverride >= 0.0
                ? unit.advanceOverride
                : QFontMetricsF(unit.font).horizontalAdvance(unit.text);
        return width;
    };

    for (size_t paragraphIndex = 0; paragraphIndex < paragraphs.size();
         ++paragraphIndex) {
        const auto& paragraph = paragraphs[paragraphIndex];
        if (!wrap) {
            unitLines.push_back(paragraph);
            lineAlignments.push_back(paragraphAlignments[paragraphIndex]);
            lineDirections.push_back(paragraphDirections[paragraphIndex]);
            lineStarts.push_back(paragraphStarts[paragraphIndex]);
            continue;
        }

        std::vector<StyledGlyphUnit> line;
        for (const auto& unit : paragraph) {
            line.push_back(unit);
            if (line.size() <= 1 || widthOf(line) <= maxWidth) continue;

            size_t breakAt = line.size();
            for (size_t i = line.size() - 1; i > 0; --i) {
                if (line[i].whitespace) {
                    breakAt = i;
                    break;
                }
            }

            std::vector<StyledGlyphUnit> overflow;
            if (breakAt < line.size()) {
                overflow.assign(line.begin() + static_cast<std::ptrdiff_t>(breakAt + 1),
                                line.end());
                line.erase(line.begin() + static_cast<std::ptrdiff_t>(breakAt),
                           line.end());
            } else {
                overflow.push_back(line.back());
                line.pop_back();
            }
            while (!line.empty() && line.back().whitespace) line.pop_back();
            if (!line.empty()) {
                lineStarts.push_back(line.front().sourceStart);
                unitLines.push_back(std::move(line));
                lineAlignments.push_back(paragraphAlignments[paragraphIndex]);
                lineDirections.push_back(paragraphDirections[paragraphIndex]);
            }
            while (!overflow.empty() && overflow.front().whitespace)
                overflow.erase(overflow.begin());
            line = std::move(overflow);
        }
        lineStarts.push_back(line.empty() ? paragraphStarts[paragraphIndex]
                                          : line.front().sourceStart);
        unitLines.push_back(std::move(line));
        lineAlignments.push_back(paragraphAlignments[paragraphIndex]);
        lineDirections.push_back(paragraphDirections[paragraphIndex]);
    }

    std::vector<std::vector<StyledGlyphRun>> lines;
    lines.reserve(unitLines.size());
    for (const auto& units : unitLines) {
        std::vector<StyledGlyphRun> line;
        for (const auto& unit : units) {
            if (!line.empty() && line.back().font == unit.font
                && line.back().baselineShift == unit.baselineShift
                && line.back().leading == unit.leading
                && line.back().advanceOverride < 0.0
                && unit.advanceOverride < 0.0
                && line.back().style == unit.style
                && line.back().sourceStart + line.back().sourceLength
                    == unit.sourceStart) {
                line.back().text += unit.text;
                line.back().sourceLength += unit.sourceLength;
            } else
                line.push_back(
                    {unit.text, unit.font, unit.baselineShift, unit.leading,
                     unit.advanceOverride, unit.style,
                     unit.sourceStart, unit.sourceLength});
        }
        lines.push_back(std::move(line));
    }

    struct LineMetrics {
        qreal width{0};
        qreal ascent{0};
        qreal height{0};
    };
    std::vector<LineMetrics> metrics(lines.size());
    const QFont fallbackFont(QString::fromStdString(layer.fontFamily()),
                             std::max(1, static_cast<int>(layer.fontSize())));
    const QFontMetricsF fallbackMetrics(fallbackFont);
    const qreal fallbackLeading = static_cast<qreal>(
        layer.leading().evaluate(localTick));
    qreal totalHeight = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto& lm = metrics[i];
        lm.ascent = fallbackMetrics.ascent();
        lm.height = lines[i].empty()
            ? fallbackMetrics.lineSpacing() + fallbackLeading : 0.0;
        for (const auto& run : lines[i]) {
            const QFontMetricsF fm(run.font);
            lm.width += run.advanceOverride >= 0.0
                ? run.advanceOverride : fm.horizontalAdvance(run.text);
            lm.ascent = std::max(lm.ascent,
                fm.ascent() + std::max<qreal>(0.0, run.baselineShift));
            const qreal underlineExtra = run.style.underline
                ? textUnderlinePadding(fm)
                    + std::max<qreal>(1.0, fm.lineWidth())
                : 0.0;
            lm.height = std::max(lm.height,
                fm.lineSpacing() + std::abs(run.baselineShift) + run.leading
                    + underlineExtra);
        }
        totalHeight += lm.height;
    }

    qreal blockTop = cy - totalHeight * 0.5;
    if (!layer.useParagraphBox() && !metrics.empty())
        blockTop = cy - metrics.front().height * 0.5;
    else if (vAlign == Qt::AlignTop) blockTop = cy;
    else if (vAlign == Qt::AlignBottom) blockTop = cy - totalHeight;

    StyledTextPath result;
    result.carets.resize(static_cast<size_t>(source.size()) + 1);
    qreal lineTop = blockTop;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& lm = metrics[i];
        qreal penX = cx - lm.width * 0.5;
        const int lineAlignment = i < lineAlignments.size()
            ? lineAlignments[i] : hAlign;
        if (lineAlignment == Qt::AlignLeft) penX = cx;
        else if (lineAlignment == Qt::AlignRight) penX = cx - lm.width;
        if (lm.width > 0.0 && lm.height > 0.0) {
            const QRectF lineRect(penX, lineTop, lm.width, lm.height);
            result.layoutRect = result.hasLayoutRect
                ? result.layoutRect.united(lineRect) : lineRect;
            result.hasLayoutRect = true;
        }
        const qreal baseline = lineTop + lm.ascent;
        if (i < lineStarts.size()) {
            const int lineStart = std::clamp(lineStarts[i], 0,
                static_cast<int>(source.size()));
            result.carets[static_cast<size_t>(lineStart)] =
                {penX, lineTop, lineTop + lm.height, true};
        }
        auto addRun = [&](const StyledGlyphRun& run) {
            const qreal runWidth = run.advanceOverride >= 0.0
                ? run.advanceOverride
                : QFontMetricsF(run.font).horizontalAdvance(run.text);
            if (run.sourceLength > 0 && run.sourceStart >= 0) {
                QTextLayout shaped(run.text, run.font);
                shaped.beginLayout();
                QTextLine shapedLine = shaped.createLine();
                if (shapedLine.isValid())
                    shapedLine.setLineWidth(1.0e9);
                shaped.endLayout();
                for (int offset = 0; offset <= run.sourceLength; ++offset) {
                    const int sourcePosition = run.sourceStart + offset;
                    if (sourcePosition < 0 || sourcePosition > source.size())
                        continue;
                    qreal advance = 0.0;
                    if (run.advanceOverride >= 0.0) {
                        advance = runWidth * offset / run.sourceLength;
                    } else if (shapedLine.isValid()) {
                        const int textPosition = std::clamp(
                            static_cast<int>(std::lround(
                                static_cast<double>(run.text.size()) * offset
                                / run.sourceLength)), 0,
                            static_cast<int>(run.text.size()));
                        advance = shapedLine.cursorToX(textPosition);
                    } else {
                        const int textPosition = std::clamp(offset, 0,
                            static_cast<int>(run.text.size()));
                        advance = QFontMetricsF(run.font).horizontalAdvance(
                            run.text.left(textPosition));
                    }
                    result.carets[static_cast<size_t>(sourcePosition)] =
                        {penX + advance, lineTop,
                         lineTop + lm.height, true};
                }
            }
            QPainterPath piece;
            if (run.advanceOverride < 0.0 && !run.text.isEmpty()) {
                const QPointF origin(penX, baseline - run.baselineShift);
                piece.addText(origin, run.font, run.text);
                if (run.style.underline) {
                    const QFontMetricsF fm(run.font);
                    const qreal lineWidth = std::max<qreal>(
                        1.0, fm.lineWidth());
                    piece.addRect(QRectF(penX,
                        baseline - run.baselineShift + fm.descent()
                            + textUnderlinePadding(fm),
                        fm.horizontalAdvance(run.text),
                        lineWidth));
                }
                result.combined.addPath(piece);
                result.pieces.push_back({piece,
                                         QRectF(penX, lineTop, runWidth,
                                                lm.height),
                                         run.style, origin, run.text, run.font,
                                         fontHasColorGlyphs(run.font)});
            }
            penX += runWidth;
        };
        if (i < lineDirections.size() && lineDirections[i]) {
            for (auto it = lines[i].rbegin(); it != lines[i].rend(); ++it)
                addRun(*it);
        } else {
            for (const auto& run : lines[i]) addRun(run);
        }
        lineTop += lm.height;
    }
    return result;
}

} // namespace

GraphicTextLayoutBounds measureGraphicTextLayout(
    const TextLayer* layer, int64_t localTick, double centerX, double centerY,
    int horizontalAlignment, int verticalAlignment)
{
    GraphicTextLayoutBounds bounds;
    if (!layer) return bounds;

    // Paragraph text is laid out relative to the selected box edge; point
    // text is laid out relative to its first-line centre.  This mirrors the
    // textCy adjustment in renderGraphicClip() below.
    qreal textCenterY = static_cast<qreal>(centerY);
    if (layer->useParagraphBox()) {
        const qreal halfBox = std::max<qreal>(1.0, layer->boxHeight()) * 0.5;
        if (verticalAlignment == Qt::AlignTop)
            textCenterY -= halfBox;
        else if (verticalAlignment == Qt::AlignBottom)
            textCenterY += halfBox;
    }

    const StyledTextPath layout = buildStyledTextPath(
        QString::fromStdString(layer->text()), *layer, localTick,
        static_cast<qreal>(centerX), textCenterY,
        horizontalAlignment, verticalAlignment);
    if (layout.combined.isEmpty()) return bounds;

    const QRectF rect = layout.combined.boundingRect();
    bounds.left = rect.left();
    bounds.top = rect.top();
    bounds.right = rect.right();
    bounds.bottom = rect.bottom();
    bounds.valid = rect.isValid() && !rect.isEmpty();
    if (layout.hasLayoutRect) {
        const QRectF logical = layout.layoutRect;
        bounds.layoutLeft = logical.left();
        bounds.layoutTop = logical.top();
        bounds.layoutRight = logical.right();
        bounds.layoutBottom = logical.bottom();
        bounds.layoutValid = logical.isValid() && !logical.isEmpty();
    }
    bounds.carets = layout.carets;
    return bounds;
}

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
    if (graphicClipBakesOuterTransform(clip, localTick)) {
        mixF(clip->positionX().evaluate(localTick));
        mixF(clip->positionY().evaluate(localTick));
        mixF(clip->scaleX().evaluate(localTick));
        mixF(clip->scaleY().evaluate(localTick));
        mixF(clip->rotation().evaluate(localTick));
        mixF(clip->anchorX().evaluate(localTick));
        mixF(clip->anchorY().evaluate(localTick));
    }
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
            mixBytes(tl->fontStyle().data(), tl->fontStyle().size()); mixU(0x4653u);
            mixF(tl->fontSize()); mixU(static_cast<uint64_t>(tl->fontWeight()));
            mixU(tl->isItalic() ? 1u : 0u); mixU(tl->allCaps() ? 1u : 0u); mixU(tl->smallCaps() ? 1u : 0u);
            mixU(tl->styleRuns().size());
            for (const auto& run : tl->styleRuns()) {
                mixU(run.start); mixU(run.length);
                mixBytes(run.fontFamily.data(), run.fontFamily.size());
                mixBytes(run.fontStyle.data(), run.fontStyle.size());
                mixF(run.fontSize); mixU(static_cast<uint64_t>(run.fontWeight));
                mixU(run.italic ? 1u : 0u);
                mixU(run.allCaps ? 1u : 0u);
                mixU(run.smallCaps ? 1u : 0u);
                mixF(run.tracking); mixF(run.baselineShift); mixF(run.leading);
                mixF(run.kerning); mixF(run.tabWidth); mixF(run.tsume);
                mixU(run.fauxBold); mixU(run.fauxItalic); mixU(run.underline);
                mixU(run.superscript); mixU(run.subscript);
                mixU(run.appearance.fillEnabled); mixU(run.appearance.fillColor);
                mixU(run.appearance.strokeEnabled); mixU(run.appearance.strokeColor);
                mixF(run.appearance.strokeWidth);
                mixU(static_cast<uint64_t>(run.appearance.strokePosition));
                mixU(run.appearance.shadowEnabled); mixU(run.appearance.shadowColor);
                mixF(run.appearance.shadowDistance); mixF(run.appearance.shadowAngle);
                mixF(run.appearance.shadowSoftness); mixF(run.appearance.shadowOpacity);
                mixU(run.appearance.backgroundEnabled);
                mixU(run.appearance.backgroundColor);
                mixF(run.appearance.backgroundPadding);
                mixU(run.overrideMask);
            }
            mixU(static_cast<uint64_t>(tl->alignment())); mixU(static_cast<uint64_t>(tl->vAlignment()));
            mixF(tl->tracking().evaluate(localTick));
            mixF(tl->leading().evaluate(localTick));
            mixF(tl->baselineShift().evaluate(localTick));
            mixF(tl->kerning()); mixF(tl->tabWidth()); mixF(tl->tsume());
            mixU(tl->fauxBold()); mixU(tl->fauxItalic()); mixU(tl->underline());
            mixU(tl->superscript()); mixU(tl->subscript());
            mixU(tl->rightToLeft()); mixU(tl->backgroundEnabled());
            mixU(tl->backgroundColor()); mixF(tl->backgroundPadding());
            mixU(tl->maskWithText());
            mixBytes(tl->linkedStyleName().data(), tl->linkedStyleName().size());
            mixU(tl->paragraphStyles().size());
            for (const auto& paragraph : tl->paragraphStyles()) {
                mixU(paragraph.start); mixU(paragraph.length);
                mixU(static_cast<uint64_t>(paragraph.alignment));
                mixU(paragraph.rightToLeft);
            }
            mixU(tl->useParagraphBox() ? 1u : 0u);
            mixF(tl->boxWidth());
            mixF(tl->boxHeight());
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

bool graphicClipBakesOuterTransform(GraphicClip* clip, int64_t localTick)
{
    if (!clip || clip->maskCount() != 0 || clip->effects().hasActiveEffects())
        return false;

    // A multi-sample GPU transform is still required for motion blur.  Static
    // and normally animated graphics are rendered at the current tick and can
    // safely bake their outer transform into the bitmap.
    return std::abs(clip->shutterAngle().evaluate(localTick)) < 0.001f;
}

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

    // A GraphicClip used to be drawn into an already-clipped project-sized
    // bitmap and only then moved/scaled by the GPU.  Text below the bitmap's
    // bottom edge was therefore discarded even when the outer clip transform
    // placed that text well inside the final Program frame.  Fold the safe
    // clip-level transform into this painter first so clipping happens only at
    // the final output boundary.
    if (graphicClipBakesOuterTransform(clip, localTick)) {
        constexpr double kReferenceWidth = 1920.0;
        constexpr double kReferenceHeight = 1080.0;
        const double posX = clip->positionX().evaluate(localTick)
            * static_cast<double>(renderW) / kReferenceWidth;
        const double posY = clip->positionY().evaluate(localTick)
            * static_cast<double>(renderH) / kReferenceHeight;
        const double anchorX = clip->anchorX().evaluate(localTick)
            * static_cast<double>(renderW) / kReferenceWidth;
        const double anchorY = clip->anchorY().evaluate(localTick)
            * static_cast<double>(renderH) / kReferenceHeight;
        const double centerX = static_cast<double>(renderW) * 0.5;
        const double centerY = static_cast<double>(renderH) * 0.5;

        painter.translate(centerX + posX, centerY + posY);
        painter.translate(anchorX, anchorY);
        painter.rotate(clip->rotation().evaluate(localTick));
        painter.scale(clip->scaleX().evaluate(localTick),
                      clip->scaleY().evaluate(localTick));
        painter.translate(-anchorX, -anchorY);
        painter.translate(-centerX, -centerY);
    }

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

            const QString sourceText = QString::fromStdString(tl->text());
            QString text = sourceText;
            // ALL CAPS takes precedence over small caps when both are set
            // (matches Premiere). Small caps only applies when allCaps is off.
            const bool smallCaps = tl->smallCaps() && !tl->allCaps();
            if (tl->allCaps()) text = text.toUpper();

            // Paragraph box: word-wrap to the box width. Off (the default)
            // keeps point-text behaviour — lines break only on '\n'.
            if (tl->useParagraphBox() && tl->boxWidth() > 1.0f) {
                const QFontMetricsF fmWrap(font);
                text = wrapToWidth(text, fmWrap,
                                   static_cast<qreal>(tl->boxWidth()),
                                   smallCaps);
            }

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
            qreal textCy = cy;
            if (tl->useParagraphBox() && tl->boxHeight() > 1.0f) {
                const qreal halfBox = static_cast<qreal>(tl->boxHeight()) * 0.5;
                if (vAlign == Qt::AlignTop) textCy = cy - halfBox;
                else if (vAlign == Qt::AlignBottom) textCy = cy + halfBox;
            }

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

            const bool useStyledLayout = !tl->styleRuns().empty()
                || !tl->paragraphStyles().empty() || !tl->fontStyle().empty()
                || tl->kerning() != 0.0f || tl->tsume() != 0.0f
                || tl->tabWidth() != 48.0f || sourceText.contains(QChar('\t'))
                || tl->fauxBold() || tl->fauxItalic() || tl->underline()
                || tl->superscript() || tl->subscript()
                || tl->rightToLeft() || tl->backgroundEnabled()
                || tl->maskWithText() || fontHasColorGlyphs(font);
            StyledTextPath styledLayout;
            QPainterPath glyphPath;
            if (useStyledLayout) {
                styledLayout = buildStyledTextPath(
                    sourceText, *tl, localTick, cx, textCy, hAlign, vAlign);
                glyphPath = styledLayout.combined;
            } else {
                glyphPath = buildTextPath(
                    lines, font, smallFont, fmFull, fmSmall, cx, textCy,
                    hAlign, vAlign,
                    static_cast<qreal>(tl->leading().evaluate(localTick)),
                    static_cast<qreal>(tl->baselineShift().evaluate(localTick)),
                    !tl->useParagraphBox());
            }

            painter.setRenderHint(QPainter::Antialiasing, true);

            if (useStyledLayout) {
                // Range appearance is painted piece-by-piece so disabling a
                // fill or changing an outline affects only the selected text.
                // A layer background is one typographic block, rather than a
                // union of glyph outlines.  Besides matching Premiere, this
                // keeps ascenders, descenders, spaces, and style-run breaks
                // from producing jagged edges.
                if (tl->backgroundEnabled() && styledLayout.hasLayoutRect) {
                    const qreal padding = tl->backgroundPadding();
                    painter.fillRect(styledLayout.layoutRect.adjusted(
                        -padding, -padding, padding, padding),
                        toQColor(tl->backgroundColor()));
                } else {
                    // Character-range backgrounds still use their own style,
                    // but cover the run's full line cell instead of tracing
                    // the visible shapes of its letters.
                    for (const auto& piece : styledLayout.pieces) {
                        const auto& appearance = piece.style.appearance;
                        if (!appearance.backgroundEnabled) continue;
                        const qreal padding = appearance.backgroundPadding;
                        painter.fillRect(piece.layoutRect.adjusted(
                            -padding, -padding, padding, padding),
                            toQColor(appearance.backgroundColor));
                    }
                }
                for (const auto& piece : styledLayout.pieces) {
                    const auto& appearance = piece.style.appearance;
                    if (!appearance.shadowEnabled) continue;
                    const float radians = appearance.shadowAngle
                        * 3.14159265f / 180.0f;
                    QColor color = toQColor(appearance.shadowColor);
                    color.setAlphaF(std::clamp<double>(
                        appearance.shadowOpacity, 0.0, 1.0));
                    const qreal dx = std::cos(radians)
                        * appearance.shadowDistance;
                    const qreal dy = std::sin(radians)
                        * appearance.shadowDistance;
                    paintTextShadow(painter, canvas, piece.path, color,
                                    dx, dy, appearance.shadowSoftness);
                }
                for (const auto& piece : styledLayout.pieces) {
                    const auto& appearance = piece.style.appearance;
                    if (!appearance.strokeEnabled
                        || appearance.strokeWidth < 0.1f
                        || appearance.strokePosition != StrokePosition::Outer)
                        continue;
                    QPen pen(toQColor(appearance.strokeColor));
                    pen.setWidthF(appearance.strokeWidth * 2.0);
                    pen.setJoinStyle(Qt::RoundJoin);
                    painter.setPen(pen);
                    painter.setBrush(Qt::NoBrush);
                    painter.drawPath(piece.path);
                }
                for (const auto& piece : styledLayout.pieces) {
                    const auto& appearance = piece.style.appearance;
                    if (!appearance.fillEnabled) continue;
                    if (piece.colorFont) {
                        // Keep COLR/CBDT/sbix/SVG glyphs in their native form.
                        // QPainterPath::addText flattens them to a monochrome
                        // outline, while drawText preserves their palettes.
                        painter.save();
                        painter.setFont(piece.font);
                        painter.setPen(toQColor(appearance.fillColor));
                        painter.drawText(piece.origin, piece.text);
                        painter.restore();
                    } else {
                        painter.fillPath(piece.path,
                                         toQColor(appearance.fillColor));
                    }
                }
                for (const auto& piece : styledLayout.pieces) {
                    const auto& appearance = piece.style.appearance;
                    if (!appearance.strokeEnabled
                        || appearance.strokeWidth < 0.1f
                        || appearance.strokePosition == StrokePosition::Outer)
                        continue;
                    QPen pen(toQColor(appearance.strokeColor));
                    pen.setWidthF(appearance.strokePosition
                        == StrokePosition::Inner
                        ? appearance.strokeWidth * 2.0
                        : appearance.strokeWidth);
                    pen.setJoinStyle(Qt::RoundJoin);
                    painter.setPen(pen);
                    painter.setBrush(Qt::NoBrush);
                    if (appearance.strokePosition == StrokePosition::Inner) {
                        painter.save();
                        painter.setClipPath(piece.path);
                        painter.drawPath(piece.path);
                        painter.restore();
                    } else {
                        painter.drawPath(piece.path);
                    }
                }
                if (tl->maskWithText()) {
                    // DestinationIn must cover the entire destination. Filling
                    // only the glyph path leaves all pixels outside the text
                    // untouched, which is not a mask at all.
                    QImage textMask(canvas.size(),
                                    QImage::Format_ARGB32_Premultiplied);
                    textMask.fill(Qt::transparent);
                    QPainter maskPainter(&textMask);
                    maskPainter.setRenderHint(QPainter::Antialiasing, true);
                    maskPainter.setTransform(painter.transform());
                    maskPainter.fillPath(glyphPath, Qt::white);
                    maskPainter.end();

                    painter.save();
                    painter.resetTransform();
                    painter.setOpacity(1.0);
                    painter.setCompositionMode(
                        QPainter::CompositionMode_DestinationIn);
                    painter.drawImage(QPoint(0, 0), textMask);
                    painter.restore();
                }
            } else {

            // Strokes honour their position relative to the glyph edge:
            //   Outer  — pen 2*w BEHIND the fill (fill hides the inner half,
            //            leaving a w px band outside — the legacy look)
            //   Center — pen w ON TOP of the fill (band straddles the edge)
            //   Inner  — pen 2*w on top, clipped to the glyphs (band inside)
            for (auto it = app.strokes.rbegin(); it != app.strokes.rend(); ++it) {
                if (!it->enabled || it->width < 0.1f) continue;
                if (it->position != StrokePosition::Outer) continue;
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
                paintTextShadow(painter, canvas, glyphPath, sc, sdx, sdy,
                                it->softness);
            }

            // Fill
            const QColor fillC = (!app.fills.empty() && app.fills[0].enabled)
                               ? toQColor(app.fills[0].color)
                               : QColor(255, 255, 255);
            painter.fillPath(glyphPath, fillC);

            // Center/Inner strokes render over the fill (see comment above).
            for (auto it = app.strokes.rbegin(); it != app.strokes.rend(); ++it) {
                if (!it->enabled || it->width < 0.1f) continue;
                if (it->position == StrokePosition::Outer) continue;
                QPen pen(toQColor(it->color));
                pen.setJoinStyle(Qt::RoundJoin);
                pen.setCapStyle(Qt::RoundCap);
                painter.setBrush(Qt::NoBrush);
                if (it->position == StrokePosition::Center) {
                    pen.setWidthF(static_cast<qreal>(it->width));
                    painter.setPen(pen);
                    painter.drawPath(glyphPath);
                } else { // Inner
                    pen.setWidthF(static_cast<qreal>(it->width) * 2.0);
                    painter.setPen(pen);
                    painter.save();
                    painter.setClipPath(glyphPath);
                    painter.drawPath(glyphPath);
                    painter.restore();
                }
            }
            }

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

// Render-state hash for renderCaptionClip's memo cache. Captions are static
// per clip (no per-tick animation), so the key covers every editable field
// plus the output dimensions — an edit changes the key, so the cache is
// self-invalidating and cannot serve a stale frame.
static constexpr int kCaptionLeadingProperty = QTextFormat::UserProperty + 1;

static uint64_t hashCaptionClipRenderState(CaptionClip* clip,
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
    mixBytes(clip->text().data(), clip->text().size());             mixU(0x5354u);
    mixBytes(clip->speaker().data(), clip->speaker().size());       mixU(0x5350u);
    mixBytes(clip->fontFamily().data(), clip->fontFamily().size()); mixU(0x4646u);
    mixBytes(clip->fontStyle().data(), clip->fontStyle().size()); mixU(0x4653u);
    mixF(clip->fontSize());
    mixU(clip->textColor()); mixU(clip->bgColor());
    mixU(static_cast<uint64_t>(clip->position()));
    mixU(clip->isBold() ? 1u : 0u);
    mixU(clip->outlineColor()); mixF(clip->outlineWidth());
    mixU(clip->showSpeaker() ? 1u : 0u);
    mixU(clip->isItalic()); mixU(clip->allCaps()); mixU(clip->smallCaps());
    mixU(clip->underline()); mixU(clip->superscript()); mixU(clip->subscript());
    mixU(clip->fauxBold()); mixU(clip->fauxItalic());
    mixF(clip->tracking()); mixF(clip->leading());
    mixU(static_cast<uint64_t>(clip->alignment()));
    mixBytes(clip->trackStyleName().data(), clip->trackStyleName().size());
    mixU(clip->styleRuns().size());
    for (const auto& run : clip->styleRuns()) {
        mixU(run.start); mixU(run.length);
        mixBytes(run.fontFamily.data(), run.fontFamily.size());
        mixBytes(run.fontStyle.data(), run.fontStyle.size());
        mixF(run.fontSize); mixU(static_cast<uint64_t>(run.fontWeight));
        mixU(run.italic ? 1u : 0u);
        mixU(run.allCaps ? 1u : 0u);
        mixU(run.smallCaps ? 1u : 0u);
        mixF(run.tracking); mixF(run.baselineShift); mixF(run.leading);
        mixF(run.kerning); mixF(run.tabWidth); mixF(run.tsume);
        mixU(run.fauxBold); mixU(run.fauxItalic); mixU(run.underline);
        mixU(run.superscript); mixU(run.subscript);
        mixU(run.appearance.fillEnabled); mixU(run.appearance.fillColor);
        mixU(run.appearance.strokeEnabled); mixU(run.appearance.strokeColor);
        mixF(run.appearance.strokeWidth);
        mixU(static_cast<uint64_t>(run.appearance.strokePosition));
        mixU(run.appearance.backgroundEnabled);
        mixU(run.appearance.backgroundColor);
        mixF(run.appearance.backgroundPadding);
        mixU(run.overrideMask);
    }
    mixU(clip->paragraphStyles().size());
    for (const auto& paragraph : clip->paragraphStyles()) {
        mixU(paragraph.start); mixU(paragraph.length);
        mixU(static_cast<uint64_t>(paragraph.alignment));
        mixU(paragraph.rightToLeft);
    }
    return h;
}

std::shared_ptr<CachedFrame> renderCaptionClip(
    CaptionClip* clip, int64_t tick, uint32_t outW, uint32_t outH,
    uint32_t refW, uint32_t refH)
{
    (void)tick;
    if (!clip) return nullptr;
    QString text = QString::fromStdString(clip->text());
    if (text.trimmed().isEmpty()) return nullptr;
    // Burn the speaker label into the cue when enabled ("SPEAKER: text").
    int captionStyleOffset = 0;
    if (clip->showSpeaker() && !clip->speaker().empty()) {
        const QString prefix = QString::fromStdString(clip->speaker()).toUpper()
            + QStringLiteral(": ");
        captionStyleOffset = prefix.size();
        text = prefix + text;
    }

    // Render at full project resolution (like renderGraphicClip) so font
    // metrics and pixel sizes match the authored values, then downscale.
    const uint32_t renderW = (refW > 0 && refW > outW) ? refW : outW;
    const uint32_t renderH = (refH > 0 && refH > outH) ? refH : outH;
    const bool needsDownscale = (renderW != outW || renderH != outH);

    // ── Memoized render ─────────────────────────────────────────────────
    // A caption is visible for seconds at a time; without this, every frame
    // it covers re-allocates a full project-res ARGB canvas and re-runs the
    // text layout. Same pattern as renderGraphicClip's memo above.
    static std::mutex                                                         s_ccMtx;
    static std::unordered_map<uint64_t, std::pair<uint64_t, std::shared_ptr<CachedFrame>>> s_ccCache;
    const uint64_t renderKey =
        hashCaptionClipRenderState(clip, outW, outH, renderW, renderH);
    {
        std::lock_guard<std::mutex> lock(s_ccMtx);
        auto it = s_ccCache.find(clip->id());
        if (it != s_ccCache.end() && it->second.first == renderKey && it->second.second)
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

    int fontPx = std::max(1, static_cast<int>(std::lround(clip->fontSize())));
    QFont font(QString::fromStdString(clip->fontFamily()), fontPx);
    font.setItalic(clip->isItalic() || clip->fauxItalic());
    if (clip->fauxBold()) font.setWeight(QFont::Bold);
    if (!clip->fontStyle().empty())
        font.setStyleName(QString::fromStdString(clip->fontStyle()));
    font.setUnderline(clip->underline());
    font.setCapitalization(clip->allCaps() ? QFont::AllUppercase
        : clip->smallCaps() ? QFont::SmallCaps : QFont::MixedCase);
    font.setLetterSpacing(QFont::AbsoluteSpacing, clip->tracking());
    font.setBold(clip->isBold());   // default on — captions read best bold
    painter.setFont(font);

    if (clip->fauxBold()) {
        font.setWeight(QFont::Bold);
        painter.setFont(font);
    }

    // Lay the text out within ~80% of frame width, word-wrapped & centered.
    const int margin = static_cast<int>(renderW) / 10;  // 10% side margins
    const int maxTextW = static_cast<int>(renderW) - 2 * margin;
    QFontMetrics fm(font);
    QRect textBound;
    std::unique_ptr<QTextDocument> richDocument;
    const bool useRichDocument = !clip->styleRuns().empty()
        || !clip->paragraphStyles().empty() || !clip->fontStyle().empty()
        || clip->isItalic() || clip->allCaps() || clip->smallCaps()
        || clip->underline() || clip->superscript() || clip->subscript()
        || clip->fauxBold() || clip->fauxItalic()
        || clip->tracking() != 0.0f || clip->leading() != 0.0f;
    if (useRichDocument) {
        richDocument = std::make_unique<QTextDocument>();
        richDocument->setDocumentMargin(0.0);
        richDocument->setDefaultFont(font);
        richDocument->setPlainText(text);
        QTextCursor all(richDocument.get());
        all.select(QTextCursor::Document);
        QTextCharFormat baseFormat;
        baseFormat.setFont(font, QTextCharFormat::FontPropertiesAll);
        baseFormat.setForeground(toQColor(clip->textColor()));
        if (clip->outlineWidth() > 0.01f) {
            QPen outline(toQColor(clip->outlineColor()));
            outline.setWidthF(clip->outlineWidth());
            baseFormat.setTextOutline(outline);
        }
        if (clip->superscript())
            baseFormat.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
        else if (clip->subscript())
            baseFormat.setVerticalAlignment(QTextCharFormat::AlignSubScript);
        baseFormat.setProperty(kCaptionLeadingProperty, clip->leading());
        all.mergeCharFormat(baseFormat);
        QTextBlockFormat blockFormat;
        blockFormat.setAlignment(clip->alignment() == GTextAlign::Left
            ? Qt::AlignLeft : clip->alignment() == GTextAlign::Right
            ? Qt::AlignRight : clip->alignment() == GTextAlign::Justify
            ? Qt::AlignJustify : Qt::AlignHCenter);
        all.mergeBlockFormat(blockFormat);
        for (const auto& run : clip->styleRuns()) {
            const int start = std::clamp<int>(
                captionStyleOffset + static_cast<int>(run.start), 0,
                text.size());
            const int end = std::clamp<int>(
                start + static_cast<int>(run.length), start, text.size());
            if (end <= start) continue;
            QFont runFont(QString::fromStdString(run.fontFamily));
            runFont.setPointSizeF(std::max(1.0,
                static_cast<double>(run.fontSize)));
            runFont.setWeight(static_cast<QFont::Weight>(
                std::clamp(run.fontWeight, 1, 1000)));
            runFont.setItalic(run.italic);
            const QString runStyle = run.overrideMask & TextOverrideFontStyle
                ? QString::fromStdString(run.fontStyle)
                : QString::fromStdString(clip->fontStyle());
            if (!runStyle.isEmpty()) runFont.setStyleName(runStyle);
            const bool fauxBold = run.overrideMask & TextOverrideFauxStyle
                ? run.fauxBold : clip->fauxBold();
            const bool fauxItalic = run.overrideMask & TextOverrideFauxStyle
                ? run.fauxItalic : clip->fauxItalic();
            if (fauxBold) runFont.setWeight(static_cast<QFont::Weight>(
                std::max(700, static_cast<int>(runFont.weight()))));
            if (fauxItalic) runFont.setItalic(true);
            runFont.setUnderline(run.overrideMask & TextOverrideDecoration
                ? run.underline : clip->underline());
            if (run.overrideMask & TextOverrideCapitalization) {
                runFont.setCapitalization(run.allCaps ? QFont::AllUppercase
                    : (run.smallCaps ? QFont::SmallCaps
                                     : QFont::MixedCase));
            }
            const float runTracking = run.overrideMask & TextOverrideTracking
                ? run.tracking : clip->tracking();
            const float runKerning = run.overrideMask & TextOverrideKerning
                ? run.kerning : 0.0f;
            runFont.setLetterSpacing(QFont::AbsoluteSpacing,
                                     runTracking + runKerning);
            if (run.overrideMask & TextOverrideTsume)
                runFont.setStretch(std::clamp(static_cast<int>(std::round(
                    100.0 * std::clamp(1.0 - run.tsume / 100.0, 0.1, 1.0))),
                    1, 4000));
            QTextCharFormat format;
            format.setFont(runFont, QTextCharFormat::FontPropertiesAll);
            if (run.overrideMask & TextOverrideBaseline) {
                format.setBaselineOffset(run.fontSize > 0.0f
                    ? 100.0 * run.baselineShift / run.fontSize : 0.0);
            }
            if (run.overrideMask & TextOverrideLeading)
                format.setProperty(kCaptionLeadingProperty, run.leading);
            const bool super = run.overrideMask & TextOverrideScript
                ? run.superscript : clip->superscript();
            const bool sub = run.overrideMask & TextOverrideScript
                ? run.subscript : clip->subscript();
            format.setVerticalAlignment(super
                ? QTextCharFormat::AlignSuperScript
                : sub ? QTextCharFormat::AlignSubScript
                      : QTextCharFormat::AlignNormal);
            if (run.overrideMask & TextOverrideFill)
                format.setForeground(run.appearance.fillEnabled
                    ? toQColor(run.appearance.fillColor)
                    : QColor(Qt::transparent));
            if (run.overrideMask & TextOverrideStroke) {
                QPen outline(Qt::NoPen);
                if (run.appearance.strokeEnabled) {
                    outline = QPen(toQColor(run.appearance.strokeColor));
                    outline.setWidthF(run.appearance.strokeWidth);
                }
                format.setTextOutline(outline);
            }
            if (run.overrideMask & TextOverrideBackground)
                format.setBackground(run.appearance.backgroundEnabled
                    ? toQColor(run.appearance.backgroundColor)
                    : QColor(Qt::transparent));
            QTextCursor range(richDocument.get());
            range.setPosition(start);
            range.setPosition(end, QTextCursor::KeepAnchor);
            range.mergeCharFormat(format);
        }
        for (const auto& paragraph : clip->paragraphStyles()) {
            const int start = std::clamp<int>(captionStyleOffset
                + static_cast<int>(paragraph.start), 0, text.size());
            const int end = std::clamp<int>(start
                + static_cast<int>(paragraph.length), start, text.size());
            QTextCursor range(richDocument.get());
            range.setPosition(start);
            range.setPosition(end, QTextCursor::KeepAnchor);
            QTextBlockFormat format;
            format.setAlignment(paragraph.alignment == GTextAlign::Left
                ? Qt::AlignLeft : paragraph.alignment == GTextAlign::Right
                ? Qt::AlignRight : paragraph.alignment == GTextAlign::Justify
                ? Qt::AlignJustify : Qt::AlignHCenter);
            format.setLayoutDirection(paragraph.rightToLeft
                ? Qt::RightToLeft : Qt::LeftToRight);
            range.mergeBlockFormat(format);
        }
        for (QTextBlock block = richDocument->begin();
             block.isValid(); block = block.next()) {
            qreal leading = 0.0;
            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid()) continue;
                const QTextCharFormat format = fragment.charFormat();
                if (format.hasProperty(kCaptionLeadingProperty))
                    leading = std::max(leading, static_cast<qreal>(
                        format.property(kCaptionLeadingProperty).toDouble()));
            }
            if (leading > 0.0) {
                QTextCursor blockCursor(block);
                QTextBlockFormat format = blockCursor.blockFormat();
                format.setLineHeight(leading,
                    QTextBlockFormat::LineDistanceHeight);
                blockCursor.setBlockFormat(format);
            }
        }
        richDocument->setTextWidth(maxTextW);
        textBound = QRect(0, 0,
            std::max(1, static_cast<int>(std::ceil(richDocument->idealWidth()))),
            std::max(1, static_cast<int>(std::ceil(richDocument->size().height()))));
    } else {
        QRect boundUnbounded(0, 0, maxTextW, static_cast<int>(renderH));
        textBound = fm.boundingRect(
            boundUnbounded, Qt::AlignHCenter | Qt::TextWordWrap, text);
    }

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

    // Outline: offset-grid stamp in the outline color behind the fill text.
    // O(width²) drawText calls, but the memo cache above means a caption
    // renders once per edit, not once per frame — so this stays cheap.
    if (richDocument) {
        auto drawRich = [&](int ox, int oy, const QColor& color) {
            painter.save();
            painter.translate(margin + ox, boxY + padY + oy);
            QAbstractTextDocumentLayout::PaintContext context;
            context.palette.setColor(QPalette::Text, color);
            context.clip = QRectF(0.0, 0.0, maxTextW,
                                  richDocument->size().height());
            richDocument->documentLayout()->draw(&painter, context);
            painter.restore();
        };
        drawRich(0, 0, toQColor(clip->textColor()));
    } else {
        if (clip->outlineWidth() > 0.01f) {
            painter.setPen(toQColor(clip->outlineColor()));
            const int ow = std::max(1, static_cast<int>(
                std::lround(clip->outlineWidth())));
            for (int ox = -ow; ox <= ow; ++ox)
                for (int oy = -ow; oy <= ow; ++oy) {
                    if (ox == 0 && oy == 0) continue;
                    painter.drawText(boxRect.translated(ox, oy),
                                     Qt::AlignCenter | Qt::TextWordWrap, text);
                }
        }

        painter.setPen(toQColor(clip->textColor()));
        painter.drawText(boxRect, Qt::AlignCenter | Qt::TextWordWrap, text);
    }
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

    // Store in the memo (one entry per clip), bounded like the graphic cache.
    {
        std::lock_guard<std::mutex> lock(s_ccMtx);
        if (s_ccCache.size() > 64) s_ccCache.clear();
        s_ccCache[clip->id()] = { renderKey, frame };
    }

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
