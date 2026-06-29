/*
 * ThumbnailGrid — Zoomable grid of media thumbnails.
 *
 * Step 16: Project Bin
 *
 * A scrollable grid widget that displays media items as thumbnail cards.
 * Each card shows:
 *   - A thumbnail preview image
 *   - The filename below the image
 *   - A colored border/badge indicating media type
 *
 * Features:
 *   - Adjustable zoom (changes thumbnail size)
 *   - Click to select, double-click to load in source monitor
 *   - Drag from grid → timeline or source monitor
 *   - Keyboard search/filter with external QLineEdit
 *   - Async thumbnail loading via ThumbnailGenerator
 *
 * Layout:
 *   ┌─────────────────────────────────────┐
 *   │ ┌──────┐  ┌──────┐  ┌──────┐      │
 *   │ │thumb │  │thumb │  │thumb │  ... │
 *   │ │      │  │      │  │      │      │
 *   │ ├──────┤  ├──────┤  ├──────┤      │
 *   │ │name  │  │name  │  │name  │      │
 *   │ └──────┘  └──────┘  └──────┘      │
 *   │ ┌──────┐  ┌──────┐  ...            │
 *   │ │ ...  │  │ ...  │                 │
 *   │ └──────┘  └──────┘                 │
 *   └─────────────────────────────────────┘
 */

#pragma once

#include "decode/ThumbnailGenerator.h"

#include <QScrollArea>
#include <QWidget>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rt {

class ThumbnailGenerator;

/// A single item in the thumbnail grid
struct ThumbnailItem
{
    /// Per-instance identity, independent of filePath. Lets the bin hold
    /// multiple independent items that reference the same source file
    /// (Premiere-style "duplicate"). 0 = unassigned (legacy).
    uint64_t              itemId{0};
    std::filesystem::path filePath;
    QString               displayName;
    MediaType             type{MediaType::Unknown};
    std::shared_ptr<Thumbnail> thumbnail;     ///< May be null until async load
    bool                  selected{false};
    bool                  visible{true};       ///< False when filtered out
    uint64_t              mediaHandle{0};      ///< MediaPool handle (0 = not opened)
    uint32_t              labelColor{0xFF888888}; ///< Label color (RGBA)
    bool                  isFolder{false};     ///< True for bin/folder items in icon view
    QString               folderName;          ///< Bin name (non-empty for folder items)
};

/// Scrollable grid of thumbnail items
class ThumbnailGrid : public QWidget
{
    Q_OBJECT

public:
    explicit ThumbnailGrid(QWidget* parent = nullptr);
    ~ThumbnailGrid() override;

    // ── Item management ─────────────────────────────────────────────────

    /// Add a media item to the grid.
    void addItem(const std::filesystem::path& filePath,
                 MediaType type = MediaType::Unknown,
                 uint64_t mediaHandle = 0);

    /// Add multiple items in batch.
    void addItems(const std::vector<std::filesystem::path>& files);

    /// Remove an item by path. Returns true if found and removed.
    bool removeItem(const std::filesystem::path& filePath);

    /// Duplicate the item at srcIndex as a new independent item with a
    /// fresh itemId (same filePath/type/handle/labelColor). Bypasses the
    /// path-dedup that addItem()/addFiles() apply. Returns the new item's
    /// index, or -1 on failure.
    int duplicateItem(int srcIndex);

    /// Remove the item whose itemId matches. Returns true if removed.
    bool removeItemById(uint64_t id);

    /// Index of the item with the given itemId, or -1 if none.
    [[nodiscard]] int indexOfItemId(uint64_t id) const;

    /// Add an item with an explicit identity restored from a saved
    /// project (preserves itemId / displayName / labelColor; allows
    /// duplicate paths). Returns the new item's index.
    int addRestoredItem(const std::filesystem::path& filePath,
                        MediaType type, uint64_t mediaHandle,
                        uint64_t itemId, const QString& displayName,
                        uint32_t labelColor);

    /// Ensure freshly-generated itemIds won't collide with restored ones.
    void ensureItemIdAbove(uint64_t id) noexcept {
        if (id >= m_nextItemId) m_nextItemId = id + 1;
    }

    /// Check if an item with this path already exists.
    [[nodiscard]] bool hasItem(const std::filesystem::path& filePath) const;

    /// Clear all items.
    void clearItems();

    /// Re-order items with Premiere/Finder-style natural sorting by display
    /// name, preserving the visual grouping (folders first, then sequences,
    /// then media). Keeps the icon view consistent with the details tree.
    void sortItems();

    /// Number of items (including filtered-out).
    [[nodiscard]] int itemCount() const noexcept;

    /// Number of visible (not filtered out) items.
    [[nodiscard]] int visibleItemCount() const noexcept;

    /// Get all items (const).
    [[nodiscard]] const std::vector<ThumbnailItem>& items() const noexcept { return m_items; }

    /// Mutable access to items (for setting label colors, etc.).
    std::vector<ThumbnailItem>& mutableItems() noexcept { return m_items; }

    /// Set the label color for an item by index.
    void setItemLabelColor(int index, uint32_t rgba);

    // ── Selection ───────────────────────────────────────────────────────

    /// Get the index of the currently selected item (-1 if none).
    [[nodiscard]] int selectedIndex() const noexcept { return m_selectedIndex; }

    /// Get the selected item (nullptr if none).
    [[nodiscard]] const ThumbnailItem* selectedItem() const noexcept;

    /// Select an item by index.
    void selectItem(int index);

    /// Clear the selection.
    void clearSelection();

    // ── Filtering ───────────────────────────────────────────────────────

    /// Set a text filter. Items whose name doesn't contain the filter
    /// (case-insensitive) are hidden.
    void setFilter(const QString& filter);

    /// Get the current filter string.
    [[nodiscard]] const QString& filter() const noexcept { return m_filter; }

    /// Set a media type filter. MediaType::Unknown means no type filter.
    void setTypeFilter(MediaType type);

    // ── Zoom ────────────────────────────────────────────────────────────

    /// Set the zoom level (0.5 = small, 1.0 = default, 2.0 = large).
    void setZoom(float zoom);

    /// Current zoom level.
    [[nodiscard]] float zoom() const noexcept { return m_zoom; }

    /// Thumbnail cell width at current zoom.
    [[nodiscard]] int cellWidth() const noexcept;

    /// Thumbnail cell height at current zoom (including label).
    [[nodiscard]] int cellHeight() const noexcept;

    // ── Thumbnail generator ─────────────────────────────────────────────

    /// Assign the shared ThumbnailGenerator used for async loading.
    void setThumbnailGenerator(ThumbnailGenerator* gen) noexcept;

    /// Assign the MediaPool for hover-scrub frame decoding.
    void setMediaPool(MediaPool* pool) noexcept { m_pool = pool; }

    /// Trigger thumbnail loading for all visible items that need one.
    void loadVisibleThumbnails();

    /// Refresh a specific item's thumbnail — evicts cache for the path
    /// and queues a fresh generation.  Used when a file is overwritten
    /// on disk (e.g. in Windows Explorer) while the bin is open.
    void refreshItem(const std::filesystem::path& filePath);

    // ── Geometry ────────────────────────────────────────────────────────

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// Emitted when an item is selected (clicked).
    void itemSelected(int index, const std::filesystem::path& filePath);

    /// Emitted when an item is double-clicked (load in source).
    void itemDoubleClicked(int index, const std::filesystem::path& filePath);

    /// Emitted when the user right-clicks an item.
    void itemContextMenu(int index, const QPoint& globalPos);

    /// Emitted when item count changes.
    void itemCountChanged(int count);

    /// Emitted when the user double-clicks empty space (no item under cursor).
    void emptySpaceDoubleClicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void recalcLayout();
    int  hitTest(const QPoint& pos) const;
    bool matchesFilter(const ThumbnailItem& item) const;
    void onThumbnailReady(const std::filesystem::path& path,
                          std::shared_ptr<Thumbnail> thumb);

    // Items
    std::vector<ThumbnailItem> m_items;
    int m_selectedIndex{-1};
    uint64_t m_nextItemId{1};   ///< Monotonic source of ThumbnailItem::itemId

    // Filtering
    QString   m_filter;
    MediaType m_typeFilter{MediaType::Unknown};

    // Appearance
    float    m_zoom{1.0f};
    int      m_baseCellWidth{140};
    int      m_baseCellHeight{120};
    int      m_labelHeight{24};
    int      m_padding{8};
    int      m_columns{1};

    // Drag
    QPoint m_dragStart;

    // Generator
    ThumbnailGenerator* m_generator{nullptr};

    // Hover-scrub
    MediaPool*          m_pool{nullptr};
    const ThumbnailItem* m_hoveredItem{nullptr};
    float               m_hoverNorm{-1.0f};
    std::shared_ptr<Thumbnail> m_hoverScrubThumb;
};

} // namespace rt
