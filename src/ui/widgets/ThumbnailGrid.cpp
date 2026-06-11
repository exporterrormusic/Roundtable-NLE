/*
 * ThumbnailGrid.cpp — Zoomable grid of media thumbnails.
 * Step 16
 */

#include "widgets/ThumbnailGrid.h"
#include "PathUtils.h"
#include "Theme.h"
#include "playback/MediaPool.h"

#include <spdlog/spdlog.h>

#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════════

ThumbnailGrid::ThumbnailGrid(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(100, 100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

ThumbnailGrid::~ThumbnailGrid() = default;

// ═════════════════════════════════════════════════════════════════════════════
//  Item management
// ═════════════════════════════════════════════════════════════════════════════

void ThumbnailGrid::addItem(const std::filesystem::path& filePath,
                             MediaType type,
                             uint64_t mediaHandle)
{
    ThumbnailItem item;
    item.itemId      = m_nextItemId++;
    item.filePath    = filePath;
    item.displayName = QString::fromStdString(pathToUtf8(filePath.filename()));
    item.type        = (type == MediaType::Unknown)
                         ? ThumbnailGenerator::detectMediaType(filePath)
                         : type;
    item.mediaHandle = mediaHandle;
    item.visible     = matchesFilter(item);

    m_items.push_back(std::move(item));

    recalcLayout();
    update();
    emit itemCountChanged(itemCount());
}

void ThumbnailGrid::addItems(const std::vector<std::filesystem::path>& files)
{
    m_items.reserve(m_items.size() + files.size());
    for (const auto& f : files)
    {
        ThumbnailItem item;
        item.itemId      = m_nextItemId++;
        item.filePath    = f;
        item.displayName = QString::fromStdString(pathToUtf8(f.filename()));
        item.type        = ThumbnailGenerator::detectMediaType(f);
        item.visible     = matchesFilter(item);
        m_items.push_back(std::move(item));
    }

    recalcLayout();
    update();
    emit itemCountChanged(itemCount());
}

bool ThumbnailGrid::removeItem(const std::filesystem::path& filePath)
{
    auto it = std::find_if(m_items.begin(), m_items.end(),
        [&](const ThumbnailItem& item) { return item.filePath == filePath; });

    if (it == m_items.end()) return false;

    int idx = static_cast<int>(std::distance(m_items.begin(), it));
    if (m_selectedIndex == idx) m_selectedIndex = -1;
    else if (m_selectedIndex > idx) --m_selectedIndex;

    m_items.erase(it);
    recalcLayout();
    update();
    emit itemCountChanged(itemCount());
    return true;
}

int ThumbnailGrid::duplicateItem(int srcIndex)
{
    if (srcIndex < 0 || srcIndex >= static_cast<int>(m_items.size()))
        return -1;

    ThumbnailItem copy = m_items[static_cast<size_t>(srcIndex)];
    copy.itemId   = m_nextItemId++;
    copy.selected = false;
    // Thumbnail is shareable (same source media), keep it for instant display.
    m_items.push_back(std::move(copy));

    recalcLayout();
    update();
    emit itemCountChanged(itemCount());
    return static_cast<int>(m_items.size()) - 1;
}

bool ThumbnailGrid::removeItemById(uint64_t id)
{
    if (id == 0) return false;
    auto it = std::find_if(m_items.begin(), m_items.end(),
        [&](const ThumbnailItem& item) { return item.itemId == id; });

    if (it == m_items.end()) return false;

    int idx = static_cast<int>(std::distance(m_items.begin(), it));
    if (m_selectedIndex == idx) m_selectedIndex = -1;
    else if (m_selectedIndex > idx) --m_selectedIndex;

    m_items.erase(it);
    recalcLayout();
    update();
    emit itemCountChanged(itemCount());
    return true;
}

int ThumbnailGrid::indexOfItemId(uint64_t id) const
{
    if (id == 0) return -1;
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].itemId == id)
            return static_cast<int>(i);
    return -1;
}

int ThumbnailGrid::addRestoredItem(const std::filesystem::path& filePath,
                                   MediaType type, uint64_t mediaHandle,
                                   uint64_t itemId, const QString& displayName,
                                   uint32_t labelColor)
{
    ThumbnailItem item;
    item.itemId      = itemId ? itemId : m_nextItemId++;
    item.filePath    = filePath;
    item.displayName = displayName.isEmpty()
        ? QString::fromStdString(pathToUtf8(filePath.filename()))
        : displayName;
    item.type        = (type == MediaType::Unknown)
                         ? ThumbnailGenerator::detectMediaType(filePath)
                         : type;
    item.mediaHandle = mediaHandle;
    item.labelColor  = labelColor;
    item.visible     = matchesFilter(item);
    ensureItemIdAbove(item.itemId);

    m_items.push_back(std::move(item));
    recalcLayout();
    update();
    emit itemCountChanged(itemCount());
    return static_cast<int>(m_items.size()) - 1;
}

bool ThumbnailGrid::hasItem(const std::filesystem::path& filePath) const
{
    return std::any_of(m_items.begin(), m_items.end(),
        [&](const ThumbnailItem& item) { return item.filePath == filePath && !item.isFolder; });
}

void ThumbnailGrid::clearItems()
{
    m_items.clear();
    m_selectedIndex = -1;
    recalcLayout();
    update();
    emit itemCountChanged(0);
}

int ThumbnailGrid::itemCount() const noexcept
{
    return static_cast<int>(m_items.size());
}

int ThumbnailGrid::visibleItemCount() const noexcept
{
    return static_cast<int>(std::count_if(m_items.begin(), m_items.end(),
        [](const ThumbnailItem& i) { return i.visible; }));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Selection
// ═════════════════════════════════════════════════════════════════════════════

const ThumbnailItem* ThumbnailGrid::selectedItem() const noexcept
{
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size()))
        return &m_items[m_selectedIndex];
    return nullptr;
}

void ThumbnailGrid::selectItem(int index)
{
    if (index < 0 || index >= static_cast<int>(m_items.size()))
        index = -1;

    if (m_selectedIndex != index)
    {
        // Deselect old
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size()))
            m_items[m_selectedIndex].selected = false;

        m_selectedIndex = index;

        // Select new
        if (m_selectedIndex >= 0)
        {
            m_items[m_selectedIndex].selected = true;
            emit itemSelected(m_selectedIndex, m_items[m_selectedIndex].filePath);
        }

        update();
    }
}

void ThumbnailGrid::clearSelection()
{
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_items.size()))
        m_items[m_selectedIndex].selected = false;
    m_selectedIndex = -1;
    update();
}

void ThumbnailGrid::setItemLabelColor(int index, uint32_t rgba)
{
    if (index >= 0 && index < static_cast<int>(m_items.size())) {
        m_items[index].labelColor = rgba;
        update();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Filtering
// ═════════════════════════════════════════════════════════════════════════════

bool ThumbnailGrid::matchesFilter(const ThumbnailItem& item) const
{
    // Type filter
    if (m_typeFilter != MediaType::Unknown && item.type != m_typeFilter)
        return false;

    // Text filter
    if (!m_filter.isEmpty())
    {
        if (!item.displayName.contains(m_filter, Qt::CaseInsensitive))
            return false;
    }

    return true;
}

void ThumbnailGrid::setFilter(const QString& filter)
{
    if (m_filter == filter) return;
    m_filter = filter;

    for (auto& item : m_items) {
        if (item.isFolder) continue;  // folder visibility managed externally
        item.visible = matchesFilter(item);
    }

    recalcLayout();
    update();
}

void ThumbnailGrid::setTypeFilter(MediaType type)
{
    if (m_typeFilter == type) return;
    m_typeFilter = type;

    for (auto& item : m_items) {
        if (item.isFolder) continue;  // folder visibility managed externally
        item.visible = matchesFilter(item);
    }

    recalcLayout();
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Zoom
// ═════════════════════════════════════════════════════════════════════════════

void ThumbnailGrid::setZoom(float zoom)
{
    m_zoom = std::clamp(zoom, 0.3f, 3.0f);
    recalcLayout();
    update();
}

int ThumbnailGrid::cellWidth() const noexcept
{
    return static_cast<int>(m_baseCellWidth * m_zoom);
}

int ThumbnailGrid::cellHeight() const noexcept
{
    return static_cast<int>((m_baseCellHeight + m_labelHeight) * m_zoom);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Thumbnail generator
// ═════════════════════════════════════════════════════════════════════════════

void ThumbnailGrid::setThumbnailGenerator(ThumbnailGenerator* gen) noexcept
{
    m_generator = gen;
}

void ThumbnailGrid::refreshItem(const std::filesystem::path& filePath)
{
    if (!m_generator) return;

    // Clear the cached thumbnail for this item so it re-fetches.
    for (auto& item : m_items) {
        if (item.filePath == filePath) {
            item.thumbnail.reset();
            break;
        }
    }

    // Evict the generator's in-memory cache for this path so the next
    // requestThumbnail generates from the new file content on disk.
    m_generator->evictPath(filePath);

    // Queue a fresh thumbnail generation.
    m_generator->requestThumbnail(
        filePath,
        [this](const std::filesystem::path& path, std::shared_ptr<Thumbnail> thumb) {
            QMetaObject::invokeMethod(this,
                [this, path, thumb]() { onThumbnailReady(path, thumb); },
                Qt::QueuedConnection);
        },
        static_cast<uint32_t>(cellWidth()));
}

void ThumbnailGrid::loadVisibleThumbnails()
{
    if (!m_generator) return;

    auto isAdjustmentPath = [](const std::filesystem::path& path) {
        for (const auto& part : path) {
            if (pathToUtf8(part) == "<adjustments>")
                return true;
        }
        return false;
    };

    for (auto& item : m_items)
    {
        if (item.visible && !item.thumbnail && !item.isFolder &&
            !isAdjustmentPath(item.filePath))
        {
            m_generator->requestThumbnail(
                item.filePath,
                [this](const std::filesystem::path& path, std::shared_ptr<Thumbnail> thumb)
                {
                    // This is called from a worker thread — must post to GUI
                    QMetaObject::invokeMethod(this,
                        [this, path, thumb]() { onThumbnailReady(path, thumb); },
                        Qt::QueuedConnection);
                },
                static_cast<uint32_t>(cellWidth()));
        }
    }
}

void ThumbnailGrid::onThumbnailReady(const std::filesystem::path& path,
                                      std::shared_ptr<Thumbnail> thumb)
{
    bool found = false;
    for (auto& item : m_items)
    {
        if (item.filePath == path)
        {
            item.thumbnail = thumb;
            found = true;
            break;
        }
    }
    if (!found) {
        spdlog::warn("ThumbnailGrid: thumbnail ready for '{}' but no matching item",
                     pathToUtf8(path.filename()));
    }
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Geometry
// ═════════════════════════════════════════════════════════════════════════════

QSize ThumbnailGrid::sizeHint() const
{
    return QSize(400, 300);
}

QSize ThumbnailGrid::minimumSizeHint() const
{
    return QSize(100, 80);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Layout
// ═════════════════════════════════════════════════════════════════════════════

void ThumbnailGrid::recalcLayout()
{
    int cw = cellWidth() + m_padding;
    int w  = width();
    m_columns = std::max(1, w / std::max(cw, 1));

    // Calculate required height
    int visCount = visibleItemCount();
    int rows     = (visCount + m_columns - 1) / std::max(m_columns, 1);
    int totalH   = rows * (cellHeight() + m_padding) + m_padding;
    setMinimumHeight(totalH);
}

int ThumbnailGrid::hitTest(const QPoint& pos) const
{
    int cw = cellWidth() + m_padding;
    int ch = cellHeight() + m_padding;
    if (cw <= 0 || ch <= 0) return -1;

    int col = (pos.x() - m_padding) / cw;
    int row = (pos.y() - m_padding) / ch;

    if (col < 0 || col >= m_columns) return -1;

    int visIdx = row * m_columns + col;

    // Map from visible index to actual index
    int found = 0;
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i)
    {
        if (m_items[i].visible)
        {
            if (found == visIdx) return i;
            ++found;
        }
    }
    return -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Events
// ═════════════════════════════════════════════════════════════════════════════

void ThumbnailGrid::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto& tc = Theme::colors();

    int cw = cellWidth();
    int ch = cellHeight();
    int thumbH = static_cast<int>(m_baseCellHeight * m_zoom);
    int pad = m_padding;

    int x = pad, y = pad;
    int col = 0;

    for (const auto& item : m_items)
    {
        if (!item.visible) continue;

        QRect cellRect(x, y, cw, ch);

        // Background
        QColor bgColor = tc.surface1;
        if (item.selected)
            bgColor = tc.accentDim;

        p.fillRect(cellRect, bgColor);

        // Type color (used for stripe and placeholder)
        QColor typeColor;
        switch (item.type)
        {
        case MediaType::Video: typeColor = tc.clipVideo; break;
        case MediaType::Image: typeColor = QColor(180, 130, 70); break;
        case MediaType::Audio: typeColor = tc.clipAudio; break;
        case MediaType::Spine: typeColor = tc.clipSpine; break;
        default:               typeColor = tc.surface3; break;
        }

        // Type indicator stripe (left edge) — skip for folders
        if (!item.isFolder)
            p.fillRect(QRect(x, y, 3, ch), typeColor);

        // Thumbnail image
        QRect thumbRect(x + 4, y + 2, cw - 6, thumbH - 4);
        if (item.isFolder)
        {
            // Draw a folder icon
            p.fillRect(thumbRect, tc.surface0);
            int fw = std::min(thumbRect.width() * 2 / 3, thumbRect.height());
            int fh = fw * 3 / 4;
            int fx = thumbRect.x() + (thumbRect.width() - fw) / 2;
            int fy = thumbRect.y() + (thumbRect.height() - fh) / 2;
            int tabW = fw * 2 / 5;
            int tabH = fh / 6;

            QPainterPath folderPath;
            // Tab on top-left
            folderPath.moveTo(fx, fy + tabH);
            folderPath.lineTo(fx, fy);
            folderPath.lineTo(fx + tabW, fy);
            folderPath.lineTo(fx + tabW + tabH, fy + tabH);
            // Top edge and body
            folderPath.lineTo(fx + fw, fy + tabH);
            folderPath.lineTo(fx + fw, fy + fh);
            folderPath.lineTo(fx, fy + fh);
            folderPath.closeSubpath();

            p.setBrush(QColor(0xCC, 0x99, 0x33));  // warm folder yellow
            p.setPen(QColor(0xAA, 0x77, 0x22));
            p.drawPath(folderPath);
            p.setPen(tc.textPrimary);
        }
        else if (item.thumbnail && item.thumbnail->valid && !item.thumbnail->pixels.empty())
        {
            QImage img(item.thumbnail->pixels.data(),
                       static_cast<int>(item.thumbnail->width),
                       static_cast<int>(item.thumbnail->height),
                       static_cast<int>(item.thumbnail->stride),
                       QImage::Format_ARGB32);  // BGRA pixel data = ARGB32 on little-endian

            // Preserve aspect ratio within thumbRect
            double srcAspect = static_cast<double>(item.thumbnail->width) / item.thumbnail->height;
            double dstAspect = static_cast<double>(thumbRect.width()) / thumbRect.height();
            QRect drawRect = thumbRect;
            if (srcAspect > dstAspect) {
                // Source is wider — letterbox vertically
                int h = static_cast<int>(thumbRect.width() / srcAspect);
                drawRect.setTop(thumbRect.top() + (thumbRect.height() - h) / 2);
                drawRect.setHeight(h);
            } else {
                // Source is taller — pillarbox horizontally
                int w = static_cast<int>(thumbRect.height() * srcAspect);
                drawRect.setLeft(thumbRect.left() + (thumbRect.width() - w) / 2);
                drawRect.setWidth(w);
            }

            // Fill background behind letterbox/pillarbox
            if (drawRect != thumbRect)
                p.fillRect(thumbRect, tc.surface0);

            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(drawRect, img);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);

            // ── Hover-scrub indicator ────────────────────────────────
            if (&item == m_hoveredItem && m_hoverScrubThumb &&
                m_hoverScrubThumb->valid && !m_hoverScrubThumb->pixels.empty()) {
                QImage scrubImg(m_hoverScrubThumb->pixels.data(),
                                static_cast<int>(m_hoverScrubThumb->width),
                                static_cast<int>(m_hoverScrubThumb->height),
                                static_cast<int>(m_hoverScrubThumb->stride),
                                QImage::Format_ARGB32);
                double sSrcAspect = static_cast<double>(m_hoverScrubThumb->width) / m_hoverScrubThumb->height;
                QRect scrubRect = thumbRect;
                if (sSrcAspect > dstAspect) {
                    int h2 = static_cast<int>(thumbRect.width() / sSrcAspect);
                    scrubRect.setTop(thumbRect.top() + (thumbRect.height() - h2) / 2);
                    scrubRect.setHeight(h2);
                } else {
                    int w2 = static_cast<int>(thumbRect.height() * sSrcAspect);
                    scrubRect.setLeft(thumbRect.left() + (thumbRect.width() - w2) / 2);
                    scrubRect.setWidth(w2);
                }
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                p.drawImage(scrubRect, scrubImg);
                p.setRenderHint(QPainter::SmoothPixmapTransform, false);

                // White scrub position line
                int lineX = thumbRect.left() + static_cast<int>(m_hoverNorm * thumbRect.width());
                p.setPen(QPen(QColor(255, 255, 255, 200), 1));
                p.drawLine(lineX, thumbRect.top(), lineX, thumbRect.bottom());
                p.setPen(tc.textPrimary);
            }
        }
        else
        {
            // Placeholder
            p.fillRect(thumbRect, typeColor.darker(200));
            p.setPen(typeColor.lighter(150));
            p.drawText(thumbRect, Qt::AlignCenter, "...");
        }

        // Media Offline overlay — only if no valid handle AND file missing
        if (!item.isFolder && item.mediaHandle == 0 &&
            !item.filePath.empty() && !std::filesystem::exists(item.filePath)) {
            // Semi-transparent red overlay
            p.fillRect(thumbRect, QColor(180, 30, 30, 140));
            // "MEDIA OFFLINE" text
            QFont offlineFont = p.font();
            offlineFont.setPixelSize(std::max(10, static_cast<int>(11 * m_zoom)));
            offlineFont.setBold(true);
            p.setFont(offlineFont);
            p.setPen(QColor(255, 255, 255, 230));
            p.drawText(thumbRect, Qt::AlignCenter, "MEDIA\nOFFLINE");
            p.setFont(QFont());  // reset
        }

        // Label
        QRect labelRect(x + 2, y + thumbH, cw - 4, m_labelHeight);
        p.setPen(tc.textPrimary);
        QFont font = p.font();
        font.setPixelSize(std::max(10, static_cast<int>(11 * m_zoom)));
        p.setFont(font);

        // Draw label color dot before the text if non-default
        int textOffset = 2;
        if (item.labelColor != 0xFF888888) {
            int dotSize = std::max(6, static_cast<int>(8 * m_zoom));
            int dotY = y + thumbH + (m_labelHeight - dotSize) / 2;
            p.setBrush(QColor::fromRgba(item.labelColor));
            p.setPen(Qt::NoPen);
            p.drawEllipse(x + 4, dotY, dotSize, dotSize);
            p.setPen(tc.textPrimary);
            textOffset = 6 + dotSize;
        }

        QString elided = p.fontMetrics().elidedText(item.displayName, Qt::ElideMiddle, cw - 8 - textOffset);
        p.drawText(QRect(x + textOffset, y + thumbH, cw - 4 - textOffset, m_labelHeight),
                   Qt::AlignLeft | Qt::AlignVCenter, elided);

        // Advance position
        col++;
        if (col >= m_columns)
        {
            col = 0;
            x = pad;
            y += ch + pad;
        }
        else
        {
            x += cw + pad;
        }
    }

    --s_paintDepth;
}

void ThumbnailGrid::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);

    if (event->button() == Qt::LeftButton)
    {
        m_dragStart = event->pos();
        int idx = hitTest(event->pos());
        selectItem(idx);
    }
    else if (event->button() == Qt::RightButton)
    {
        int idx = hitTest(event->pos());
        selectItem(idx);
        if (idx >= 0)
            emit itemContextMenu(idx, event->globalPosition().toPoint());
    }
    QWidget::mousePressEvent(event);
}

void ThumbnailGrid::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        int idx = hitTest(event->pos());
        if (idx >= 0)
            emit itemDoubleClicked(idx, m_items[idx].filePath);
        else
            emit emptySpaceDoubleClicked();
    }
    QWidget::mouseDoubleClickEvent(event);
}

void ThumbnailGrid::mouseMoveEvent(QMouseEvent* event)
{
    // ── Drag initiation (left-button held) ──────────────────────────────
    if (event->buttons() & Qt::LeftButton) {
        if ((event->pos() - m_dragStart).manhattanLength() < QApplication::startDragDistance())
            return;
        if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_items.size()))
            return;
        const auto& item = m_items[m_selectedIndex];
        if (item.isFolder) return;

        auto* drag = new QDrag(this);
        auto* mime = new QMimeData;
        // Adjustment-layer bin item — synthetic sentinel path, no media handle.
        // Detected by the "<adjustments>" path component (same scheme as the
        // list-view tree). Emits its own MIME type so the timeline knows to
        // create an AdjustmentClip.
        bool isAdjustment = false;
        for (const auto& part : item.filePath) {
            if (pathToUtf8(part) == "<adjustments>") { isAdjustment = true; break; }
        }
        if (isAdjustment) {
            mime->setData("application/x-roundtable-adjustment",
                          item.displayName.toUtf8());
        } else {
            mime->setUrls({QUrl::fromLocalFile(QString::fromStdString(pathToUtf8(item.filePath)))});
            mime->setData("application/x-roundtable-media",
                          QByteArray::number(static_cast<qulonglong>(item.mediaHandle)));
        }
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
        return;
    }

    // ── Hover-scrub (no button) ────────────────────────────────────────
    int idx = hitTest(event->pos());
    const ThumbnailItem* newHovered = nullptr;
    if (idx >= 0 && idx < static_cast<int>(m_items.size()))
        newHovered = &m_items[idx];

    // If we moved to a different item, clear scrub state
    if (newHovered != m_hoveredItem) {
        m_hoveredItem = newHovered;
        m_hoverScrubThumb.reset();
        m_hoverNorm = -1.0f;
        if (!newHovered || newHovered->type != MediaType::Video) {
            update();
            return;
        }
    }

    // Only scrub videos with loaded thumbnails
    if (!m_hoveredItem || m_hoveredItem->type != MediaType::Video || !m_pool)
        return;

    // Calculate the thumbnail rect for this item (mirror paintEvent layout)
    int cw = cellWidth();
    int ch = cellHeight();
    int thumbH = static_cast<int>(m_baseCellHeight * m_zoom);
    int pad = m_padding;

    // Find the item's position in the grid
    int visIdx = 0;
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
        if (&m_items[i] == m_hoveredItem) break;
        if (m_items[i].visible) ++visIdx;
    }
    int col = visIdx % m_columns;
    int row = visIdx / m_columns;
    int itemX = pad + col * (cw + pad);
    int itemY = pad + row * (ch + pad);
    QRect thumbRect(itemX + 4, itemY + 2, cw - 6, thumbH - 4);

    // Check if mouse is within the thumbnail area
    if (!thumbRect.contains(event->pos())) {
        if (m_hoverNorm >= 0.0f) {
            m_hoverNorm = -1.0f;
            m_hoverScrubThumb.reset();
            update();
        }
        return;
    }

    float norm = static_cast<float>(event->pos().x() - thumbRect.left()) /
                 static_cast<float>(thumbRect.width());
    norm = std::clamp(norm, 0.0f, 1.0f);
    m_hoverNorm = norm;

    // Get frame count from MediaPool
    uint64_t handle = m_hoveredItem->mediaHandle;
    if (handle == 0) return;
    const auto* info = m_pool->getInfo(handle);
    if (!info || info->frameCount <= 1) return;

    int64_t frameNum = static_cast<int64_t>(norm * (info->frameCount - 1));
    auto frame = m_pool->getFrame(handle, frameNum, ResolutionTier::Quarter, true);
    if (frame && !frame->pixels.empty()) {
        auto thumb = std::make_shared<Thumbnail>();
        thumb->width  = frame->width;
        thumb->height = frame->height;
        thumb->stride = frame->stride;
        thumb->pixels = frame->pixels;
        thumb->valid  = true;
        m_hoverScrubThumb = thumb;
    }
    update();
}

void ThumbnailGrid::leaveEvent(QEvent* event)
{
    if (m_hoveredItem) {
        m_hoveredItem = nullptr;
        m_hoverScrubThumb.reset();
        m_hoverNorm = -1.0f;
        update();
    }
    QWidget::leaveEvent(event);
}

void ThumbnailGrid::resizeEvent(QResizeEvent* event)
{
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;
    recalcLayout();
    QWidget::resizeEvent(event);
    s_inResize = false;
}

} // namespace rt

