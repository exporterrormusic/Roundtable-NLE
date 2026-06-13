/*
 * ShotComposerInternal.h — shared helpers for ShotComposer translation units.
 * Split from ShotComposer.cpp for maintainability.
 */
#pragma once

#include "Theme.h"

#include <QImage>
#include <QPainterPath>
#include <QPixmap>
#include <QStyledItemDelegate>
#include <QWidget>
#include <QResizeEvent>
#include <QSizePolicy>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

// ═══════════════════════════════════════════════════════════════════════════
// Packed-alpha helper — unpack BGRA top-half + greyscale bottom-half
// ═══════════════════════════════════════════════════════════════════════════
inline QImage unpackPackedAlpha(const uint8_t* pixels, uint32_t w, uint32_t fullH)
{
    uint32_t halfH = fullH / 2;
    QImage out(static_cast<int>(w), static_cast<int>(halfH),
               QImage::Format_ARGB32_Premultiplied);
    uint32_t stride = w * 4;
    for (uint32_t y = 0; y < halfH; ++y) {
        const uint8_t* rgbRow   = pixels + y * stride;
        const uint8_t* alphaRow = pixels + (y + halfH) * stride;
        auto* dst = reinterpret_cast<uint32_t*>(out.scanLine(static_cast<int>(y)));
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t b = rgbRow[x * 4 + 0];
            uint8_t g = rgbRow[x * 4 + 1];
            uint8_t r = rgbRow[x * 4 + 2];
            uint8_t a = alphaRow[x * 4 + 1];
            r = static_cast<uint8_t>((r * a) / 255);
            g = static_cast<uint8_t>((g * a) / 255);
            b = static_cast<uint8_t>((b * a) / 255);
            dst[x] = (static_cast<uint32_t>(a) << 24) |
                     (static_cast<uint32_t>(r) << 16) |
                     (static_cast<uint32_t>(g) <<  8) |
                     static_cast<uint32_t>(b);
        }
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// AspectRatioContainer — centres its child within a fixed aspect ratio box
// ═══════════════════════════════════════════════════════════════════════════
class AspectRatioContainer : public QWidget
{
public:
    explicit AspectRatioContainer(QWidget* child, double ratio = 16.0 / 9.0,
                                  QWidget* parent = nullptr)
        : QWidget(parent), m_child(child), m_ratio(ratio)
    {
        child->setParent(this);
        setMinimumSize(320, 180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setStyleSheet(QStringLiteral("background: %1;").arg(
            Theme::hex(Theme::colors().surface0)));
    }

protected:
    void resizeEvent(QResizeEvent* ev) override
    {
        QWidget::resizeEvent(ev);
        layoutChild();
    }

private:
    void layoutChild()
    {
        int w = width();
        int h = height();
        int childW = w;
        int childH = static_cast<int>(w / m_ratio);
        if (childH > h) {
            childH = h;
            childW = static_cast<int>(h * m_ratio);
        }
        int x = (w - childW) / 2;
        int y = (h - childH) / 2;
        m_child->setGeometry(x, y, childW, childH);
    }

    QWidget* m_child;
    double   m_ratio;
};

// ── Video-character file lookup (maps lowercase filename → char info) ───────
struct VCInfo { std::string charName; std::string mutePath; std::string talkPath; };

inline const std::unordered_map<std::string, VCInfo>& videoCharacterFiles()
{
    // Wells uses H.264 GREEN-suffixed .mp4 files with a chroma-key
    // green background (#18FF00).  The chroma key is applied inline
    // after decode (see chromaKeyInPlace in decode/PixelOps.h) so the
    // green is never visible anywhere — compositor, thumbnails,
    // library, export — all see a clean alpha frame.
    // NVDEC hardware decode (fast) replaces the ProRes 4444 .mov
    // (software-only, too slow for real-time playback).
    // Preserve the old .mov and .mp4 keys so existing shot presets
    // resolve through ClipSerialization migration.
    static const std::unordered_map<std::string, VCInfo> table {
        // CHRONO — stacked-alpha HEVC proxy (the GREEN chroma-key .mp4s are gone).
        { "wells-chrono-mute.mp4", { "Wells", "assets/videos/WELLS-CHRONO-MUTE_HEVC.mp4", "assets/videos/WELLS-CHRONO-TALK_HEVC.mp4" } },
        { "wells-chrono-talk.mp4", { "Wells", "assets/videos/WELLS-CHRONO-MUTE_HEVC.mp4", "assets/videos/WELLS-CHRONO-TALK_HEVC.mp4" } },
        { "wells-chrono-mute.mov", { "Wells", "assets/videos/WELLS-CHRONO-MUTE_HEVC.mp4", "assets/videos/WELLS-CHRONO-TALK_HEVC.mp4" } },
        { "wells-chrono-talk.mov", { "Wells", "assets/videos/WELLS-CHRONO-MUTE_HEVC.mp4", "assets/videos/WELLS-CHRONO-TALK_HEVC.mp4" } },
        // DRESS outfit — stacked-alpha HEVC (top RGB / bottom alpha, 1080x3776),
        // converted from the ProRes 4444 originals.  Keys cover both the HEVC
        // files and the original .mov so library/migration resolve them to Wells
        // instead of listing them as plain background videos.
        { "wells-dress-mute_hevc.mp4", { "Wells", "assets/videos/WELLS-DRESS-MUTE_HEVC.mp4", "assets/videos/WELLS-DRESS-TALK_HEVC.mp4" } },
        { "wells-dress-talk_hevc.mp4", { "Wells", "assets/videos/WELLS-DRESS-MUTE_HEVC.mp4", "assets/videos/WELLS-DRESS-TALK_HEVC.mp4" } },
        { "wells-dress-mute.mov",      { "Wells", "assets/videos/WELLS-DRESS-MUTE_HEVC.mp4", "assets/videos/WELLS-DRESS-TALK_HEVC.mp4" } },
        { "wells-dress-talk.mov",      { "Wells", "assets/videos/WELLS-DRESS-MUTE_HEVC.mp4", "assets/videos/WELLS-DRESS-TALK_HEVC.mp4" } },
    };
    return table;
}

// ── Video-character outfit catalog ──────────────────────────────────────────
// A video character can have multiple outfits, each swapping the mute/talk
// video pair (mirrors Spine outfit switching).  The first entry is the
// default outfit used when the character is first added to a shot.
struct VCOutfit { std::string name; std::string mutePath; std::string talkPath; };

inline const std::vector<VCOutfit>& videoCharacterOutfitsFor(const std::string& charName)
{
    static const std::unordered_map<std::string, std::vector<VCOutfit>> table {
        { "Wells", {
            // Stacked-alpha HEVC proxies (NVDEC, 1080x3776) — same workflow as
            // the primary character library; export prefers the HEVC over .mov.
            { "CHRONO", "assets/videos/WELLS-CHRONO-MUTE_HEVC.mp4", "assets/videos/WELLS-CHRONO-TALK_HEVC.mp4" },
            { "DRESS",  "assets/videos/WELLS-DRESS-MUTE_HEVC.mp4",  "assets/videos/WELLS-DRESS-TALK_HEVC.mp4" },
        }},
    };
    static const std::vector<VCOutfit> kEmpty;
    auto it = table.find(charName);
    return it != table.end() ? it->second : kEmpty;
}

// ── Filter icon generators (implemented in ShotComposerThumbnailGen.cpp) ──
QPixmap makeAllFilterIcon(int sz);
QPixmap makeUnassignedFilterIcon(int sz);
QPixmap makeFilterDividerIcon(int width);

// ── Custom delegate that clips item painting to rounded rect ─────────────
// Without this, QListWidget in IconMode draws the icon on top of the item
// background without clipping to border-radius, causing images to visually
// spill outside the rounded button area.
class RoundedClipDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(option.rect).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        painter->setClipPath(path);
        QStyledItemDelegate::paint(painter, option, index);
        painter->restore();
    }
};

} // namespace rt
