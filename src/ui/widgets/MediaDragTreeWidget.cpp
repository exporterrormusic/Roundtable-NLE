/*
 * MediaDragTreeWidget.cpp — Premiere Pro-style drag from tree widgets.
 */

#include "widgets/MediaDragTreeWidget.h"
#include "Theme.h"

#include <QDrag>
#include <QHeaderView>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QUrl>
#include <QApplication>

#include <functional>

#include <spdlog/spdlog.h>

namespace rt {
namespace {

void restoreDragSelection(QTreeWidget* tree,
                          const QList<QTreeWidgetItem*>& items,
                          QTreeWidgetItem* currentItem)
{
    if (!tree || items.isEmpty()) return;

    tree->clearSelection();
    for (auto* item : items) {
        if (item)
            item->setSelected(true);
    }
    // NoUpdate: setCurrentItem() defaults to ClearAndSelect in ExtendedSelection
    // mode, which would wipe out the multi-selection we just set above.
    if (currentItem)
        tree->setCurrentItem(currentItem, 0, QItemSelectionModel::NoUpdate);
}

} // namespace

MediaDragTreeWidget::MediaDragTreeWidget(QWidget* parent)
    : QTreeWidget(parent)
{
    // Disable Qt's built-in drag so we can distinguish rubber-band selection
    // (click on empty space) from item drags (click on an item).
    setDragDropMode(QAbstractItemView::NoDragDrop);

    // Premiere Pro-style click-pause-click rename timer
    m_renameTimer = new QTimer(this);
    m_renameTimer->setSingleShot(true);
    m_renameTimer->setInterval(500);
    connect(m_renameTimer, &QTimer::timeout, this, [this]() {
        if (m_renameCandidate && (m_renameCandidate->flags() & Qt::ItemIsEditable))
            editItem(m_renameCandidate, 0);
        m_renameCandidate = nullptr;
    });
}

// ── Mouse handling: rubber band on empty space, drag on items ────────────

void MediaDragTreeWidget::mousePressEvent(QMouseEvent* event)
{
    m_dragStarted = false;
    m_deferSingleSelectOnRelease = false;
    m_pressItem = nullptr;
    m_pendingRubberBand = false;
    m_dragItemsSnapshot.clear();

    if (event->button() == Qt::LeftButton) {
        m_leftPressActive = true;
        m_dragStartPos = event->pos();
        auto* hit = itemAt(event->pos());

        // Treat clicks past the right edge of the last visible column as
        // "empty space" so rubber-band selection works even when items
        // span the full row width (which is the default for QTreeWidget).
        // This matches Windows Explorer: dragging starting from the blank
        // area past the columns lassoes rows.
        if (hit && header()) {
            const int totalColsRight = header()->length() - header()->offset();
            if (event->pos().x() > totalColsRight)
                hit = nullptr;
        }

        m_pressItem = hit;
        if (!hit) {
            // Empty-space click: arm rubber band, but don't start it until
            // the cursor crosses drag threshold.
            m_pendingRubberBand = true;
            m_rubberBanding = false;
            QTreeWidget::mousePressEvent(event); // allow base to clear selection
        } else {
            m_rubberBanding = false;
            m_pendingRubberBand = false;
            const bool hasModifiers = (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));

            // Snapshot the selection BEFORE the base class processes the click
            // so we can detect multi-selection → single transitions.
            auto preSelection = selectedItems();
            bool wasMultiSel = (!hasModifiers && preSelection.size() > 1);

            // QTreeWidget::mousePressEvent() clears a multi-selection down
            // to a single item in ExtendedSelection mode.  We still need
            // the base class for expand/collapse and to set up its internal
            // state machine (pressedIndex, state etc.) for mouseMoveEvent.
            // Save the full QItemSelection beforehand and atomically
            // restore it afterwards so the selection never visually changes.
            QItemSelection savedSel;
            if (wasMultiSel)
                savedSel = selectionModel()->selection();

            QTreeWidget::mousePressEvent(event);

            if (wasMultiSel) {
                // Atomically restore the multi-selection.  This emits a
                // single selectionChanged signal so the view repaints all
                // selected items at once (vs. clearSelection + setSelected
                // which can cause intermediate paint flashes).
                selectionModel()->select(savedSel, QItemSelectionModel::ClearAndSelect);
                // NoUpdate: the no-arg setCurrentItem() collapses the multi-
                // selection back to single in ExtendedSelection mode.
                setCurrentItem(hit, 0, QItemSelectionModel::NoUpdate);
                spdlog::info("mousePress: wasMultiSel, saved {} sel ranges, now {} items selected",
                             savedSel.size(), selectedItems().size());
                m_deferSingleSelectOnRelease = true;
                m_dragItemsSnapshot = preSelection;
            }

            // ── Post-process: apply our custom selection/drag logic ──────
            if (!wasMultiSel) {
                if (!hasModifiers && hit == m_pressItem
                    && preSelection.size() == 1
                    && preSelection.first() == hit) {
                    // Same single item clicked again (no modifiers).
                    // Snapshot the selection for drag, and arm the rename timer.
                    m_dragItemsSnapshot = preSelection;
                    if (m_renameTimer->isActive() && m_renameCandidate == hit) {
                        m_renameTimer->stop();
                        m_renameCandidate = nullptr;
                    } else {
                        if (m_renameTimer->isActive()) {
                            m_renameTimer->stop();
                            m_renameCandidate = nullptr;
                        }
                        m_renameCandidate = hit;
                        m_renameTimer->start();
                    }
                } else {
                    // New item or modifier-click — cancel rename and snapshot
                    if (m_renameTimer->isActive()) {
                        m_renameTimer->stop();
                        m_renameCandidate = nullptr;
                    }
                    m_dragItemsSnapshot = selectedItems();
                }
            }
        }
    } else {
        QTreeWidget::mousePressEvent(event);
    }
}

void MediaDragTreeWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_leftPressActive && m_pendingRubberBand) {
        if ((event->pos() - m_dragStartPos).manhattanLength()
                >= QApplication::startDragDistance()) {
            m_pendingRubberBand = false;
            m_rubberBanding = true;
            if (!m_rubberBand)
                m_rubberBand = new QRubberBand(QRubberBand::Rectangle, viewport());
            m_rubberBand->setGeometry(QRect(m_dragStartPos, QSize()));
            m_rubberBand->show();
        } else {
            QTreeWidget::mouseMoveEvent(event);
            return;
        }
    }

    if (m_rubberBanding && m_rubberBand) {
        // Update rubber band rectangle
        m_rubberBand->setGeometry(QRect(m_dragStartPos, event->pos()).normalized());

        // Select items intersecting the rubber band
        QRect selRect = QRect(m_dragStartPos, event->pos()).normalized();
        // Preserve modifier behaviour: Ctrl toggles, plain replaces
        if (!(event->modifiers() & Qt::ControlModifier))
            clearSelection();
        // Recurse into all descendants so items nested inside sub-bins
        // also get picked up by the lasso.
        std::function<void(QTreeWidgetItem*)> selectIfIn =
            [&](QTreeWidgetItem* node) {
                if (!node) return;
                QRect r = visualItemRect(node);
                if (!r.isEmpty() && selRect.intersects(r))
                    node->setSelected(true);
                for (int c = 0; c < node->childCount(); ++c)
                    selectIfIn(node->child(c));
            };
        for (int i = 0; i < topLevelItemCount(); ++i)
            selectIfIn(topLevelItem(i));
        return;  // Don't call base — it would start a drag
    }

    // If user pressed on an item and moved far enough, start an item drag
    if ((event->buttons() & Qt::LeftButton) && m_leftPressActive && !m_rubberBanding && !m_pendingRubberBand) {
        if ((event->pos() - m_dragStartPos).manhattanLength()
                >= QApplication::startDragDistance()) {

            // Only start a drag if the pressed item actually has ItemIsDragEnabled.
            // Non-draggable items (character/outfit/stance group headers) should
            // fall through to the base class so their expand/collapse still works.
            bool canDrag = false;
            if (m_pressItem && (m_pressItem->flags() & Qt::ItemIsDragEnabled))
                canDrag = true;
            if (!canDrag) {
                // Not draggable — let base class handle the move
                QTreeWidget::mouseMoveEvent(event);
                return;
            }

            m_dragStarted = true;
            // Cancel rename timer when a drag starts
            if (m_renameTimer->isActive()) {
                m_renameTimer->stop();
                m_renameCandidate = nullptr;
            }
            startDrag(Qt::CopyAction);
            // Some platforms consume release during drag; hard-reset state.
            m_leftPressActive = false;
            m_pendingRubberBand = false;
            if (m_rubberBanding && m_rubberBand)
                m_rubberBand->hide();
            m_rubberBanding = false;
            return;
        }
    }
    QTreeWidget::mouseMoveEvent(event);
}

void MediaDragTreeWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_rubberBanding && m_rubberBand) {
        m_rubberBand->hide();
        m_rubberBanding = false;
    }
    m_pendingRubberBand = false;

    if (event->button() == Qt::LeftButton &&
        m_deferSingleSelectOnRelease && !m_dragStarted && m_pressItem) {
        clearSelection();
        m_pressItem->setSelected(true);
        setCurrentItem(m_pressItem);
    }

    m_deferSingleSelectOnRelease = false;
    m_dragStarted = false;
    m_pressItem = nullptr;
    m_leftPressActive = false;

    QTreeWidget::mouseReleaseEvent(event);
}

void MediaDragTreeWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Double-click should never trigger rename — cancel the pending timer
    if (m_renameTimer->isActive()) {
        m_renameTimer->stop();
        m_renameCandidate = nullptr;
    }
    QTreeWidget::mouseDoubleClickEvent(event);
}

// ── Drag: supports multi-selection ───────────────────────────────────────

void MediaDragTreeWidget::startDrag(Qt::DropActions /*supportedActions*/)
{
    QList<QTreeWidgetItem*> items = m_dragItemsSnapshot.isEmpty()
        ? selectedItems()
        : m_dragItemsSnapshot;
    if (items.isEmpty()) return;

    // Only rebuild the selection if it doesn't already match the drag
    // snapshot.  mousePressEvent() already preserves multi-selection,
    // and calling restoreDragSelection() here would clearSelection()
    // first, causing a visual flash to single-selection before the
    // re-select can paint.
    QList<QTreeWidgetItem*> curSel = selectedItems();
    bool selMatches = (curSel.size() == items.size());
    if (selMatches) {
        for (auto* it : items) {
            if (!curSel.contains(it)) { selMatches = false; break; }
        }
    }
    if (!selMatches)
        restoreDragSelection(this, items, m_pressItem ? m_pressItem : currentItem());
    else if (m_pressItem)
        setCurrentItem(m_pressItem, 0, QItemSelectionModel::NoUpdate);

    // Diagnostic: verify the selection state right before drag starts
    spdlog::info("startDrag: {} items in snapshot, {} currently selected, selMatches={}",
                 items.size(), selectedItems().size(), selMatches);

    // Keep the tree widget focused so selected items paint with the
    // active-state colours during the drag (Windows OLE drag may
    // otherwise deactivate the window and dim the selection).
    setFocus(Qt::MouseFocusReason);

    // Force a synchronous repaint so the multi-selection is visually
    // committed before drag->exec() enters the modal event loop.
    viewport()->repaint();

    bool hasBinItems = false;
    for (auto* it : items) {
        if (it->data(0, Qt::UserRole + 2).toBool()) {
            hasBinItems = true;
            break;
        }
    }

    // Internal bin move drag (bin items are not media and should never
    // carry timeline media MIME data).
    if (hasBinItems) {
        auto* mime = new QMimeData;
        mime->setData("application/x-roundtable-bin-item", QByteArray("1"));
        QByteArray itemPtrData;
        for (auto* item : items) {
            if (!itemPtrData.isEmpty()) itemPtrData.append(',');
            itemPtrData.append(QByteArray::number(static_cast<qulonglong>(reinterpret_cast<quintptr>(item))));
        }
        mime->setData("application/x-roundtable-tree-item-ptrs", itemPtrData);

        const auto& tc = Theme::colors();
        // Minimal drag pixmap — single item shows name, multi-item shows tiny count badge
        QPixmap pix;
        if (items.size() == 1) {
            QString label = items.first()->text(0);
            QFont font = this->font();
            QFontMetrics fm(font);
            int w = qBound(80, fm.horizontalAdvance(label) + 16, 200);
            int h = 22;
            pix = QPixmap(w, h);
            pix.fill(Qt::transparent);
            QPainter p(&pix);
            p.setRenderHint(QPainter::Antialiasing);
            QColor bg = tc.surface2;
            bg.setAlpha(200);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(QRect(0, 0, w, h), 4, 4);
            p.setBrush(tc.accent);
            p.drawRoundedRect(QRect(0, 0, 4, h), 2, 2);
            p.setPen(tc.text);
            p.setFont(font);
            p.drawText(QRect(8, 0, w - 12, h), Qt::AlignVCenter | Qt::AlignLeft, label);
            p.end();
        } else {
            // Tiny count badge — stays out of the way
            int sz = 20;
            pix = QPixmap(sz, sz);
            pix.fill(Qt::transparent);
            QPainter p(&pix);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(80, 130, 200, 200));
            p.drawEllipse(QRect(0, 0, sz, sz));
            p.setPen(QColor(255, 255, 255, 240));
            QFont f = p.font();
            f.setPixelSize(11);
            f.setBold(true);
            p.setFont(f);
            p.drawText(QRect(0, 0, sz, sz), Qt::AlignCenter, QString::number(items.size()));
            p.end();
        }

        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(pix);
        drag->setHotSpot(QPoint(pix.width() / 2, pix.height() / 2));
        spdlog::info("ProjectBinDrag: start bin drag ({} selected)", items.size());
        const Qt::DropAction result = drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::CopyAction);
        spdlog::info("ProjectBinDrag: bin drag finished with action {}", static_cast<int>(result));
        // Clear stale item pointers — drag->exec() entered a nested event loop
        // during which the tree may have been cleared, invalidating all pointers.
        m_pressItem = nullptr;
        m_renameCandidate = nullptr;
        m_dragItemsSnapshot.clear();
        restoreDragSelection(this, items, currentItem());
        return;
    }

    // Filter out folder/bin items
    QList<QTreeWidgetItem*> mediaItems;
    for (auto* it : items) {
        if (!it->data(0, Qt::UserRole + 2).toBool())
            mediaItems.append(it);
    }
    if (mediaItems.isEmpty()) return;

    auto* mime = new QMimeData;
    QList<QUrl> urls;
    QByteArray itemPtrData;
    for (auto* item : mediaItems) {
        if (!itemPtrData.isEmpty()) itemPtrData.append(',');
        itemPtrData.append(QByteArray::number(static_cast<qulonglong>(reinterpret_cast<quintptr>(item))));
    }
    if (!itemPtrData.isEmpty())
        mime->setData("application/x-roundtable-tree-item-ptrs", itemPtrData);

    // If single item, preserve the original MIME data format
    if (mediaItems.size() == 1) {
        auto* item = mediaItems.first();
        bool isSequence = item->data(0, Qt::UserRole + 3).toBool();
        bool isAdjustment = item->data(0, Qt::UserRole + 6).toBool();
        if (isAdjustment) {
            mime->setData("application/x-roundtable-adjustment",
                          item->data(0, Qt::UserRole + 7).toString().toUtf8());
        } else if (isSequence) {
            size_t seqIndex = item->data(0, Qt::UserRole + 4).toULongLong();
            int64_t seqDuration = item->data(0, Qt::UserRole + 5).toLongLong();
            bool seqHasAudio = item->data(0, Qt::UserRole + 7).toBool();
            mime->setData("application/x-roundtable-sequence",
                          QByteArray::number(static_cast<qulonglong>(seqIndex)));
            mime->setData("application/x-roundtable-sequence-name",
                          item->text(0).toUtf8());
            mime->setData("application/x-roundtable-sequence-duration",
                          QByteArray::number(static_cast<qlonglong>(seqDuration)));
            mime->setData("application/x-roundtable-sequence-has-audio",
                          QByteArray(seqHasAudio ? "1" : "0"));
        } else {
            uint64_t handle = item->data(0, Qt::UserRole + 1).toULongLong();
            mime->setData("application/x-roundtable-media",
                          QByteArray::number(static_cast<qulonglong>(handle)));
            QString fp = item->data(0, Qt::UserRole).toString();
            if (!fp.isEmpty())
                urls.append(QUrl::fromLocalFile(fp));
        }
    } else {
        // Multi-item: encode all handles and URLs
        QByteArray handleData;
        for (auto* item : mediaItems) {
            bool isSequence = item->data(0, Qt::UserRole + 3).toBool();
            if (isSequence) continue;  // sequences use separate MIME type
            uint64_t handle = item->data(0, Qt::UserRole + 1).toULongLong();
            if (!handleData.isEmpty()) handleData.append(',');
            handleData.append(QByteArray::number(static_cast<qulonglong>(handle)));
            QString fp = item->data(0, Qt::UserRole).toString();
            if (!fp.isEmpty())
                urls.append(QUrl::fromLocalFile(fp));
        }
        if (!handleData.isEmpty())
            mime->setData("application/x-roundtable-media", handleData);
    }
    if (!urls.isEmpty())
        mime->setUrls(urls);

    // Minimal drag pixmap — single item shows name, multi-item shows tiny count badge
    const auto& tc = Theme::colors();
    QPixmap pix;
    if (mediaItems.size() == 1) {
        QString label = mediaItems.first()->text(0);
        QFont font = this->font();
        QFontMetrics fm(font);
        int w = qBound(80, fm.horizontalAdvance(label) + 16, 200);
        int h = 22;
        pix = QPixmap(w, h);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        QColor bg = tc.surface2;
        bg.setAlpha(200);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(QRect(0, 0, w, h), 4, 4);
        p.setBrush(tc.accent);
        p.drawRoundedRect(QRect(0, 0, 4, h), 2, 2);
        p.setPen(tc.text);
        p.setFont(font);
        p.drawText(QRect(8, 0, w - 12, h), Qt::AlignVCenter | Qt::AlignLeft, label);
        p.end();
    } else {
        // Tiny count badge — stays out of the way
        int sz = 20;
        pix = QPixmap(sz, sz);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(80, 130, 200, 200));
        p.drawEllipse(QRect(0, 0, sz, sz));
        p.setPen(QColor(255, 255, 255, 240));
        QFont f = p.font();
        f.setPixelSize(11);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(0, 0, sz, sz), Qt::AlignCenter, QString::number(mediaItems.size()));
        p.end();
    }

    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(pix);
    drag->setHotSpot(QPoint(pix.width() / 2, pix.height() / 2));
    spdlog::info("ProjectBinDrag: start media drag ({} selected)", mediaItems.size());
    const Qt::DropAction result = drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::CopyAction);
    // Clear stale item pointers — drag->exec() entered a nested event loop
    // during which the tree may have been cleared, invalidating all pointers.
    m_pressItem = nullptr;
    m_renameCandidate = nullptr;
    m_dragItemsSnapshot.clear();
    restoreDragSelection(this, items, currentItem());
    spdlog::info("ProjectBinDrag: media drag finished with action {}", static_cast<int>(result));
}

} // namespace rt
