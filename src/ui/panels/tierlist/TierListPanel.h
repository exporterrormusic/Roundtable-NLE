/*
 * TierListPanel — authoring UI for a TierListClip (docked as a timeline tab).
 *
 * Layout ("Side Rail + Pool Bin"):
 *   ┌─────────────────────────┬───────────────┐
 *   │  BOARD (tier rows,      │   EVENTS      │
 *   │  drop target)           │   (rail)      │
 *   ├─────────────────────────┤               │
 *   │  POOL (entry thumbnails, │               │
 *   │  search, add images)    │               │
 *   └─────────────────────────┴───────────────┘
 *
 * Authoring, no ticks/code: drag a pool entry onto a tier row → auto-creates a
 * linked POPUP + DROP event pair; each event is timed with Set Start / Set End
 * buttons that snap to the playhead.  Every edit re-renders the clip live.
 *
 * The two helper widgets below are intentionally NOT Q_OBJECT — they use
 * std::function callbacks so only TierListPanel needs moc.
 */

#pragma once

#include "timeline/TierListClip.h"   // TierEntry / TierEvent (used by Snapshot)
#include "timeline/TimelineObserver.h"

#include <QWidget>
#include <QListWidget>
#include <QColor>
#include <QString>
#include <QStringList>
#include <QPixmap>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class QLineEdit;
class QPushButton;
class QToolButton;
class QSlider;
class QVBoxLayout;
class QLabel;
class QComboBox;
class QMouseEvent;
class QKeyEvent;

namespace rt {

class Timeline;
class Track;
class Clip;
class TierListClip;
class CommandStack;

// ── Board: paints the tier rows and accepts pool-entry drops ────────────────
class TierListBoardWidget : public QWidget
{
public:
    explicit TierListBoardWidget(QWidget* parent = nullptr);

    /// Replace the displayed tiers (label + colour), each tier's entry
    /// thumbnails (drawn left→right in the row) and the entry ids backing
    /// them (same shape as `rows`; used for intra-row reorder drags).
    void setBoard(std::vector<QString> labels,
                  std::vector<QColor>  colors,
                  std::vector<std::vector<QPixmap>> rows,
                  std::vector<std::vector<uint64_t>> ids = {});

    /// The vertical side-label drawn on the left (mirrors the real render).
    void setListTitle(const QString& title);

    /// Called on drop: (entryId, tierIndex). Set by the panel.
    std::function<void(uint64_t, int)> onDropEntry;

    /// Called when an entry thumbnail is dragged to a new slot WITHIN its
    /// row: (entryId, tierIndex, fromIndex, toIndex). Set by the panel.
    std::function<void(uint64_t, int, int, int)> onReorderEntry;

    /// Called when an entry is dragged to a DIFFERENT tier row:
    /// (entryId, fromTier, toTier, toIndex).  Set by the panel.
    std::function<void(uint64_t, int, int, int)> onMoveEntry;

    /// Called when the user presses Delete with a board entry selected.
    /// (entryId).  Set by the panel.
    std::function<void(uint64_t)> onDeleteBoardEntry;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    QSize minimumSizeHint() const override;

private:
    int tierAt(int y) const;
    /// Layout rects of a row's VISIBLE thumbnails (same math as paintEvent;
    /// index-aligned with m_rows/m_ids, truncated at the overflow cutoff).
    std::vector<QRectF> entryRects(int tier) const;

    QString                            m_title;
    std::vector<QString>               m_labels;
    std::vector<QColor>                m_colors;
    std::vector<std::vector<QPixmap>>  m_rows;
    std::vector<std::vector<uint64_t>> m_ids;
    int                                m_hoverTier{-1};
    uint64_t                           m_selectedId{0};  ///< clicked entry, 0 = none

    // Drag-initiation state — selection happens on press, drag on move.
    bool     m_dragArmed{false};
    QPointF  m_dragStartPos;
    int      m_dragTier{0};
    size_t   m_dragIdx{0};
    uint64_t m_dragEntryId{0};
};

// ── Pool list: a QListWidget whose drag carries the entry id ────────────────
// Drag initiation is MANUAL (press + move past threshold), matching every
// working drag source in the app (ThumbnailGrid, MediaDragTreeWidget) — the
// built-in item-view drag machinery never fires here.
class TierListPoolListWidget : public QListWidget
{
public:
    explicit TierListPoolListWidget(QWidget* parent = nullptr);

    /// Role on each item holding the uint64 entry id.
    static constexpr int kEntryIdRole = Qt::UserRole + 1;

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

private:
    QPoint m_dragStartPos;
};

// Detail list view for the pool (QTreeWidget: Name / Rank / Used); defined in
// the .cpp. Drag carries the entry id via the same custom MIME.
class TierListPoolTreeWidget;

// ── The panel ───────────────────────────────────────────────────────────────
class TierListPanel : public QWidget, public TimelineObserver
{
    Q_OBJECT

public:
    explicit TierListPanel(QWidget* parent = nullptr);
    ~TierListPanel() override;

    void setTimeline(Timeline* timeline);
    void setCommandStack(CommandStack* stack) noexcept { m_commandStack = stack; }

    /// TimelineObserver: the timeline (and every clip it owns) is being freed —
    /// drop all cached pointers before returning.
    void onTimelineDestroyed(Timeline* tl) override;

    /// Bind to the selected clip. Non-TierList clips clear the panel.
    void setClip(Clip* clip, Track* track = nullptr);

    /// Rebuild everything from the bound clip.
    void refresh();

signals:
    /// Emitted after any edit so the workspace can re-render + mark the monitor.
    void tierListEdited();

private:
    void buildUI();
    void rebuildPool();
    void rebuildEvents();
    void rebuildBoard();
    void showBound(bool bound);
    QWidget* buildEventRow(size_t eventIndex);
    /// Enforce the sequence invariant: each event's start ≥ the previous
    /// (earlier-starting) event's end, and each event's end ≥ its own start.
    void normalizeEvents();

    void setPoolView(bool iconMode);
    void applyPoolMetrics();          ///< icon size + item hints from mode + zoom
    void onPoolZoom(int percent);
    void showEntryContextMenu(uint64_t entryId, const QPoint& globalPos);
    void removeEntry(uint64_t entryId);
    void renameEntry(uint64_t entryId);

    // Undo/redo: snapshot the whole editable state, swap between snapshots.
    struct Snapshot {
        std::vector<TierDef>     tiers;
        std::vector<TierEntry>   entries;
        std::vector<std::string> subbins;
        std::vector<TierEvent>   events;
        std::string              title;
        TierEntryAspect          aspect{TierEntryAspect::Banner};
        float                    customW{16.0f};
        float                    customH{9.0f};
        float                    topMargin{0.0f};
        float                    rightMargin{0.0f};
        uint32_t                 bgColor{0xFF000000u};
    };
    Snapshot snapshot() const;
    void     restoreSnapshot(TierListClip* clip, const Snapshot& s);
    void     pushUndo(const QString& label, const Snapshot& before);
    /// Resolve a clip id to the live TierListClip on the timeline (nullptr if
    /// it was deleted). Undo/redo lambdas capture ids, never Clip pointers.
    TierListClip* findTierListClipById(uint64_t clipId) const;

    void onAddImages();
    void onSearch(const QString& text);   ///< applies search + subbin filter
    void onEntryDroppedOnTier(uint64_t entryId, int tier);
    void onReorderInRow(uint64_t entryId, int tier, int fromIndex, int toIndex);
    void onMoveEntry(uint64_t entryId, int fromTier, int toTier, int toIndex);
    /// Delete all Popup + Drop events for an entry (board selection + Delete key).
    void onDeleteBoardEntry(uint64_t entryId);
    /// Handle a media file dropped onto the events rail — auto-creates a
    /// TierEntry + linked Popup + Drop event pair.
    void onMediaDroppedOnEvents(const QString& filePath);
    /// Find the most recent (latest start time) DROP event for an entry, or nullptr.
    TierEvent* findDropEventForEntry(uint64_t entryId);
    /// Intercept drops on the events rail panel.
    bool eventFilter(QObject* obj, QEvent* event) override;
    /// The "Grid…" dialog: tiers (add/remove/rename/recolor), entry aspect,
    /// background colour and safe margins.
    void openGridDialog();
    /// Assign an entry to a subbin ("" = none), creating the subbin if new.
    void moveEntryToSubbin(uint64_t entryId, const QString& subbin);

    uint64_t nextEntryId() const;
    int64_t  playheadEventTick() const; ///< persistent list time across split segments
    std::vector<std::vector<uint64_t>> computeFinalRows() const;

    Timeline*     m_timeline{nullptr};
    CommandStack* m_commandStack{nullptr};
    TierListClip* m_clip{nullptr};
    Track*        m_track{nullptr};

    // Widgets
    QWidget*                m_placeholder{nullptr};
    QWidget*                m_content{nullptr};
    TierListBoardWidget*    m_board{nullptr};
    TierListPoolListWidget* m_pool{nullptr};       ///< icon (thumbnail) view
    TierListPoolTreeWidget* m_poolTree{nullptr};   ///< detail list view
    QLineEdit*              m_search{nullptr};
    QComboBox*              m_subbinFilter{nullptr};
    QPushButton*            m_addBtn{nullptr};
    QWidget*                m_eventsHost{nullptr};
    QVBoxLayout*            m_eventsLayout{nullptr};
    QLabel*                 m_titleLabel{nullptr};
    QLineEdit*              m_titleEdit{nullptr};
    QToolButton*            m_listViewBtn{nullptr};
    QToolButton*            m_iconViewBtn{nullptr};
    QSlider*                m_zoomSlider{nullptr};
    bool                    m_poolIconMode{true};
    int                     m_poolZoom{100};   ///< percent (30–300)
};

} // namespace rt
