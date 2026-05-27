/*
 * ProjectBinInternal.h — Shared helpers for ProjectBin .cpp files.
 *
 * Contains: Premiere Pro label color constants, premiereDefaultLabel(),
 * and makePremiereBinIcon().  All inline to avoid ODR issues across TUs.
 */

#pragma once

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QString>
#include <QTreeWidgetItem>
#include <QVariant>

#include <filesystem>

#include "media/ThumbnailGenerator.h"   // MediaType

namespace rt {

// Thumbnail data roles for list-view items (shared across ProjectBin TUs)
inline constexpr int kThumbPixmapRole   = Qt::UserRole + 20;   // QPixmap (static thumb)
inline constexpr int kScrubPixmapRole   = Qt::UserRole + 21;   // QPixmap (hover-scrub frame)
inline constexpr int kMediaTypeRole     = Qt::UserRole + 22;   // int (MediaType enum)
inline constexpr int kFrameCountRole    = Qt::UserRole + 23;   // qint64 (total frames)
inline constexpr int kHoverNormRole     = Qt::UserRole + 24;   // float (hover position 0..1, -1 = not hovering)
inline constexpr int kMediaHandleRole   = Qt::UserRole + 1;    // uint64_t (media handle)
inline constexpr int kBinItemIdRole     = Qt::UserRole + 30;   // quint64 (ThumbnailItem::itemId, per-instance identity)

inline const QColor kLabelBin       (204, 153,  51);
inline const QColor kLabelSequence  ( 64, 186,  96);
inline const QColor kLabelVideo     ( 64, 130, 210);
inline const QColor kLabelAudio     ( 74, 180, 110);
inline const QColor kLabelImage     (160,  90, 210);
inline const QColor kLabelSpine     (200, 140,  50);
inline const QColor kLabelAdjustment(255, 170,  68);  // 0xFFFFAA44 — matches ClipSerialization
inline const QColor kLabelUnknown   (136, 136, 136);

// Sentinel path components for synthetic bin items. The angle brackets make
// these invalid Windows filesystem names, so they cannot collide with a real
// imported file.
inline constexpr const char* kAdjustmentSentinelDir = "<adjustments>";
inline constexpr const char* kAdjustmentSentinelExt = ".adj";

/// True if `path` represents a synthetic adjustment-layer bin item.
inline bool projectBinIsAdjustmentPath(const std::filesystem::path& path)
{
    for (const auto& part : path) {
        if (part.string() == kAdjustmentSentinelDir)
            return true;
    }
    return false;
}

inline QColor premiereDefaultLabel(MediaType type, bool isSequence = false, bool isBin = false)
{
    if (isBin)      return kLabelBin;
    if (isSequence) return kLabelSequence;
    switch (type) {
    case MediaType::Video: return kLabelVideo;
    case MediaType::Audio: return kLabelAudio;
    case MediaType::Image: return kLabelImage;
    case MediaType::Spine: return kLabelSpine;
    default:               return kLabelUnknown;
    }
}

inline QIcon makePremiereBinIcon(const QColor& color, const QString& shape, int sz = 16)
{
    qreal dpr = 2.0;
    QPixmap px(sz * dpr, sz * dpr);
    px.setDevicePixelRatio(dpr);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);

    if (shape == "bin") {
        p.setBrush(color);
        QPainterPath tab;
        tab.moveTo(1, 4);
        tab.lineTo(1, 2);
        tab.quadTo(1, 1, 2, 1);
        tab.lineTo(6, 1);
        tab.lineTo(7, 4);
        tab.closeSubpath();
        p.drawPath(tab);
        p.drawRoundedRect(QRectF(1, 4, sz - 2, sz - 5), 1.5, 1.5);
    }
    else if (shape == "sequence") {
        // Film reel frame: frame rectangle with sprocket holes on sides
        p.setBrush(color);
        // Main film frame
        p.drawRoundedRect(QRectF(3, 2, sz - 6, sz - 4), 1.5, 1.5);
        // Inner darker frame area
        QColor inner = color.darker(130);
        inner.setAlpha(200);
        p.setBrush(inner);
        p.drawRoundedRect(QRectF(5, 4, sz - 10, sz - 8), 1, 1);
        // Sprocket holes - left side
        p.setBrush(color.lighter(150));
        double holeH = std::max(1.5, (sz - 8) / 5.0);
        for (int i = 0; i < 4; ++i) {
            double y = 3 + (i + 0.5) * ((sz - 6) / 4.0);
            p.drawRoundedRect(QRectF(1, y - holeH / 2, 2.5, holeH), 0.5, 0.5);
            p.drawRoundedRect(QRectF(sz - 3.5, y - holeH / 2, 2.5, holeH), 0.5, 0.5);
        }
        // Center play triangle
        p.setBrush(color.lighter(180));
        QPainterPath tri;
        double cx = sz / 2.0 + 1.5;
        double cy = sz / 2.0;
        tri.moveTo(cx - 3, cy - 2.5);
        tri.lineTo(cx + 2.5, cy);
        tri.lineTo(cx - 3, cy + 2.5);
        tri.closeSubpath();
        p.drawPath(tri);
    }
    else if (shape == "video") {
        p.setBrush(color);
        p.drawRoundedRect(QRectF(1, 2, sz - 2, sz - 4), 2, 2);
        p.setBrush(QColor(14, 14, 20));
        for (int i = 0; i < 3; ++i) {
            double y = 3.5 + i * 3.0;
            p.drawRect(QRectF(2.5, y, 1.5, 1.5));
            p.drawRect(QRectF(sz - 4.0, y, 1.5, 1.5));
        }
    }
    else if (shape == "audio") {
        p.setPen(QPen(color, 1.5));
        double cy = sz / 2.0;
        double bars[] = {3, 6, 5, 7, 4, 6, 3};
        int n = 7;
        double spacing = (sz - 4.0) / (n - 1);
        for (int i = 0; i < n; ++i) {
            double x = 2 + i * spacing;
            double h = bars[i];
            p.drawLine(QPointF(x, cy - h / 2), QPointF(x, cy + h / 2));
        }
    }
    else if (shape == "image") {
        p.setBrush(color);
        p.drawRoundedRect(QRectF(1, 2, sz - 2, sz - 4), 2, 2);
        p.setBrush(QColor(14, 14, 20));
        QPainterPath mountain;
        mountain.moveTo(2, sz - 3);
        mountain.lineTo(sz / 2.0 - 1, 5);
        mountain.lineTo(sz - 2, sz - 3);
        mountain.closeSubpath();
        p.drawPath(mountain);
        p.setBrush(QColor(220, 200, 80));
        p.drawEllipse(QPointF(sz - 4.5, 4.5), 1.5, 1.5);
    }
    else if (shape == "spine") {
        p.setBrush(color);
        QPainterPath diamond;
        diamond.moveTo(sz / 2.0, 1);
        diamond.lineTo(sz - 2, sz / 2.0);
        diamond.lineTo(sz / 2.0, sz - 1);
        diamond.lineTo(2, sz / 2.0);
        diamond.closeSubpath();
        p.drawPath(diamond);
    }
    else if (shape == "adjustment") {
        // Stacked-layers glyph — three offset rounded rectangles
        p.setBrush(color);
        p.drawRoundedRect(QRectF(2.5, 2.0,    sz - 7.0, 4.0), 1.0, 1.0);
        QColor mid = color; mid.setAlpha(210);
        p.setBrush(mid);
        p.drawRoundedRect(QRectF(3.5, sz / 2.0 - 2.0, sz - 7.0, 4.0), 1.0, 1.0);
        QColor low = color; low.setAlpha(160);
        p.setBrush(low);
        p.drawRoundedRect(QRectF(4.5, sz - 6.0, sz - 7.0, 4.0), 1.0, 1.0);
    }
    else {
        p.setBrush(color);
        p.drawRoundedRect(QRectF(3, 1, sz - 6, sz - 2), 1.5, 1.5);
    }

    p.end();
    return QIcon(px);
}

// ── Bin tree helpers (shared across ProjectBin TUs) ──────────────────────

/// Get the persistent key for a tree widget item. Media items use their
/// per-instance "@<itemId>" so two entries that reference the same file
/// stay distinct across save/reload; bins/sequences fall back to the
/// path/text key (kept for backward compatibility with old projects).
inline QString projectBinItemKey(QTreeWidgetItem* item)
{
    const quint64 id = item->data(0, kBinItemIdRole).toULongLong();
    if (id != 0)
        return QStringLiteral("@") + QString::number(id);
    QString key = item->data(0, Qt::UserRole).toString();
    if (key.isEmpty()) key = item->text(0);
    return key;
}

/// Find a child bin by name under a parent tree item.
inline QTreeWidgetItem* projectBinFindChildBin(QTreeWidgetItem* parent, const QString& name)
{
    if (!parent) return nullptr;
    for (int i = 0; i < parent->childCount(); ++i) {
        auto* child = parent->child(i);
        if (child->data(0, Qt::UserRole + 2).toBool() && child->text(0) == name)
            return child;
    }
    return nullptr;
}

/// Create a new bin tree item with Premiere Pro styling.
inline QTreeWidgetItem* projectBinCreateBinItem(const QString& name)
{
    auto* binItem = new QTreeWidgetItem();
    binItem->setText(0, name);
    binItem->setData(0, Qt::UserRole + 2, true);
    binItem->setIcon(0, makePremiereBinIcon(kLabelBin, "bin"));
    binItem->setData(0, Qt::UserRole + 10, QVariant::fromValue(kLabelBin));
    binItem->setFlags(binItem->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsEditable);
    return binItem;
}

} // namespace rt
